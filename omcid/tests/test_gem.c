// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the GEM ports programmed from the GEM port network CTPs. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "me/gem.h"
#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/array_size.h"
#include "sys/log.h"

#include "harness.h"
#include "log_capture.h"

static uint8_t tx_frames[8][OMCI_FRAME_LEN];
static size_t tx_n;

static void on_send(void *ctx, const uint8_t frame[static OMCI_FRAME_LEN])
{
	(void)ctx;
	if (tx_n < ARRAY_SIZE(tx_frames))
		memcpy(tx_frames[tx_n++], frame, OMCI_FRAME_LEN);
}

static const struct omci_tx_ops eng_tx_ops = { .send = on_send };

static uint8_t create_gem(struct omci_engine *eng, uint16_t inst,
			  uint16_t port_id, uint16_t tcont, uint8_t dir)
{
	struct omci_frame f = {
		.tci = inst,
		.msg_type = 4,
		.ar = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = OMCI_GEM_CLASS_ID,
		.instance = inst,
	};
	uint8_t wire[OMCI_FRAME_LEN];

	f.contents[0] = (uint8_t)(port_id >> 8);
	f.contents[1] = (uint8_t)port_id;
	f.contents[2] = (uint8_t)(tcont >> 8);
	f.contents[3] = (uint8_t)tcont;
	f.contents[4] = dir;
	(void)!omci_frame_build(&f, wire);

	tx_n = 0;
	if (omci_engine_rx(eng, wire) != OMCI_OK || tx_n == 0)
		return 0xff;

	return tx_frames[tx_n - 1][8];
}

static void gem_create(struct omci_mib *mib, uint16_t instance,
		       uint16_t port_id, uint16_t tcont, uint8_t dir)
{
	const uint8_t pid[2] = { (uint8_t)(port_id >> 8), (uint8_t)port_id };
	const uint8_t tc[2] = { (uint8_t)(tcont >> 8), (uint8_t)tcont };

	CHECK_EQ(omci_mib_create(mib, OMCI_GEM_CLASS_ID, instance), OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, OMCI_GEM_CLASS_ID, instance,
				   OMCI_GEM_ATTR_PORT_ID, pid, sizeof(pid)),
		 OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, OMCI_GEM_CLASS_ID, instance,
				   OMCI_GEM_ATTR_TCONT, tc, sizeof(tc)),
		 OMCI_OK);
	CHECK_EQ(omci_mib_attr_set(mib, OMCI_GEM_CLASS_ID, instance,
				   OMCI_GEM_ATTR_DIRECTION, &dir, sizeof(dir)),
		 OMCI_OK);
}

/* Set attribute @attr of @cls instance @inst to the @size-byte big-endian @v. */
static void attr_put(struct omci_mib *mib, uint16_t cls, uint16_t inst,
		     uint8_t attr, uint32_t v, size_t size)
{
	uint8_t buf[4];

	for (size_t i = 0; i < size; i++)
		buf[i] = (uint8_t)(v >> (8u * (size - 1u - i)));
	CHECK_EQ(omci_mib_attr_set(mib, cls, inst, attr, buf, size), OMCI_OK);
}

/* A priority queue's related port: its T-CONT and its priority there. */
static uint32_t related(uint16_t tcont, uint16_t prio)
{
	return (uint32_t)tcont << 16 | prio;
}

/* The port of Port-ID @port_id as the last programming gave it, or NULL. */
static const struct omci_hw_gem_port *programmed(struct omci_mib *mib,
						 uint16_t port_id)
{
	const struct omci_hw_gem_port *ap = NULL;
	unsigned int refused = 0;
	size_t n;

	(void)!omci_gem_program(mib, &refused);
	n = omci_hw_gem_recorded(&ap);
	for (size_t i = 0; i < n; i++)
		if (ap[i].port_id == port_id)
			return &ap[i];
	return NULL;
}

static int index_of(const struct omci_hw_gem_port *p, size_t n,
		    uint16_t port_id)
{
	for (size_t i = 0; i < n; i++)
		if (p[i].port_id == port_id)
			return (int)i;
	return -1;
}

