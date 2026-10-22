// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Tests that the class table is the data table. */

#include <stdio.h>
#include <string.h>

#include "me/action.h"
#include "me/layout.h"
#include "me/table.h"

#include "harness.h"
#include "model_tsv.h"
#include "omcid/array_size.h"

static void test_the_classes_are_the_data_classes(void)
{
	CHECK_EQ(omci_me_class_count, (size_t)tsv_class_count);
	CHECK_EQ(tsv_class_count, 72u);

	unsigned int attrs = 0;

	for (unsigned int i = 0; i < tsv_class_count; i++) {
		const struct tsv_class *t = &tsv_classes[i];
		const struct omci_me_class *c = omci_me_class_by_id(t->id);

		CHECK(c != NULL);
		if (c == NULL)
			continue;

		if (strcmp(c->name, t->name) != 0)
			printf("    class %u: name \"%s\", data says \"%s\"\n",
			       t->id, c->name, t->name);
		CHECK(strcmp(c->name, t->name) == 0);
		CHECK_EQ(c->class_id, t->id);
		CHECK_EQ(c->creator, t->by_olt ? OMCI_ME_CREATED_BY_OLT :
						 OMCI_ME_CREATED_BY_ONU);
		CHECK_EQ(c->actions, t->actions);
		CHECK_EQ(c->alarms, t->alarms);
		CHECK_EQ(c->attr_count, t->attr_count);
		attrs += t->attr_count;

		CHECK_EQ(omci_me_action_mask(t->id), t->actions);
		for (uint8_t mt = 0; mt < 32u; mt++)
			CHECK_EQ(omci_me_action_allowed(t->id, mt),
				 (t->actions & (1u << mt)) != 0u);
	}
	CHECK_EQ(attrs, 648u);
	printf("  %u classes, %u attributes\n", tsv_class_count, attrs);
}

static void test_every_attribute_is_found_under_its_number(void)
{
	for (unsigned int i = 0; i < tsv_class_count; i++) {
		const struct tsv_class *t = &tsv_classes[i];
		const struct omci_me_class *c = omci_me_class_by_id(t->id);

		if (c == NULL)
			continue;
		for (unsigned int n = 1; n <= t->attr_count; n++) {
			const struct tsv_attr *w = &t->attrs[n - 1];
			const struct omci_me_attr *a =
				omci_me_attr_get(c, (uint8_t)n);

			CHECK(a != NULL);
			if (a == NULL)
				continue;
			if (strcmp(a->name, w->name) != 0)
				printf("    %u/%u: name \"%s\", data says \"%s\"\n",
				       t->id, n, a->name, w->name);
			CHECK(strcmp(a->name, w->name) == 0);
			CHECK_EQ(a->size, w->size);
			CHECK_EQ((a->access & OMCI_ME_ACC_READ) != 0u, w->read);
			CHECK_EQ((a->access & OMCI_ME_ACC_WRITE) != 0u,
				 w->write);
			CHECK_EQ((a->access & OMCI_ME_ACC_SET_BY_CREATE) != 0u,
				 w->create);
			CHECK_EQ((a->flags & OMCI_ME_ATTR_MANDATORY) != 0u,
				 w->mandatory);
			CHECK_EQ((a->flags & OMCI_ME_ATTR_TABLE) != 0u,
				 w->table);
			CHECK_EQ(omci_me_attr_is_table(a), w->table);
			CHECK_EQ((a->flags & OMCI_ME_ATTR_AVC) != 0u, w->avc);

			CHECK_EQ(a->access & ~(OMCI_ME_ACC_READ |
					       OMCI_ME_ACC_WRITE |
					       OMCI_ME_ACC_SET_BY_CREATE),
				 0u);
			CHECK_EQ(a->flags & ~(OMCI_ME_ATTR_MANDATORY |
					      OMCI_ME_ATTR_TABLE |
					      OMCI_ME_ATTR_AVC),
				 0u);
		}

		CHECK(omci_me_attr_get(c, 0) == NULL);
		CHECK(omci_me_attr_get(c, (uint8_t)(t->attr_count + 1u)) ==
		      NULL);
		CHECK(omci_me_attr_get(c, 17) == NULL);
		CHECK(omci_me_attr_get(c, 255) == NULL);
	}

	CHECK(omci_me_attr_get(NULL, 1) == NULL);
}

