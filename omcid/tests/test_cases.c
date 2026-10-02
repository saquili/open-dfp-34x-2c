// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Run the engine through every case of every axis of omci/cases.h. */

#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "me/pm.h"
#include "me/table.h"
#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/array_size.h"
#include "omcid/ctl_proto.h"
#include "omci/cases.h"

#include "harness.h"

static uint8_t g_tap_verdict;

#define CASE_HANG_SECONDS 300u
#define CASE_CEILING_US 1000000L

static volatile sig_atomic_t g_case_in_flight = -1;

static void on_hang(int sig)
{
	(void)sig;
	static const char msg[] =
		"\nHANG: omci_engine_rx did not return; mutate case ";
	(void)!write(2, msg, sizeof(msg) - 1u);

	char d[12];
	int n = 0;
	long v = (long)g_case_in_flight;

	if (v < 0) {
		(void)!write(2, "?", 1);
	} else {
		do {
			d[n++] = (char)('0' + (v % 10));
			v /= 10;
		} while (v > 0 && n < (int)sizeof(d));
		while (n-- > 0)
			(void)!write(2, &d[n], 1);
	}
	(void)!write(2, "\n", 1);
	_exit(1);
}

static void on_tap(void *ctx, uint8_t verdict, uint8_t detail,
		   const uint8_t *frame)
{
	(void)ctx;
	(void)detail;
	(void)frame;
	if (verdict != OMCI_CTL_TAP_TX)
		g_tap_verdict = verdict;
}
static const struct omci_tap_ops tap_ops = { .frame = on_tap };

static uint32_t axis_digest(enum omci_case_axis axis)
{
	uint32_t h = 2166136261u;
	const uint32_t n = omci_case_count(axis);

	for (uint32_t i = 0; i < n; i++) {
		uint8_t wire[OMCI_FRAME_LEN];
		(void)omci_case_build(axis, i, wire);
		for (unsigned int b = 0; b < OMCI_FRAME_LEN; b++) {
			h ^= wire[b];
			h *= 16777619u;
		}
	}
	return h;
}

