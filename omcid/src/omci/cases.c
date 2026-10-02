// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The case sets of cases.h. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "me/table.h"
#include "me/upload.h"
#include "mib/store.h"
#include "omci/attrmask.h"
#include "omcid/array_size.h"
#include "omcid/byteorder.h"
#include "omci/frame.h"
#include "omci/cases.h"

#define BASE_COUNT 3u

/* A rule of the extended VLAN tagging operation table, for base frame 1. */
static const uint8_t me171_rule[16] = {
	0xff, 0xff, 0xf8, 0x00, 0x0f, 0xff, 0xe0, 0x00,
	0x40, 0x00, 0x00, 0x00, 0x00, 0x0b, 0xb8, 0x00,
};

/*
 * The valid frames the axes start from: 0 is a Get of every attribute of
 * an instance of class 257, 1 a Set of a VLAN rule on class 171, and 2 a
 * MIB upload next on the ONU data entity.
 */
static void base_frame(unsigned int which, struct omci_frame *f)
{
	memset(f, 0, sizeof(*f));
	f->device_id = OMCI_DEVICE_ID_BASELINE;
	f->ar = true;

	switch (which) {
	case 0:
		f->tci = 0x0101;
		f->msg_type = 9u;
		f->class_id = 257u;
		omci_be16_store(&f->contents[0], 0xFFFFu);
		break;

	case 1:
		f->tci = 0x0202;
		f->msg_type = 8u;
		f->class_id = 171u;
		f->instance = 0x0102u;
		omci_be16_store(&f->contents[0], 0x0400u);
		memcpy(&f->contents[2], me171_rule, sizeof(me171_rule));
		break;

	default:
		f->tci = 0x0303;
		f->msg_type = 14u;

		f->class_id = 2u;
		omci_be16_store(&f->contents[0], 0u);
		break;
	}
}

/* The sizes of the groups of the envelope axis, in their order. */
#define ENV_BYTE2 256u
#define ENV_DEVID 256u
#define ENV_TRAILER (4u * 256u)
#define ENV_MIC 33u
#define ENV_TCI 21u

#define ENV_CLASS_EXTRA 3u
#define ENV_CLASS_UNREG 4u

static uint32_t env_class_count(void)
{
	return (uint32_t)omci_me_class_count + ENV_CLASS_EXTRA;
}

static uint32_t env_count(void)
{
	return ENV_BYTE2 + ENV_DEVID + ENV_TRAILER + ENV_MIC + ENV_TCI +
	       env_class_count() + ENV_CLASS_UNREG;
}

/* Class identifiers that no class of the standard uses. */
static const uint16_t g_env_unreg[ENV_CLASS_UNREG] = { 0x0001u, 0x8000u,
						       0xEA60u, 0xFFFDu };

static uint16_t env_unreg_id(uint32_t off)
{
	return g_env_unreg[off];
}

/* The group of envelope case @i, and the case's place within it. */
static void env_group(uint32_t i, unsigned int *group, uint32_t *off)
{
	const uint32_t start[7] = {
		0,
		ENV_BYTE2,
		ENV_BYTE2 + ENV_DEVID,
		ENV_BYTE2 + ENV_DEVID + ENV_TRAILER,
		ENV_BYTE2 + ENV_DEVID + ENV_TRAILER + ENV_MIC,
		ENV_BYTE2 + ENV_DEVID + ENV_TRAILER + ENV_MIC + ENV_TCI,
		ENV_BYTE2 + ENV_DEVID + ENV_TRAILER + ENV_MIC + ENV_TCI +
			env_class_count(),
	};

	for (unsigned int g = 7; g-- > 0;) {
		if (i >= start[g]) {
			*group = g;
			*off = i - start[g];
			return;
		}
	}
}

static void env_build(uint32_t i, uint8_t wire[static OMCI_FRAME_LEN])
{
	struct omci_frame f;

	base_frame(0, &f);
	(void)!omci_frame_build(&f, wire);

	unsigned int g = 0;
	uint32_t off = 0;

	env_group(i, &g, &off);

	switch (g) {
	case 0:
		wire[2] = (uint8_t)off;
		break;

	case 1:
		wire[3] = (uint8_t)off;
		break;

	case 2:
		wire[40u + (off / 256u)] = (uint8_t)(off % 256u);
		break;

	case 3:
		if (off > 0) {
			const uint32_t bit = off - 1u;

			wire[44u + (bit / 8u)] ^= (uint8_t)(1u << (bit % 8u));
		}
		return;

	case 4: {
		uint16_t tci;

		if (off < 16u)
			tci = (uint16_t)(1u << off);
		else {
			static const uint16_t edge[5] = { 0x0000u, 0x0001u,
							  0x7FFEu, 0x7FFFu,
							  0xFFFFu };
			tci = edge[off - 16u];
		}
		f.priority = (tci & 0x8000u) != 0u;
		f.tci = (uint16_t)(tci & 0x7FFFu);
		(void)!omci_frame_build(&f, wire);
		return;
	}

	case 5:
		if (off < (uint32_t)omci_me_class_count)
			f.class_id = omci_me_classes[off].class_id;
		else {
			static const uint16_t extra[ENV_CLASS_EXTRA] = {
				0x0000u, 0xFFFFu, 0x0BB8u
			};
			f.class_id = extra[off - (uint32_t)omci_me_class_count];
		}
		(void)!omci_frame_build(&f, wire);
		return;

	default:
		f.class_id = env_unreg_id(off);
		(void)!omci_frame_build(&f, wire);
		return;
	}

	omci_be32_store(&wire[44], omci_frame_mic(wire, 44u));
}

