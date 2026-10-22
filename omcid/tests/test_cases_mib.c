// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests of the case axes made from a MIB. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/hw.h"
#include "me/defaults.h"
#include "me/layout.h"
#include "me/table.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omci/cases.h"
#include "omci/engine.h"
#include "omci/frame.h"
#include "omcid/array_size.h"
#include "sys/log.h"

#include "harness.h"
#include "log_capture.h"

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

static uint32_t label_digest(enum omci_case_axis axis)
{
	uint32_t h = 2166136261u;
	const uint32_t n = omci_case_count(axis);

	for (uint32_t i = 0; i < n; i++) {
		char label[128];

		omci_case_label(axis, i, label, sizeof(label));
		for (const char *c = label; *c != '\0'; c++) {
			h ^= (uint8_t)*c;
			h *= 16777619u;
		}
		h ^= (uint8_t)'\n';
		h *= 16777619u;
	}
	return h;
}

static struct omci_mib *g_mib;

static bool populated(void)
{
	struct omci_hw_caps caps;

	g_mib = NULL;
	return omci_mib_init(&g_mib) == OMCI_OK &&
	       omci_hw_caps(&caps) == OMCI_OK &&
	       omci_me_defaults_apply(g_mib, &caps) == OMCI_OK;
}

static void test_an_axis_that_is_not_prepared_has_no_cases(void)
{
	uint8_t wire[OMCI_FRAME_LEN];
	char label[16] = "x";

	CHECK(omci_case_axis_populated(OMCI_CASE_REACH));
	CHECK(omci_case_axis_populated(OMCI_CASE_POPULATED));
	CHECK(!omci_case_axis_populated(OMCI_CASE_ENVELOPE));
	CHECK(!omci_case_axis_populated(OMCI_CASE_AXIS_COUNT));

	for (unsigned int a = OMCI_CASE_REACH; a <= OMCI_CASE_POPULATED; a++) {
		const enum omci_case_axis axis = (enum omci_case_axis)a;

		CHECK_EQ(omci_case_count(axis), 0u);
		CHECK(!omci_case_build(axis, 0, wire));
		omci_case_label(axis, 0, label, sizeof(label));
		CHECK_EQ(label[0], '\0');
		CHECK(omci_case_capacity(axis) > 0u);
	}
	CHECK_EQ(omci_case_capacity(OMCI_CASE_ENVELOPE), 0u);
	CHECK_EQ(omci_case_capacity(OMCI_CASE_AXIS_COUNT), 0u);
	CHECK_EQ(strcmp(omci_case_axis_name(OMCI_CASE_REACH), "reach"), 0);
	CHECK_EQ(strcmp(omci_case_axis_name(OMCI_CASE_POPULATED), "populated"),
		 0);
	CHECK_EQ(omci_case_axis_by_name("reach"), OMCI_CASE_REACH);
	CHECK_EQ(omci_case_axis_by_name("populated"), OMCI_CASE_POPULATED);
}

