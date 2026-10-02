// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of MIB upload. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "me/table.h"
#include "me/upload.h"
#include "mib/store.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/array_size.h"

#include "harness.h"
#include "model_tsv.h"

#define MT_MIB_UPLOAD 13u
#define MT_MIB_UPLOAD_NEXT 14u
#define ME_ONU_DATA 2u
#define ME_OMCI 287u

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

struct want_unit {
	uint16_t mask;
	unsigned int bytes;
	unsigned int count;
};

/* The classes that G.988 leaves out of a MIB upload. */
static bool never_uploaded(uint16_t id)
{
	static const uint16_t never[] = { 83u,	149u, 154u, 158u, 287u, 288u,
					  289u, 303u, 304u, 308u, 317u };

	for (unsigned int i = 0; i < ARRAY_SIZE(never); i++)
		if (never[i] == id)
			return true;
	return false;
}

/*
 * Whether a MIB upload reports attribute @a of the class: of a PM entity,
 * which begins with its interval end time, only the control block.
 */
static bool want_reported(const struct tsv_class *c, unsigned int a)
{
	if (c->attrs[a - 1].table)
		return false;
	if (c->attr_count > 0u &&
	    strcmp(c->attrs[0].name, "Interval end time") == 0)
		return a == 2u;
	return true;
}

static unsigned int want_units(const struct tsv_class *c, struct want_unit *out,
			       unsigned int cap)
{
	unsigned int n = 0;
	struct want_unit u = { 0, 0, 0 };

	for (unsigned int a = 1; a <= c->attr_count; a++) {
		const struct tsv_attr *t = &c->attrs[a - 1];

		if (!want_reported(c, a))
			continue;
		if (u.bytes + t->size > 26u) {
			if (n < cap)
				out[n] = u;
			n++;
			u = (struct want_unit){ 0, 0, 0 };
		}
		u.mask = (uint16_t)(u.mask | (0x8000u >> (a - 1u)));
		u.bytes += t->size;
		u.count++;
	}
	if (u.count > 0u) {
		if (n < cap)
			out[n] = u;
		n++;
	}
	if (n == 0u) {
		if (cap > 0u)
			out[0] = (struct want_unit){ 0, 0, 0 };
		return 1u;
	}
	return n;
}

static int upload(struct omci_engine *eng, uint16_t tid, bool priority)
{
	uint8_t wire[OMCI_FRAME_LEN];

	if (priority)
		omci_t_build_pri(wire, tid, MT_MIB_UPLOAD, true, false,
				 ME_ONU_DATA, 0u);
	else
		omci_t_build(wire, tid, MT_MIB_UPLOAD, true, false, ME_ONU_DATA,
			     0u);
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(eng, wire), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	if (omci_t_tx.n != 1u)
		return -1;

	const uint8_t *f = omci_t_tx.frame[0];

	CHECK_EQ(f[2] & 0x3fu, 0x20u | MT_MIB_UPLOAD);
	for (unsigned int i = 10; i < 40; i++)
		CHECK_EQ(f[i], 0u);
	return be16(&f[8]);
}

static const uint8_t *upload_next(struct omci_engine *eng, uint16_t tid,
				  bool priority, uint16_t seq)
{
	uint8_t wire[OMCI_FRAME_LEN];

	if (priority)
		omci_t_build_pri(wire, tid, MT_MIB_UPLOAD_NEXT, true, false,
				 ME_ONU_DATA, 0u);
	else
		omci_t_build(wire, tid, MT_MIB_UPLOAD_NEXT, true, false,
			     ME_ONU_DATA, 0u);
	wire[8] = (uint8_t)(seq >> 8);
	wire[9] = (uint8_t)seq;
	omci_t_tx_reset();
	CHECK_EQ(omci_t_rx(eng, wire), OMCI_OK);
	CHECK_EQ(omci_t_tx.n, 1u);
	return omci_t_tx.n == 1u ? omci_t_tx.frame[0] : NULL;
}

static unsigned int expected_units(struct omci_mib *mib,
				   unsigned int *instances)
{
	unsigned int units = 0;

	*instances = 0;
	struct omci_mib_cursor cur;

	for (enum omci_status rc = omci_mib_walk_first(mib, &cur);
	     rc == OMCI_OK; rc = omci_mib_walk_next(mib, &cur)) {
		const struct tsv_class *c = tsv_class(cur.class_id);

		if (c == NULL || never_uploaded(c->id))
			continue;
		struct want_unit scratch[8];

		units += want_units(c, scratch, 8u);
		(*instances)++;
	}
	return units;
}

