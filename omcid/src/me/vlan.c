// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The VLAN rules of vlan.h. A rule is 16 bytes, four big-endian words: the
 * outer and inner tag filters, then the outer and inner tag treatments
 * (G.988 9.3.13).
 */

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "me/vlan.h"
#include "hw/hw.h"
#include "me/bridge.h"
#include "me/gem.h"
#include "mib/store.h"
#include "omcid/array_size.h"
#include "omcid/byteorder.h"
#include "sys/log.h"

/*
 * Special values of the fields: VID 4096 in a filter matches any VID;
 * priority 15 in a filter means no tag and in a treatment means add none;
 * priority 14 marks a default rule; priority 8 in a filter matches any
 * priority and in a treatment copies the inner tag's. Removing three tags
 * discards the frame.
 */
enum {
	VLAN_VID_WILDCARD = 4096,
	VLAN_PRI_NO_TAG = 15,
	VLAN_PRI_DEFAULT = 14,
	VLAN_PRI_ANY = 8,
	VLAN_PRI_COPY_INNER = 8,
	VLAN_TAGS_REMOVE_0 = 0,
	VLAN_TAGS_REMOVE_1 = 1,
	VLAN_TAGS_REMOVE_3 = 3,
};

/* The fields of a filter word. */
static unsigned int filter_pri(uint32_t w)
{
	return (w >> 28) & 0xFu;
}
static unsigned int filter_vid(uint32_t w)
{
	return (w >> 15) & 0x1FFFu;
}
static unsigned int filter_tpid(uint32_t w)
{
	return (w >> 12) & 0x7u;
}

/* The fields of a treatment word. */
static unsigned int treat_pri(uint32_t w)
{
	return (w >> 16) & 0xFu;
}
static unsigned int treat_vid(uint32_t w)
{
	return (w >> 3) & 0x1FFFu;
}
static unsigned int treat_tpid(uint32_t w)
{
	return w & 0x7u;
}

static bool vid_is_programmable(unsigned int vid)
{
	return vid >= 1u && vid <= 4094u;
}

/* True when a filter asks for a tag with a given VID. */
static bool filter_names_a_tag(uint32_t w)
{
	return filter_pri(w) != VLAN_PRI_NO_TAG &&
	       filter_vid(w) != VLAN_VID_WILDCARD;
}

const char *omci_vlan_reject_name(enum omci_vlan_reject r)
{
	switch (r) {
	case OMCI_VLAN_REJECT_NONE:
		return "none";
	case OMCI_VLAN_REJECT_ETHERTYPE:
		return "ethertype filter";
	case OMCI_VLAN_REJECT_PCP_REMARK:
		return "priority remark";
	case OMCI_VLAN_REJECT_SHAPE:
		return "unencodable filter/treatment shape";
	case OMCI_VLAN_REJECT_VID_RANGE:
		return "VID outside 1..4094";
	case OMCI_VLAN_REJECT_DISCARD:
		return "discard of the frames of a VID; not carried";
	default:
		return "?";
	}
}

/* A rule whose filter names the priority of the tag takes only that one. */
static void match_priority(struct omci_vlan_rule *out)
{
	if (out->filter_inner_pri > 7u)
		return;
	out->match_has_pri = true;
	out->match_pri = out->filter_inner_pri;
}

/* Mark a rule as one the hardware cannot carry. */
static bool refuse(struct omci_vlan_rule *out, enum omci_vlan_reject why)
{
	out->op = OMCI_VLAN_OP_UNSUPPORTED;
	out->reject = why;
	return true;
}

bool omci_vlan_decode(const uint8_t *row, size_t len,
		      struct omci_vlan_rule *out)
{
	if (out == NULL)
		return false;

	*out = (struct omci_vlan_rule){ .op = OMCI_VLAN_OP_NONE,
					.assign_pri = OMCI_HW_VLAN_PRI_KEEP };

	if (row == NULL || len < 16)
		return false;

	const uint32_t w0 = omci_be32(&row[0]);
	const uint32_t w1 = omci_be32(&row[4]);
	const uint32_t w2 = omci_be32(&row[8]);
	const uint32_t w3 = omci_be32(&row[12]);

