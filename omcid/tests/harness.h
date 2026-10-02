/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* The test harness: checks that count, frame builders and a capture of what the engine sends. */

#ifndef OMCI_TEST_HARNESS_H
#define OMCI_TEST_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/attrs.h"

#define OMCI_T_MAX_SITES 2048u

static int omci_t_failures;
static int omci_t_checks;
static unsigned int omci_t_sites_used;
static unsigned long omci_t_sites[OMCI_T_MAX_SITES];

static inline void omci_t_site(const char *file, int line)
{
	const unsigned long key =
		(unsigned long)(uintptr_t)file * 31u + (unsigned long)line + 1u;
	for (unsigned int probe = 0; probe < OMCI_T_MAX_SITES; probe++) {
		const unsigned int slot =
			(unsigned int)((key + probe) % OMCI_T_MAX_SITES);
		if (omci_t_sites[slot] == key)
			return;
		if (omci_t_sites[slot] == 0ul) {
			omci_t_sites[slot] = key;
			omci_t_sites_used++;
			return;
		}
	}

	printf("  WARN: more than %u distinct assertion sites — the count below is short\n",
	       OMCI_T_MAX_SITES);
}

#define CHECK(c)                                                              \
	do {                                                                  \
		omci_t_site(__FILE__, __LINE__);                              \
		omci_t_checks++;                                              \
		if (!(c)) {                                                   \
			omci_t_failures++;                                    \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
		}                                                             \
	} while (0)