static void env_label(uint32_t i, char *out, size_t cap)
{
	unsigned int g = 0;
	uint32_t off = 0;

	env_group(i, &g, &off);

	switch (g) {
	case 0:
		(void)snprintf(out, cap, "envelope/byte2=0x%02x",
			       (unsigned int)off);
		break;
	case 1:
		(void)snprintf(out, cap, "envelope/devid=0x%02x",
			       (unsigned int)off);
		break;
	case 2:
		(void)snprintf(out, cap, "envelope/trailer[%u]=0x%02x",
			       (unsigned int)(off / 256u),
			       (unsigned int)(off % 256u));
		break;
	case 3:
		if (off == 0)
			(void)snprintf(out, cap, "envelope/mic=correct");
		else
			(void)snprintf(out, cap, "envelope/mic-bit=%u",
				       (unsigned int)(off - 1u));
		break;
	case 4:
		(void)snprintf(out, cap, "envelope/tci#%u", (unsigned int)off);
		break;
	case 5: {
		uint16_t cls = 0xFFFFu;

		if (off < (uint32_t)omci_me_class_count)
			cls = omci_me_classes[off].class_id;
		else {
			static const uint16_t extra[ENV_CLASS_EXTRA] = {
				0x0000u, 0xFFFFu, 0x0BB8u
			};
			cls = extra[off - (uint32_t)omci_me_class_count];
		}
		(void)snprintf(out, cap, "envelope/class=%u",
			       (unsigned int)cls);
		break;
	}

	default:
		(void)snprintf(out, cap, "envelope/unreg-class=%u",
			       (unsigned int)env_unreg_id(off));
		break;
	}
}

/* The mutation axis: base frame, byte position and value, the value fastest. */
static uint32_t mut_count(void)
{
	return BASE_COUNT * OMCI_FRAME_LEN * 256u;
}

static void mut_decode(uint32_t i, unsigned int *base, unsigned int *pos,
		       unsigned int *val)
{
	*val = (unsigned int)(i % 256u);
	i /= 256u;
	*pos = (unsigned int)(i % OMCI_FRAME_LEN);
	i /= OMCI_FRAME_LEN;
	*base = (unsigned int)(i % BASE_COUNT);
}

static void mut_build(uint32_t i, uint8_t wire[static OMCI_FRAME_LEN])
{
	unsigned int base = 0, pos = 0, val = 0;

	mut_decode(i, &base, &pos, &val);

	struct omci_frame f;

	base_frame(base, &f);

	f.tci = (uint16_t)((i % 0x7FFFu) + 1u);

	(void)!omci_frame_build(&f, wire);
	wire[pos] = (uint8_t)val;

	if (pos < 44u)
		omci_be32_store(&wire[44], omci_frame_mic(wire, 44u));
}

static void mut_label(uint32_t i, char *out, size_t cap)
{
	unsigned int base = 0, pos = 0, val = 0;

	mut_decode(i, &base, &pos, &val);
	(void)snprintf(out, cap, "mutate/base%u[%u]=0x%02x", base, pos, val);
}

/* The message type axis: 32 types, 4 combinations of the request and answer bits, 2 classes. */
#define MT_CODES 32u
#define MT_FLAGS 4u
#define MT_CLASS 2u

static uint32_t mt_count(void)
{
	return MT_CODES * MT_FLAGS * MT_CLASS;
}

static void mt_decode(uint32_t i, unsigned int *mt, unsigned int *flags,
		      unsigned int *cls)
{
	*cls = (unsigned int)(i % MT_CLASS);
	i /= MT_CLASS;
	*flags = (unsigned int)(i % MT_FLAGS);
	i /= MT_FLAGS;
	*mt = (unsigned int)(i % MT_CODES);
}

static void mt_build(uint32_t i, uint8_t wire[static OMCI_FRAME_LEN])
{
	unsigned int mt = 0, flags = 0, cls = 0;

	mt_decode(i, &mt, &flags, &cls);

	struct omci_frame f;

	base_frame(0, &f);
	f.msg_type = (uint8_t)mt;
	f.ar = (flags & 1u) != 0u;
	f.ak = (flags & 2u) != 0u;
	f.class_id = cls == 0 ? 257u : 0x0BB8u;
	(void)!omci_frame_build(&f, wire);
}