	out->filter_outer_pri = (uint8_t)filter_pri(w0);
	out->filter_outer_vid = (uint16_t)filter_vid(w0);
	out->filter_outer_tpid = (uint8_t)filter_tpid(w0);
	out->filter_inner_pri = (uint8_t)filter_pri(w1);
	out->filter_inner_vid = (uint16_t)filter_vid(w1);
	out->filter_inner_tpid = (uint8_t)filter_tpid(w1);
	out->filter_ethertype = (uint8_t)(w1 & 0xFu);

	out->tags_to_remove = (uint8_t)((w2 >> 30) & 0x3u);
	out->treat_outer_pri = (uint8_t)treat_pri(w2);
	out->treat_outer_vid = (uint16_t)treat_vid(w2);
	out->treat_outer_tpid = (uint8_t)treat_tpid(w2);
	out->treat_inner_pri = (uint8_t)treat_pri(w3);
	out->treat_inner_vid = (uint16_t)treat_vid(w3);
	out->treat_inner_tpid = (uint8_t)treat_tpid(w3);

	/*
	 * A discard of the frames of one VID is not carried. A discard that
	 * names no tag is accepted; of those, the default rule of the
	 * single-tagged frames drops what no other rule takes, and the others
	 * leave nothing to program.
	 */
	if (out->tags_to_remove == VLAN_TAGS_REMOVE_3) {
		if (filter_names_a_tag(w0) || filter_names_a_tag(w1))
			return refuse(out, OMCI_VLAN_REJECT_DISCARD);
		if (out->filter_outer_pri == VLAN_PRI_NO_TAG &&
		    out->filter_inner_pri == VLAN_PRI_DEFAULT)
			out->op = OMCI_VLAN_OP_DISCARD_TAGGED;
		return true;
	}
	if (out->filter_outer_pri == VLAN_PRI_DEFAULT ||
	    out->filter_inner_pri == VLAN_PRI_DEFAULT)
		return true;

	/*
	 * An untagged frame given one tag: neither filter names a tag, nothing
	 * is removed, and one treatment names the tag. It is a C-tag, with the
	 * priority the treatment gives; an untagged frame has none to copy.
	 */
	if (out->filter_outer_pri == VLAN_PRI_NO_TAG &&
	    out->filter_inner_pri == VLAN_PRI_NO_TAG &&
	    out->tags_to_remove == VLAN_TAGS_REMOVE_0 &&
	    (out->treat_outer_pri == VLAN_PRI_NO_TAG) !=
		    (out->treat_inner_pri == VLAN_PRI_NO_TAG)) {
		const bool inner = out->treat_inner_pri != VLAN_PRI_NO_TAG;
		const unsigned int pri = inner ? out->treat_inner_pri :
						 out->treat_outer_pri;
		const unsigned int vid = inner ? out->treat_inner_vid :
						 out->treat_outer_vid;

		if (out->filter_ethertype != 0u)
			return refuse(out, OMCI_VLAN_REJECT_ETHERTYPE);
		if (pri > 7u)
			return refuse(out, OMCI_VLAN_REJECT_PCP_REMARK);
		if (!vid_is_programmable(vid))
			return refuse(out, OMCI_VLAN_REJECT_VID_RANGE);

		out->op = OMCI_VLAN_OP_CVID_PUSH;
		out->match_cvid = 0u;
		out->assign_cvid = (uint16_t)vid;
		out->assign_pri = (uint8_t)pri;
		return true;
	}

	/* An untagged frame given two tags is beyond the switch. */
	if (out->filter_outer_pri == VLAN_PRI_NO_TAG &&
	    out->filter_inner_pri == VLAN_PRI_NO_TAG &&
	    out->tags_to_remove == VLAN_TAGS_REMOVE_0 &&
	    out->treat_outer_pri != VLAN_PRI_NO_TAG)
		return refuse(out, OMCI_VLAN_REJECT_SHAPE);