static void test_the_units_of_every_class_are_the_packing_of_the_data(void)
{
	for (unsigned int i = 0; i < tsv_class_count; i++) {
		const struct tsv_class *t = &tsv_classes[i];
		const struct omci_me_class *c = omci_me_class_by_id(t->id);
		struct want_unit want[8];

		CHECK(c != NULL);
		if (c == NULL)
			continue;
		const unsigned int n = want_units(t, want, 8u);

		CHECK_EQ(omci_me_upload_segments(c), n);
		CHECK_EQ(omci_me_upload_includes(c), !never_uploaded(t->id));

		unsigned int covered = 0;

		for (unsigned int u = 0; u < n && u < 8u; u++) {
			struct omci_me_upload_unit unit;

			CHECK(omci_me_upload_unit_at(c, u, &unit));
			CHECK_EQ(unit.mask, want[u].mask);
			CHECK_EQ(unit.count, want[u].count);
			CHECK_EQ(unit.bytes, want[u].bytes);
			CHECK(unit.bytes <= 26u);

			unsigned int bits = 0, last = 0;

			for (unsigned int k = 0; k < unit.count; k++) {
				CHECK(unit.attrs[k] > last);
				last = unit.attrs[k];
				bits |= 0x8000u >> (unit.attrs[k] - 1u);
			}
			CHECK_EQ(bits, unit.mask);
			covered |= unit.mask;
		}

		unsigned int all = 0;

		for (unsigned int a = 1; a <= t->attr_count; a++)
			if (want_reported(t, a))
				all |= 0x8000u >> (a - 1u);
		CHECK_EQ(covered, all);

		struct omci_me_upload_unit past;

		CHECK(!omci_me_upload_unit_at(c, n, &past));
	}

	struct omci_me_upload_unit unit;

	CHECK(!omci_me_upload_unit_at(NULL, 0, &unit));
	CHECK_EQ(omci_me_upload_segments(NULL), 1u);
	CHECK(!omci_me_upload_includes(NULL));
}

static void test_a_pm_entity_reports_only_its_control_block(void)
{
	static const uint16_t pm[] = { 321u, 322u };

	for (unsigned int i = 0; i < 2u; i++) {
		const struct omci_me_class *c = omci_me_class_by_id(pm[i]);
		struct omci_me_upload_unit unit;

		CHECK(c != NULL);
		if (c == NULL)
			continue;
		CHECK(omci_me_upload_includes(c));
		CHECK_EQ(omci_me_upload_segments(c), 1u);
		CHECK(omci_me_upload_unit_at(c, 0, &unit));
		CHECK_EQ(unit.mask, 0x4000u);
		CHECK_EQ(unit.count, 1u);
		CHECK_EQ(unit.attrs[0], 2u);
		CHECK_EQ(unit.bytes, 2u);
	}
}

static void test_the_count_is_of_messages_not_of_instances(void)
{
	unsigned int wide = 0, single = 0;

	for (unsigned int i = 0; i < tsv_class_count; i++) {
		struct want_unit scratch[8];
		const unsigned int n = want_units(&tsv_classes[i], scratch, 8u);

		if (n > 1u)
			wide++;
		else
			single++;
	}

	CHECK(wide > 0u);
	CHECK(single > 0u);
	printf("  %u classes need more than one message, %u need one\n", wide,
	       single);
}