static void mt_label(uint32_t i, char *out, size_t cap)
{
	unsigned int mt = 0, flags = 0, cls = 0;

	mt_decode(i, &mt, &flags, &cls);
	(void)snprintf(out, cap, "msgtype/mt=%u,ar=%u,ak=%u,%s", mt,
		       (flags & 1u) != 0u, (flags & 2u) != 0u,
		       cls == 0 ? "known" : "unknown");
}

static const struct {
	uint16_t class_id;
	uint16_t instance;
} g_mtclass[] = {
	{ 2u, 0x0000u },   { 5u, 0x0101u },    { 6u, 0x0101u },
	{ 7u, 0x0000u },   { 11u, 0x0101u },   { 134u, 0x0000u },
	{ 171u, 0x0102u }, { 256u, 0x0000u },  { 257u, 0x0000u },
	{ 262u, 0x8001u }, { 263u, 0x8001u },  { 322u, 0x0101u },
	{ 287u, 0x0000u }, { 3000u, 0x0000u },
};

#define MTC_CLASSES (ARRAY_SIZE(g_mtclass))
#define MTC_CHANNELS 2u

static uint32_t mtc_count(void)
{
	return MT_CODES * (uint32_t)MTC_CLASSES * MTC_CHANNELS;
}

static void mtc_decode(uint32_t i, unsigned int *mt, unsigned int *cls,
		       unsigned int *pri)
{
	*pri = (unsigned int)(i % MTC_CHANNELS);
	i /= MTC_CHANNELS;
	*cls = (unsigned int)(i % MTC_CLASSES);
	i /= MTC_CLASSES;
	*mt = (unsigned int)(i % MT_CODES);
}

static void mtc_build(uint32_t i, uint8_t wire[static OMCI_FRAME_LEN])
{
	unsigned int mt = 0, cls = 0, pri = 0;

	mtc_decode(i, &mt, &cls, &pri);

	struct omci_frame f;

	base_frame(0, &f);
	f.msg_type = (uint8_t)mt;
	f.class_id = g_mtclass[cls].class_id;
	f.instance = g_mtclass[cls].instance;
	f.priority = pri != 0u;

	f.ar = true;
	f.ak = false;

	if (mt == 18u)
		f.contents[0] = 7u;

	(void)!omci_frame_build(&f, wire);
}

static void mtc_label(uint32_t i, char *out, size_t cap)
{
	unsigned int mt = 0, cls = 0, pri = 0;

	mtc_decode(i, &mt, &cls, &pri);
	(void)snprintf(out, cap, "mtclass/mt=%u,class=%u,inst=%04x,%s", mt,
		       g_mtclass[cls].class_id, g_mtclass[cls].instance,
		       pri ? "pri" : "ord");
}

/* Bounds on the cases made from a MIB, from its rows and its classes. */
#define MIB_CLASSES_MAX 512u
#define REACH_MAX (OMCI_MIB_MAX_ROWS * 66u + MIB_CLASSES_MAX * 50u + 8863u)
#define POPULATED_MAX (OMCI_MIB_MAX_ROWS * 97u)

/* What a case made from a MIB exercises; it decides the label. */
enum mkind {
	K_SET,
	K_SET_AVC,
	K_SETAGAIN,
	K_SETBACK,
	K_CREATE,
	K_TEST_ABSENT,
	K_TEST_PRESENT,
	K_GET,
	K_GET_ABSENT,
	K_TABLE_GET,
	K_TABLE_NEXT,
	K_UPLOAD_START,
	K_UPLOAD_NEXT,
	K_UPLOAD_PAST,
	K_MIBRESET,
	K_SWDL_START,
	K_SWDL_SECTION_AR,
	K_SWDL_SECTION,
	K_SWDL_END,
	K_SWDL_ACTIVATE,
	K_SWDL_COMMIT,
	K_SWDL_REBOOT,
	K_SYNC,
	K_GETALL,
	K_GETPART,
	K_SETGET_SET,
	K_SETGET_GET,
};

/* A case made from a MIB: the frame, its kind and what it is about. */
struct mcase {
	uint8_t wire[OMCI_FRAME_LEN];
	uint8_t kind;
	uint8_t attr;
	uint16_t arg;
};

/* A list of cases, and the next transaction identifier to give out. */
struct mset {
	struct mcase *c;
	uint32_t n;
	uint16_t tid;
};

static struct mcase g_reach[REACH_MAX];
static struct mcase g_pop[POPULATED_MAX];
static struct mset g_reach_set = { g_reach, 0, 1 };
static struct mset g_pop_set = { g_pop, 0, 1 };

enum {
	MT_CREATE = 4,
	MT_SET = 8,
	MT_GET = 9,
	MT_MIB_UPLOAD = 13,
	MT_MIB_UPLOAD_NEXT = 14,
	MT_MIB_RESET = 15,
	MT_TEST = 18,
	MT_START = 19,
	MT_SECTION = 20,
	MT_END = 21,
	MT_ACTIVATE = 22,
	MT_COMMIT = 23,
	MT_SYNC = 24,
	MT_REBOOT = 25,
	MT_GET_NEXT = 26,
};