	if (out->filter_inner_vid == VLAN_VID_WILDCARD)
		return true;

	if (out->filter_ethertype != 0u)
		return refuse(out, OMCI_VLAN_REJECT_ETHERTYPE);

	const bool inner_remarks = out->treat_inner_pri <= 7u &&
				   out->treat_inner_pri !=
					   out->filter_inner_pri;

	/*
	 * One tag in, one tag out with a new VID: a C-VID translation, which
	 * may give the tag a new priority, though not 0: the offload cannot
	 * tell a priority of 0 from none and keeps the frame's own.
	 */
	if (out->tags_to_remove == VLAN_TAGS_REMOVE_1 &&
	    !filter_names_a_tag(w0) && filter_names_a_tag(w1) &&
	    out->treat_outer_pri == VLAN_PRI_NO_TAG &&
	    out->treat_inner_pri != VLAN_PRI_NO_TAG) {
		if (inner_remarks && out->treat_inner_pri == 0u)
			return refuse(out, OMCI_VLAN_REJECT_PCP_REMARK);

		if (out->treat_inner_vid == VLAN_VID_WILDCARD)
			return true;

		if (!vid_is_programmable(out->filter_inner_vid) ||
		    !vid_is_programmable(out->treat_inner_vid))
			return refuse(out, OMCI_VLAN_REJECT_VID_RANGE);

		out->op = OMCI_VLAN_OP_CVID_XLATE;
		out->match_cvid = out->filter_inner_vid;
		out->assign_cvid = out->treat_inner_vid;
		if (inner_remarks)
			out->assign_pri = out->treat_inner_pri;
		match_priority(out);
		return true;
	}

	/* A tag kept and an outer one added: an S-VID push. */
	if (out->tags_to_remove == VLAN_TAGS_REMOVE_0 &&
	    !filter_names_a_tag(w0) && filter_names_a_tag(w1) &&
	    out->treat_outer_pri != VLAN_PRI_NO_TAG) {
		if (inner_remarks)
			return refuse(out, OMCI_VLAN_REJECT_PCP_REMARK);
		if (!vid_is_programmable(out->filter_inner_vid) ||
		    !vid_is_programmable(out->treat_outer_vid))
			return refuse(out, OMCI_VLAN_REJECT_VID_RANGE);

		if (out->treat_inner_vid != VLAN_VID_WILDCARD &&
		    out->treat_inner_vid != out->filter_inner_vid)
			return refuse(out, OMCI_VLAN_REJECT_SHAPE);

		/*
		 * The outer tag's priority: as given, or copied from the inner
		 * tag, which is the filter's priority when it names one.
		 */
		if (out->treat_outer_pri <= 7u)
			out->assign_pri = out->treat_outer_pri;
		else if (out->treat_outer_pri != VLAN_PRI_COPY_INNER)
			return refuse(out, OMCI_VLAN_REJECT_PCP_REMARK);
		else if (out->filter_inner_pri <= 7u)
			out->assign_pri = out->filter_inner_pri;
		else
			out->assign_pri = OMCI_HW_VLAN_PRI_COPY;

		out->op = OMCI_VLAN_OP_SVID_PUSH;
		out->match_cvid = out->filter_inner_vid;
		out->assign_svid = out->treat_outer_vid;
		match_priority(out);
		return true;
	}

	/* A rule that names no tag and adds none lets frames pass as they are. */
	if (!filter_names_a_tag(w0) && !filter_names_a_tag(w1) &&
	    out->treat_outer_pri == VLAN_PRI_NO_TAG &&
	    out->treat_inner_pri == VLAN_PRI_NO_TAG)
		return true;

	return refuse(out, OMCI_VLAN_REJECT_SHAPE);
}

/* Whether a decoded rule becomes a rule of the hardware. */
static bool programmable(enum omci_vlan_op op)
{
	return op == OMCI_VLAN_OP_CVID_XLATE || op == OMCI_VLAN_OP_SVID_PUSH ||
	       op == OMCI_VLAN_OP_CVID_PUSH ||
	       op == OMCI_VLAN_OP_DISCARD_TAGGED;
}