int main(void)
{
	struct omci_mib *mib = NULL;
	const struct omci_hw_gem_port *ap = NULL;
	unsigned int refused = 0;
	size_t n;
	int i;

	CHECK_EQ(omci_mib_init(&mib), OMCI_OK);

	{
		struct omci_hw_caps caps;

		CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
		CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
	}

	gem_create(mib, 0x07ffu, 2047u, 0x0000u, 2u);
	gem_create(mib, 0x07feu, 2046u, 0x0000u, 2u);
	gem_create(mib, 0x0116u, 278u, 0x8000u, 3u);
	gem_create(mib, 0x0117u, 279u, 0x8001u, 3u);
	gem_create(mib, 0x0118u, 280u, 0x8002u, 3u);
	gem_create(mib, 0x0119u, 281u, 0x8002u, 3u);

	CHECK_EQ(omci_gem_program(mib, &refused), OMCI_OK);
	CHECK_EQ(refused, 0u);

	n = omci_hw_gem_recorded(&ap);
	CHECK_EQ(n, 6u);
	printf("    the six GEM ports the OLT created reach the seam\n");

	i = index_of(ap, n, 278u);
	CHECK(i >= 0);
	if (i >= 0) {
		CHECK_EQ(ap[i].direction, (uint8_t)OMCI_HW_GEM_DIR_BOTH);

		CHECK_EQ(ap[i].tcont, 0u);
	}

	i = index_of(ap, n, 279u);
	CHECK(i >= 0);
	if (i >= 0)
		CHECK_EQ(ap[i].tcont, 1u);

	i = index_of(ap, n, 280u);
	CHECK(i >= 0);
	if (i >= 0)
		CHECK_EQ(ap[i].tcont, 2u);
	i = index_of(ap, n, 281u);
	CHECK(i >= 0);
	if (i >= 0)
		CHECK_EQ(ap[i].tcont, 2u);

	i = index_of(ap, n, 2047u);
	CHECK(i >= 0);
	if (i >= 0) {
		CHECK_EQ(ap[i].direction, (uint8_t)OMCI_HW_GEM_DIR_DOWNSTREAM);

		CHECK_EQ(ap[i].tcont, 0u);
	}
	printf("    the T-CONT pointer becomes an index: 0x8000 is 0, 0x8001 is 1\n");

	{
		gem_create(mib, 0x0200u, 300u, 0x8000u, 7u);
		CHECK_EQ(omci_gem_program(mib, &refused), OMCI_OK);
		CHECK_EQ(refused, 1u);
		CHECK_EQ(omci_hw_gem_recorded(&ap), 6u);
		CHECK_EQ(index_of(ap, 6u, 300u), -1);
		CHECK(omci_gem_instance_rejected(0x0200u));
		CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_CLASS_ID, 0x0200u),
			 OMCI_OK);
	}
	{
		gem_create(mib, 0x0201u, 301u, 0x0042u, 3u);
		CHECK_EQ(omci_gem_program(mib, &refused), OMCI_OK);
		CHECK_EQ(refused, 1u);
		CHECK_EQ(omci_hw_gem_recorded(&ap), 6u);
		CHECK_EQ(index_of(ap, 6u, 301u), -1);
		CHECK(omci_gem_instance_rejected(0x0201u));
		CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_CLASS_ID, 0x0201u),
			 OMCI_OK);
	}
	{
		char log[512];

		gem_create(mib, 0x0202u, 278u, 0x8000u, 3u);
		omci_log_level_set(OMCI_LOG_WARN);
		OMCI_T_CAPTURE_LOG(log, {
			CHECK_EQ(omci_gem_program(mib, &refused), OMCI_OK);
		});
		omci_log_level_set(OMCI_LOG_ERR);
		CHECK_EQ(
			omci_t_log_count(
				log,
				"me268: instance 0x0202 asks for Port-ID 278, which another instance already has"),
			1u);
		CHECK_EQ(refused, 1u);
		CHECK_EQ(omci_hw_gem_recorded(&ap), 6u);
		CHECK(omci_gem_instance_rejected(0x0202u));
		CHECK(!omci_gem_instance_rejected(0x0116u));
		CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_CLASS_ID, 0x0202u),
			 OMCI_OK);
	}
	{
		gem_create(mib, 0x0203u, 5000u, 0x0000u, 2u);
		CHECK_EQ(omci_gem_program(mib, &refused), OMCI_OK);
		CHECK_EQ(refused, 1u);
		CHECK_EQ(omci_hw_gem_recorded(&ap), 6u);
		CHECK(omci_gem_instance_rejected(0x0203u));
		CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_CLASS_ID, 0x0203u),
			 OMCI_OK);
	}
	{
		struct omci_hw_caps caps;

		CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
		CHECK(!omci_mib_exists(mib, OMCI_GEM_TCONT_CLASS_ID,
				       (uint16_t)(0x8000u | caps.num_tcont)));

		gem_create(mib, 0x0205u, 303u,
			   (uint16_t)(0x8000u | caps.num_tcont), 3u);
		CHECK_EQ(omci_gem_program(mib, &refused), OMCI_OK);
		CHECK_EQ(refused, 1u);
		CHECK_EQ(omci_hw_gem_recorded(&ap), 6u);
		CHECK(omci_gem_instance_rejected(0x0205u));
		CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_CLASS_ID, 0x0205u),
			 OMCI_OK);
	}
	printf("    a bad direction, a foreign T-CONT, a duplicate Port-ID and an impossible Port-ID are each refused alone\n");
	printf("    and a pointer shaped like a T-CONT that names no ME 262 is refused too\n");

	{
		const uint8_t tc[2] = { 0x80u, 0x03u };

		CHECK_EQ(omci_mib_attr_set(mib, OMCI_GEM_CLASS_ID, 0x0116u,
					   OMCI_GEM_ATTR_TCONT, tc, sizeof(tc)),
			 OMCI_OK);
		CHECK_EQ(omci_gem_program(mib, &refused), OMCI_OK);
		n = omci_hw_gem_recorded(&ap);
		i = index_of(ap, n, 278u);
		CHECK(i >= 0);
		if (i >= 0)
			CHECK_EQ(ap[i].tcont, 3u);
		printf("    a Set that re-points a port at another T-CONT reaches the seam\n");
	}

	{
		const struct omci_hw_gem_port *before = NULL;
		const size_t before_n = omci_gem_last(&before);
		uint16_t kept[OMCI_HW_GEM_MAX_PORTS];

		for (size_t k = 0; k < before_n; k++)
			kept[k] = before[k].port_id;

		gem_create(mib, 0x0204u, 302u, 0x8000u, 3u);
		omci_hw_gem_fail_next();
		CHECK_EQ(omci_gem_program(mib, &refused), OMCI_ERR_HW);

		n = omci_gem_last(&before);
		CHECK_EQ(n, before_n);
		for (size_t k = 0; k < n && k < before_n; k++)
			CHECK_EQ(before[k].port_id, kept[k]);
		printf("    a refused apply leaves the record of what is in force alone\n");
	}

	{
		CHECK(!omci_gem_instance_rejected(0x0116u));

		omci_hw_gem_reject_next(278u);
		CHECK_EQ(omci_gem_program(mib, &refused), OMCI_DECLINED);

		CHECK(omci_gem_instance_rejected(0x0116u));
		CHECK(!omci_gem_instance_rejected(0x0117u));
		printf("    one refused Port-ID names its own ME 268 instance and no other\n");

		CHECK_EQ(omci_gem_program(mib, &refused), OMCI_OK);
		CHECK(!omci_gem_instance_rejected(0x0116u));
		printf("    and the next clean reconcile clears it\n");

		omci_hw_gem_reject_next(278u);
		CHECK_EQ(omci_gem_program(mib, &refused), OMCI_DECLINED);
		CHECK(omci_gem_instance_rejected(0x0116u));
		CHECK(!omci_gem_instance_rejected(0u));
		CHECK_EQ(omci_gem_program(mib, &refused), OMCI_OK);
		CHECK(!omci_gem_instance_rejected(0x0116u));
	}

	{
		gem_create(mib, 0x0301u, 9000u, 0x8000u, 3u);
		CHECK_EQ(omci_gem_program(mib, &refused), OMCI_OK);
		CHECK_EQ(refused, 1u);
		CHECK(omci_gem_instance_rejected(0x0301u));
		CHECK(!omci_gem_instance_rejected(0x0116u));
		printf("    a Port-ID this daemon refuses is reported against its own instance too\n");
		CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_CLASS_ID, 0x0301u),
			 OMCI_OK);
	}

	{
		/*
		 * The synthetic ONU has two queues per T-CONT: T-CONT 0x8001's
		 * are 0x8002 (priority 0) and 0x8003 (priority 1), its traffic
		 * scheduler is 0x8001.
		 */
		const struct omci_hw_gem_port *g;
		char log[512];

		gem_create(mib, 0x0500u, 500u, 0x8001u, 3u);
		gem_create(mib, 0x0501u, 501u, 0x8001u, 1u);
		gem_create(mib, 0x0503u, 503u, 0x0000u, 2u);

		g = programmed(mib, 500u);
		CHECK(g != NULL);
		if (g != NULL) {
			CHECK_EQ(g->pbits, 0xffu);
			CHECK_EQ(g->queue, 0u);
			CHECK_EQ(g->weight, 0u);
			CHECK_EQ(g->cir, 0u);
			CHECK_EQ(g->pir, 0u);
		}
		printf("    a port without a priority queue, descriptor or mapper takes every priority, queue 0, no weight, no rates\n");

		attr_put(mib, OMCI_GEM_CLASS_ID, 0x0500u, OMCI_GEM_ATTR_TM_US,
			 0x8003u, 2);
		g = programmed(mib, 500u);
		CHECK(g != NULL && g->queue == 1u && g->weight == 0u);
		printf("    the queue is the priority of the queue the traffic management pointer names, strict under a strict scheduler\n");

		attr_put(mib, OMCI_GEM_SCHED_CLASS_ID, 0x8001u,
			 OMCI_GEM_SCHED_ATTR_POLICY, OMCI_GEM_POLICY_WRR, 1);
		attr_put(mib, OMCI_GEM_PRIQ_CLASS_ID, 0x8003u,
			 OMCI_GEM_PRIQ_ATTR_WEIGHT, 0u, 1);
		g = programmed(mib, 500u);
		CHECK(g != NULL && g->weight == 1u);
		attr_put(mib, OMCI_GEM_PRIQ_CLASS_ID, 0x8003u,
			 OMCI_GEM_PRIQ_ATTR_WEIGHT, 9u, 1);
		g = programmed(mib, 500u);
		CHECK(g != NULL && g->weight == 9u);
		printf("    under a weighted round robin scheduler the queue's weight comes along, 0 taken as 1\n");

		attr_put(mib, OMCI_GEM_PRIQ_CLASS_ID, 0x8003u,
			 OMCI_GEM_PRIQ_ATTR_SCHEDULER, 0u, 2);
		g = programmed(mib, 500u);
		CHECK(g != NULL && g->weight == 0u);
		attr_put(mib, OMCI_GEM_TCONT_CLASS_ID, 0x8001u,
			 OMCI_GEM_TCONT_ATTR_POLICY, OMCI_GEM_POLICY_WRR, 1);
		g = programmed(mib, 500u);
		CHECK(g != NULL && g->weight == 9u);
		printf("    a queue without a scheduler takes the T-CONT's policy\n");

		attr_put(mib, OMCI_GEM_PRIQ_CLASS_ID, 0x8003u,
			 OMCI_GEM_PRIQ_ATTR_RELATED_PORT,
			 related(0x8001u, 0x0fffu), 4);
		g = programmed(mib, 500u);
		CHECK(g != NULL && g->queue == 1u);
		attr_put(mib, OMCI_GEM_PRIQ_CLASS_ID, 0x8003u,
			 OMCI_GEM_PRIQ_ATTR_RELATED_PORT, related(0x8001u, 1u),
			 4);
		printf("    a priority beyond the T-CONT's queues takes its last queue\n");

		omci_log_level_set(OMCI_LOG_WARN);
		OMCI_T_CAPTURE_LOG(log, {
			attr_put(mib, OMCI_GEM_CLASS_ID, 0x0500u,
				 OMCI_GEM_ATTR_TM_US, 0x8000u, 2);
			g = programmed(mib, 500u);
		});
		omci_log_level_set(OMCI_LOG_ERR);
		CHECK(g != NULL && g->queue == 0u);
		CHECK_EQ(
			omci_t_log_count(
				log,
				"me268: instance 0x0500 sends through priority queue 0x8000, which belongs to 0x8000 and not to its T-CONT 0x8001; the queue's priority is kept"),
			1u);
		printf("    a queue of another T-CONT is named in the log, its priority kept\n");

		attr_put(mib, OMCI_GEM_PRIQ_CLASS_ID, 0x8002u,
			 OMCI_GEM_PRIQ_ATTR_RELATED_PORT, related(0x80ffu, 1u),
			 4);
		attr_put(mib, OMCI_GEM_CLASS_ID, 0x0500u, OMCI_GEM_ATTR_TM_US,
			 0x8002u, 2);
		g = programmed(mib, 500u);
		CHECK(g != NULL && g->queue == 0u);
		attr_put(mib, OMCI_GEM_PRIQ_CLASS_ID, 0x8002u,
			 OMCI_GEM_PRIQ_ATTR_RELATED_PORT, related(0x8001u, 0u),
			 4);
		printf("    a queue of a T-CONT with no queues of its own gives queue 0\n");

		attr_put(mib, OMCI_GEM_CLASS_ID, 0x0500u, OMCI_GEM_ATTR_TM_US,
			 0x8003u, 2);
		attr_put(mib, OMCI_GEM_ONUG_CLASS_ID, 0u,
			 OMCI_GEM_ONUG_ATTR_TM_OPTION,
			 OMCI_GEM_TM_RATE_CONTROLLED, 1);
		g = programmed(mib, 500u);
		CHECK(g != NULL && g->queue == 0u && g->weight == 0u);
		attr_put(mib, OMCI_GEM_ONUG_CLASS_ID, 0u,
			 OMCI_GEM_ONUG_ATTR_TM_OPTION, 0u, 1);
		g = programmed(mib, 500u);
		CHECK(g != NULL && g->queue == 1u && g->weight == 9u);
		printf("    a rate-controlled ONU's pointer names the T-CONT, never a queue\n");

		CHECK_EQ(omci_mib_create(mib, OMCI_GEM_TD_CLASS_ID, 0x0001u),
			 OMCI_OK);
		attr_put(mib, OMCI_GEM_TD_CLASS_ID, 0x0001u,
			 OMCI_GEM_TD_ATTR_CIR, 1000000u, 4);
		attr_put(mib, OMCI_GEM_TD_CLASS_ID, 0x0001u,
			 OMCI_GEM_TD_ATTR_PIR, 2000000u, 4);
		attr_put(mib, OMCI_GEM_CLASS_ID, 0x0500u, OMCI_GEM_ATTR_TD_US,
			 0x0001u, 2);
		g = programmed(mib, 500u);
		CHECK(g != NULL && g->cir == 1000000u && g->pir == 2000000u);
		attr_put(mib, OMCI_GEM_TD_CLASS_ID, 0x0001u,
			 OMCI_GEM_TD_ATTR_PIR, 500000u, 4);
		g = programmed(mib, 500u);
		CHECK(g != NULL && g->cir == 1000000u && g->pir == 1000000u);
		attr_put(mib, OMCI_GEM_CLASS_ID, 0x0500u, OMCI_GEM_ATTR_TD_US,
			 0xffffu, 2);
		g = programmed(mib, 500u);
		CHECK(g != NULL && g->cir == 0u && g->pir == 0u);
		printf("    the rates come from the traffic descriptor, a peak rate below the committed one raised to it\n");

		CHECK_EQ(omci_mib_create(mib, OMCI_GEM_IW_CLASS_ID, 0x0600u),
			 OMCI_OK);
		attr_put(mib, OMCI_GEM_IW_CLASS_ID, 0x0600u,
			 OMCI_GEM_IW_ATTR_CTP, 0x0500u, 2);
		CHECK_EQ(omci_mib_create(mib, OMCI_GEM_IW_CLASS_ID, 0x0601u),
			 OMCI_OK);
		attr_put(mib, OMCI_GEM_IW_CLASS_ID, 0x0601u,
			 OMCI_GEM_IW_ATTR_CTP, 0x0501u, 2);
		CHECK_EQ(omci_mib_create(mib, OMCI_GEM_MAPPER_CLASS_ID,
					 0x0700u),
			 OMCI_OK);
		for (uint8_t b = 0; b < 8u; b++)
			attr_put(mib, OMCI_GEM_MAPPER_CLASS_ID, 0x0700u,
				 (uint8_t)(OMCI_GEM_MAPPER_ATTR_PBIT0 + b),
				 b < 4u	 ? 0x0600u :
				 b == 4u ? 0x0699u :
				 b == 5u ? 0xffffu :
					   0x0601u,
				 2);
		g = programmed(mib, 500u);
		CHECK(g != NULL && g->pbits == 0x0fu);
		g = programmed(mib, 501u);
		CHECK(g != NULL && g->pbits == 0xc0u);
		g = programmed(mib, 503u);
		CHECK(g != NULL && g->pbits == 0xffu && g->queue == 0u);
		printf("    a port takes the priorities a mapper sends it; a port no mapper names takes all\n");

		CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_MAPPER_CLASS_ID,
					 0x0700u),
			 OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_IW_CLASS_ID, 0x0600u),
			 OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_IW_CLASS_ID, 0x0601u),
			 OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_TD_CLASS_ID, 0x0001u),
			 OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_CLASS_ID, 0x0500u),
			 OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_CLASS_ID, 0x0501u),
			 OMCI_OK);
		CHECK_EQ(omci_mib_delete(mib, OMCI_GEM_CLASS_ID, 0x0503u),
			 OMCI_OK);
	}

	omci_mib_fini(mib);

	{
		struct omci_hw_caps caps;
		struct omci_mib *emib = NULL;
		struct omci_engine *eng = NULL;

		CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
		CHECK_EQ(omci_mib_init(&emib), OMCI_OK);
		CHECK_EQ(omci_engine_init(&eng, emib, &eng_tx_ops, NULL),
			 OMCI_OK);
		CHECK_EQ(omci_me_defaults_apply(emib, &caps), OMCI_OK);

		CHECK_EQ(create_gem(eng, 0x0401u, 401u, 0x8000u, 3u), 0u);

		omci_hw_gem_reject_next(402u);
		CHECK_EQ(create_gem(eng, 0x0402u, 402u, 0x8000u, 3u), 3u);
		printf("    a Create the MAC refused is answered parameter error, not OK\n");

		omci_hw_gem_reject_next(402u);
		CHECK_EQ(create_gem(eng, 0x0403u, 403u, 0x8000u, 3u), 0u);
		printf("    a port still refused does not make the NEXT Create fail\n");

		omci_hw_gem_fail_next();
		CHECK_EQ(create_gem(eng, 0x0404u, 404u, 0x8000u, 3u), 1u);
		printf("    a reconcile that could not run at all is processing error instead\n");

		omci_engine_fini(eng);
		omci_mib_fini(emib);
	}

	return OMCI_T_REPORT("test_gem");
}