static void madd(struct mset *s, bool ar, uint8_t mt, uint16_t cls,
		 uint16_t inst, const uint8_t *contents, size_t n, uint8_t kind,
		 uint8_t attr, uint16_t arg)
{
	struct mcase *c = &s->c[s->n++];
	uint8_t *w = c->wire;

	memset(w, 0, OMCI_FRAME_LEN);
	omci_be16_store(&w[0], s->tid++);
	w[2] = (uint8_t)((ar ? 0x40u : 0u) | mt);
	w[3] = OMCI_DEVICE_ID_BASELINE;
	omci_be16_store(&w[4], cls);
	omci_be16_store(&w[6], inst);
	if (n > 0u)
		memcpy(&w[8], contents, n);
	omci_be32_store(&w[40], OMCI_TRAILER);
	omci_be32_store(&w[44], omci_frame_mic(w, 44));
	c->kind = kind;
	c->attr = attr;
	c->arg = arg;
}

static struct {
	uint16_t cls;
	uint16_t id;
} g_minst[OMCI_MIB_MAX_ROWS];
static unsigned int g_minst_n;

static void list_instances(const struct omci_mib *mib)
{
	struct omci_mib_cursor cur;

	g_minst_n = 0;
	for (enum omci_status rc = omci_mib_walk_first(mib, &cur);
	     rc == OMCI_OK; rc = omci_mib_walk_next(mib, &cur)) {
		g_minst[g_minst_n].cls = cur.class_id;
		g_minst[g_minst_n].id = cur.instance;
		g_minst_n++;
	}
}

static bool accepts(const struct omci_me_class *cls, unsigned int mt)
{
	return (cls->actions & (1u << mt)) != 0u;
}

static bool value_writable(const struct omci_me_attr *a)
{
	return (a->access & OMCI_ME_ACC_WRITE) != 0u &&
	       !omci_me_attr_is_table(a) && a->size <= 30u;
}

static void current(const struct omci_mib *mib, unsigned int k, uint8_t attr,
		    uint8_t cur[static 64])
{
	size_t got = 0;
	(void)!omci_mib_attr_get(mib, g_minst[k].cls, g_minst[k].id, attr, cur,
				 64, &got);
}

static void table_pair(struct mset *s, uint16_t cls, uint16_t inst,
		       uint8_t attr)
{
	const uint16_t bit = omci_attr_bit(attr);
	const uint8_t get[2] = { (uint8_t)(bit >> 8), (uint8_t)bit };

	madd(s, true, MT_GET, cls, inst, get, sizeof(get), K_TABLE_GET, attr,
	     0);
	for (unsigned int seq = 0; seq < 2; seq++) {
		const uint8_t next[4] = { (uint8_t)(bit >> 8), (uint8_t)bit, 0,
					  (uint8_t)seq };
		madd(s, true, MT_GET_NEXT, cls, inst, next, sizeof(next),
		     K_TABLE_NEXT, attr, (uint16_t)seq);
	}
}

static void reach_sets(struct mset *s, const struct omci_mib *mib)
{
	for (unsigned int k = 0; k < g_minst_n; k++) {
		const struct omci_me_class *cls =
			omci_me_class_by_id(g_minst[k].cls);
		for (uint8_t attr = 1; attr <= cls->attr_count; attr++) {
			const struct omci_me_attr *a =
				omci_me_attr_get(cls, attr);
			if (!value_writable(a))
				continue;
			uint8_t cur[64];

			current(mib, k, attr, cur);
			uint8_t body[32];
			const uint16_t bit = omci_attr_bit(attr);
			const bool avc = (a->flags & OMCI_ME_ATTR_AVC) != 0u;

			body[0] = (uint8_t)(bit >> 8);
			body[1] = (uint8_t)bit;
			for (size_t i = 0; i < a->size; i++)
				body[2 + i] = (uint8_t)(cur[i] ^ 0x5Au);
			madd(s, true, MT_SET, g_minst[k].cls, g_minst[k].id,
			     body, 2u + a->size, avc ? K_SET_AVC : K_SET, attr,
			     0);
			if (avc) {
				madd(s, true, MT_SET, g_minst[k].cls,
				     g_minst[k].id, body, 2u + a->size,
				     K_SETAGAIN, attr, 0);
				for (size_t i = 0; i < a->size; i++)
					body[2 + i] = cur[i];
				madd(s, true, MT_SET, g_minst[k].cls,
				     g_minst[k].id, body, 2u + a->size,
				     K_SETBACK, attr, 0);
			}
		}
	}
}

