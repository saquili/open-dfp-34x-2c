// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of what the engine keeps across a start again of the daemon: the
 * number that counts the OLT's changes, and the bytes of omci_engine_save()
 * that omci_engine_load() goes on from.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "me/gem.h"
#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "harness.h"

enum {
	MT_CREATE = 4,
	MT_DELETE = 6,
	MT_SET = 8,
	MT_GET = 9,
	MT_MIB_RESET = 15,
	MT_SYNC_TIME = 24,

	ONU_DATA = 2,
	ONU_G = 256,
};

static uint16_t g_tci = 0x100u;

/* Send the OLT's request; the result byte of the answer, or 0xff for none. */
static uint8_t request(struct omci_engine *eng, uint8_t mt, uint16_t class_id,
		       uint16_t instance, const uint8_t *contents, size_t len)
{
	struct omci_frame f = {
		.tci = g_tci++,
		.msg_type = mt,
		.ar = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = class_id,
		.instance = instance,
	};
	uint8_t wire[OMCI_FRAME_LEN];

	memcpy(f.contents, contents, len);
	(void)!omci_frame_build(&f, wire);
	omci_t_tx_reset();
	if (omci_engine_rx(eng, wire) != OMCI_OK || omci_t_last_tx() == NULL)
		return 0xff;
	return omci_t_last_tx()[8];
}

static const uint8_t g_gem[] = { 0x01, 0x91, 0x80, 0x00, 0x03 };
static const uint8_t g_alloc[] = { 0x80, 0x00, 0x01, 0x00 };
static const uint8_t g_mds[] = { 0x80, 0x00, 0x2a };
static const uint8_t g_date[] = { 0x07, 0xea, 0x08, 0x0f, 0x0c, 0x1e, 0x2d };

/* A fixture whose MIB has the ONU's own instances. */
static void up(struct omci_t_fixture *fx)
{
	struct omci_hw_caps caps;

	CHECK(omci_t_up(fx));
	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(fx->mib, &caps), OMCI_OK);
}

static void test_the_olts_changes_are_counted(void)
{
	struct omci_t_fixture fx;
	const uint8_t none[1] = { 0 };

	up(&fx);
	CHECK_EQ(omci_engine_changes(NULL), 0u);
	CHECK_EQ(omci_engine_changes(fx.eng), 0u);

	/* A read changes nothing, and neither does a request refused unread. */
	CHECK_EQ(request(fx.eng, MT_GET, ONU_DATA, 0, g_mds, 2), 0u);
	CHECK(request(fx.eng, MT_SET, 0xfffd, 0, g_mds, sizeof(g_mds)) != 0u);
	CHECK_EQ(omci_engine_changes(fx.eng), 0u);

	CHECK_EQ(request(fx.eng, MT_CREATE, OMCI_GEM_CLASS_ID, 0x0191, g_gem,
			 sizeof(g_gem)),
		 0u);
	CHECK_EQ(omci_engine_changes(fx.eng), 1u);
	CHECK_EQ(request(fx.eng, MT_SET, ONU_DATA, 0, g_mds, sizeof(g_mds)),
		 0u);
	CHECK_EQ(request(fx.eng, MT_DELETE, OMCI_GEM_CLASS_ID, 0x0191, none, 0),
		 0u);
	CHECK_EQ(request(fx.eng, MT_SYNC_TIME, ONU_G, 0, g_date,
			 sizeof(g_date)),
		 0u);
	CHECK_EQ(request(fx.eng, MT_MIB_RESET, ONU_DATA, 0, none, 0), 0u);
	CHECK_EQ(omci_engine_changes(fx.eng), 5u);

	/* A change that fails is counted all the same. */
	CHECK(request(fx.eng, MT_DELETE, OMCI_GEM_CLASS_ID, 0x0191, none, 0) !=
	      0u);
	CHECK_EQ(omci_engine_changes(fx.eng), 6u);
	omci_t_down(&fx);
}

static uint8_t g_saved[OMCI_ENGINE_SAVE_MAX];
static uint8_t g_again[OMCI_ENGINE_SAVE_MAX];

/*
 * An engine set up by the OLT: a T-CONT and a GEM port on it, the MIB data
 * sync and the time of day. Return: the bytes it saved, in g_saved.
 */