static void test_every_axis_is_what_it_was_when_it_was_pinned(void)
{
	CHECK(populated());
	omci_case_prepare(OMCI_CASE_REACH, g_mib);
	omci_case_prepare(OMCI_CASE_POPULATED, g_mib);
	omci_case_prepare(OMCI_CASE_MUTATE, g_mib);

	static const struct {
		enum omci_case_axis axis;
		uint32_t cases;
		uint32_t digest;
	} pin[] = {
		{ OMCI_CASE_ENVELOPE, 1669u, 0x02894449u },
		{ OMCI_CASE_MUTATE, 36864u, 0x7ce87501u },
		{ OMCI_CASE_MSGTYPE, 256u, 0x7b94fe45u },
		{ OMCI_CASE_MTCLASS, 896u, 0x321cec05u },
		{ OMCI_CASE_REACH, 9469u, 0x3babe70au },
		{ OMCI_CASE_POPULATED, 633u, 0x5d981c98u },
	};
	for (size_t k = 0; k < ARRAY_SIZE(pin); k++) {
		CHECK_EQ(omci_case_count(pin[k].axis), pin[k].cases);
		const uint32_t d = axis_digest(pin[k].axis);

		if (d != pin[k].digest)
			printf("    axis %s: digest %08x, pinned %08x\n",
			       omci_case_axis_name(pin[k].axis), d,
			       pin[k].digest);
		CHECK_EQ(d, pin[k].digest);
	}

	static const struct {
		enum omci_case_axis axis;
		uint32_t digest;
	} label_pin[] = {
		{ OMCI_CASE_ENVELOPE, 0x50f3c206u },
		{ OMCI_CASE_MUTATE, 0xaceb5285u },
		{ OMCI_CASE_MSGTYPE, 0xb6b6d2f5u },
		{ OMCI_CASE_MTCLASS, 0x26e548e5u },
		{ OMCI_CASE_REACH, 0x954f8fd9u },
		{ OMCI_CASE_POPULATED, 0x949c3851u },
	};
	for (size_t k = 0; k < ARRAY_SIZE(label_pin); k++) {
		const uint32_t d = label_digest(label_pin[k].axis);

		if (d != label_pin[k].digest)
			printf("    axis %s: label digest %08x, pinned %08x\n",
			       omci_case_axis_name(label_pin[k].axis), d,
			       label_pin[k].digest);
		CHECK_EQ(d, label_pin[k].digest);
	}

	omci_mib_fini(g_mib);
	CHECK(populated());
	omci_case_prepare(OMCI_CASE_REACH, g_mib);
	omci_case_prepare(OMCI_CASE_POPULATED, g_mib);
	CHECK_EQ(axis_digest(OMCI_CASE_REACH), 0x3babe70au);
	CHECK_EQ(axis_digest(OMCI_CASE_POPULATED), 0x5d981c98u);
	omci_mib_fini(g_mib);
}

static void test_a_second_preparation_leaves_nothing_of_the_first(void)
{
	CHECK(populated());
	omci_case_prepare(OMCI_CASE_REACH, g_mib);
	omci_mib_fini(g_mib);

	static const struct {
		uint16_t class_id;
		unsigned int gone;
		uint32_t cases;
		uint32_t digest;
	} without[] = {
		{ 5u, 3u, 9433u, 0xbc1a7185u },
		{ 256u, 1u, 9460u, 0xfabe9dc8u },
		{ 263u, 1u, 9455u, 0x3428c353u },
		{ 2u, 1u, 9465u, 0xf006245fu },
	};
	for (size_t k = 0; k < ARRAY_SIZE(without); k++) {
		CHECK(populated());

		uint16_t inst[8];
		unsigned int n = 0;
		struct omci_mib_cursor cur;

		for (enum omci_status rc = omci_mib_walk_first(g_mib, &cur);
		     rc == OMCI_OK; rc = omci_mib_walk_next(g_mib, &cur))
			if (cur.class_id == without[k].class_id && n < 8u)
				inst[n++] = cur.instance;
		CHECK_EQ(n, without[k].gone);
		for (unsigned int i = 0; i < n; i++)
			CHECK_EQ(omci_mib_delete(g_mib, without[k].class_id,
						 inst[i]),
				 OMCI_OK);

		omci_case_prepare(OMCI_CASE_REACH, g_mib);
		CHECK_EQ(omci_case_count(OMCI_CASE_REACH), without[k].cases);
		const uint32_t d = axis_digest(OMCI_CASE_REACH);

		if (d != without[k].digest)
			printf("    reach without class %u: digest %08x, pinned %08x\n",
			       without[k].class_id, d, without[k].digest);
		CHECK_EQ(d, without[k].digest);
		omci_mib_fini(g_mib);
	}
}

static void test_every_case_is_a_frame_with_a_label(void)
{
	CHECK(populated());
	omci_case_prepare(OMCI_CASE_REACH, g_mib);
	omci_case_prepare(OMCI_CASE_POPULATED, g_mib);

	for (unsigned int a = OMCI_CASE_REACH; a <= OMCI_CASE_POPULATED; a++) {
		const enum omci_case_axis axis = (enum omci_case_axis)a;
		const uint32_t n = omci_case_count(axis);
		unsigned int bad_frame = 0, no_label = 0, wrong_tid = 0;

		for (uint32_t i = 0; i < n; i++) {
			uint8_t wire[OMCI_FRAME_LEN];
			struct omci_frame f = { 0 };
			char label[96];
			(void)omci_case_build(axis, i, wire);
			if (omci_frame_parse(wire, &f) != OMCI_OK ||
			    !omci_frame_mic_ok(wire))
				bad_frame++;
			if (f.tci != (uint16_t)(i + 1u))
				wrong_tid++;
			omci_case_label(axis, i, label, sizeof(label));
			if (label[0] == '\0' || strchr(label, '/') == NULL)
				no_label++;
		}
		CHECK_EQ(bad_frame, 0u);
		CHECK_EQ(wrong_tid, 0u);
		CHECK_EQ(no_label, 0u);

		uint8_t wire[OMCI_FRAME_LEN];
		char label[8] = "x";

		CHECK(!omci_case_build(axis, n, wire));
		CHECK(omci_frame_mic_ok(wire));
		omci_case_label(axis, n, label, sizeof(label));
		CHECK_EQ(label[0], '\0');
	}
	omci_mib_fini(g_mib);
}