static void reach_creates(struct mset *s)
{
	for (size_t k = 0; k < omci_me_class_count; k++) {
		const struct omci_me_class *cls = &omci_me_classes[k];

		if (!accepts(cls, MT_CREATE))
			continue;

		uint8_t body[32] = { 0 };
		size_t n = 0;

		for (uint8_t attr = 1; attr <= cls->attr_count; attr++) {
			const struct omci_me_attr *a =
				omci_me_attr_get(cls, attr);
			if ((a->access & OMCI_ME_ACC_SET_BY_CREATE) != 0u)
				n += a->size;
		}
		madd(s, true, MT_CREATE, cls->class_id, 0x7C01u, body, n,
		     K_CREATE, 0, 0);
	}
}

static void reach_tests(struct mset *s)
{
	for (size_t k = 0; k < omci_me_class_count; k++)
		madd(s, true, MT_TEST, omci_me_classes[k].class_id, 0x7FFEu,
		     NULL, 0, K_TEST_ABSENT, 0, 0);
	for (unsigned int k = 0; k < g_minst_n; k++) {
		const uint8_t body[1] = { g_minst[k].cls == 263u ? 7u : 0u };

		madd(s, true, MT_TEST, g_minst[k].cls, g_minst[k].id, body,
		     sizeof(body), K_TEST_PRESENT, 0, 0);
	}
}

static void reach_gets(struct mset *s)
{
	for (unsigned int k = 0; k < g_minst_n; k++) {
		const uint8_t body[2] = { 0xFF, 0xFF };

		madd(s, true, MT_GET, g_minst[k].cls, g_minst[k].id, body,
		     sizeof(body), K_GET, 0, 0);
	}

	static const uint16_t absent[] = { 24, 133, 329 };

	for (size_t k = 0; k < ARRAY_SIZE(absent); k++) {
		const uint8_t body[2] = { 0xFF, 0xFF };

		madd(s, true, MT_GET, absent[k], 0, body, sizeof(body),
		     K_GET_ABSENT, 0, 0);
	}
}

static void reach_tables(struct mset *s)
{
	for (unsigned int k = 0; k < g_minst_n; k++) {
		const struct omci_me_class *cls =
			omci_me_class_by_id(g_minst[k].cls);
		for (uint8_t attr = 1; attr <= cls->attr_count; attr++)
			if (omci_me_attr_is_table(omci_me_attr_get(cls, attr)))
				table_pair(s, g_minst[k].cls, g_minst[k].id,
					   attr);
	}

	for (size_t k = 0; k < omci_me_class_count; k++) {
		const struct omci_me_class *cls = &omci_me_classes[k];

		if (!accepts(cls, MT_CREATE))
			continue;
		for (uint8_t attr = 1; attr <= cls->attr_count; attr++)
			if (omci_me_attr_is_table(omci_me_attr_get(cls, attr)))
				table_pair(s, cls->class_id, 0x7C01u, attr);
	}
}

static void reach_upload(struct mset *s)
{
	unsigned int units = 0;

	for (unsigned int k = 0; k < g_minst_n; k++) {
		const struct omci_me_class *cls =
			omci_me_class_by_id(g_minst[k].cls);
		if (omci_me_upload_includes(cls))
			units += omci_me_upload_segments(cls);
	}
	madd(s, true, MT_MIB_UPLOAD, 2, 0, NULL, 0, K_UPLOAD_START, 0,
	     (uint16_t)units);
	for (unsigned int seq = 0; seq < units; seq++) {
		const uint8_t body[2] = { (uint8_t)(seq >> 8), (uint8_t)seq };

		madd(s, true, MT_MIB_UPLOAD_NEXT, 2, 0, body, sizeof(body),
		     K_UPLOAD_NEXT, 0, (uint16_t)seq);
	}
	const unsigned int past[3] = { units, units + 1u, 0xFFFFu };

	for (unsigned int k = 0; k < 3; k++) {
		const uint8_t body[2] = { (uint8_t)(past[k] >> 8),
					  (uint8_t)past[k] };
		madd(s, true, MT_MIB_UPLOAD_NEXT, 2, 0, body, sizeof(body),
		     K_UPLOAD_PAST, 0, (uint16_t)past[k]);
	}
}

static const struct {
	uint16_t cls, inst;
	const char *what;
} g_swdl_to[] = {
	{ 7, 0, "image0" }, { 7, 1, "image1" },	    { 7, 9, "absent" },
	{ 256, 0, "onug" }, { 3000, 0, "noclass" },
};

