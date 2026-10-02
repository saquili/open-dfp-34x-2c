/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* Read data/g988-me-min.tsv, so that tests compare the daemon with the table it was made from. */

#ifndef OMCI_TEST_MODEL_TSV_H
#define OMCI_TEST_MODEL_TSV_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omcid/array_size.h"

#define TSV_MAX_CLASSES 40u
#define TSV_MAX_ATTRS 16u

struct tsv_attr {
	uint8_t no;
	char name[96];
	uint16_t size;
	bool read, write, create;
	bool mandatory, table, avc;
};

struct tsv_class {
	uint16_t id;
	char name[96];
	bool by_olt;
	uint32_t actions;
	uint32_t alarms;
	unsigned int attr_count;
	struct tsv_attr attrs[TSV_MAX_ATTRS];
};

static struct tsv_class tsv_classes[TSV_MAX_CLASSES];
static unsigned int tsv_class_count;

static inline const struct tsv_class *tsv_class(uint16_t id)
{
	for (unsigned int i = 0; i < tsv_class_count; i++)
		if (tsv_classes[i].id == id)
			return &tsv_classes[i];
	return NULL;
}

static inline unsigned int tsv_split(char *line, char **field, unsigned int max)
{
	unsigned int n = 0;

	field[n++] = line;
	for (char *p = line; *p != '\0' && n < max; p++)
		if (*p == '\t') {
			*p = '\0';
			field[n++] = p + 1;
		}
	return n;
}

static inline uint32_t tsv_mask(const char *list)
{
	uint32_t mask = 0;

	if (strcmp(list, "-") == 0)
		return 0;
	for (const char *p = list; *p != '\0';) {
		char *end = NULL;
		const unsigned long v = strtoul(p, &end, 10);

		if (end == p)
			break;
		mask |= (uint32_t)1u << v;
		p = (*end == ',' || *end == ':' || *end == ';') ? end + 1 : end;
		if (*end == ':')
			while (*p != '\0' && *p != ';')
				p++;
		if (*p == ';')
			p++;
	}
	return mask;
}

static inline bool tsv_has(const char *list, unsigned int no)
{
	return (tsv_mask(list) & ((uint32_t)1u << no)) != 0u;
}

static inline bool tsv_load(const char *path)
{
	FILE *fh = fopen(path, "r");
	char line[1024];
	char avc_of[TSV_MAX_CLASSES][64];
	unsigned int lineno = 0;

	if (fh == NULL) {
		printf("tsv: cannot open %s\n", path);
		return false;
	}
	tsv_class_count = 0;
	memset(tsv_classes, 0, sizeof(tsv_classes));
	memset(avc_of, 0, sizeof(avc_of));

	while (fgets(line, sizeof(line), fh) != NULL) {
		char *field[9];

		lineno++;
		line[strcspn(line, "\n")] = '\0';
		if (line[0] == '\0' || (line[0] == '#' && line[1] == ' '))
			continue;
		if (tsv_split(line, field, 9) != 9) {
			printf("tsv: line %u does not have 9 fields\n", lineno);
			fclose(fh);
			return false;
		}

		if (strcmp(field[0], "#class") == 0) {
			if (tsv_class_count >= TSV_MAX_CLASSES) {
				fclose(fh);
				return false;
			}
			struct tsv_class *c = &tsv_classes[tsv_class_count];

			c->id = (uint16_t)strtoul(field[1], NULL, 10);
			snprintf(c->name, sizeof(c->name), "%s", field[2]);
			c->by_olt = strcmp(field[3], "OLT") == 0;
			c->actions = tsv_mask(field[4]);
			c->alarms = tsv_mask(field[5]);
			snprintf(avc_of[tsv_class_count], sizeof(avc_of[0]),
				 "%s", field[6]);
			tsv_class_count++;
			continue;
		}

		struct tsv_class *c = NULL;
		const uint16_t id = (uint16_t)strtoul(field[0], NULL, 10);
		unsigned int idx = 0;

		for (; idx < tsv_class_count; idx++)
			if (tsv_classes[idx].id == id) {
				c = &tsv_classes[idx];
				break;
			}
		const unsigned int no =
			(unsigned int)strtoul(field[2], NULL, 10);
		if (c == NULL || no != c->attr_count + 1u ||
		    no > TSV_MAX_ATTRS) {
			printf("tsv: line %u: attribute %s of class %s is out of order\n",
			       lineno, field[2], field[0]);
			fclose(fh);
			return false;
		}

		struct tsv_attr *a = &c->attrs[c->attr_count++];

		a->no = (uint8_t)no;
		snprintf(a->name, sizeof(a->name), "%s", field[3]);
		a->size = (uint16_t)strtoul(field[4], NULL, 10);
		a->read = strchr(field[5], 'R') != NULL;
		a->write = strchr(field[5], 'W') != NULL;
		a->create = strchr(field[5], 'C') != NULL;
		a->mandatory = strcmp(field[6], "mandatory") == 0;
		a->table = strcmp(field[7], "yes") == 0;
		a->avc = tsv_has(avc_of[idx], no);
	}
	fclose(fh);
	return tsv_class_count > 0;
}

static inline bool tsv_open(int argc, char **argv)
{
	const char *const tried[] = { argc > 1 ? argv[1] : NULL,
				      getenv("OMCID_TSV"),
				      "data/g988-me-min.tsv",
				      "../data/g988-me-min.tsv" };

	for (unsigned int i = 0; i < ARRAY_SIZE(tried); i++) {
		FILE *probe = tried[i] != NULL ? fopen(tried[i], "r") : NULL;

		if (probe == NULL)
			continue;
		fclose(probe);
		return tsv_load(tried[i]);
	}
	printf("tsv: data/g988-me-min.tsv not found (pass its path as the first argument)\n");
	return false;
}

#endif
