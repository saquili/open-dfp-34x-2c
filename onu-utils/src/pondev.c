// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The messages of the "pon" family, read and printed (pondev.h). */

#include <errno.h>
#include <string.h>

#include <libmnl/libmnl.h>
#include <linux/genetlink.h>

#include "pondev.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* A 32-bit attribute, or -EINVAL when it has another length. */
static int get_u32(const struct nlattr *a, uint32_t *v)
{
	if (mnl_attr_get_payload_len(a) != sizeof(uint32_t))
		return -EINVAL;
	*v = mnl_attr_get_u32(a);
	return 0;
}

/* A counter of the kernel's variable-width type: 32 or 64 bits. */
static int get_uint(const struct nlattr *a, uint64_t *v)
{
	switch (mnl_attr_get_payload_len(a)) {
	case sizeof(uint32_t):
		*v = mnl_attr_get_u32(a);
		return 0;
	case sizeof(uint64_t):
		*v = mnl_attr_get_u64(a);
		return 0;
	default:
		return -EINVAL;
	}
}

static int parse_stats(const struct nlattr *nest, struct pon_dev *d)
{
	const struct nlattr *a;

	mnl_attr_for_each_nested(a, nest) {
		const uint16_t type = mnl_attr_get_type(a);

		if (type == 0 || type > PON_A_DEV_STATS_MAX)
			continue;
		if (get_uint(a, &d->stats[type]))
			return -EINVAL;
		d->stats_have |= 1u << type;
	}
	return 0;
}

int pon_dev_parse(const struct nlmsghdr *nlh, struct pon_dev *d)
{
	uint32_t *const field[PON_A_DEV_MAX + 1] = {
		[PON_A_DEV_ID] = &d->id,
		[PON_A_DEV_IFINDEX] = &d->ifindex,
		[PON_A_DEV_TYPE] = &d->type,
		[PON_A_DEV_ONU_STATE] = &d->state,
		[PON_A_DEV_ONU_ID] = &d->onu_id,
		[PON_A_DEV_ALLOC_ID] = &d->alloc_id,
		[PON_A_DEV_OMCC_PORT_ID] = &d->omcc,
		[PON_A_DEV_ALARMS] = &d->alarms,
		[PON_A_DEV_NUM_GEM_PORTS] = &d->gem_ports,
		[PON_A_DEV_NUM_TCONTS] = &d->tconts,
		[PON_A_DEV_NUM_TCONT_QUEUES] = &d->tcont_queues,
	};
	const struct nlattr *a;

	memset(d, 0, sizeof(*d));
	mnl_attr_for_each(a, nlh, sizeof(struct genlmsghdr)) {
		const uint16_t type = mnl_attr_get_type(a);

		if (type == 0 || type > PON_A_DEV_MAX)
			continue;
		if (field[type]) {
			if (get_u32(a, field[type]))
				return -EINVAL;
		} else if (type == PON_A_DEV_STATS) {
			if (parse_stats(a, d))
				return -EINVAL;
		} else if (type == PON_A_DEV_SERIAL_NUMBER) {
			if (mnl_attr_get_payload_len(a) != PON_SERIAL_LEN)
				return -EINVAL;
			memcpy(d->serial, mnl_attr_get_payload(a),
			       PON_SERIAL_LEN);
		} else {
			continue;
		}
		d->have |= 1u << type;
	}
	return 0;
}

int pon_tcont_parse(const struct nlmsghdr *nlh, struct pon_tcont *t)
{
	uint32_t *const field[PON_A_TCONT_MAX + 1] = {
		[PON_A_TCONT_ID] = &t->dev,
		[PON_A_TCONT_INDEX] = &t->index,
		[PON_A_TCONT_ALLOC_ID] = &t->alloc_id,
	};
	const struct nlattr *a;

	memset(t, 0, sizeof(*t));
	mnl_attr_for_each(a, nlh, sizeof(struct genlmsghdr)) {
		const uint16_t type = mnl_attr_get_type(a);

		if (type == 0 || type > PON_A_TCONT_MAX || !field[type])
			continue;
		if (get_u32(a, field[type]))
			return -EINVAL;
		t->have |= 1u << type;
	}
	return 0;
}

const char *pon_state_name(uint32_t state)
{
	static const char *const name[] = {
		[PON_ONU_STATE_O1] = "O1 initial",
		[PON_ONU_STATE_O2] = "O2 standby",
		[PON_ONU_STATE_O3] = "O3 serial number",
		[PON_ONU_STATE_O4] = "O4 ranging",
		[PON_ONU_STATE_O5] = "O5 operation",
		[PON_ONU_STATE_O6] = "O6 intermittent LODS",
		[PON_ONU_STATE_O7] = "O7 emergency stop",
	};

	if (state < ARRAY_SIZE(name) && name[state])
		return name[state];
	return "unknown";
}