/* The hardware rule of a programmable operation. */
static enum omci_hw_vlan_kind kind_of(enum omci_vlan_op op)
{
	switch (op) {
	case OMCI_VLAN_OP_SVID_PUSH:
		return OMCI_HW_VLAN_SVID_PUSH;
	case OMCI_VLAN_OP_CVID_PUSH:
		return OMCI_HW_VLAN_CVID_PUSH;
	case OMCI_VLAN_OP_DISCARD_TAGGED:
		return OMCI_HW_VLAN_DISCARD_TAGGED;
	default:
		return OMCI_HW_VLAN_CVID_XLATE;
	}
}

static uint16_t net_vid_of(const struct omci_hw_vlan_xlate *r)
{
	return (r->kind == OMCI_HW_VLAN_SVID_PUSH) ? r->assign_svid :
						     r->assign_cvid;
}

/* The hardware rule of a decoded rule. */
static struct omci_hw_vlan_xlate hw_rule_of(const struct omci_vlan_rule *d)
{
	return (struct omci_hw_vlan_xlate){
		.kind = kind_of(d->op),
		.match_cvid = d->match_cvid,
		.assign_cvid = d->assign_cvid,
		.assign_svid = d->assign_svid,
		.assign_pri = d->assign_pri,
		.match_has_pri = d->match_has_pri,
		.match_pri = d->match_pri,
	};
}

/*
 * Whether the GEM ports of VLAN @net divide its priorities upstream: a port
 * that carries it takes some of them and not all. A port that carries the
 * VLAN @sub, whose frames a translation leads into @net, carries @net as
 * well. The ports are those the bridges and the GEM ports were last
 * programmed with.
 */
static bool gem_divides(uint16_t net, uint16_t sub)
{
	const struct omci_hw_bridge_port *bp;
	const struct omci_hw_gem_port *gp;
	const size_t nb = omci_bridge_last(&bp), ng = omci_gem_last(&gp);

	for (size_t i = 0; i < nb; i++) {
		bool carries = bp[i].num_vlan == 0u;

		if (bp[i].port_id == OMCI_HW_BRIDGE_UNI)
			continue;
		for (uint8_t v = 0; v < bp[i].num_vlan; v++)
			carries = carries || bp[i].vlan[v] == net ||
				  bp[i].vlan[v] == sub;
		for (size_t j = 0; j < ng && carries; j++)
			if (gp[j].port_id == bp[i].port_id &&
			    !gp[j].multicast &&
			    (gp[j].direction & OMCI_HW_GEM_DIR_UPSTREAM) &&
			    gp[j].pbits != 0u && gp[j].pbits != 0xffu)
				return true;
	}
	return false;
}

/*
 * Whether a rule takes each priority apart (struct omci_hw_vlan_xlate): a
 * translation that keeps any priority, into a VLAN whose GEM ports divide
 * the priorities. An S-VID push that copies any priority is taken apart by
 * the hardware layer anyway, as the tag it adds names its priority.
 */
static bool wants_per_pri(const struct omci_hw_vlan_xlate *r)
{
	return r->kind == OMCI_HW_VLAN_CVID_XLATE && !r->match_has_pri &&
	       r->assign_pri == OMCI_HW_VLAN_PRI_KEEP &&
	       gem_divides(r->assign_cvid, r->match_cvid);
}

/*
 * Whether two rules are of the same family: a discard of what no rule takes
 * matches no VID, and shares frames and tags with no rule but another one.
 */
static bool same_family(const struct omci_hw_vlan_xlate *a,
			const struct omci_hw_vlan_xlate *b)
{
	return (a->kind == OMCI_HW_VLAN_DISCARD_TAGGED) ==
	       (b->kind == OMCI_HW_VLAN_DISCARD_TAGGED);
}

/* Whether two rules take the same frames: one VID, and one priority or any. */
static bool same_match(const struct omci_hw_vlan_xlate *a,
		       const struct omci_hw_vlan_xlate *b)
{
	return same_family(a, b) && a->match_cvid == b->match_cvid &&
	       a->match_has_pri == b->match_has_pri &&
	       (!a->match_has_pri || a->match_pri == b->match_pri);
}