static const uint8_t *response_to(const uint8_t *req)
{
	const uint8_t mt = req[2] & 0x1Fu;

	for (unsigned int i = 0; i < omci_t_tx.n && i < OMCI_T_MAX_TX; i++)
		if ((omci_t_tx.frame[i][2] & 0x20u) != 0u &&
		    (omci_t_tx.frame[i][2] & 0x1Fu) == mt)
			return omci_t_tx.frame[i];
	return NULL;
}

static bool family_is(enum omci_case_axis axis, uint32_t i, const char *family)
{
	char label[96];

	omci_case_label(axis, i, label, sizeof(label));
	const size_t n = strlen(family);

	return strncmp(label, family, n) == 0 && label[n] == '/';
}

static void
test_reach_has_the_families_it_was_counted_with_and_is_answered_as_it_was(void)
{
	CHECK(populated());
	omci_case_prepare(OMCI_CASE_REACH, g_mib);
	struct omci_engine *eng = NULL;

	CHECK_EQ(omci_engine_init(&eng, g_mib, &omci_t_tx_ops, NULL), OMCI_OK);
	omci_log_level_set(OMCI_LOG_ERR);

	static const struct {
		const char *family;
		unsigned int n;
	} families[] = {
		{ "create", 49 }, { "get", 41 },	{ "mibreset", 1 },
		{ "set", 246 },	  { "setagain", 5 },	{ "setback", 5 },
		{ "swdl", 35 },	  { "synctime", 8820 }, { "table", 69 },
		{ "test", 110 },  { "upload", 88 },
	};
	unsigned int tally[11] = { 0 };
	unsigned int total_families = 0;
	const uint32_t n = omci_case_count(OMCI_CASE_REACH);
	unsigned int answered = 0, no_result_byte = 0;
	unsigned int results[16] = { 0 };

	for (uint32_t i = 0; i < n; i++) {
		for (size_t k = 0; k < ARRAY_SIZE(families); k++)
			if (family_is(OMCI_CASE_REACH, i, families[k].family)) {
				tally[k]++;
				total_families++;
			}

		uint8_t wire[OMCI_FRAME_LEN];
		(void)omci_case_build(OMCI_CASE_REACH, i, wire);
		omci_t_tx_reset();
		(void)!omci_engine_rx(eng, wire);
		const uint8_t *rsp = response_to(wire);

		if (rsp == NULL)
			continue;
		answered++;
		const uint8_t mt = (uint8_t)(wire[2] & 0x1Fu);

		if (mt >= 11u && mt <= 14u)
			no_result_byte++;
		else if (rsp[8] < 16u)
			results[rsp[8]]++;
	}
	omci_engine_fini(eng);
	omci_mib_fini(g_mib);

	for (size_t k = 0; k < ARRAY_SIZE(families); k++) {
		if (tally[k] != families[k].n)
			printf("    family %s: %u cases, counted %u\n",
			       families[k].family, tally[k], families[k].n);
		CHECK_EQ(tally[k], families[k].n);
	}
	CHECK_EQ(total_families, n);
	CHECK_EQ(answered, n);

	CHECK_EQ(no_result_byte, 88u);
	CHECK_EQ(results[0], 788u);
	CHECK_EQ(results[2], 139u);
	CHECK_EQ(results[3], 8411u);
	CHECK_EQ(results[4], 1u);
	CHECK_EQ(results[5], 5u);
	CHECK_EQ(results[9], 37u);
	CHECK_EQ(results[1] + results[6] + results[7] + results[8], 0u);
}