#define CHECK_EQ(g, w)                                                    \
	do {                                                              \
		omci_t_site(__FILE__, __LINE__);                          \
		omci_t_checks++;                                          \
		const unsigned long a_ = (unsigned long)(g),              \
				    b_ = (unsigned long)(w);              \
		if (a_ != b_) {                                           \
			omci_t_failures++;                                \
			printf("  FAIL %s:%d: %s == %lu, expected %lu\n", \
			       __FILE__, __LINE__, #g, a_, b_);           \
		}                                                         \
	} while (0)

#define CHECK_MEM(got, want, n)                                                \
	do {                                                                   \
		omci_t_site(__FILE__, __LINE__);                               \
		omci_t_checks++;                                               \
		const uint8_t *g_ = (const uint8_t *)(got);                    \
		const uint8_t *w_ = (const uint8_t *)(want);                   \
		const size_t n_ = (size_t)(n);                                 \
		if (memcmp(g_, w_, n_) != 0) {                                 \
			omci_t_failures++;                                     \
			printf("  FAIL %s:%d: %s differs from %s\n", __FILE__, \
			       __LINE__, #got, #want);                         \
			printf("        got  ");                               \
			for (size_t i_ = 0; i_ < n_; i_++)                     \
				printf("%02x", g_[i_]);                        \
			printf("\n        want ");                             \
			for (size_t i_ = 0; i_ < n_; i_++)                     \
				printf("%02x", w_[i_]);                        \
			printf("\n");                                          \
		}                                                              \
	} while (0)

#define RUN(fn)                      \
	do {                         \
		printf("%s\n", #fn); \
		fn();                \
	} while (0)

#define RUN_MIB(fn)                                      \
	do {                                             \
		printf("%s\n", #fn);                     \
		struct omci_mib *mib_ = NULL;            \
		CHECK_EQ(omci_mib_init(&mib_), OMCI_OK); \
		fn(mib_);                                \
		omci_mib_fini(mib_);                     \
	} while (0)

#define OMCI_T_REPORT(name)                                                 \
	(printf("%s: %u distinct assertions, %d executions, %d failures\n", \
		(name), omci_t_sites_used, omci_t_checks, omci_t_failures), \
	 omci_t_failures == 0 ? 0 : 1)

#define OMCI_T_MAX_TX 512u

struct omci_t_tx {
	uint8_t frame[OMCI_T_MAX_TX][OMCI_FRAME_LEN];
	unsigned int n;
};

static struct omci_t_tx omci_t_tx;

static void omci_t_on_send(void *ctx, const uint8_t f[static OMCI_FRAME_LEN])
{
	(void)ctx;
	if (omci_t_tx.n < OMCI_T_MAX_TX)
		memcpy(omci_t_tx.frame[omci_t_tx.n], f, OMCI_FRAME_LEN);
	omci_t_tx.n++;
}

static const struct omci_tx_ops omci_t_tx_ops = { .send = omci_t_on_send };

static inline void omci_t_tx_reset(void)
{
	omci_t_tx.n = 0;
}

static inline const uint8_t *omci_t_last_tx(void)
{
	if (omci_t_tx.n == 0 || omci_t_tx.n > OMCI_T_MAX_TX)
		return NULL;
	return omci_t_tx.frame[omci_t_tx.n - 1u];
}

static inline void omci_t_build(uint8_t out[OMCI_FRAME_LEN], uint16_t tci,
				uint8_t mt, bool ar, bool ak, uint16_t class_id,
				uint16_t inst)
{
	struct omci_frame f = {
		.tci = tci,
		.msg_type = mt,
		.ar = ar,
		.ak = ak,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = class_id,
		.instance = inst,
	};
	(void)!omci_frame_build(&f, out);
}

static inline void omci_t_build_pri(uint8_t out[OMCI_FRAME_LEN], uint16_t tci,
				    uint8_t mt, bool ar, bool ak,
				    uint16_t class_id, uint16_t inst)
{
	struct omci_frame f = {
		.tci = (uint16_t)(tci & 0x7FFFu),
		.priority = true,
		.msg_type = mt,
		.ar = ar,
		.ak = ak,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = class_id,
		.instance = inst,
	};
	(void)!omci_frame_build(&f, out);
}

static inline __must_check enum omci_status
omci_t_rx(struct omci_engine *eng, const uint8_t wire[static OMCI_FRAME_LEN])
{
	uint8_t buf[OMCI_FRAME_LEN];
	struct omci_frame f;

	memcpy(buf, wire, OMCI_FRAME_LEN);
	if (omci_frame_parse(buf, &f) != OMCI_OK)
		return omci_engine_rx(eng, buf);
	(void)!omci_frame_build(&f, buf);
	return omci_engine_rx(eng, buf);
}

struct omci_t_fixture {
	struct omci_mib *mib;
	struct omci_engine *eng;
};

static inline __must_check bool omci_t_up(struct omci_t_fixture *fx)
{
	fx->mib = NULL;
	fx->eng = NULL;
	omci_t_tx_reset();
	if (omci_mib_init(&fx->mib) != OMCI_OK)
		return false;
	if (omci_engine_init(&fx->eng, fx->mib, &omci_t_tx_ops, NULL) !=
	    OMCI_OK) {
		omci_mib_fini(fx->mib);
		fx->mib = NULL;
		return false;
	}
	return true;
}

static inline void omci_t_down(struct omci_t_fixture *fx)
{
	if (fx->eng != NULL)
		omci_engine_fini(fx->eng);
	if (fx->mib != NULL)
		omci_mib_fini(fx->mib);
	fx->eng = NULL;
	fx->mib = NULL;
}

struct omci_t_case {
	uint32_t index;
	uint8_t status;
	uint8_t tx_count;
	uint8_t reply[OMCI_FRAME_LEN];
};

static inline void omci_t_case_run(struct omci_engine *eng, uint32_t index,
				   const uint8_t wire[static OMCI_FRAME_LEN],
				   struct omci_t_case *out)
{
	omci_t_tx_reset();
	const enum omci_status rc = omci_engine_rx(eng, wire);

	out->index = index;
	out->status = (uint8_t)rc;
	out->tx_count = (uint8_t)(omci_t_tx.n > 255u ? 255u : omci_t_tx.n);
	if (omci_t_tx.n > 0 && omci_t_tx.n <= OMCI_T_MAX_TX)
		memcpy(out->reply, omci_t_tx.frame[0], OMCI_FRAME_LEN);
	else
		memset(out->reply, 0, OMCI_FRAME_LEN);
}

#endif
