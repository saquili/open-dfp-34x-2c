// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the standard classes without counters: the VEIP and its slot, the
 * LCT UNI, and the tables the ONU keeps for each bridge port.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "me/fdb.h"
#include "me/table.h"
#include "me/tables.h"
#include "me/upload.h"
#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/array_size.h"
#include "omcid/byteorder.h"

#include "harness.h"

#define ME_CARDHOLDER 5u
#define ME_CIRCUIT_PACK 6u
#define ME_BRIDGE 45u
#define ME_BRIDGE_PORT 47u
#define ME_BRIDGE_PORT_FILTER 49u
#define ME_BRIDGE_PORT_TABLE 50u
#define ME_BRIDGE_PORT_PREASSIGN 79u
#define ME_LCT_UNI 83u
#define ME_VEIP 329u

#define MT_CREATE 4u
#define MT_DELETE 6u
#define MT_SET 8u
#define MT_GET 9u
#define MT_AVC 17u

#define VEIP 0x0601u
#define VEIP_SLOT 0x0106u
#define PORT 0x0201u

/* Result codes of an answer (G.988 A.3). */
#define R_OK 0u
#define R_PARAM_ERROR 3u
#define R_DEVICE_BUSY 6u

static uint64_t attr(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		     uint8_t no)
{
	uint8_t b[32];
	size_t len = 0;
	uint64_t v = 0;

	if (omci_mib_attr_get(mib, cls, inst, no, b, sizeof(b), &len) !=
	    OMCI_OK)
		return UINT64_MAX;
	for (size_t k = 0; k < len && k < 8u; k++)
		v = (v << 8) | b[k];
	return v;
}

static bool up(struct omci_t_fixture *fx)
{
	struct omci_hw_caps caps;

	if (!omci_t_up(fx))
		return false;
	return omci_hw_caps(&caps) == OMCI_OK &&
	       omci_me_defaults_apply(fx->mib, &caps) == OMCI_OK;
}

/* Send a request and return the result byte of its answer, or 0xff. */
static uint8_t request(struct omci_engine *eng, uint16_t tci, uint8_t mt,
		       uint16_t cls, uint16_t inst, const uint8_t *contents,
		       size_t len)
{
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, tci, mt, true, false, cls, inst);
	if (contents != NULL)
		memcpy(&wire[8], contents, len);
	omci_t_tx_reset();
	if (omci_t_rx(eng, wire) != OMCI_OK)
		return 0xffu;
	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++)
		if ((omci_t_tx.frame[i][2] & 0x20u) != 0u)
			return omci_t_tx.frame[i][8];
	return 0xffu;
}

static void test_the_veip_has_its_slot_and_the_lct_uni_is_there(void)
{
	struct omci_t_fixture fx;
	const struct omci_me_class *lct = omci_me_class_by_id(ME_LCT_UNI);

	CHECK(up(&fx));

	CHECK_EQ(attr(fx.mib, ME_CARDHOLDER, VEIP_SLOT, 1u), 48u);
	CHECK_EQ(attr(fx.mib, ME_CIRCUIT_PACK, VEIP_SLOT, 1u), 48u);
	CHECK_EQ(attr(fx.mib, ME_CIRCUIT_PACK, VEIP_SLOT, 2u), 1u);
	/* Locked, disabled, and with no IANA assigned port. */
	CHECK_EQ(attr(fx.mib, ME_VEIP, VEIP, 1u), 1u);
	CHECK_EQ(attr(fx.mib, ME_VEIP, VEIP, 2u), 1u);
	CHECK_EQ(attr(fx.mib, ME_VEIP, VEIP, 5u), 0xffffu);

	CHECK_EQ(attr(fx.mib, ME_LCT_UNI, 0u, 1u), 0u);
	/* An LCT UNI is not reported in a MIB upload (G.988 9.13.3). */
	CHECK(lct != NULL);
	CHECK(!omci_me_upload_includes(lct));

	omci_t_down(&fx);
}

static void test_the_veip_is_enabled_while_the_stick_routes(void)
{
	struct omci_t_fixture fx;
	bool avc = false;

	CHECK(up(&fx));
	omci_engine_tick(fx.eng, 1000u);
	CHECK_EQ(attr(fx.mib, ME_VEIP, VEIP, 2u), 1u);

	omci_hw_routing_force(true);
	omci_t_tx_reset();
	omci_engine_tick(fx.eng, 1001u);
	CHECK_EQ(attr(fx.mib, ME_VEIP, VEIP, 2u), 0u);
	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++) {
		const uint8_t *f = omci_t_tx.frame[i];

		if ((f[2] & 0x1Fu) == MT_AVC && omci_be16(&f[4]) == ME_VEIP)
			avc = true;
	}
	CHECK(avc);

	/* Nothing changes while the mode stays. */
	omci_t_tx_reset();
	omci_engine_tick(fx.eng, 1002u);
	CHECK_EQ(attr(fx.mib, ME_VEIP, VEIP, 2u), 0u);

	omci_hw_routing_force(false);
	omci_engine_tick(fx.eng, 1003u);
	CHECK_EQ(attr(fx.mib, ME_VEIP, VEIP, 2u), 1u);

	omci_t_down(&fx);
}