static void reach_swdl(struct mset *s)
{
	for (uint8_t k = 0; k < ARRAY_SIZE(g_swdl_to); k++) {
		const uint16_t cls = g_swdl_to[k].cls;
		const uint16_t inst = g_swdl_to[k].inst;
		const uint8_t start[8] = { 0x1F, 0x00, 0x00, 0x01,
					   0x00, 0x01, 0x00, (uint8_t)inst };
		const uint8_t end[9] = { 0x12, 0x34, 0x56, 0x78, 0x00,
					 0x00, 0x01, 0x00, 0x01 };
		const uint8_t sect[32] = { 0x00, 0xAA, 0xBB, 0xCC };
		const uint8_t act[1] = { 0x00 };
		const uint8_t rbt[1] = { 0x00 };

		madd(s, true, MT_START, cls, inst, start, sizeof(start),
		     K_SWDL_START, k, 0);
		madd(s, true, MT_SECTION, cls, inst, sect, sizeof(sect),
		     K_SWDL_SECTION_AR, k, 0);
		madd(s, false, MT_SECTION, cls, inst, sect, sizeof(sect),
		     K_SWDL_SECTION, k, 0);
		madd(s, true, MT_END, cls, inst, end, sizeof(end), K_SWDL_END,
		     k, 0);
		madd(s, true, MT_ACTIVATE, cls, inst, act, sizeof(act),
		     K_SWDL_ACTIVATE, k, 0);
		madd(s, true, MT_COMMIT, cls, inst, NULL, 0, K_SWDL_COMMIT, k,
		     0);
		madd(s, true, MT_REBOOT, cls, inst, rbt, sizeof(rbt),
		     K_SWDL_REBOOT, k, 0);
	}
}

static void reach_synctime(struct mset *s)
{
	static const unsigned int years[] = { 0,    1969, 1970, 2024,
					      2200, 2201, 65535 };
	static const unsigned int months[] = { 0, 1, 2, 12, 13 };
	static const unsigned int days[] = { 0, 1, 28, 29, 30, 31, 32 };
	static const unsigned int hours[] = { 0, 23, 24 };
	static const unsigned int mins[] = { 0, 59, 60 };
	static const unsigned int secs[] = { 0, 59, 60, 61 };

	for (size_t a = 0; a < ARRAY_SIZE(years); a++)
		for (size_t b = 0; b < ARRAY_SIZE(months); b++)
			for (size_t c = 0; c < ARRAY_SIZE(days); c++)
				for (size_t d = 0; d < ARRAY_SIZE(hours); d++)
					for (size_t e = 0; e < ARRAY_SIZE(mins);
					     e++)
						for (size_t f = 0;
						     f < ARRAY_SIZE(secs);
						     f++) {
							const uint8_t body[7] = {
								(uint8_t)(years[a] >> 8),
								(uint8_t)years[a],
								(uint8_t)months[b],
								(uint8_t)days[c],
								(uint8_t)hours[d],
								(uint8_t)mins[e],
								(uint8_t)secs[f]
							};
							madd(s, true, MT_SYNC,
							     256, 0, body,
							     sizeof(body),
							     K_SYNC, 0, 0);
						}
}

static void reach_prepare(const struct omci_mib *mib)
{
	struct mset *s = &g_reach_set;

	s->n = 0;
	s->tid = 1;
	list_instances(mib);
	reach_sets(s, mib);
	reach_creates(s);
	reach_tests(s);
	reach_gets(s);
	reach_tables(s);
	reach_upload(s);
	madd(s, true, MT_MIB_RESET, 2, 0, NULL, 0, K_MIBRESET, 0, 0);
	reach_swdl(s);
	reach_synctime(s);
}

static uint16_t class_mask(const struct omci_me_class *cls)
{
	uint16_t mask = 0;

	for (uint8_t attr = 1; attr <= cls->attr_count; attr++)
		mask = (uint16_t)(mask | omci_attr_bit(attr));
	return mask;
}

static unsigned int reply_bytes(const struct omci_me_attr *a)
{
	return omci_me_attr_is_table(a) ? 4u : a->size;
}

static void populated_prepare(const struct omci_mib *mib)
{
	struct mset *s = &g_pop_set;

	s->n = 0;
	s->tid = 1;
	list_instances(mib);

	for (unsigned int k = 0; k < g_minst_n; k++) {
		const uint16_t mask =
			class_mask(omci_me_class_by_id(g_minst[k].cls));
		const uint8_t body[2] = { (uint8_t)(mask >> 8), (uint8_t)mask };

		madd(s, true, MT_GET, g_minst[k].cls, g_minst[k].id, body,
		     sizeof(body), K_GETALL, 0, 0);
	}

	for (unsigned int k = 0; k < g_minst_n; k++) {
		const struct omci_me_class *cls =
			omci_me_class_by_id(g_minst[k].cls);
		uint16_t mask = 0;
		unsigned int bytes = 0;
		uint16_t piece = 0;

		for (uint8_t attr = 1; attr <= cls->attr_count; attr++) {
			const unsigned int need =
				reply_bytes(omci_me_attr_get(cls, attr));
			if (mask != 0u && bytes + need > 25u) {
				const uint8_t body[2] = { (uint8_t)(mask >> 8),
							  (uint8_t)mask };
				madd(s, true, MT_GET, g_minst[k].cls,
				     g_minst[k].id, body, sizeof(body),
				     K_GETPART, 0, piece++);
				mask = 0;
				bytes = 0;
			}
			mask = (uint16_t)(mask | omci_attr_bit(attr));
			bytes += need;
		}
		const uint8_t body[2] = { (uint8_t)(mask >> 8), (uint8_t)mask };

		madd(s, true, MT_GET, g_minst[k].cls, g_minst[k].id, body,
		     sizeof(body), K_GETPART, 0, piece);
	}

	for (unsigned int k = 0; k < g_minst_n; k++) {
		const struct omci_me_class *cls =
			omci_me_class_by_id(g_minst[k].cls);
		for (uint8_t attr = 1; attr <= cls->attr_count; attr++) {
			const struct omci_me_attr *a =
				omci_me_attr_get(cls, attr);
			if (!value_writable(a))
				continue;
			uint8_t cur[64];

			current(mib, k, attr, cur);
			uint8_t body[32];
			const uint16_t bit = omci_attr_bit(attr);

			body[0] = (uint8_t)(bit >> 8);
			body[1] = (uint8_t)bit;
			for (size_t i = 0; i < a->size; i++)
				body[2 + i] = (uint8_t)(cur[i] ^ 0x5Au);
			madd(s, true, MT_SET, g_minst[k].cls, g_minst[k].id,
			     body, 2u + a->size, K_SETGET_SET, attr, 0);
			madd(s, true, MT_GET, g_minst[k].cls, g_minst[k].id,
			     body, 2, K_SETGET_GET, attr, 0);
		}
	}

	for (unsigned int k = 0; k < g_minst_n; k++) {
		const struct omci_me_class *cls =
			omci_me_class_by_id(g_minst[k].cls);
		for (uint8_t attr = 1; attr <= cls->attr_count; attr++)
			if (omci_me_attr_is_table(omci_me_attr_get(cls, attr)))
				table_pair(s, g_minst[k].cls, g_minst[k].id,
					   attr);
	}
}

