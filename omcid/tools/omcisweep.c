// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * omcisweep: drive the engine through every case of an axis of cases.h and
 * record each answer, so that two builds can be compared case by case.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omci/cases.h"

#define REC_LEN 56u
#define REC_INDEX 0u
#define REC_STATUS 4u
#define REC_TXCOUNT 5u
#define REC_PAD 6u
#define REC_REPLY 8u

#define HDR_LEN 16u
static const char HDR_MAGIC[8] = { 'O', 'M', 'C', 'I', 'S', 'W', 'P', '1' };

static unsigned int g_tx_n;
static uint8_t g_tx_first[OMCI_FRAME_LEN];

static void on_send(void *ctx, const uint8_t f[static OMCI_FRAME_LEN])
{
	(void)ctx;
	if (g_tx_n == 0)
		memcpy(g_tx_first, f, OMCI_FRAME_LEN);
	g_tx_n++;
}
static const struct omci_tx_ops tx_ops = { .send = on_send };

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

int main(int argc, char **argv)
{
	const char *axis_name = NULL;
	const char *out_path = NULL;
	bool quiet = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			out_path = argv[++i];
		else if (strcmp(argv[i], "-q") == 0)
			quiet = true;
		else if (argv[i][0] != '-' && axis_name == NULL)
			axis_name = argv[i];
		else {
			fprintf(stderr, "usage: %s <axis> [-o FILE] [-q]\n",
				argv[0]);
			return 2;
		}
	}

	if (axis_name == NULL) {
		fprintf(stderr,
			"usage: %s <axis> [-o FILE] [-q]\naxes:", argv[0]);
		for (unsigned int a = 0; a < OMCI_CASE_AXIS_COUNT; a++)
			fprintf(stderr, " %s",
				omci_case_axis_name((enum omci_case_axis)a));
		fprintf(stderr, "\n");
		return 2;
	}

	const enum omci_case_axis axis = omci_case_axis_by_name(axis_name);

	if (axis == OMCI_CASE_AXIS_COUNT) {
		fprintf(stderr, "omcisweep: no such axis: %s\n", axis_name);
		return 2;
	}

	struct omci_mib *mib = NULL;
	struct omci_engine *eng = NULL;

	if (omci_mib_init(&mib) != OMCI_OK ||
	    omci_engine_init(&eng, mib, &tx_ops, NULL) != OMCI_OK) {
		fprintf(stderr,
			"omcisweep: cannot bring up a MIB and an engine\n");
		return 1;
	}

	if (omci_case_axis_populated(axis)) {
		struct omci_hw_caps caps;

		if (omci_hw_caps(&caps) != OMCI_OK ||
		    omci_me_defaults_apply(mib, &caps) != OMCI_OK) {
			fprintf(stderr, "omcisweep: cannot populate the MIB\n");
			return 1;
		}
		omci_case_prepare(axis, mib);
	}
	const uint32_t n = omci_case_count(axis);

	FILE *out = stdout;

	if (out_path != NULL) {
		out = fopen(out_path, "wb");
		if (out == NULL) {
			fprintf(stderr, "omcisweep: cannot write %s\n",
				out_path);
			return 1;
		}
	}

	uint8_t hdr[HDR_LEN];

	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, HDR_MAGIC, sizeof(HDR_MAGIC));
	put_be32(&hdr[8], (uint32_t)axis);
	put_be32(&hdr[12], n);
	if (fwrite(hdr, 1, sizeof(hdr), out) != sizeof(hdr)) {
		fprintf(stderr, "omcisweep: short write on the header\n");
		return 1;
	}

	uint32_t digest = 2166136261u;

	for (uint32_t i = 0; i < n; i++) {
		uint8_t wire[OMCI_FRAME_LEN];
		(void)omci_case_build(axis, i, wire);
		for (unsigned int b = 0; b < OMCI_FRAME_LEN; b++) {
			digest ^= wire[b];
			digest *= 16777619u;
		}
	}
	fprintf(stderr, "omcisweep: axis %s, %lu cases, input digest %08lx\n",
		axis_name, (unsigned long)n, (unsigned long)digest);

	unsigned long answered = 0, silent = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint8_t wire[OMCI_FRAME_LEN];
		(void)omci_case_build(axis, i, wire);

		g_tx_n = 0;
		memset(g_tx_first, 0, sizeof(g_tx_first));
		const enum omci_status rc = omci_engine_rx(eng, wire);

		if (g_tx_n > 0)
			answered++;
		else
			silent++;

		uint8_t rec[REC_LEN];

		memset(rec, 0, sizeof(rec));
		put_be32(&rec[REC_INDEX], i);
		rec[REC_STATUS] = (uint8_t)rc;
		rec[REC_TXCOUNT] = (uint8_t)(g_tx_n > 255u ? 255u : g_tx_n);
		memcpy(&rec[REC_REPLY], g_tx_first, OMCI_FRAME_LEN);

		if (fwrite(rec, 1, sizeof(rec), out) != sizeof(rec)) {
			fprintf(stderr, "omcisweep: short write at case %lu\n",
				(unsigned long)i);
			return 1;
		}

		if (!quiet && n > 100u && (i % (n / 10u)) == 0)
			fprintf(stderr, "  %lu%%\n",
				(unsigned long)(100u * i / n));
	}

	if (out != stdout)
		(void)fclose(out);

	fprintf(stderr, "omcisweep: %lu cases, %lu answered, %lu silent\n",
		(unsigned long)n, answered, silent);

	omci_engine_fini(eng);
	omci_mib_fini(mib);
	return 0;
}