/* Whether an earlier rule takes every frame a later one would. */
static bool covers(const struct omci_hw_vlan_xlate *earlier,
		   const struct omci_hw_vlan_xlate *later)
{
	return same_family(earlier, later) &&
	       earlier->match_cvid == later->match_cvid &&
	       (!earlier->match_has_pri ||
		(later->match_has_pri &&
		 earlier->match_pri == later->match_pri));
}

/* Whether two rules give frames the same tag toward the network. */
static bool same_net(const struct omci_hw_vlan_xlate *a,
		     const struct omci_hw_vlan_xlate *b)
{
	return same_family(a, b) && net_vid_of(a) == net_vid_of(b) &&
	       (a->kind == OMCI_HW_VLAN_SVID_PUSH) ==
		       (b->kind == OMCI_HW_VLAN_SVID_PUSH);
}

/*
 * A new rule against those taken so far, which come before it in list
 * order: a rule that matches first takes the frame (G.988 9.3.13). An
 * earlier rule that takes all of the new rule's frames leaves it nothing:
 * the new one is a duplicate when it gives them the same network tag, and
 * shadowed when not. An earlier rule that gives frames the same network tag
 * from other frames makes the new one a many-to-one mapping: it works
 * upstream, and the way back downstream is the earlier rule's.
 */
enum slot_verdict {
	SLOT_NEW,
	SLOT_UP_ONLY,
	SLOT_DUP,
	SLOT_CONFLICT,
};

static enum slot_verdict slot_for(const struct omci_hw_vlan_xlate *rules,
				  size_t n, const struct omci_hw_vlan_xlate *r,
				  size_t *rival)
{
	enum slot_verdict v = SLOT_NEW;

	for (size_t k = 0; k < n; k++) {
		if (covers(&rules[k], r)) {
			if (rival != NULL)
				*rival = k;
			return same_net(&rules[k], r) ? SLOT_DUP :
							SLOT_CONFLICT;
		}
		if (v == SLOT_NEW && same_net(&rules[k], r)) {
			if (rival != NULL)
				*rival = k;
			v = SLOT_UP_ONLY;
		}
	}
	return v;
}

/* What the hardware refused, and the rules in force, at the last programming. */
static struct omci_hw_reject g_rejected;

static struct omci_hw_vlan_xlate g_in_force[OMCI_HW_VLAN_MAX_RULES];
static size_t g_in_force_n;

/* How the last programming chose its rules; see omci_vlan_last_reconcile(). */
static size_t g_last_plain;
static size_t g_last_preferred;
static unsigned int g_yielded;

static bool was_in_force(const struct omci_hw_vlan_xlate *r)
{
	for (size_t i = 0; i < g_in_force_n; i++)
		if (same_match(&g_in_force[i], r) &&
		    same_net(&g_in_force[i], r))
			return true;
	return false;
}

void omci_vlan_forget_in_force(void)
{
	g_in_force_n = 0;
}

size_t omci_vlan_last_reconcile(unsigned int *yielded, size_t *preferred)
{
	if (yielded != NULL)
		*yielded = g_yielded;
	if (preferred != NULL)
		*preferred = g_last_preferred;
	return g_last_plain;
}

/* The fates of the last programming. */
static struct omci_vlan_fate_rec g_fate[OMCI_VLAN_FATE_MAX];
static size_t g_fate_n;
static unsigned int g_fate_over;

const char *omci_vlan_fate_name(enum omci_vlan_fate f)
{
	switch (f) {
	case OMCI_VLAN_FATE_PROGRAMMED:
		return "programmed";
	case OMCI_VLAN_FATE_COLLAPSED:
		return "collapsed";
	case OMCI_VLAN_FATE_SHADOWED:
		return "shadowed";
	case OMCI_VLAN_FATE_OVER_BUDGET:
		return "over-budget";
	case OMCI_VLAN_FATE_UNSUPPORTED:
		return "unsupported";
	case OMCI_VLAN_FATE_INERT:
		return "inert";
	case OMCI_VLAN_FATE_REFUSED:
		return "refused";
	default:
		return "?";
	}
}