void omci_case_prepare(enum omci_case_axis axis, const struct omci_mib *mib)
{
	if (axis == OMCI_CASE_REACH)
		reach_prepare(mib);
	else if (axis == OMCI_CASE_POPULATED)
		populated_prepare(mib);
}

bool omci_case_axis_populated(enum omci_case_axis axis)
{
	return axis == OMCI_CASE_REACH || axis == OMCI_CASE_POPULATED;
}

uint32_t omci_case_capacity(enum omci_case_axis axis)
{
	if (axis == OMCI_CASE_REACH)
		return REACH_MAX;
	return axis == OMCI_CASE_POPULATED ? POPULATED_MAX : 0u;
}

static const struct mset *mset_of(enum omci_case_axis axis)
{
	return axis == OMCI_CASE_REACH ? &g_reach_set : &g_pop_set;
}

static void mlabel(const struct mcase *c, char *out, size_t cap)
{
	const unsigned int cls = omci_be16(&c->wire[4]);
	const unsigned int inst = omci_be16(&c->wire[6]);
	const char *what =
		c->attr < ARRAY_SIZE(g_swdl_to) ? g_swdl_to[c->attr].what : "?";

	switch ((enum mkind)c->kind) {
	case K_SET:
		(void)snprintf(out, cap, "set/%u/%04x/attr%u", cls, inst,
			       c->attr);
		break;
	case K_SET_AVC:
		(void)snprintf(out, cap, "set/%u/%04x/attr%u/avc", cls, inst,
			       c->attr);
		break;
	case K_SETAGAIN:
		(void)snprintf(out, cap, "setagain/%u/%04x/attr%u/avc", cls,
			       inst, c->attr);
		break;
	case K_SETBACK:
		(void)snprintf(out, cap, "setback/%u/%04x/attr%u/avc", cls,
			       inst, c->attr);
		break;
	case K_CREATE:
		(void)snprintf(out, cap, "create/%u/7c01", cls);
		break;
	case K_TEST_ABSENT:
		(void)snprintf(out, cap, "test/%u/7ffe/absent", cls);
		break;
	case K_TEST_PRESENT:
		(void)snprintf(out, cap, "test/%u/%04x/present", cls, inst);
		break;
	case K_GET:
		(void)snprintf(out, cap, "get/%u/%04x", cls, inst);
		break;
	case K_GET_ABSENT:
		(void)snprintf(out, cap, "get/%u/0000", cls);
		break;
	case K_TABLE_GET:
		(void)snprintf(out, cap, "table/%u/%04x/attr%u/get", cls, inst,
			       c->attr);
		break;
	case K_TABLE_NEXT:
		(void)snprintf(out, cap, "table/%u/%04x/attr%u/next%u", cls,
			       inst, c->attr, c->arg);
		break;
	case K_UPLOAD_START:
		(void)snprintf(out, cap, "upload/start/%u", c->arg);
		break;
	case K_UPLOAD_NEXT:
		(void)snprintf(out, cap, "upload/next/%u", c->arg);
		break;
	case K_UPLOAD_PAST:
		(void)snprintf(out, cap, "upload/next/%u/past", c->arg);
		break;
	case K_MIBRESET:
		(void)snprintf(out, cap, "mibreset/2/0000");
		break;
	case K_SWDL_START:
		(void)snprintf(out, cap, "swdl/start/%s", what);
		break;
	case K_SWDL_SECTION_AR:
		(void)snprintf(out, cap, "swdl/section-ar/%s", what);
		break;
	case K_SWDL_SECTION:
		(void)snprintf(out, cap, "swdl/section/%s", what);
		break;
	case K_SWDL_END:
		(void)snprintf(out, cap, "swdl/end/%s", what);
		break;
	case K_SWDL_ACTIVATE:
		(void)snprintf(out, cap, "swdl/activate/%s", what);
		break;
	case K_SWDL_COMMIT:
		(void)snprintf(out, cap, "swdl/commit/%s", what);
		break;
	case K_SWDL_REBOOT:
		(void)snprintf(out, cap, "swdl/reboot/%s", what);
		break;
	case K_SYNC:
		(void)snprintf(out, cap,
			       "synctime/%04u-%02u-%02u/%02u:%02u:%02u",
			       (unsigned int)omci_be16(&c->wire[8]),
			       c->wire[10], c->wire[11], c->wire[12],
			       c->wire[13], c->wire[14]);
		break;
	case K_GETALL:
		(void)snprintf(out, cap, "getall/%u/%04x", cls, inst);
		break;
	case K_GETPART:
		(void)snprintf(out, cap, "getpart/%u/%04x/piece%u", cls, inst,
			       c->arg);
		break;
	case K_SETGET_SET:
		(void)snprintf(out, cap, "setget/%u/%04x/attr%u/set", cls, inst,
			       c->attr);
		break;
	case K_SETGET_GET:
		(void)snprintf(out, cap, "setget/%u/%04x/attr%u/get", cls, inst,
			       c->attr);
		break;
	}
}