static unsigned int reply_bytes_of(const struct omci_me_class *cls)
{
	unsigned int bytes = 0;

	for (uint8_t attr = 1; attr <= cls->attr_count; attr++) {
		const struct omci_me_attr *a = omci_me_attr_get(cls, attr);

		bytes += omci_me_attr_is_table(a) ? 4u : a->size;
	}
	return bytes;
}

static void test_populated_asks_for_what_the_model_holds_and_gets_it_back(void)
{
	CHECK(populated());
	omci_case_prepare(OMCI_CASE_POPULATED, g_mib);
	struct omci_engine *eng = NULL;

	CHECK_EQ(omci_engine_init(&eng, g_mib, &omci_t_tx_ops, NULL), OMCI_OK);
	omci_log_level_set(OMCI_LOG_ERR);

	const uint32_t n = omci_case_count(OMCI_CASE_POPULATED);
	unsigned int getall = 0, getall_ok = 0, getall_full = 0, getpart = 0,
		     getpart_ok = 0, setget = 0, setget_back = 0;
	uint8_t last_set[OMCI_FRAME_LEN];

	memset(last_set, 0, sizeof(last_set));

	for (uint32_t i = 0; i < n; i++) {
		uint8_t wire[OMCI_FRAME_LEN];
		(void)omci_case_build(OMCI_CASE_POPULATED, i, wire);
		omci_t_tx_reset();
		(void)!omci_engine_rx(eng, wire);
		const uint8_t *rsp = response_to(wire);

		CHECK(rsp != NULL);
		if (rsp == NULL)
			continue;
		const struct omci_me_class *cls = omci_me_class_by_id(
			(uint16_t)((wire[4] << 8) | wire[5]));

		if (family_is(OMCI_CASE_POPULATED, i, "getall")) {
			getall++;
			const bool fits = reply_bytes_of(cls) <= 25u;

			if (rsp[8] == 0u)
				getall_ok++;

			if (fits && rsp[8] == 0u)
				getall_full++;
			CHECK_EQ(rsp[8], fits ? 0u : 9u);
		} else if (family_is(OMCI_CASE_POPULATED, i, "getpart")) {
			getpart++;
			if (rsp[8] == 0u)
				getpart_ok++;
		} else if (family_is(OMCI_CASE_POPULATED, i, "setget")) {
			setget++;
			if ((wire[2] & 0x1Fu) == 8u) {
				CHECK_EQ(rsp[8], 0u);
				memcpy(last_set, wire, sizeof(last_set));
			} else {
				CHECK_EQ(rsp[8], 0u);
				const struct omci_me_attr *a = NULL;

				for (uint8_t attr = 1; attr <= cls->attr_count;
				     attr++)
					if (omci_attr_in_mask(
						    (uint16_t)((last_set[8]
								<< 8) |
							       last_set[9]),
						    attr))
						a = omci_me_attr_get(cls, attr);
				CHECK(a != NULL);
				if (a != NULL && memcmp(&rsp[11], &last_set[10],
							a->size) == 0)
					setget_back++;
			}
		}
	}
	omci_engine_fini(eng);
	omci_mib_fini(g_mib);

	CHECK_EQ(getall, 38u);
	CHECK_EQ(getall_ok, getall_full);
	CHECK(getall_ok > 0u && getall_ok < getall);
	CHECK(getpart >= getall);
	CHECK_EQ(getpart_ok, getpart);
	CHECK(setget > 0u && setget % 2u == 0u);
	CHECK_EQ(setget_back, setget / 2u);
}

static void test_every_upload_next_asks_for_its_own_unit(void)
{
	CHECK(populated());
	for (uint16_t inst = 0x5000u; inst < 0x5000u + 80u; inst++)
		CHECK_EQ(omci_mib_create(g_mib, 134, inst), OMCI_OK);
	omci_case_prepare(OMCI_CASE_REACH, g_mib);

	const uint32_t n = omci_case_count(OMCI_CASE_REACH);
	unsigned int checked = 0, wrong = 0, past = 0;
	unsigned long highest = 0;

	for (uint32_t i = 0; i < n; i++) {
		char label[96];

		omci_case_label(OMCI_CASE_REACH, i, label, sizeof(label));
		unsigned long seq = 0;

		if (sscanf(label, "upload/next/%lu", &seq) != 1)
			continue;
		uint8_t wire[OMCI_FRAME_LEN];
		(void)omci_case_build(OMCI_CASE_REACH, i, wire);
		checked++;
		past += strstr(label, "/past") != NULL ? 1u : 0u;
		highest = seq > highest && strstr(label, "/past") == NULL ?
				  seq :
				  highest;
		if ((unsigned long)((wire[8] << 8) | wire[9]) != seq)
			wrong++;
	}
	omci_mib_fini(g_mib);

	printf("    %u upload next cases, %u of them past the end, the last unit %lu\n",
	       checked, past, highest);
	CHECK(highest > 300ul);
	CHECK_EQ(past, 3u);
	CHECK_EQ(wrong, 0u);
}