static const char *type_name(uint32_t type)
{
	switch (type) {
	case PON_TYPE_GPON:
		return "GPON";
	case PON_TYPE_XGPON:
		return "XG-PON";
	case PON_TYPE_XGSPON:
		return "XGS-PON";
	case PON_TYPE_NGPON2:
		return "NG-PON2";
	default:
		return "PON";
	}
}

static bool has(const struct pon_dev *d, unsigned int attr)
{
	return d->have & (1u << attr);
}

static void print_u32(FILE *f, const struct pon_dev *d, unsigned int attr,
		      const char *label, uint32_t v)
{
	if (has(d, attr))
		fprintf(f, "  %s %u", label, v);
}

static void print_alarms(FILE *f, uint32_t alarms)
{
	fputs("  alarms", f);
	if (!alarms) {
		fputs(" none", f);
		return;
	}
	if (alarms & PON_ALARM_LOS)
		fputs(" LOS", f);
	if (alarms & PON_ALARM_LOF)
		fputs(" LOF", f);
	if (alarms & PON_ALARM_LOM)
		fputs(" LOM", f);
	if (alarms & PON_ALARM_PEE)
		fputs(" PEE", f);
	if (alarms & ~(uint32_t)(PON_ALARM_LOS | PON_ALARM_LOF | PON_ALARM_LOM |
				 PON_ALARM_PEE))
		fprintf(f, " 0x%x", alarms);
}

static void print_stats(FILE *f, const struct pon_dev *d)
{
	static const struct {
		unsigned int attr;
		const char *label;
	} counter[] = {
		{ PON_A_DEV_STATS_SN_REQUESTS, "serial number requests" },
		{ PON_A_DEV_STATS_RANGING_REQUESTS, "ranging requests" },
		{ PON_A_DEV_STATS_PLOAM_RX, "PLOAM messages" },
		{ PON_A_DEV_STATS_PLOAM_CRC_ERRORS, "PLOAM CRC errors" },
		{ PON_A_DEV_STATS_PLOAM_OVERFLOWS, "PLOAM overflows" },
		{ PON_A_DEV_STATS_BWMAP_RX, "bandwidth maps" },
		{ PON_A_DEV_STATS_BWMAP_CRC_ERRORS,
		  "bandwidth map CRC errors" },
		{ PON_A_DEV_STATS_BWMAP_OVERFLOWS, "bandwidth map overflows" },
		{ PON_A_DEV_STATS_GEM_FRAMES, "GEM frames" },
		{ PON_A_DEV_STATS_BIP_ERRORS, "BIP errors" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(counter); i++)
		if (d->stats_have & (1u << counter[i].attr))
			fprintf(f, "    %-26s %llu\n", counter[i].label,
				(unsigned long long)d->stats[counter[i].attr]);
}

void pon_dev_print(FILE *f, const struct pon_dev *d, const char *ifname,
		   bool serial)
{
	fprintf(f, "%s  %s", ifname && *ifname ? ifname : "pon?",
		type_name(d->type));
	if (has(d, PON_A_DEV_ONU_STATE))
		fprintf(f, "  %s", pon_state_name(d->state));
	print_u32(f, d, PON_A_DEV_ONU_ID, "ONU-ID", d->onu_id);
	print_u32(f, d, PON_A_DEV_ALLOC_ID, "Alloc-ID", d->alloc_id);
	print_u32(f, d, PON_A_DEV_OMCC_PORT_ID, "OMCC", d->omcc);
	fputc('\n', f);

	fputs("   ", f);
	if (has(d, PON_A_DEV_ALARMS))
		print_alarms(f, d->alarms);
	print_u32(f, d, PON_A_DEV_NUM_GEM_PORTS, "GEM ports up to", d->gem_ports);
	print_u32(f, d, PON_A_DEV_NUM_TCONTS, "T-CONTs up to", d->tconts);
	print_u32(f, d, PON_A_DEV_NUM_TCONT_QUEUES, "queues each",
		  d->tcont_queues);
	if (!has(d, PON_A_DEV_SERIAL_NUMBER)) {
		fputs("  serial number not set", f);
	} else if (!serial) {
		fputs("  serial number set (-s shows it)", f);
	} else {
		fputs("  serial number ", f);
		/* The vendor ID is four letters, the rest is binary. */
		for (unsigned int i = 0; i < 4; i++)
			fputc(d->serial[i] >= 0x20 && d->serial[i] < 0x7f ?
				      d->serial[i] :
				      '?',
			      f);
		for (unsigned int i = 4; i < PON_SERIAL_LEN; i++)
			fprintf(f, "%02X", d->serial[i]);
	}
	fputc('\n', f);
	print_stats(f, d);
}

void pon_tcont_print(FILE *f, const struct pon_tcont *t)
{
	if (t->have & 1u << PON_A_TCONT_ALLOC_ID)
		fprintf(f, "    T-CONT %u  Alloc-ID %u\n", t->index, t->alloc_id);
	else
		fprintf(f, "    T-CONT %u  idle\n", t->index);
}
