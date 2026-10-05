// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests that the engine reaches the hardware only through the backend in use. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"

#include "harness.h"

enum {
	C_CAPS,
	C_SOURCE,
	C_VLAN,
	C_GEM,
	C_BRIDGE,
	C_ALARMS,
	C_PON,
	C_COUNTER,
	C_OPTICS,
	C_COUNT
};

static unsigned int g_calls[C_COUNT];
static const struct omci_hw_ops *g_host;

static enum omci_status spy_caps(struct omci_hw_caps *out)
{
	g_calls[C_CAPS]++;
	return g_host->caps(out);
}
static const char *spy_source(void)
{
	g_calls[C_SOURCE]++;
	return g_host->source();
}
static enum omci_status spy_vlan(const struct omci_hw_vlan_xlate *r, size_t n,
				 struct omci_hw_reject *rej)
{
	g_calls[C_VLAN]++;
	return g_host->vlan_apply(r, n, rej);
}
static enum omci_status spy_gem(const struct omci_hw_gem_port *p, size_t n,
				struct omci_hw_reject *rej)
{
	g_calls[C_GEM]++;
	return g_host->gem_apply(p, n, rej);
}
static enum omci_status spy_bridge(const struct omci_hw_bridge_port *p,
				   size_t n, struct omci_hw_reject *rej)
{
	g_calls[C_BRIDGE]++;
	return g_host->bridge_apply(p, n, rej);
}
static enum omci_status spy_alarms(uint32_t *mask)
{
	g_calls[C_ALARMS]++;
	return g_host->alarms(mask);
}
static enum omci_status spy_pon(struct omci_hw_pon *out)
{
	g_calls[C_PON]++;
	return g_host->pon(out);
}
static enum omci_status spy_counter(unsigned int port,
				    uint64_t out[OMCI_HW_PORT_COUNTERS])
{
	g_calls[C_COUNTER]++;
	return g_host->port_counters(port, out);
}
static enum omci_status spy_optics(struct omci_hw_optics *out)
{
	g_calls[C_OPTICS]++;
	return g_host->optics(out);
}

static const struct omci_hw_ops g_spy = {
	.caps = spy_caps,
	.source = spy_source,
	.vlan_apply = spy_vlan,
	.gem_apply = spy_gem,
	.bridge_apply = spy_bridge,
	.alarms = spy_alarms,
	.pon = spy_pon,
	.port_counters = spy_counter,
	.optics = spy_optics,
};

static enum omci_status odd_caps(struct omci_hw_caps *out)
{
	const enum omci_status rc = g_host->caps(out);

	if (rc == OMCI_OK)
		out->num_tcont = 1;
	return rc;
}
static enum omci_status odd_optics(struct omci_hw_optics *out)
{
	memset(out, 0, sizeof(*out));
	out->temperature = 0x1234u;
	out->valid = (uint8_t)(1u << (unsigned int)OMCI_HW_OPTIC_TEMPERATURE);
	return OMCI_OK;
}
static struct omci_hw_ops g_odd;

static void request(struct omci_engine *eng, uint16_t tid, uint8_t mt,
		    uint16_t cls, uint16_t inst, uint8_t first)
{
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, tid, mt, true, false, cls, inst);
	wire[8] = first;
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(eng, wire), OMCI_OK);
}

static void test_the_table_is_complete_and_is_the_host_backends(void)
{
	const struct omci_hw_ops *t = omci_hw();

	CHECK(t != NULL);
	if (t == NULL)
		return;
	CHECK(t->caps != NULL);
	CHECK(t->source != NULL);
	CHECK(t->vlan_apply != NULL);
	CHECK(t->gem_apply != NULL);
	CHECK(t->bridge_apply != NULL);
	CHECK(t->alarms != NULL);
	CHECK(t->pon != NULL);
	CHECK(t->port_counters != NULL);
	CHECK(t->gem_counts != NULL);
	CHECK(t->optics != NULL);

	struct omci_hw_caps a, b;

	CHECK_EQ(t->caps(&a), OMCI_OK);
	CHECK_EQ(omci_hw_caps(&b), OMCI_OK);
	CHECK_EQ(a.port_count, b.port_count);
	CHECK_EQ(a.num_tcont, b.num_tcont);
	CHECK_EQ(a.ani_instance, b.ani_instance);
	CHECK(strcmp(a.identity.vendor_id, b.identity.vendor_id) == 0);
	CHECK(strcmp(t->source(), omci_hw_source()) == 0);
}

static void test_every_kind_of_request_reaches_the_table(void)
{
	struct omci_t_fixture fx;

	g_host = omci_hw();
	memset(g_calls, 0, sizeof(g_calls));
	omci_hw_ops_set(&g_spy);
	if (!omci_t_up(&fx)) {
		omci_hw_ops_set(NULL);
		return;
	}

	request(fx.eng, 0x0A01u, 15u, 2u, 0u, 0u);
	CHECK(g_calls[C_CAPS] >= 1u);
	CHECK(g_calls[C_VLAN] >= 1u);
	CHECK(g_calls[C_GEM] >= 1u);
	CHECK(g_calls[C_BRIDGE] >= 1u);

	request(fx.eng, 0x0A02u, 18u, 263u, 0x8001u, 7u);
	CHECK(g_calls[C_OPTICS] >= 1u);

	request(fx.eng, 0x0A03u, 11u, 2u, 0u, 0u);
	CHECK(g_calls[C_ALARMS] >= 1u);

	omci_engine_tick(fx.eng, 1u);
	CHECK(g_calls[C_PON] >= 1u);

	request(fx.eng, 0x0A04u, 4u, 296u, 0x0101u, 0u);
	request(fx.eng, 0x0A05u, 9u, 296u, 0x0101u, 0x40u);
	CHECK(g_calls[C_COUNTER] >= 1u);

	omci_t_down(&fx);
	omci_hw_ops_set(NULL);
	CHECK(omci_hw() == g_host);
}

static void test_the_engine_uses_what_the_table_answers(void)
{
	struct omci_t_fixture fx;

	g_host = omci_hw();
	g_odd = *g_host;
	g_odd.caps = odd_caps;
	g_odd.optics = odd_optics;
	omci_hw_ops_set(&g_odd);
	if (!omci_t_up(&fx)) {
		omci_hw_ops_set(NULL);
		return;
	}

	request(fx.eng, 0x0A11u, 15u, 2u, 0u, 0u);
	CHECK(omci_mib_exists(fx.mib, 262u, 0x8000u));
	CHECK(!omci_mib_exists(fx.mib, 262u, 0x8001u));

	request(fx.eng, 0x0A12u, 18u, 263u, 0x8001u, 7u);
	CHECK_EQ(omci_t_tx.n, 2u);
	if (omci_t_tx.n == 2u) {
		const uint8_t *r = omci_t_tx.frame[1];

		CHECK_EQ(r[8u + 12u], 12u);
		CHECK_EQ((((unsigned int)r[8u + 13u] << 8) | r[8u + 14u]),
			 0x1234u);
	}

	omci_t_down(&fx);
	omci_hw_ops_set(NULL);
}

int main(void)
{
	RUN(test_the_table_is_complete_and_is_the_host_backends);
	RUN(test_every_kind_of_request_reaches_the_table);
	RUN(test_the_engine_uses_what_the_table_answers);
	return OMCI_T_REPORT("test_hw_ops");
}