size_t omci_vlan_fates(const struct omci_vlan_fate_rec **out,
		       unsigned int *overflow)
{
	if (out != NULL)
		*out = g_fate;
	if (overflow != NULL)
		*overflow = g_fate_over;
	return g_fate_n;
}

static void fate_put(bool record, uint16_t instance, size_t row,
		     enum omci_vlan_fate f, uint16_t match_cvid,
		     uint16_t net_vid, const struct omci_hw_vlan_xlate *rival)
{
	if (!record)
		return;
	if (g_fate_n >= OMCI_VLAN_FATE_MAX) {
		g_fate_over++;
		return;
	}

	static_assert(OMCI_MIB_TABLE_ENTRIES <= 0x100u,
		      "a row index must fit struct omci_vlan_fate_rec");

	struct omci_vlan_fate_rec *r = &g_fate[g_fate_n++];

	r->instance = instance;
	r->row = (uint8_t)row;
	r->fate = (uint8_t)f;
	r->match_cvid = match_cvid;
	r->net_vid = net_vid;
	r->rival_match = rival != NULL ? rival->match_cvid : 0u;
	r->rival_net = rival != NULL ? net_vid_of(rival) : 0u;
}

/* A row of a rule table, and its place in the table. */
struct listed_row {
	uint8_t bytes[16];
	size_t index;
};

/* Put @r among the @n rows of @out, which are in list order. */
static void insert_listed(struct listed_row *out, size_t n,
			  const struct listed_row *r)
{
	size_t at = n;

	while (at > 0 && memcmp(out[at - 1].bytes, r->bytes, 8) > 0) {
		out[at] = out[at - 1];
		at--;
	}
	out[at] = *r;
}

/*
 * The rows of an instance's rule table in list order, which sorts them on
 * their first eight bytes, the filter (G.988 9.3.13), whatever order the OLT
 * set them in.
 *
 * Return: their number.
 */
static size_t rows_in_list_order(const struct omci_mib *mib, uint16_t class_id,
				 uint16_t instance, uint8_t attr_no,
				 struct listed_row *out, size_t cap)
{
	const size_t count =
		omci_mib_table_count(mib, class_id, instance, attr_no);
	size_t n = 0;

	for (size_t i = 0; i < count && n < cap; i++) {
		struct listed_row r = { .index = i };

		if (omci_mib_table_get(mib, class_id, instance, attr_no, i,
				       r.bytes, sizeof(r.bytes)) == OMCI_OK)
			insert_listed(out, n++, &r);
	}
	return n;
}

/*
 * Collect the programmable rules of every instance, in MIB order and each
 * instance's rules in list order, into
 * @rules. With @prefer_in_force, a first pass takes the rules that were in
 * force and a second one the rest. @warn logs the rules left out and
 * @record keeps their fates.
 *
 * Return: the number of rules collected.
 */