static unsigned int reply_bytes_of_mask(const struct omci_me_class *cls,
					uint16_t mask)
{
	unsigned int bytes = 0;

	for (uint8_t attr = 1; attr <= cls->attr_count; attr++) {
		const struct omci_me_attr *a = omci_me_attr_get(cls, attr);

		if (omci_attr_in_mask(mask, attr))
			bytes += omci_me_attr_is_table(a) ? 4u : a->size;
	}
	return bytes;
}

static uint16_t all_attributes_of(const struct omci_me_class *cls)
{
	uint16_t mask = 0;

	for (uint8_t attr = 1; attr <= cls->attr_count; attr++)
		mask = (uint16_t)(mask | omci_attr_bit(attr));
	return mask;
}

static void test_the_pieces_of_a_get_are_the_fewest_that_fit(void)
{
	CHECK(populated());
	unsigned int made = 0;

	for (size_t k = 0; k < omci_me_class_count; k++) {
		const uint16_t cls = omci_me_classes[k].class_id;
		struct omci_mib_cursor cur;
		bool have = false;

		for (enum omci_status rc = omci_mib_walk_first(g_mib, &cur);
		     rc == OMCI_OK; rc = omci_mib_walk_next(g_mib, &cur))
			have = have || cur.class_id == cls;
		if (!have) {
			CHECK_EQ(omci_mib_create(g_mib, cls,
						 (uint16_t)(0x6000u + k)),
				 OMCI_OK);
			made++;
		}
	}
	CHECK(made > 10u);
	omci_case_prepare(OMCI_CASE_POPULATED, g_mib);

	struct omci_engine *eng = NULL;

	CHECK_EQ(omci_engine_init(&eng, g_mib, &omci_t_tx_ops, NULL), OMCI_OK);
	omci_log_level_set(OMCI_LOG_ERR);

	const uint32_t n = omci_case_count(OMCI_CASE_POPULATED);
	uint16_t prev_cls = 0xffffu, prev_inst = 0xffffu, covered = 0,
		 prev_mask = 0;
	unsigned int pieces = 0, instances = 0, split_wrongly = 0, overfull = 0,
		     not_answered = 0, getall_wrong = 0;
	unsigned int not_covered = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint8_t wire[OMCI_FRAME_LEN];
		(void)omci_case_build(OMCI_CASE_POPULATED, i, wire);
		const uint16_t cls_id = (uint16_t)((wire[4] << 8) | wire[5]);
		const uint16_t inst = (uint16_t)((wire[6] << 8) | wire[7]);
		const struct omci_me_class *cls = omci_me_class_by_id(cls_id);
		const uint16_t mask = (uint16_t)((wire[8] << 8) | wire[9]);

		omci_t_tx_reset();
		(void)!omci_engine_rx(eng, wire);
		const uint8_t *rsp = response_to(wire);
		const uint8_t result = rsp != NULL ? rsp[8] : 0xffu;

		if (family_is(OMCI_CASE_POPULATED, i, "getall")) {
			const bool fits = reply_bytes_of(cls) <= 25u;

			if (result != (fits ? 0u : 9u))
				getall_wrong++;
		} else if (family_is(OMCI_CASE_POPULATED, i, "getpart")) {
			pieces++;
			if (result != 0u)
				not_answered++;
			if (reply_bytes_of_mask(cls, mask) > 25u)
				overfull++;
			if (cls_id == prev_cls && inst == prev_inst) {
				if ((mask & covered) != 0u)
					split_wrongly++;
				uint8_t first = 1;

				while (!omci_attr_in_mask(mask, first))
					first++;
				if (reply_bytes_of_mask(cls, prev_mask) +
					    reply_bytes_of_mask(
						    cls,
						    omci_attr_bit(first)) <=
				    25u)
					split_wrongly++;
				covered = (uint16_t)(covered | mask);
			} else {
				if (instances > 0u &&
				    covered != all_attributes_of(
						       omci_me_class_by_id(
							       prev_cls)))
					not_covered++;
				covered = mask;
				instances++;
			}
			prev_cls = cls_id;
			prev_inst = inst;
			prev_mask = mask;
		}
	}
	if (instances > 0u &&
	    covered != all_attributes_of(omci_me_class_by_id(prev_cls)))
		not_covered++;
	omci_engine_fini(eng);
	omci_mib_fini(g_mib);

	printf("    %u instances, %u pieces\n", instances, pieces);
	CHECK(pieces > instances);
	CHECK_EQ(not_covered, 0u);
	CHECK_EQ(split_wrongly, 0u);
	CHECK_EQ(overfull, 0u);
	CHECK_EQ(not_answered, 0u);
	CHECK_EQ(getall_wrong, 0u);
}