static void test_a_bridge_port_comes_and_goes_with_its_tables(void)
{
	static const uint16_t tables[] = { ME_BRIDGE_PORT_FILTER,
					   ME_BRIDGE_PORT_TABLE,
					   ME_BRIDGE_PORT_PREASSIGN };
	const uint8_t port[10] = { 0x00, 0x01, 0x02, 0x01, 0x01, 0x01 };
	struct omci_t_fixture fx;

	CHECK(up(&fx));
	CHECK_EQ(request(fx.eng, 0x0100u, MT_CREATE, ME_BRIDGE, 1u, NULL, 0),
		 R_OK);
	CHECK_EQ(request(fx.eng, 0x0101u, MT_CREATE, ME_BRIDGE_PORT, PORT, port,
			 sizeof(port)),
		 R_OK);
	for (size_t i = 0; i < ARRAY_SIZE(tables); i++)
		CHECK(omci_mib_exists(fx.mib, tables[i], PORT));
	/* Every filter forwards at first. */
	CHECK_EQ(attr(fx.mib, ME_BRIDGE_PORT_PREASSIGN, PORT, 9u), 0u);

	/* The OLT can make none of them itself. */
	CHECK(request(fx.eng, 0x0102u, MT_CREATE, ME_BRIDGE_PORT_PREASSIGN,
		      0x0301u, NULL, 0) != R_OK);

	CHECK_EQ(request(fx.eng, 0x0103u, MT_DELETE, ME_BRIDGE_PORT, PORT, NULL,
			 0),
		 R_OK);
	for (size_t i = 0; i < ARRAY_SIZE(tables); i++)
		CHECK(!omci_mib_exists(fx.mib, tables[i], PORT));

	omci_t_down(&fx);
}

/* Set one row of the MAC filter table of the port. */
static uint8_t filter_row(struct omci_engine *eng, uint16_t tci, uint8_t entry,
			  uint8_t filter_byte, uint8_t last)
{
	const uint8_t body[10] = { 0x80u, 0x00u, entry, filter_byte, 0x00u,
				   0x00u, 0x5eu, 0x00u, 0x53u,	     last };

	return request(eng, tci, MT_SET, ME_BRIDGE_PORT_FILTER, PORT, body,
		       sizeof(body));
}

static size_t filter_rows(struct omci_mib *mib)
{
	return omci_mib_table_count(mib, ME_BRIDGE_PORT_FILTER, PORT, 1u);
}

static void test_the_mac_filter_table_is_kept_by_entry_number(void)
{
	const uint8_t port[10] = { 0x00, 0x01, 0x02, 0x01, 0x01, 0x01 };
	struct omci_t_fixture fx;
	uint8_t row[8];

	CHECK(up(&fx));
	CHECK_EQ(request(fx.eng, 0x0200u, MT_CREATE, ME_BRIDGE, 1u, NULL, 0),
		 R_OK);
	CHECK_EQ(request(fx.eng, 0x0201u, MT_CREATE, ME_BRIDGE_PORT, PORT, port,
			 sizeof(port)),
		 R_OK);

	/* Add entries 0 and 1, then 1 again with another address. */
	CHECK_EQ(filter_row(fx.eng, 0x0202u, 0u, 0x41u, 0x10u), R_OK);
	CHECK_EQ(filter_row(fx.eng, 0x0203u, 1u, 0x41u, 0x11u), R_OK);
	CHECK_EQ(filter_row(fx.eng, 0x0204u, 1u, 0x43u, 0x12u), R_OK);
	CHECK_EQ(filter_rows(fx.mib), 2u);
	CHECK_EQ(omci_mib_table_get(fx.mib, ME_BRIDGE_PORT_FILTER, PORT, 1u, 1,
				    row, sizeof(row)),
		 OMCI_OK);
	CHECK_EQ(row[1], 0x43u);
	CHECK_EQ(row[7], 0x12u);

	/* Remove entry 0, and entry 5, which is not there. */
	CHECK_EQ(filter_row(fx.eng, 0x0205u, 0u, 0x01u, 0x10u), R_OK);
	CHECK_EQ(filter_row(fx.eng, 0x0206u, 5u, 0x00u, 0x10u), R_OK);
	CHECK_EQ(filter_rows(fx.mib), 1u);

	/* 11 is reserved: refused. */
	CHECK_EQ(filter_row(fx.eng, 0x0207u, 2u, 0xC1u, 0x13u), R_PARAM_ERROR);

	/* The table holds as many addresses as it may, and no more. */
	for (uint8_t e = 0; e < OMCI_ME_MAC_FILTER_ROWS; e++)
		CHECK_EQ(filter_row(fx.eng, (uint16_t)(0x0210u + e), e, 0x41u,
				    e),
			 R_OK);
	CHECK_EQ(filter_rows(fx.mib), OMCI_ME_MAC_FILTER_ROWS);
	CHECK(filter_row(fx.eng, 0x0230u, 0x20u, 0x41u, 0x20u) != R_OK);
	/* Replacing an entry there is still room for. */
	CHECK_EQ(filter_row(fx.eng, 0x0231u, 3u, 0x40u, 0x21u), R_OK);

	/* Clear the whole table. */
	CHECK_EQ(filter_row(fx.eng, 0x0232u, 0u, 0x80u, 0u), R_OK);
	CHECK_EQ(filter_rows(fx.mib), 0u);

	omci_t_down(&fx);
}