static size_t gather(const struct omci_mib *mib, uint16_t class_id,
		     uint8_t attr_no, struct omci_hw_vlan_xlate *rules,
		     size_t cap, unsigned int *bad_out, bool warn, bool record,
		     bool prefer_in_force, unsigned int *over_out)
{
	size_t n = 0;
	unsigned int bad = 0;
	unsigned int over = 0;

	if (record) {
		g_fate_n = 0;
		g_fate_over = 0;
	}

	bool warned_full = false;

	struct omci_mib_cursor cur;

	for (unsigned int pass = 0; pass < (prefer_in_force ? 2u : 1u);
	     pass++) {
		for (enum omci_status w = omci_mib_walk_first(mib, &cur);
		     w == OMCI_OK; w = omci_mib_walk_next(mib, &cur)) {
			if (cur.class_id != class_id)
				continue;

			static struct listed_row listed[OMCI_MIB_TABLE_ENTRIES];
			const size_t count = rows_in_list_order(
				mib, class_id, cur.instance, attr_no, listed,
				ARRAY_SIZE(listed));

			for (size_t j = 0; j < count; j++) {
				const size_t i = listed[j].index;
				uint8_t row[16];

				memcpy(row, listed[j].bytes, sizeof(row));

				struct omci_vlan_rule d;
				(void)omci_vlan_decode(row, sizeof(row), &d);

				struct omci_hw_vlan_xlate cand = hw_rule_of(&d);

				if (prefer_in_force) {
					if (!programmable(d.op)) {
						if (pass == 0u)
							continue;
					} else if ((pass == 0u) !=
						   was_in_force(&cand)) {
						continue;
					}
				}

				if (d.op == OMCI_VLAN_OP_UNSUPPORTED) {
					bad++;
					if (warn)
						OMCI_WARN(
							"me171: entry %u refused — %s",
							(unsigned int)i,
							omci_vlan_reject_name(
								d.reject));

					fate_put(record, cur.instance, i,
						 OMCI_VLAN_FATE_UNSUPPORTED,
						 d.match_cvid, 0u, NULL);
					continue;
				}
				if (d.op == OMCI_VLAN_OP_NONE) {
					fate_put(record, cur.instance, i,
						 OMCI_VLAN_FATE_INERT,
						 d.match_cvid, 0u, NULL);
					continue;
				}

				const uint16_t net_vid =
					(d.op == OMCI_VLAN_OP_SVID_PUSH) ?
						d.assign_svid :
						d.assign_cvid;

				size_t rival = 0;
				const enum slot_verdict v =
					slot_for(rules, n, &cand, &rival);

				if (v == SLOT_DUP || v == SLOT_CONFLICT) {
					if (v == SLOT_CONFLICT && warn)
						OMCI_WARN(
							"me171: C-VID %u<->%u conflicts with the existing %u<->%u rule (a VID would match two entries in one direction); ignoring it",
							d.match_cvid, net_vid,
							rules[rival].match_cvid,
							net_vid_of(
								&rules[rival]));
					fate_put(
						record, cur.instance, i,
						v == SLOT_DUP ?
							OMCI_VLAN_FATE_COLLAPSED :
							OMCI_VLAN_FATE_SHADOWED,
						d.match_cvid, net_vid,
						&rules[rival]);
					continue;
				}

				if (n >= cap) {
					bad++;
					if (warn && !warned_full) {
						OMCI_WARN(
							"me171: more than %zu programmable rules — the rest are NOT programmed",
							cap);
						warned_full = true;
					}
					over++;
					fate_put(record, cur.instance, i,
						 OMCI_VLAN_FATE_OVER_BUDGET,
						 d.match_cvid, net_vid, NULL);
					continue;
				}

				if (v == SLOT_UP_ONLY) {
					cand.up_only = true;
					if (warn)
						OMCI_INFO(
							"me171: C-VID %u<->%u works upstream only; the way back is that of the %u<->%u rule before it",
							d.match_cvid, net_vid,
							rules[rival].match_cvid,
							net_vid_of(
								&rules[rival]));
				}

				cand.per_pri = wants_per_pri(&cand);
				if (cand.per_pri && warn)
					OMCI_INFO(
						"me171: C-VID %u<->%u keeps any priority, and the GEM ports of VLAN %u divide the priorities: each priority is taken apart",
						d.match_cvid, net_vid, net_vid);

				rules[n] = cand;
				fate_put(record, cur.instance, i,
					 OMCI_VLAN_FATE_PROGRAMMED,
					 d.match_cvid, net_vid, NULL);
				n++;
			}
		}
	}

	if (bad_out != NULL)
		*bad_out = bad;
	if (over_out != NULL)
		*over_out = over;
	return n;
}