static void test_the_whole_upload_describes_the_mib(void)
{
	struct omci_t_fixture fx;
	struct omci_hw_caps caps;

	CHECK(omci_t_up(&fx));
	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(fx.mib, &caps), OMCI_OK);

	unsigned int instances = 0;
	const unsigned int want = expected_units(fx.mib, &instances);
	const int got = upload(fx.eng, 0x0100u, false);

	printf("  the default MIB: %zu instances, %u of them uploaded, %d units\n",
	       omci_mib_count(fx.mib), instances, got);
	CHECK_EQ(got, (int)want);
	CHECK(got > 0);
	CHECK(instances < omci_mib_count(fx.mib));

	uint16_t prev_class = 0, prev_inst = 0;
	unsigned int prev_mask_end = 0;
	unsigned int instances_seen = 0;
	unsigned int units_seen = 0;

	for (int seq = 0; seq < got; seq++) {
		const uint8_t *f = upload_next(fx.eng, (uint16_t)(0x0200 + seq),
					       false, (uint16_t)seq);

		CHECK(f != NULL);
		if (f == NULL)
			continue;
		CHECK_EQ(f[2] & 0x3fu, 0x20u | MT_MIB_UPLOAD_NEXT);

		const uint16_t cls = be16(&f[8]);
		const uint16_t inst = be16(&f[10]);
		const uint16_t mask = be16(&f[12]);
		const struct tsv_class *t = tsv_class(cls);

		CHECK(t != NULL);
		CHECK(cls != ME_OMCI);
		CHECK(omci_mib_exists(fx.mib, cls, inst));
		if (t == NULL)
			continue;
		units_seen++;

		const bool same = seq > 0 && cls == prev_class &&
				  inst == prev_inst;
		if (seq > 0 && !same)
			CHECK(cls > prev_class ||
			      (cls == prev_class && inst > prev_inst));
		if (!same) {
			instances_seen++;
			prev_mask_end = 0;
		}
		prev_class = cls;
		prev_inst = inst;

		unsigned int at = 14u, last = prev_mask_end;

		for (unsigned int a = 1; a <= 16u; a++) {
			if ((mask & (0x8000u >> (a - 1u))) == 0u)
				continue;
			CHECK(a <= t->attr_count);
			CHECK(a > last);
			last = a;
			if (a > t->attr_count)
				break;
			const struct tsv_attr *w = &t->attrs[a - 1];

			CHECK(!w->table);
			uint8_t held[64];
			size_t n = 0;

			CHECK_EQ(omci_mib_attr_get(fx.mib, cls, inst,
						   (uint8_t)a, held,
						   sizeof(held), &n),
				 OMCI_OK);
			CHECK_EQ(n, (size_t)w->size);
			CHECK(at + w->size <= 40u);
			CHECK_MEM(&f[at], held, w->size);
			at += w->size;
		}
		prev_mask_end = last;
		CHECK(at - 14u <= 26u);
		for (unsigned int k = at; k < 40u; k++)
			CHECK_EQ(f[k], 0u);
	}

	CHECK_EQ(units_seen, (unsigned int)got);
	CHECK_EQ(instances_seen, instances);

	for (unsigned int extra = 0; extra < 3u; extra++) {
		const uint8_t *f =
			upload_next(fx.eng, (uint16_t)(0x0800 + extra), false,
				    (uint16_t)(got + (int)extra));
		CHECK(f != NULL);
		if (f != NULL)
			for (unsigned int k = 8; k < 40u; k++)
				CHECK_EQ(f[k], 0u);
	}
	const uint8_t *last_ff = upload_next(fx.eng, 0x0810u, false, 0xffffu);

	CHECK(last_ff != NULL);
	if (last_ff != NULL)
		for (unsigned int k = 8; k < 40u; k++)
			CHECK_EQ(last_ff[k], 0u);

	omci_t_down(&fx);
}

static void test_every_class_but_the_omci_entity_appears(void)
{
	struct omci_t_fixture fx;
	struct omci_hw_caps caps;
	bool seen[TSV_MAX_CLASSES] = { false };

	CHECK(omci_t_up(&fx));
	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(fx.mib, &caps), OMCI_OK);

	for (unsigned int i = 0; i < tsv_class_count; i++)
		if (tsv_classes[i].by_olt)
			CHECK_EQ(omci_mib_create(fx.mib, tsv_classes[i].id,
						 0x0abcu),
				 OMCI_OK);

	const int got = upload(fx.eng, 0x0300u, false);

	CHECK(got > 0);
	for (int seq = 0; seq < got; seq++) {
		const uint8_t *f = upload_next(fx.eng, (uint16_t)(0x0400 + seq),
					       false, (uint16_t)seq);
		if (f == NULL)
			continue;
		for (unsigned int i = 0; i < tsv_class_count; i++)
			if (tsv_classes[i].id == be16(&f[8]))
				seen[i] = true;
	}

	unsigned int classes = 0, left_out = 0;

	for (unsigned int i = 0; i < tsv_class_count; i++) {
		if (never_uploaded(tsv_classes[i].id)) {
			CHECK(!seen[i]);
			left_out++;
			continue;
		}
		if (!seen[i])
			printf("    class %u never appears in the upload\n",
			       tsv_classes[i].id);
		CHECK(seen[i]);
		classes += seen[i] ? 1u : 0u;
	}
	CHECK_EQ(left_out, 1u);
	CHECK_EQ(classes, tsv_class_count - left_out);

	omci_t_down(&fx);
}