int main(void)
{
	static const enum omci_case_axis axes[] = {
		OMCI_CASE_ENVELOPE,
		OMCI_CASE_MUTATE,
		OMCI_CASE_MSGTYPE,
		OMCI_CASE_MTCLASS,
	};

	for (unsigned int a = 0; a < ARRAY_SIZE(axes); a++) {
		const enum omci_case_axis axis = axes[a];
		const uint32_t n = omci_case_count(axis);

		CHECK(n > 0);
		CHECK(omci_case_axis_name(axis) != NULL);
		CHECK_EQ(omci_case_axis_by_name(omci_case_axis_name(axis)),
			 axis);

		uint8_t first[OMCI_FRAME_LEN], again[OMCI_FRAME_LEN];

		for (uint32_t i = 0; i < n; i += (n / 97u) + 1u) {
			CHECK(omci_case_build(axis, i, first));
			CHECK(omci_case_build(axis, i, again));
			CHECK_MEM(first, again, OMCI_FRAME_LEN);
		}

		uint8_t zero_before[OMCI_FRAME_LEN], zero_after[OMCI_FRAME_LEN];

		CHECK(omci_case_build(axis, 0, zero_before));
		for (uint32_t i = 0; i < n; i++) {
			uint8_t scratch[OMCI_FRAME_LEN];
			(void)omci_case_build(axis, i, scratch);
		}
		CHECK(omci_case_build(axis, 0, zero_after));
		CHECK_MEM(zero_before, zero_after, OMCI_FRAME_LEN);

		uint8_t wire[OMCI_FRAME_LEN];

		CHECK(omci_case_build(axis, n - 1u, wire));
		CHECK(!omci_case_build(axis, n, wire));
		CHECK(!omci_case_build(axis, 0xFFFFFFFFu, wire));

		char label[64];

		for (uint32_t i = 0; i < n; i += (n / 41u) + 1u) {
			omci_case_label(axis, i, label, sizeof(label));
			CHECK(label[0] != '\0');
		}

		printf("  axis %-9s %6lu cases  digest %08x\n",
		       omci_case_axis_name(axis), (unsigned long)n,
		       axis_digest(axis));
	}

	CHECK_EQ(omci_case_count(OMCI_CASE_MUTATE), 3u * 48u * 256u);
	CHECK_EQ(omci_case_count(OMCI_CASE_MSGTYPE), 32u * 4u * 2u);
	CHECK_EQ(omci_case_count(OMCI_CASE_MTCLASS), 32u * 14u * 2u);
	CHECK(omci_case_count(OMCI_CASE_ENVELOPE) > 1500u);

	{
		struct omci_mib *mib = NULL;
		struct omci_engine *eng = NULL;
		struct omci_hw_caps caps;

		CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
		CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
		CHECK_EQ(omci_engine_init(&eng, mib, &omci_t_tx_ops, NULL),
			 OMCI_OK);

		CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
		CHECK_EQ(omci_mib_create(mib, 321u, 0u), OMCI_OK);

		CHECK(omci_pm_owns(321u));

		omci_engine_tap_set(eng, &tap_ops, NULL);

		unsigned int reached = 0, replayed = 0, answered = 0,
			     mic_rejected = 0;
		unsigned int refused_before_dispatch = 0;
		unsigned int readers = 0, reader_wrote = 0, over_ceiling = 0;
		unsigned int pm_exempted = 0, pm_exempted_wrote = 0;
		long slowest_us = 0;
		uint32_t slowest_i = 0;

		struct sigaction sa = { 0 };

		sa.sa_handler = on_hang;
		CHECK_EQ(sigaction(SIGALRM, &sa, NULL), 0);
		(void)alarm(CASE_HANG_SECONDS);
		const uint32_t nm = omci_case_count(OMCI_CASE_MUTATE);

		for (uint32_t i = 0; i < nm; i++) {
			uint8_t wire[OMCI_FRAME_LEN];
			(void)omci_case_build(OMCI_CASE_MUTATE, i, wire);

			const uint32_t gen_before = omci_mib_generation(mib);
			const uint8_t mt_before = (uint8_t)(wire[2] & 0x1Fu);
			const uint16_t req_class =
				(uint16_t)(((unsigned int)wire[4] << 8) |
					   wire[5]);
			const uint16_t req_inst =
				(uint16_t)(((unsigned int)wire[6] << 8) |
					   wire[7]);

			struct timespec t0, t1;
			(void)clock_gettime(CLOCK_MONOTONIC, &t0);
			g_case_in_flight = (sig_atomic_t)i;
			omci_t_tx_reset();
			g_tap_verdict = 0xFFu;
			const enum omci_status rc = omci_engine_rx(eng, wire);
			(void)clock_gettime(CLOCK_MONOTONIC, &t1);
			{
				const long us =
					(long)((t1.tv_sec - t0.tv_sec) *
						       1000000L +
					       (t1.tv_nsec - t0.tv_nsec) /
						       1000L);
				if (us > slowest_us) {
					slowest_us = us;
					slowest_i = i;
				}
				if (us > CASE_CEILING_US)
					over_ceiling++;
			}
			if (rc == OMCI_OK && omci_t_tx.n > 0)
				answered++;

			const bool writer =
				(mt_before == 15u || mt_before == 4u ||
				 mt_before == 6u || mt_before == 8u ||
				 (mt_before >= 19u && mt_before <= 23u));

			if (g_tap_verdict == OMCI_CTL_TAP_RX_DISPATCH &&
			    !writer) {
				const bool pm_publish =
					(mt_before == 9u || mt_before == 28u) &&
					omci_pm_owns(req_class) &&
					omci_mib_exists(mib, req_class,
							req_inst);
				if (pm_publish) {
					pm_exempted++;
					if (omci_mib_generation(mib) !=
					    gen_before)
						pm_exempted_wrote++;
				} else {
					readers++;
					if (omci_mib_generation(mib) !=
					    gen_before)
						reader_wrote++;
				}
			}

			if (g_tap_verdict == OMCI_CTL_TAP_RX_DISPATCH)
				reached++;
			if (g_tap_verdict == OMCI_CTL_TAP_RX_REPLAY)
				replayed++;

			if (g_tap_verdict == OMCI_CTL_TAP_RX_PARSE ||
			    g_tap_verdict == OMCI_CTL_TAP_RX_MIC ||
			    g_tap_verdict == OMCI_CTL_TAP_RX_ARAK ||
			    g_tap_verdict == OMCI_CTL_TAP_RX_REPLAY ||
			    g_tap_verdict == OMCI_CTL_TAP_RX_ACTION ||
			    g_tap_verdict == OMCI_CTL_TAP_RX_UNKNOWN_ME ||
			    g_tap_verdict == OMCI_CTL_TAP_RX_UNHANDLED)
				refused_before_dispatch++;
			if (rc == OMCI_ERR_MIC)
				mic_rejected++;
		}
		printf("  mutate: %u/%lu reach a handler, %u refused before dispatch (%u on the MIC), %u replayed, %u answered\n",
		       reached, (unsigned long)nm, refused_before_dispatch,
		       mic_rejected, replayed, answered);

		CHECK(readers > 10000u);
		CHECK_EQ(reader_wrote, 0u);

		CHECK(pm_exempted > 0u);
		CHECK(pm_exempted_wrote > 0u);

		(void)alarm(0);
		g_case_in_flight = -1;

		CHECK_EQ(over_ceiling, 0u);
		printf("  mutate: every case returned; slowest %ld us (case %u) against a %ld us ceiling\n",
		       slowest_us, slowest_i, CASE_CEILING_US);

		CHECK_EQ(replayed, 0u);
		CHECK(reached > 27000u);
		CHECK(answered >= reached);

		CHECK_EQ(reached + refused_before_dispatch, nm);

		CHECK(mic_rejected <= 3u * 4u * 256u);

		omci_engine_fini(eng);
		omci_mib_fini(mib);
	}

	{
		char label[64];

		omci_case_label(OMCI_CASE_ENVELOPE,
				omci_case_count(OMCI_CASE_ENVELOPE), label,
				sizeof(label));
		CHECK(label[0] == '\0');
	}

	{
		const uint32_t ne = omci_case_count(OMCI_CASE_ENVELOPE);
		uint32_t h = 2166136261u;

		for (uint32_t i = 0; i < 1590u; i++) {
			uint8_t wire[OMCI_FRAME_LEN];
			(void)omci_case_build(OMCI_CASE_ENVELOPE, i, wire);
			for (unsigned int b = 0; b < OMCI_FRAME_LEN; b++) {
				h ^= wire[b];
				h *= 16777619u;
			}
		}
		CHECK_EQ(h, 0x9eb6a0feu);

		uint32_t t = 2166136261u;

		for (uint32_t i = ne - 7u; i < ne; i++) {
			uint8_t wire[OMCI_FRAME_LEN];
			(void)omci_case_build(OMCI_CASE_ENVELOPE, i, wire);
			for (unsigned int b = 0; b < OMCI_FRAME_LEN; b++) {
				t ^= wire[b];
				t *= 16777619u;
			}
		}
		CHECK_EQ(t, 0x95e46d78u);

		CHECK_EQ(ne, 1590u + (uint32_t)omci_me_class_count + 3u + 4u);

		printf("  envelope: %lu cases, prefix 9eb6a0fe and tail 95e46d78 both unmoved\n",
		       (unsigned long)ne);
	}

	{
		struct omci_mib *mib = NULL;
		struct omci_engine *eng = NULL;
		struct omci_hw_caps caps;

		CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
		CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
		CHECK_EQ(omci_engine_init(&eng, mib, &omci_t_tx_ops, NULL),
			 OMCI_OK);
		CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
		omci_engine_tap_set(eng, &tap_ops, NULL);

		unsigned int answered = 0, echo_wrong = 0;
		unsigned int ans_byte2 = 0, ans_devid = 0, ans_trailer = 0,
			     ans_tci = 0, ans_class = 0;
		unsigned int ans_unreg = 0, unreg_unknown_me = 0;
		unsigned int devid_foreign_answered = 0, devid_foreign = 0;
		unsigned int trailer_wrong_answered = 0, trailer_wrong = 0;
		unsigned int mic_flip_answered = 0, mic_flips = 0,
			     mic_ok_answered = 0;

		const uint32_t ne = omci_case_count(OMCI_CASE_ENVELOPE);

		for (uint32_t i = 0; i < ne; i++) {
			uint8_t wire[OMCI_FRAME_LEN];

			if (!omci_case_build(OMCI_CASE_ENVELOPE, i, wire))
				continue;

			char label[64];

			omci_case_label(OMCI_CASE_ENVELOPE, i, label,
					sizeof(label));

			const uint16_t req_tci =
				(uint16_t)(((unsigned int)wire[0] << 8) |
					   wire[1]);
			const uint8_t devid = wire[3];
			const uint32_t trailer = ((uint32_t)wire[40] << 24) |
						 ((uint32_t)wire[41] << 16) |
						 ((uint32_t)wire[42] << 8) |
						 (uint32_t)wire[43];

			omci_t_tx_reset();
			g_tap_verdict = 0xFFu;
			(void)!omci_engine_rx(eng, wire);
			const bool spoke = (omci_t_tx.n > 0);

			if (spoke) {
				answered++;
				const uint16_t rsp =
					(uint16_t)(((unsigned int)omci_t_tx
							    .frame[0][0]
						    << 8) |
						   omci_t_tx.frame[0][1]);
				if (rsp != req_tci)
					echo_wrong++;
			}

			if (strstr(label, "devid=") != NULL &&
			    devid != OMCI_DEVICE_ID_BASELINE) {
				devid_foreign++;
				if (spoke)
					devid_foreign_answered++;
			}
			if (strstr(label, "trailer[") != NULL &&
			    trailer != 0x28u) {
				trailer_wrong++;
				if (spoke)
					trailer_wrong_answered++;
			}
			if (strstr(label, "mic-bit=") != NULL) {
				mic_flips++;
				if (spoke)
					mic_flip_answered++;
			}
			if (strstr(label, "mic=correct") != NULL && spoke)
				mic_ok_answered++;

			if (strstr(label, "unreg-class=") != NULL &&
			    g_tap_verdict == OMCI_CTL_TAP_RX_UNKNOWN_ME)
				unreg_unknown_me++;

			if (spoke) {
				if (strstr(label, "byte2=") != NULL)
					ans_byte2++;
				else if (strstr(label, "devid=") != NULL)
					ans_devid++;
				else if (strstr(label, "trailer[") != NULL)
					ans_trailer++;
				else if (strstr(label, "tci#") != NULL)
					ans_tci++;
				else if (strstr(label, "unreg-class=") != NULL)
					ans_unreg++;
				else if (strstr(label, "class=") != NULL)
					ans_class++;
			}
		}

		CHECK_EQ(echo_wrong, 0u);
		CHECK_EQ(ans_byte2, 66u);
		CHECK_EQ(ans_devid, 1u);
		CHECK_EQ(ans_trailer, 4u);
		CHECK_EQ(ans_tci, 21u);
		CHECK_EQ(ans_class, (unsigned int)omci_me_class_count + 3u);

		CHECK_EQ(ans_unreg, 4u);
		CHECK_EQ(unreg_unknown_me, 4u);

		CHECK_EQ(answered, ans_byte2 + ans_devid + ans_trailer +
					   ans_tci + ans_class + ans_unreg +
					   mic_ok_answered);

		CHECK_EQ(devid_foreign, 255u);
		CHECK_EQ(devid_foreign_answered, 0u);

		CHECK_EQ(trailer_wrong, 1020u);
		CHECK_EQ(trailer_wrong_answered, 0u);

		CHECK_EQ(mic_flips, 32u);
		CHECK_EQ(mic_flip_answered, 0u);
		CHECK_EQ(mic_ok_answered, 1u);

		printf("  envelope: %u/%lu answered, all echoing the request id; %u foreign device ids, %u bad trailers and %u MIC flips all refused\n",
		       answered, (unsigned long)ne, devid_foreign,
		       trailer_wrong, mic_flips);

		omci_engine_fini(eng);
		omci_mib_fini(mib);
	}

	{
		struct omci_mib *mib = NULL;
		struct omci_engine *eng = NULL;
		struct omci_hw_caps caps;

		CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
		CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
		CHECK_EQ(omci_engine_init(&eng, mib, &omci_t_tx_ops, NULL),
			 OMCI_OK);
		CHECK_EQ(omci_me_defaults_apply(mib, &caps), OMCI_OK);
		omci_engine_tap_set(eng, &tap_ops, NULL);

		unsigned int non_request = 0, non_request_refused = 0,
			     exempt_download_section = 0;
		unsigned int request = 0, req_answered = 0;
		unsigned int v_dispatch = 0, v_action = 0, v_unknown_me = 0,
			     v_unhandled = 0;

		const uint32_t nt = omci_case_count(OMCI_CASE_MSGTYPE);

		for (uint32_t i = 0; i < nt; i++) {
			uint8_t wire[OMCI_FRAME_LEN];

			if (!omci_case_build(OMCI_CASE_MSGTYPE, i, wire))
				continue;

			const uint8_t mt_byte = wire[2];
			const bool ar = (mt_byte & 0x40u) != 0u;
			const bool ak = (mt_byte & 0x20u) != 0u;
			const uint8_t mt = (uint8_t)(mt_byte & 0x1Fu);
			const bool is_reqst = ar && !ak;

			if (!is_reqst) {
				non_request++;
				const enum omci_status rc =
					omci_engine_rx(eng, wire);
				if (rc == OMCI_ERR_PROTO)
					non_request_refused++;
				else if (mt == 20u)
					exempt_download_section++;
				continue;
			}

			request++;
			g_tap_verdict = 0xFFu;
			omci_t_tx_reset();
			(void)!omci_engine_rx(eng, wire);
			if (omci_t_tx.n > 0u)
				req_answered++;

			switch (g_tap_verdict) {
			case OMCI_CTL_TAP_RX_DISPATCH:
				v_dispatch++;
				break;
			case OMCI_CTL_TAP_RX_ACTION:
				v_action++;
				break;
			case OMCI_CTL_TAP_RX_UNKNOWN_ME:
				v_unknown_me++;
				break;
			case OMCI_CTL_TAP_RX_UNHANDLED:
				v_unhandled++;
				break;
			default:
				break;
			}
		}

		CHECK_EQ(non_request, 192u);
		CHECK_EQ(non_request_refused, 190u);
		CHECK_EQ(exempt_download_section, 2u);

		CHECK_EQ(request, 64u);
		CHECK_EQ(req_answered, 64u);
		CHECK_EQ(v_dispatch + v_action + v_unknown_me + v_unhandled,
			 64u);

		CHECK_EQ(v_dispatch, 2u);
		CHECK_EQ(v_action, 11u);
		CHECK_EQ(v_unknown_me, 13u);
		CHECK_EQ(v_unhandled, 38u);

		printf("  msgtype: %u non-requests, %u refused on AR/AK, %u DownloadSection exemptions; "
		       "%u requests all answered (%u dispatch, %u class-refused, %u unknown ME, %u unhandled)\n",
		       non_request, non_request_refused,
		       exempt_download_section, request, v_dispatch, v_action,
		       v_unknown_me, v_unhandled);

		omci_engine_fini(eng);
		omci_mib_fini(mib);
	}

	{
		static uint32_t digest[3u * 48u * 256u];
		const uint32_t nm = omci_case_count(OMCI_CASE_MUTATE);

		CHECK_EQ(nm, (uint32_t)(ARRAY_SIZE(digest)));

		unsigned int diverged = 0;

		struct sigaction sa2 = { 0 };

		sa2.sa_handler = on_hang;
		CHECK_EQ(sigaction(SIGALRM, &sa2, NULL), 0);
		(void)alarm(CASE_HANG_SECONDS);

		for (unsigned int pass = 0; pass < 2u; pass++) {
			struct omci_mib *m = NULL;
			struct omci_engine *e = NULL;
			struct omci_hw_caps caps;

			CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
			CHECK_EQ(omci_mib_init(&m), OMCI_OK);
			CHECK_EQ(omci_engine_init(&e, m, &omci_t_tx_ops, NULL),
				 OMCI_OK);
			CHECK_EQ(omci_me_defaults_apply(m, &caps), OMCI_OK);

			for (uint32_t i = 0; i < nm; i++) {
				uint8_t wire[OMCI_FRAME_LEN];
				(void)omci_case_build(OMCI_CASE_MUTATE, i,
						      wire);
				g_case_in_flight = (sig_atomic_t)i;

				omci_t_tx_reset();
				const enum omci_status rc =
					omci_engine_rx(e, wire);

				uint32_t h = 2166136261u;
				const uint8_t st = (uint8_t)rc,
					      nf = (uint8_t)omci_t_tx.n;
				h = (h ^ st) * 16777619u;
				h = (h ^ nf) * 16777619u;
				const unsigned int kept =
					(omci_t_tx.n < OMCI_T_MAX_TX) ?
						omci_t_tx.n :
						OMCI_T_MAX_TX;
				for (unsigned int k = 0; k < kept; k++)
					for (unsigned int b = 0;
					     b < OMCI_FRAME_LEN; b++)
						h = (h ^
						     omci_t_tx.frame[k][b]) *
						    16777619u;

				if (pass == 0u) {
					digest[i] = h;
				} else if (digest[i] != h) {
					diverged++;
					if (diverged == 1u) {
						char label[64];

						omci_case_label(
							OMCI_CASE_MUTATE, i,
							label, sizeof(label));
						printf("    DIVERGED at %s: %08x then %08x\n",
						       label,
						       (unsigned int)digest[i],
						       (unsigned int)h);
					}
				}
			}

			omci_engine_fini(e);
			omci_mib_fini(m);
		}
		(void)alarm(0);
		g_case_in_flight = -1;

		CHECK_EQ(diverged, 0u);
		printf("  mutate: %lu cases answer identically across two independent runs\n",
		       (unsigned long)nm);
	}

	{
		unsigned int test_on_anig = 0, classes_with_a_test = 0;
		uint16_t seen[32];
		unsigned int n_seen = 0;

		const uint32_t nc = omci_case_count(OMCI_CASE_MTCLASS);

		for (uint32_t i = 0; i < nc; i++) {
			uint8_t wire[OMCI_FRAME_LEN];
			struct omci_frame f;

			CHECK(omci_case_build(OMCI_CASE_MTCLASS, i, wire));
			if (omci_frame_parse(wire, &f) != OMCI_OK)
				continue;
			if (f.msg_type != 18u)
				continue;

			if (f.class_id == 263u)
				test_on_anig++;

			bool already = false;

			for (unsigned int k = 0; k < n_seen; k++)
				already = already || seen[k] == f.class_id;
			if (!already && n_seen < 32u) {
				seen[n_seen++] = f.class_id;
				classes_with_a_test++;
			}
		}
		printf("  mtclass pairs a Test with %u distinct classes, %u of them ANI-G\n",
		       classes_with_a_test, test_on_anig);

		CHECK(test_on_anig > 0);
		CHECK_EQ(classes_with_a_test, 14u);
	}

	{
		struct omci_mib *mib = NULL;
		struct omci_engine *eng = NULL;

		CHECK_EQ(omci_mib_init(&mib), OMCI_OK);
		CHECK_EQ(omci_engine_init(&eng, mib, &omci_t_tx_ops, NULL),
			 OMCI_OK);

		unsigned int answered = 0, engaged = 0;
		const uint32_t nc = omci_case_count(OMCI_CASE_MTCLASS);

		for (uint32_t i = 0; i < nc; i++) {
			uint8_t wire[OMCI_FRAME_LEN];
			(void)omci_case_build(OMCI_CASE_MTCLASS, i, wire);
			omci_t_tx_reset();
			if (omci_engine_rx(eng, wire) != OMCI_OK ||
			    omci_t_tx.n == 0)
				continue;
			answered++;

			const uint8_t result = omci_t_tx.frame[0][8];

			if (result != 2u && result != 4u && result != 5u)
				engaged++;
		}
		printf("  mtclass: %u/%lu answered, %u of those actually did something\n",
		       answered, (unsigned long)nc, engaged);

		CHECK_EQ(answered, nc);

		CHECK(engaged > nc / 8u);

		omci_engine_fini(eng);
		omci_mib_fini(mib);
	}

	for (uint32_t i = 0; i < 256u; i++) {
		uint8_t wire[OMCI_FRAME_LEN];
		char label[64];

		CHECK(omci_case_build(OMCI_CASE_ENVELOPE, i, wire));
		omci_case_label(OMCI_CASE_ENVELOPE, i, label, sizeof(label));

		unsigned int named = 0xFFFFu;

		CHECK_EQ(sscanf(label, "envelope/byte2=0x%02x", &named), 1);
		CHECK_EQ(wire[2], named);
		CHECK(omci_frame_mic_ok(wire));
	}

	CHECK_EQ(omci_case_axis_by_name("nope"), OMCI_CASE_AXIS_COUNT);
	CHECK_EQ(omci_case_axis_by_name(NULL), OMCI_CASE_AXIS_COUNT);

	return OMCI_T_REPORT("test_cases");
}