enum omci_status omci_vlan_program(const struct omci_mib *mib,
				   uint16_t class_id, uint8_t attr_no,
				   unsigned int *refused)
{
	struct omci_hw_vlan_xlate rules[OMCI_HW_VLAN_MAX_RULES];
	const size_t cap = ARRAY_SIZE(rules);

	struct omci_vlan_fate_rec saved[OMCI_VLAN_FATE_MAX];
	const size_t saved_n = g_fate_n;
	const unsigned int saved_over = g_fate_over;

	for (size_t i = 0; i < saved_n; i++)
		saved[i] = g_fate[i];

	unsigned int over_plain = 0;
	const size_t plain = gather(mib, class_id, attr_no, rules, cap, NULL,
				    false, false, false, &over_plain);

	g_last_plain = plain;

	size_t n = plain;

	/*
	 * Too many rules: keep those in force where that programs as many rules
	 * as taking them in order would; otherwise take them in order.
	 */
	if (over_plain > 0u) {
		n = gather(mib, class_id, attr_no, rules, cap, NULL, false,
			   false, true, NULL);
		if (n >= plain)
			n = gather(mib, class_id, attr_no, rules, cap, refused,
				   true, true, true, NULL);

		g_last_preferred = n;
		if (n < plain) {
			g_yielded++;
			n = gather(mib, class_id, attr_no, rules, cap, refused,
				   true, true, false, NULL);
		}
	} else {
		n = gather(mib, class_id, attr_no, rules, cap, refused, true,
			   true, false, NULL);
		g_last_preferred = n;
	}

	struct omci_hw_reject rejected = { 0 };
	const enum omci_status applied =
		omci_hw()->vlan_apply(rules, n, &rejected);

	if (applied != OMCI_OK && applied != OMCI_DECLINED) {
		for (size_t i = 0; i < saved_n; i++)
			g_fate[i] = saved[i];
		g_fate_n = saved_n;
		g_fate_over = saved_over;
		return applied;
	}

	/* A rule the hardware refused is not in force. */
	g_rejected = rejected;
	for (size_t i = 0; i < g_fate_n; i++)
		if (g_fate[i].fate == OMCI_VLAN_FATE_PROGRAMMED &&
		    omci_hw_reject_has(&rejected, g_fate[i].match_cvid))
			g_fate[i].fate = OMCI_VLAN_FATE_REFUSED;

	g_in_force_n = 0;
	for (size_t i = 0; i < n; i++) {
		if (omci_hw_reject_has(&rejected, rules[i].match_cvid)) {
			OMCI_WARN(
				"me171: the switch would not take C-VID %u -> %u; it is NOT in force",
				rules[i].match_cvid, net_vid_of(&rules[i]));
			continue;
		}
		g_in_force[g_in_force_n++] = rules[i];
	}

	return applied;
}

size_t omci_vlan_in_force(const struct omci_hw_vlan_xlate **out)
{
	*out = g_in_force;
	return g_in_force_n;
}

bool omci_vlan_per_pri_stale(void)
{
	for (size_t i = 0; i < g_in_force_n; i++)
		if (g_in_force[i].per_pri != wants_per_pri(&g_in_force[i]))
			return true;
	return false;
}

bool omci_vlan_row_refused(const uint8_t *row, size_t len)
{
	struct omci_vlan_rule d;

	if (g_rejected.n == 0)
		return false;
	if (!omci_vlan_decode(row, len, &d))
		return false;
	return omci_hw_reject_has(&g_rejected, (uint16_t)d.match_cvid);
}

bool omci_vlan_has_room(const struct omci_mib *mib, uint16_t class_id,
			uint8_t attr_no, const uint8_t *row, size_t len)
{
	struct omci_vlan_rule d;

	if (!omci_vlan_decode(row, len, &d))
		return true;

	if (!programmable(d.op))
		return true;

	struct omci_hw_vlan_xlate rules[OMCI_HW_VLAN_MAX_RULES];
	const size_t cap = ARRAY_SIZE(rules);
	const size_t n = gather(mib, class_id, attr_no, rules, cap, NULL, false,
				false, false, NULL);

	const struct omci_hw_vlan_xlate cand = hw_rule_of(&d);
	const enum slot_verdict v = slot_for(rules, n, &cand, NULL);

	if (v == SLOT_DUP || v == SLOT_CONFLICT)
		return true;

	return n < cap;
}