static void test_the_count_follows_the_mib(void)
{
	struct omci_t_fixture fx;
	struct omci_hw_caps caps;

	CHECK(omci_t_up(&fx));
	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(fx.mib, &caps), OMCI_OK);

	const int before = upload(fx.eng, 0x0500u, false);
	const struct tsv_class *c = NULL;
	struct want_unit scratch[8];
	unsigned int need = 0;

	/* A class the OLT creates whose instance takes more than one unit. */
	for (unsigned int i = 0; i < tsv_class_count && need < 2u; i++) {
		if (!tsv_classes[i].by_olt || never_uploaded(tsv_classes[i].id))
			continue;
		c = &tsv_classes[i];
		need = want_units(c, scratch, 8u);
	}
	CHECK(need > 1u);
	if (c == NULL)
		return;
	CHECK_EQ(omci_mib_create(fx.mib, c->id, 0x0101u), OMCI_OK);
	CHECK_EQ(upload(fx.eng, 0x0501u, false), before + (int)need);

	CHECK_EQ(omci_mib_delete(fx.mib, c->id, 0x0101u), OMCI_OK);
	CHECK_EQ(upload(fx.eng, 0x0502u, false), before);

	/* A PM entity takes one unit: its control block. */
	CHECK_EQ(omci_mib_create(fx.mib, 322u, 0x0101u), OMCI_OK);
	CHECK_EQ(upload(fx.eng, 0x0503u, false), before + 1);
	CHECK_EQ(omci_mib_delete(fx.mib, 322u, 0x0101u), OMCI_OK);

	omci_t_down(&fx);
}

static void test_no_upload_in_progress(void)
{
	struct omci_t_fixture fx;
	struct omci_hw_caps caps;

	CHECK(omci_t_up(&fx));
	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(fx.mib, &caps), OMCI_OK);

	const uint8_t *f = upload_next(fx.eng, 0x0600u, false, 0u);

	CHECK(f != NULL);
	if (f != NULL)
		for (unsigned int k = 8; k < 40u; k++)
			CHECK_EQ(f[k], 0u);

	omci_t_down(&fx);
}

static void test_the_channels_are_independent(void)
{
	struct omci_t_fixture fx;
	struct omci_hw_caps caps;

	CHECK(omci_t_up(&fx));
	CHECK_EQ(omci_hw_caps(&caps), OMCI_OK);
	CHECK_EQ(omci_me_defaults_apply(fx.mib, &caps), OMCI_OK);

	const int low = upload(fx.eng, 0x0700u, false);
	const uint8_t *first = upload_next(fx.eng, 0x0701u, false, 0u);
	uint8_t keep[OMCI_FRAME_LEN] = { 0 };

	CHECK(first != NULL);
	if (first != NULL)
		memcpy(keep, first, sizeof(keep));

	CHECK_EQ(omci_mib_create(fx.mib, 322u, 0x0101u), OMCI_OK);
	const int high = upload(fx.eng, 0x0702u, true);

	CHECK(high > low);

	const uint8_t *again = upload_next(fx.eng, 0x0703u, false, 0u);

	CHECK(again != NULL);
	if (again != NULL)
		CHECK_MEM(&again[8], &keep[8], 32);
	const uint8_t *edge =
		upload_next(fx.eng, 0x0704u, false, (uint16_t)low);
	CHECK(edge != NULL);
	if (edge != NULL)
		for (unsigned int k = 8; k < 40u; k++)
			CHECK_EQ(edge[k], 0u);

	const uint8_t *hi_unit =
		upload_next(fx.eng, 0x0705u, true, (uint16_t)(high - 1));
	CHECK(hi_unit != NULL);
	if (hi_unit != NULL)
		CHECK_EQ(be16(&hi_unit[8]), 322u);

	omci_t_down(&fx);
}

int main(int argc, char **argv)
{
	if (!tsv_open(argc, argv)) {
		printf("test_upload: no table to read\n");
		return 1;
	}

	RUN(test_the_units_of_every_class_are_the_packing_of_the_data);
	RUN(test_a_pm_entity_reports_only_its_control_block);
	RUN(test_the_count_is_of_messages_not_of_instances);
	RUN(test_the_whole_upload_describes_the_mib);
	RUN(test_every_class_but_the_omci_entity_appears);
	RUN(test_the_count_follows_the_mib);
	RUN(test_no_upload_in_progress);
	RUN(test_the_channels_are_independent);

	return OMCI_T_REPORT("test_upload");
}