static void port_of(struct omci_engine *eng, uint16_t tci, uint16_t inst,
		    uint8_t tp_type)
{
	const uint8_t port[10] = { 0x00, 0x01, 0x02, tp_type, 0x01, 0x01 };

	CHECK_EQ(request(eng, tci, MT_CREATE, ME_BRIDGE_PORT, inst, port,
			 sizeof(port)),
		 R_OK);
}

static void test_the_bridge_table_lists_the_addresses_of_its_side(void)
{
	static const struct omci_hw_fdb fdb[] = {
		{ { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x01 }, false, true },
		{ { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x02 }, true, false },
		{ { 0x00, 0x00, 0x5e, 0x00, 0x53, 0x03 }, false, false },
	};
	const struct omci_me_table_source *src = &omci_fdb_bridge_table;
	struct omci_t_fixture fx;
	uint8_t row[8];
	size_t n = 0;

	CHECK(up(&fx));
	CHECK_EQ(request(fx.eng, 0x0300u, MT_CREATE, ME_BRIDGE, 1u, NULL, 0),
		 R_OK);
	port_of(fx.eng, 0x0301u, 0x0101u, 1u);
	port_of(fx.eng, 0x0302u, 0x0102u, 5u);
	port_of(fx.eng, 0x0303u, 0x0103u, 11u);
	omci_hw_fdb_force(fdb, ARRAY_SIZE(fdb));

	/* The UNI side: a learned address and a set one. */
	CHECK_EQ(src->count(0x0101u), 2u);
	CHECK(src->entry(0x0101u, 0, row, sizeof(row)));
	CHECK_EQ(omci_be16(&row[0]), 0x0004u);
	CHECK_EQ(row[7], 0x01u);
	CHECK(src->entry(0x0101u, 1, row, sizeof(row)));
	CHECK_EQ(omci_be16(&row[0]), 0x0000u);
	CHECK_EQ(row[7], 0x03u);
	CHECK(!src->entry(0x0101u, 2, row, sizeof(row)));
	CHECK(!src->entry(0x0101u, 0, row, 7u));
	CHECK(!src->entry(0x0101u, 0, NULL, sizeof(row)));

	/* A row of another port reads that port first. */
	CHECK(src->entry(0x0102u, 0, row, sizeof(row)));
	CHECK_EQ(row[7], 0x02u);
	CHECK_EQ(src->count(0x0102u), 1u);

	/* A VEIP, a port that is not there, a switch that does not answer. */
	CHECK_EQ(src->count(0x0103u), 0u);
	CHECK_EQ(src->count(0x0999u), 0u);
	omci_hw_fdb_fail_next(true);
	CHECK_EQ(src->count(0x0101u), 0u);
	omci_hw_fdb_fail_next(false);
	CHECK_EQ(omci_hw_fdb(NULL, 0, &n), OMCI_ERR_INVAL);

	/* A Get of the table gives its size: two rows of eight bytes. */
	const uint8_t mask[2] = { 0x80u, 0x00u };
	uint8_t wire[OMCI_FRAME_LEN];

	omci_t_build(wire, 0x0304u, MT_GET, true, false, ME_BRIDGE_PORT_TABLE,
		     0x0101u);
	memcpy(&wire[8], mask, sizeof(mask));
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(fx.eng, wire), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	CHECK_EQ(omci_t_tx.frame[0][8], R_OK);
	CHECK_EQ(omci_be32(&omci_t_tx.frame[0][11]), 16u);

	omci_hw_fdb_force(NULL, 0);
	omci_t_down(&fx);
	CHECK_EQ(src->count(0x0101u), 0u);
}

int main(void)
{
	RUN(test_the_veip_has_its_slot_and_the_lct_uni_is_there);
	RUN(test_the_veip_is_enabled_while_the_stick_routes);
	RUN(test_a_bridge_port_comes_and_goes_with_its_tables);
	RUN(test_the_mac_filter_table_is_kept_by_entry_number);
	RUN(test_the_bridge_table_lists_the_addresses_of_its_side);
	return OMCI_T_REPORT("test_std_classes");
}