uint32_t omci_case_count(enum omci_case_axis axis)
{
	switch (axis) {
	case OMCI_CASE_ENVELOPE:
		return env_count();
	case OMCI_CASE_MUTATE:
		return mut_count();
	case OMCI_CASE_MSGTYPE:
		return mt_count();
	case OMCI_CASE_MTCLASS:
		return mtc_count();
	case OMCI_CASE_REACH:
		return g_reach_set.n;
	case OMCI_CASE_POPULATED:
		return g_pop_set.n;
	default:
		return 0;
	}
}

bool omci_case_build(enum omci_case_axis axis, uint32_t index,
		     uint8_t wire[static OMCI_FRAME_LEN])
{
	const uint32_t n = omci_case_count(axis);

	if (n == 0 || index >= n) {
		struct omci_frame f;

		base_frame(0, &f);
		(void)!omci_frame_build(&f, wire);
		return false;
	}

	switch (axis) {
	case OMCI_CASE_ENVELOPE:
		env_build(index, wire);
		break;
	case OMCI_CASE_MUTATE:
		mut_build(index, wire);
		break;
	case OMCI_CASE_MSGTYPE:
		mt_build(index, wire);
		break;
	case OMCI_CASE_REACH:
	case OMCI_CASE_POPULATED:
		memcpy(wire, mset_of(axis)->c[index].wire, OMCI_FRAME_LEN);
		break;
	case OMCI_CASE_MTCLASS:
	default:
		mtc_build(index, wire);
		break;
	}
	return true;
}

void omci_case_label(enum omci_case_axis axis, uint32_t index, char *out,
		     size_t cap)
{
	if (out == NULL || cap == 0)
		return;
	out[0] = '\0';

	if (index >= omci_case_count(axis))
		return;

	switch (axis) {
	case OMCI_CASE_ENVELOPE:
		env_label(index, out, cap);
		break;
	case OMCI_CASE_MUTATE:
		mut_label(index, out, cap);
		break;
	case OMCI_CASE_MSGTYPE:
		mt_label(index, out, cap);
		break;
	case OMCI_CASE_REACH:
	case OMCI_CASE_POPULATED:
		mlabel(&mset_of(axis)->c[index], out, cap);
		break;
	case OMCI_CASE_MTCLASS:
	default:
		mtc_label(index, out, cap);
		break;
	}
}

static const char *const g_axis_name[OMCI_CASE_AXIS_COUNT] = {
	[OMCI_CASE_ENVELOPE] = "envelope", [OMCI_CASE_MUTATE] = "mutate",
	[OMCI_CASE_MSGTYPE] = "msgtype",   [OMCI_CASE_MTCLASS] = "mtclass",
	[OMCI_CASE_REACH] = "reach",	   [OMCI_CASE_POPULATED] = "populated",
};

const char *omci_case_axis_name(enum omci_case_axis axis)
{
	return axis < OMCI_CASE_AXIS_COUNT ? g_axis_name[axis] : NULL;
}

enum omci_case_axis omci_case_axis_by_name(const char *name)
{
	if (name == NULL)
		return OMCI_CASE_AXIS_COUNT;
	for (unsigned int a = 0; a < OMCI_CASE_AXIS_COUNT; a++)
		if (strcmp(name, g_axis_name[a]) == 0)
			return (enum omci_case_axis)a;
	return OMCI_CASE_AXIS_COUNT;
}