static size_t save_set_up(void)
{
	struct omci_t_fixture fx;
	size_t len = 0;

	up(&fx);
	CHECK_EQ(request(fx.eng, MT_SET, OMCI_GEM_TCONT_CLASS_ID, 0x8000u,
			 g_alloc, sizeof(g_alloc)),
		 0u);
	CHECK_EQ(request(fx.eng, MT_CREATE, OMCI_GEM_CLASS_ID, 0x0191, g_gem,
			 sizeof(g_gem)),
		 0u);
	CHECK_EQ(request(fx.eng, MT_SET, ONU_DATA, 0, g_mds, sizeof(g_mds)),
		 0u);
	omci_engine_tick(fx.eng, 100u);
	CHECK_EQ(request(fx.eng, MT_SYNC_TIME, ONU_G, 0, g_date,
			 sizeof(g_date)),
		 0u);
	omci_engine_tick(fx.eng, 200u);
	CHECK_EQ(omci_engine_save(fx.eng, g_saved, sizeof(g_saved), &len),
		 OMCI_OK);
	CHECK(len > 36u);
	omci_t_down(&fx);
	return len;
}

static void test_an_engine_goes_on_where_it_stood(void)
{
	const size_t len = save_set_up();
	const struct omci_hw_gem_port *ports = NULL;
	const uint16_t *alloc = NULL;
	struct omci_t_fixture fx;
	size_t len2 = 0;

	/* The hardware forgets, as a backend that starts again does. */
	CHECK_EQ(omci_hw_gem_apply(NULL, 0, NULL), OMCI_OK);
	CHECK_EQ(omci_hw_tcont_apply(NULL, 0, NULL), OMCI_OK);

	CHECK(omci_t_up(&fx));
	CHECK_EQ(omci_engine_load(fx.eng, g_saved, len), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 0u);

	/* The data path is programmed again from the MIB. */
	CHECK_EQ(omci_hw_gem_recorded(&ports), 1u);
	CHECK_EQ(ports[0].port_id, 0x0191u);
	CHECK(omci_hw_tcont_recorded(&alloc) > 0u);
	CHECK_EQ(alloc[0], 0x0100u);

	/* The OLT reads what it set, and the bytes come out the same. */
	CHECK_EQ(request(fx.eng, MT_GET, ONU_DATA, 0, g_mds, 2), 0u);
	CHECK_EQ(omci_t_last_tx()[11], 0x2au);
	CHECK_EQ(omci_engine_save(fx.eng, g_again, sizeof(g_again), &len2),
		 OMCI_OK);
	CHECK_EQ(len2, len);
	CHECK_EQ(memcmp(g_again, g_saved, len), 0);
	omci_t_down(&fx);
}

/* The saved bytes with byte @at changed to @v, loaded into a new engine. */
static enum omci_status load_changed(size_t len, size_t at, uint8_t v)
{
	struct omci_t_fixture fx;
	enum omci_status rc;

	up(&fx);
	memcpy(g_again, g_saved, len);
	g_again[at] = v;
	rc = omci_engine_load(fx.eng, g_again, len);
	CHECK(rc == OMCI_OK || omci_mib_count(fx.mib) == 0u);
	omci_t_down(&fx);
	return rc;
}

static void test_bytes_of_another_kind_are_refused(void)
{
	const size_t len = save_set_up();
	struct omci_t_fixture fx;
	size_t n = 0;

	up(&fx);
	CHECK_EQ(omci_engine_save(NULL, g_again, sizeof(g_again), &n),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_engine_save(fx.eng, NULL, sizeof(g_again), &n),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_engine_save(fx.eng, g_again, sizeof(g_again), NULL),
		 OMCI_ERR_INVAL);
	CHECK_EQ(omci_engine_save(fx.eng, g_again, 35u, &n),
		 OMCI_ERR_TRUNCATED);
	CHECK_EQ(omci_engine_save(fx.eng, g_again, 40u, &n),
		 OMCI_ERR_TRUNCATED);
	CHECK_EQ(omci_engine_load(NULL, g_saved, len), OMCI_ERR_INVAL);
	CHECK_EQ(omci_engine_load(fx.eng, NULL, len), OMCI_ERR_INVAL);
	CHECK(omci_mib_count(fx.mib) > 0u);
	omci_t_down(&fx);

	CHECK_EQ(load_changed(35u, 0, 'O'), OMCI_ERR_TRUNCATED);
	CHECK_EQ(load_changed(len, 0, 'X'), OMCI_ERR_PROTO);
	CHECK_EQ(load_changed(len, 5, 2), OMCI_ERR_PROTO);
	CHECK_EQ(load_changed(len, 36, 'X'), OMCI_ERR_PROTO);
	CHECK_EQ(load_changed(len - 1u, 0, 'O'), OMCI_ERR_TRUNCATED);
	CHECK_EQ(load_changed(len, 0, 'O'), OMCI_OK);
}

int main(void)
{
	RUN(test_the_olts_changes_are_counted);
	RUN(test_an_engine_goes_on_where_it_stood);
	RUN(test_bytes_of_another_kind_are_refused);
	return OMCI_T_REPORT("test_keep");
}