/* Fill the MIB with one class until its rows or its arena run out. */
static void fill(uint16_t class_id)
{
	char log[256];

	OMCI_T_CAPTURE_LOG(log, {
		for (uint16_t inst = 0x4000u;
		     omci_mib_create(g_mib, class_id, inst) == OMCI_OK; inst++)
			;
	});
	CHECK(strstr(log, "mib full: 512 rows") != NULL ||
	      strstr(log, "mib arena full") != NULL);
}

/*
 * The class whose instances, as many as the MIB holds, have the most
 * writable attributes among them: the most cases a full MIB can ask for.
 */
static uint16_t busiest_class(void)
{
	uint16_t busiest = 0;
	unsigned long most = 0;

	for (size_t k = 0; k < omci_me_class_count; k++) {
		const struct omci_me_class *c = &omci_me_classes[k];
		const size_t row = omci_me_row_size(c);
		unsigned long fit = OMCI_MIB_MAX_ROWS, writable = 0;

		if (row > 0u && OMCI_MIB_ARENA_BYTES / row < fit)
			fit = OMCI_MIB_ARENA_BYTES / row;
		for (uint8_t attr = 1; attr <= c->attr_count; attr++)
			if ((omci_me_attr_get(c, attr)->access &
			     OMCI_ME_ACC_WRITE) != 0u)
				writable++;
		if (fit * writable > most) {
			most = fit * writable;
			busiest = c->class_id;
		}
	}
	return busiest;
}

static void test_a_mib_filled_to_its_capacity_fits_the_arrays(void)
{
	const uint16_t busiest = busiest_class();

	CHECK(busiest != 0u);

	CHECK(populated());
	fill(busiest);
	CHECK(omci_mib_count(g_mib) > 256u);
	omci_case_prepare(OMCI_CASE_REACH, g_mib);
	omci_case_prepare(OMCI_CASE_POPULATED, g_mib);
	CHECK(omci_case_count(OMCI_CASE_REACH) > 9384u);
	CHECK(omci_case_count(OMCI_CASE_REACH) <=
	      omci_case_capacity(OMCI_CASE_REACH));
	CHECK(omci_case_count(OMCI_CASE_POPULATED) > 594u);
	CHECK(omci_case_count(OMCI_CASE_POPULATED) <=
	      omci_case_capacity(OMCI_CASE_POPULATED));
	omci_mib_fini(g_mib);
}

int main(void)
{
	omci_log_level_set(OMCI_LOG_ERR);
	RUN(test_an_axis_that_is_not_prepared_has_no_cases);
	RUN(test_every_axis_is_what_it_was_when_it_was_pinned);
	RUN(test_a_second_preparation_leaves_nothing_of_the_first);
	RUN(test_every_case_is_a_frame_with_a_label);
	RUN(test_reach_has_the_families_it_was_counted_with_and_is_answered_as_it_was);
	RUN(test_populated_asks_for_what_the_model_holds_and_gets_it_back);
	RUN(test_the_pieces_of_a_get_are_the_fewest_that_fit);
	RUN(test_every_upload_next_asks_for_its_own_unit);
	RUN(test_a_mib_filled_to_its_capacity_fits_the_arrays);
	return OMCI_T_REPORT("test_cases_mib");
}
