// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the UNI's rate towards the subscriber, from the outbound traffic
 * descriptor of its bridge port, as Set requests of the OLT change it.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/bridge.h"
#include "me/defaults.h"
#include "me/gem.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "sys/log.h"

#include "harness.h"
#include "log_capture.h"

#define UNI_PORT 0x0101u
#define NET_PORT 0x0102u
#define TD_A 0x0001u
#define TD_B 0x0002u

static uint8_t tx[4][OMCI_FRAME_LEN];
static size_t tx_count;

static void on_send(void *ctx, const uint8_t frame[static OMCI_FRAME_LEN])
{
	(void)ctx;
	if (tx_count < 4)
		memcpy(tx[tx_count++], frame, OMCI_FRAME_LEN);
}

static const struct omci_tx_ops tx_ops = { .send = on_send };

static void set_u16(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		    uint8_t attr, uint16_t v)
{
	const uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };

	CHECK_EQ(omci_mib_attr_set(mib, cls, inst, attr, b, sizeof(b)),
		 OMCI_OK);
}

static void set_u8(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		   uint8_t attr, uint8_t v)
{
	CHECK_EQ(omci_mib_attr_set(mib, cls, inst, attr, &v, sizeof(v)),
		 OMCI_OK);
}

static void be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* A GEM port both ways on T-CONT 0, with its interworking point. */
static void gem(struct omci_mib *mib, uint16_t inst, uint16_t port_id)
{
	CHECK_EQ(omci_mib_create(mib, OMCI_GEM_CLASS_ID, inst), OMCI_OK);
	set_u16(mib, OMCI_GEM_CLASS_ID, inst, OMCI_GEM_ATTR_PORT_ID, port_id);
	set_u16(mib, OMCI_GEM_CLASS_ID, inst, OMCI_GEM_ATTR_TCONT, 0x8000u);
	set_u8(mib, OMCI_GEM_CLASS_ID, inst, OMCI_GEM_ATTR_DIRECTION, 3u);
	CHECK_EQ(omci_mib_create(mib, OMCI_GEM_IW_CLASS_ID, inst), OMCI_OK);
	set_u16(mib, OMCI_GEM_IW_CLASS_ID, inst, OMCI_GEM_IW_ATTR_CTP, inst);
}

static void bridge_port(struct omci_mib *mib, uint16_t inst, uint8_t num,
			uint8_t tp_type, uint16_t tp_ptr)
{
	CHECK_EQ(omci_mib_create(mib, OMCI_BRIDGE_PORT_CLASS_ID, inst),
		 OMCI_OK);
	set_u16(mib, OMCI_BRIDGE_PORT_CLASS_ID, inst,
		OMCI_BRIDGE_PORT_ATTR_BRIDGE, 0x0001u);
	set_u8(mib, OMCI_BRIDGE_PORT_CLASS_ID, inst, OMCI_BRIDGE_PORT_ATTR_NUM,
	       num);
	set_u8(mib, OMCI_BRIDGE_PORT_CLASS_ID, inst,
	       OMCI_BRIDGE_PORT_ATTR_TPTYPE, tp_type);
	set_u16(mib, OMCI_BRIDGE_PORT_CLASS_ID, inst,
		OMCI_BRIDGE_PORT_ATTR_TPPTR, tp_ptr);
}

/* Send a Set of @class/@inst with the attributes of @mask, @value in order. */
static uint8_t set(struct omci_engine *eng, uint16_t tid, uint16_t class_id,
		   uint16_t inst, uint16_t mask, const uint8_t *value,
		   size_t len)
{
	struct omci_frame f = {
		.tci = tid,
		.msg_type = 8,
		.ar = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = class_id,
		.instance = inst,
	};
	uint8_t wire[OMCI_FRAME_LEN];

	f.contents[0] = (uint8_t)(mask >> 8);
	f.contents[1] = (uint8_t)mask;
	memcpy(&f.contents[2], value, len);
	(void)!omci_frame_build(&f, wire);

	tx_count = 0;
	if (omci_engine_rx(eng, wire) != OMCI_OK || tx_count == 0)
		return 0xff;
	return tx[0][8];
}

/* Point bridge port @port's outbound traffic descriptor at @td. */
static uint8_t point(struct omci_engine *eng, uint16_t tid, uint16_t port,
		     uint16_t td)
{
	const uint8_t v[2] = { (uint8_t)(td >> 8), (uint8_t)td };

	return set(eng, tid, OMCI_BRIDGE_PORT_CLASS_ID, port,
		   omci_attr_bit(OMCI_BRIDGE_PORT_ATTR_TD_OUT), v, sizeof(v));
}

/* Give traffic descriptor @td committed rate @cir and peak rate @pir. */
static uint8_t rates(struct omci_engine *eng, uint16_t tid, uint16_t td,
		     uint32_t cir, uint32_t pir)
{
	uint8_t v[8];

	be32(v, cir);
	be32(v + 4, pir);
	return set(eng, tid, OMCI_GEM_TD_CLASS_ID, td,
		   omci_attr_bit(OMCI_GEM_TD_ATTR_CIR) |
			   omci_attr_bit(OMCI_GEM_TD_ATTR_PIR),
		   v, sizeof(v));
}