static void test_the_tables(void)
{
	unsigned int tables = 0, classes = 0;

	for (unsigned int i = 0; i < tsv_class_count; i++) {
		unsigned int here = 0;

		for (unsigned int n = 1; n <= tsv_classes[i].attr_count; n++)
			here += tsv_classes[i].attrs[n - 1].table ? 1u : 0u;
		tables += here;
		classes += here > 0u ? 1u : 0u;

		if (here > 0u)
			CHECK((tsv_classes[i].actions & (1u << 26)) != 0u);
	}
	CHECK_EQ(tables, 25u);
	CHECK_EQ(classes, 12u);
}

static void
test_the_row_layout_is_the_sizes_of_the_attributes_that_are_not_tables(void)
{
	for (unsigned int i = 0; i < tsv_class_count; i++) {
		const struct tsv_class *t = &tsv_classes[i];
		const struct omci_me_class *c = omci_me_class_by_id(t->id);
		size_t off = 0;

		if (c == NULL)
			continue;
		for (unsigned int n = 1; n <= t->attr_count; n++) {
			const struct tsv_attr *w = &t->attrs[n - 1];

			if (w->table) {
				CHECK_EQ(omci_me_attr_offset(c, (uint8_t)n),
					 OMCI_ME_NO_OFFSET);
				continue;
			}
			CHECK_EQ(omci_me_attr_offset(c, (uint8_t)n), off);
			off += w->size;
		}
		CHECK_EQ(omci_me_row_size(c), off);
		CHECK_EQ(omci_me_attr_offset(c, 0), OMCI_ME_NO_OFFSET);
		CHECK_EQ(omci_me_attr_offset(c, (uint8_t)(t->attr_count + 1u)),
			 OMCI_ME_NO_OFFSET);
	}
	CHECK_EQ(omci_me_row_size(NULL), 0u);
	CHECK_EQ(omci_me_attr_offset(NULL, 1), OMCI_ME_NO_OFFSET);
}

static void test_the_classes_are_in_ascending_order_and_nothing_else_is_in(void)
{
	for (size_t i = 1; i < omci_me_class_count; i++)
		CHECK(omci_me_classes[i - 1].class_id <
		      omci_me_classes[i].class_id);

	static const uint16_t absent[] = { 0,	   1,	   3,	   23,
					   51,	   158,	   330,	   335,
					   3000,   0x8000, 0xea60, 0xfffd,
					   0xfffe, 0xffff };
	for (size_t i = 0; i < ARRAY_SIZE(absent); i++) {
		CHECK(omci_me_class_by_id(absent[i]) == NULL);
		CHECK_EQ(omci_me_action_mask(absent[i]), 0u);
		CHECK(omci_me_action_allowed(absent[i], 4));
	}
}

static void test_the_creators_and_the_actions_agree(void)
{
	for (unsigned int i = 0; i < tsv_class_count; i++) {
		const struct tsv_class *t = &tsv_classes[i];

		CHECK((t->actions & (1u << 9)) != 0u);
		if (t->by_olt) {
			CHECK((t->actions & (1u << 4)) != 0u);
			CHECK((t->actions & (1u << 6)) != 0u);
		}

		for (unsigned int n = 1; n <= t->attr_count; n++)
			if (t->attrs[n - 1].create)
				CHECK((t->actions & (1u << 4)) != 0u);
	}
}

int main(int argc, char **argv)
{
	if (!tsv_open(argc, argv)) {
		printf("test_model: no table to read\n");
		return 1;
	}

	RUN(test_the_classes_are_the_data_classes);
	RUN(test_every_attribute_is_found_under_its_number);
	RUN(test_the_tables);
	RUN(test_the_row_layout_is_the_sizes_of_the_attributes_that_are_not_tables);
	RUN(test_the_classes_are_in_ascending_order_and_nothing_else_is_in);
	RUN(test_the_creators_and_the_actions_agree);

	return OMCI_T_REPORT("test_model");
}