static uint64_t rate_now(unsigned int *changes)
{
	return omci_hw_uni_rate_recorded(changes);
}

int main(void)
{
	struct omci_hw_caps caps;
	struct omci_mib *mib = NULL;
	struct omci_engine *eng = NULL;
	unsigned int changes = 0, before = 0;
	char log[2048];

	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
	CHECK_EQ(omci_engine_init(&eng, mib, &tx_ops, NULL), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);

	gem(mib, 0x0116u, 278u);
	CHECK_EQ(omci_mib_create(mib, OMCI_BRIDGE_CLASS_ID, 0x0001u), OMCI_OK);
	bridge_port(mib, UNI_PORT, 1u, OMCI_BRIDGE_TP_PPTP_ETH, 0x0101u);
	bridge_port(mib, NET_PORT, 2u, OMCI_BRIDGE_TP_GEM_IW, 0x0116u);
	CHECK_EQ(omci_mib_create(mib, OMCI_GEM_TD_CLASS_ID, TD_A), OMCI_OK);
	CHECK_EQ(omci_mib_create(mib, OMCI_GEM_TD_CLASS_ID, TD_B), OMCI_OK);
	omci_engine_reprogram(eng);

	CHECK_EQ(rate_now(&changes), 0u);
	CHECK(changes >= 1u);
	printf("    without a traffic descriptor the UNI is not limited\n");

	/* The committed rate alone, then a peak rate above it. */
	CHECK_EQ(rates(eng, 1, TD_A, 1250000u, 0u), 0u);
	CHECK_EQ(point(eng, 2, UNI_PORT, TD_A), 0u);
	CHECK_EQ(rate_now(NULL), 1250000u);
	CHECK_EQ(rates(eng, 3, TD_A, 1250000u, 2500000u), 0u);
	CHECK_EQ(rate_now(NULL), 2500000u);
	CHECK_EQ(rates(eng, 4, TD_A, 1250000u, 1000000u), 0u);
	CHECK_EQ(rate_now(NULL), 1250000u);
	printf("    the UNI follows its descriptor: the peak rate, the committed rate when that is higher\n");

	/* A Set that changes nothing of the rate tells the backend nothing. */
	(void)rate_now(&before);
	CHECK_EQ(rates(eng, 5, TD_A, 1250000u, 1000000u), 0u);
	(void)rate_now(&changes);
	CHECK_EQ(changes, before);
	printf("    an unchanged rate is not set again\n");

	/* A descriptor of the network side does not limit the UNI. */
	CHECK_EQ(rates(eng, 6, TD_B, 100000u, 100000u), 0u);
	CHECK_EQ(point(eng, 7, NET_PORT, TD_B), 0u);
	CHECK_EQ(rate_now(NULL), 1250000u);
	printf("    the outbound descriptor of a network-side port leaves the UNI alone\n");

	/* A failed change is said, and made at the next one. */
	omci_hw_uni_rate_fail_next();
	omci_log_level_set(OMCI_LOG_WARN);
	OMCI_T_CAPTURE_LOG(log, {
		CHECK_EQ(rates(eng, 8, TD_A, 1250000u, 3000000u), 0u);
	});
	omci_log_level_set(OMCI_LOG_ERR);
	CHECK_EQ(omci_t_log_count(
			 log,
			 "me47: the UNI's outbound rate of 3000000 bytes/s was not set"),
		 1u);
	CHECK_EQ(rate_now(NULL), 1250000u);
	CHECK_EQ(point(eng, 9, UNI_PORT, TD_A), 0u);
	CHECK_EQ(rate_now(NULL), 3000000u);
	printf("    a rate the backend did not take is tried again at the next change\n");

	/* A reset of the data path gives the backend the rate again. */
	(void)rate_now(&before);
	omci_engine_reprogram(eng);
	CHECK_EQ(rate_now(&changes), 3000000u);
	CHECK_EQ(changes, before + 1u);
	printf("    a reprogramming of the data path sets the rate again\n");

	/* A null pointer, then a deleted descriptor, take the limit away. */
	CHECK_EQ(point(eng, 10, UNI_PORT, 0xffffu), 0u);
	CHECK_EQ(rate_now(NULL), 0u);
	CHECK_EQ(point(eng, 11, UNI_PORT, TD_A), 0u);
	CHECK_EQ(rate_now(NULL), 3000000u);
	CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_TD_CLASS_ID, TD_A), OMCI_OK);
	omci_engine_reprogram(eng);
	CHECK_EQ(rate_now(NULL), 0u);
	printf("    a null pointer or a missing descriptor leaves the UNI unlimited\n");

	omci_engine_fini(eng);
	omci_mib_fini(mib);

	printf("\n%d omci_t_checks, %d omci_t_failures\n", omci_t_checks,
	       omci_t_failures);
	return OMCI_T_REPORT("test_uni_rate");
}
