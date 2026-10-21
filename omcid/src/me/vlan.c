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
#include "me/svcrule.h"
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
 *
 * The filter on the EtherType (G.988 9.3.13, Filter Ethertype, the low four
 * bits of the inner filter word) takes the frames of IPoE, PPPoE, ARP, IPv6 or
 * EAPOL alone (enum omci_hw_ethertype); its other codes are reserved. A rule
 * on an untagged or a single-tagged frame may name one; one on a
 * double-tagged frame, and a discard, may not here.
 *
 * A treatment priority of 10, derived from the frame's DSCP by the map the ME
 * holds, is not carried: the switch has one DSCP map, by which a port that
 * trusts the DSCP gives every frame its priority (dcb), and the ME's map
 * would replace it; a classifier action that takes its priority from it is
 * not measured. A tag added with that priority is refused; a translation with
 * it keeps the frame's priority, as one that copies the priority does.
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
		return "ethertype filter of a double-tagged frame or a discard; not carried";
	case OMCI_VLAN_REJECT_ETHERTYPE_CODE:
		return "reserved ethertype filter";
	case OMCI_VLAN_REJECT_ETHERTYPE_PASS:
		return "untagged ethertype left as is ahead of a rule that tags it; not carried";
	case OMCI_VLAN_REJECT_PCP_REMARK:
		return "priority remark";
	case OMCI_VLAN_REJECT_SHAPE:
		return "unencodable filter/treatment shape";
	case OMCI_VLAN_REJECT_VID_RANGE:
		return "VID outside 1..4094";
	case OMCI_VLAN_REJECT_DISCARD:
		return "discard of the double-tagged frames of a VID; not carried";
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

	if (out->filter_ethertype > OMCI_HW_ETHERTYPE_EAPOL)
		return refuse(out, OMCI_VLAN_REJECT_ETHERTYPE_CODE);

	/*
	 * A discard of the single-tagged frames of one VID drops them, of the
	 * priority the filter names or of any; nothing comes back downstream
	 * (G.988 9.3.13, treatment tags to remove 3). A discard of the
	 * double-tagged frames of a VID is not carried. A discard that names no
	 * tag is accepted; of those, the default rule of the single-tagged
	 * frames drops what no other rule takes, and the others leave nothing to
	 * program.
	 */
	if (out->tags_to_remove == VLAN_TAGS_REMOVE_3) {
		if (filter_names_a_tag(w0) ||
		    (filter_names_a_tag(w1) &&
		     out->filter_outer_pri != VLAN_PRI_NO_TAG))
			return refuse(out, OMCI_VLAN_REJECT_DISCARD);
		if (filter_names_a_tag(w1)) {
			if (out->filter_ethertype != 0u)
				return refuse(out, OMCI_VLAN_REJECT_ETHERTYPE);
			if (!vid_is_programmable(out->filter_inner_vid))
				return refuse(out, OMCI_VLAN_REJECT_VID_RANGE);
			out->op = OMCI_VLAN_OP_DISCARD_VID;
			out->match_cvid = out->filter_inner_vid;
			match_priority(out);
			return true;
		}
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

	/*
	 * An untagged frame given two tags: a C-tag by the inner treatment and
	 * an S-tag in front of it by the outer one, each with the priority and
	 * the VID its treatment gives; an untagged frame has none to copy.
	 */
	if (out->filter_outer_pri == VLAN_PRI_NO_TAG &&
	    out->filter_inner_pri == VLAN_PRI_NO_TAG &&
	    out->tags_to_remove == VLAN_TAGS_REMOVE_0 &&
	    out->treat_outer_pri != VLAN_PRI_NO_TAG) {
		if (out->treat_inner_pri > 7u || out->treat_outer_pri > 7u)
			return refuse(out, OMCI_VLAN_REJECT_PCP_REMARK);
		if (!vid_is_programmable(out->treat_inner_vid) ||
		    !vid_is_programmable(out->treat_outer_vid))
			return refuse(out, OMCI_VLAN_REJECT_VID_RANGE);

		out->op = OMCI_VLAN_OP_SCVID_PUSH;
		out->match_cvid = 0u;
		out->assign_cvid = out->treat_inner_vid;
		out->inner_has_pri = true;
		out->inner_pri = out->treat_inner_pri;
		out->assign_svid = out->treat_outer_vid;
		out->assign_pri = out->treat_outer_pri;
		return true;
	}

	if (out->filter_inner_vid == VLAN_VID_WILDCARD)
		return true;

	if (out->filter_ethertype != 0u &&
	    out->filter_outer_pri != VLAN_PRI_NO_TAG)
		return refuse(out, OMCI_VLAN_REJECT_ETHERTYPE);

	const bool inner_remarks = out->treat_inner_pri <= 7u &&
				   out->treat_inner_pri !=
					   out->filter_inner_pri;

	/*
	 * One tag in, one tag out with a new VID: a C-VID translation, which
	 * may give the tag a new priority, 0 included.
	 */
	if (out->tags_to_remove == VLAN_TAGS_REMOVE_1 &&
	    !filter_names_a_tag(w0) && filter_names_a_tag(w1) &&
	    out->treat_outer_pri == VLAN_PRI_NO_TAG &&
	    out->treat_inner_pri != VLAN_PRI_NO_TAG) {
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

	/*
	 * A tag kept and an outer one added: an S-VID push, which may give the
	 * tag it keeps a new priority.
	 */
	if (out->tags_to_remove == VLAN_TAGS_REMOVE_0 &&
	    !filter_names_a_tag(w0) && filter_names_a_tag(w1) &&
	    out->treat_outer_pri != VLAN_PRI_NO_TAG) {
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
		if (inner_remarks) {
			out->inner_has_pri = true;
			out->inner_pri = out->treat_inner_pri;
		}
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
	       op == OMCI_VLAN_OP_DISCARD_TAGGED ||
	       op == OMCI_VLAN_OP_DISCARD_VID || op == OMCI_VLAN_OP_SCVID_PUSH;
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
	case OMCI_VLAN_OP_DISCARD_VID:
		return OMCI_HW_VLAN_DISCARD_VID;
	case OMCI_VLAN_OP_SCVID_PUSH:
		return OMCI_HW_VLAN_SCVID_PUSH;
	default:
		return OMCI_HW_VLAN_CVID_XLATE;
	}
}

/* Whether a rule gives its frames a tag toward the network: a discard gives none. */
static bool gives_net(const struct omci_hw_vlan_xlate *r)
{
	return r->kind != OMCI_HW_VLAN_DISCARD_TAGGED &&
	       r->kind != OMCI_HW_VLAN_DISCARD_VID;
}

/* Whether a rule puts an S-tag in front of the frames it gives the network. */
static bool adds_stag(const struct omci_hw_vlan_xlate *r)
{
	return r->kind == OMCI_HW_VLAN_SVID_PUSH ||
	       r->kind == OMCI_HW_VLAN_SCVID_PUSH;
}

/* The VID a rule gives its frames toward the network, 0 for none. */
static uint16_t net_vid_of(const struct omci_hw_vlan_xlate *r)
{
	if (!gives_net(r))
		return 0u;
	return adds_stag(r) ? r->assign_svid : r->assign_cvid;
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
		.inner_has_pri = d->inner_has_pri,
		.inner_pri = d->inner_pri,
		.ethertype = d->filter_ethertype,
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

/*
 * Whether two rules take the same frames: one VID, one priority or any, and
 * one EtherType or any.
 */
static bool same_match(const struct omci_hw_vlan_xlate *a,
		       const struct omci_hw_vlan_xlate *b)
{
	return same_family(a, b) && a->match_cvid == b->match_cvid &&
	       a->match_has_pri == b->match_has_pri &&
	       (!a->match_has_pri || a->match_pri == b->match_pri) &&
	       a->ethertype == b->ethertype;
}

/* Whether an earlier rule takes every frame a later one would. */
static bool covers(const struct omci_hw_vlan_xlate *earlier,
		   const struct omci_hw_vlan_xlate *later)
{
	return same_family(earlier, later) &&
	       earlier->match_cvid == later->match_cvid &&
	       (!earlier->match_has_pri ||
		(later->match_has_pri &&
		 earlier->match_pri == later->match_pri)) &&
	       (earlier->ethertype == OMCI_HW_ETHERTYPE_ANY ||
		earlier->ethertype == later->ethertype);
}

/*
 * Whether two rules give frames the same tag toward the network; two
 * discards, which give none, do the same thing.
 */
static bool same_net(const struct omci_hw_vlan_xlate *a,
		     const struct omci_hw_vlan_xlate *b)
{
	if (!same_family(a, b))
		return false;
	if (!gives_net(a) || !gives_net(b))
		return gives_net(a) == gives_net(b);
	return net_vid_of(a) == net_vid_of(b) && adds_stag(a) == adds_stag(b);
}

/*
 * A new rule against those taken so far, which come before it in list
 * order: a rule that matches first takes the frame (G.988 9.3.13). An
 * earlier rule that takes all of the new rule's frames leaves it nothing:
 * the new one is a duplicate when it gives them the same network tag, and
 * shadowed when not. An earlier rule that gives frames the same network tag
 * from other frames makes the new one a many-to-one mapping: it works
 * upstream, and the way back downstream is the earlier rule's. A discard has
 * no way back, so no such mapping.
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
		if (v == SLOT_NEW && gives_net(r) && same_net(&rules[k], r)) {
			if (rival != NULL)
				*rival = k;
			v = SLOT_UP_ONLY;
		}
	}
	return v;
}

/*
 * The priorities that the rules before a discard of any priority take from
 * its VID, bit p for priority p; @rival is the last of those rules. A rule on
 * one EtherType takes none whole, and the discard, which decides first in the
 * switch, takes its frames as well.
 */
static uint8_t taken_pri(const struct omci_hw_vlan_xlate *rules, size_t n,
			 const struct omci_hw_vlan_xlate *r, size_t *rival)
{
	uint8_t taken = 0;

	for (size_t k = 0; k < n; k++) {
		if (!same_family(&rules[k], r) ||
		    rules[k].match_cvid != r->match_cvid ||
		    !rules[k].match_has_pri ||
		    rules[k].ethertype != OMCI_HW_ETHERTYPE_ANY)
			continue;
		taken |= (uint8_t)(1u << rules[k].match_pri);
		*rival = k;
	}
	return taken;
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

/*
 * The fates of the last programming, and the key of the rule of each
 * (omci_hw_vlan_key()), by which the hardware refuses it.
 */
static struct omci_vlan_fate_rec g_fate[OMCI_VLAN_FATE_MAX];
static uint16_t g_fate_key[OMCI_VLAN_FATE_MAX];
static size_t g_fate_n;
static unsigned int g_fate_over;

/*
 * The filters of the rules that leave the untagged frames of an EtherType as
 * they are, ahead of a rule that tags them, which the last programming left
 * out (see passes_ahead_of_tag()).
 */
static uint8_t g_pass_refused[OMCI_VLAN_FATE_MAX][8];
static size_t g_pass_refused_n;

/*
 * The rules of the services (me/svcrule.h): a person's, as the backend gave
 * them for the programming under way; the ones the last recorded gathering
 * made for clashes, and those of the gathering under way.
 */
static struct omci_svc_rule g_svc[OMCI_SVC_MAX];
static size_t g_svc_n;
static struct omci_svc_rule g_made[OMCI_SVC_MAX];
static size_t g_made_n;
static struct omci_svc_rule g_making[OMCI_SVC_MAX];
static size_t g_making_n;

/*
 * The VLAN of an IP host's service, voice as a rule, on the fibre and
 * towards the host (0 untagged), of the last recorded gathering and of the
 * one under way; a fibre VLAN of 0 while there is none.
 */
static uint16_t g_phone[2], g_phoning[2];

/* The class 171 instance each gathered rule came of, by its place. */
static uint16_t g_rule_inst[OMCI_HW_VLAN_MAX_RULES];

/* The host VIDs that the MIB's rules match and the person's moves give, a bit each. */
static uint8_t g_host_used[4096u / 8u];

/* Where the rules of an instance of class 171 lead: an Ethernet UNI, the VEIP, an IP host, or else. */
enum out_kind { OUT_OTHER, OUT_UNI, OUT_VEIP, OUT_IPHOST };

/* The association type of class 171 (G.988 9.3.13), attribute 1, and its pointer, 7. */
enum {
	ME171_ASSOC_TYPE = 1,
	ME171_ASSOC_PTR = 7,
	ME171_ASSOC_BRIDGE_PORT = 0,
	ME171_ASSOC_PPTP_ETH = 2,
	ME171_ASSOC_IP_HOST = 3,
	ME171_ASSOC_VEIP = 10,
};

static enum out_kind out_of(const struct omci_mib *mib, uint16_t class_id,
			    uint16_t instance)
{
	uint8_t type = 0xffu, ptr[2] = { 0, 0 }, tp = 0;

	(void)!omci_mib_attr_get(mib, class_id, instance, ME171_ASSOC_TYPE,
				 &type, 1u, NULL);
	if (type == ME171_ASSOC_PPTP_ETH)
		return OUT_UNI;
	if (type == ME171_ASSOC_VEIP)
		return OUT_VEIP;
	if (type == ME171_ASSOC_IP_HOST)
		return OUT_IPHOST;
	if (type != ME171_ASSOC_BRIDGE_PORT ||
	    omci_mib_attr_get(mib, class_id, instance, ME171_ASSOC_PTR, ptr,
			      sizeof(ptr), NULL) != OMCI_OK ||
	    omci_mib_attr_get(mib, OMCI_BRIDGE_PORT_CLASS_ID,
			      (uint16_t)(ptr[0] << 8 | ptr[1]),
			      OMCI_BRIDGE_PORT_ATTR_TPTYPE, &tp, 1u,
			      NULL) != OMCI_OK)
		return OUT_OTHER;
	return tp == OMCI_BRIDGE_TP_PPTP_ETH ? OUT_UNI :
	       tp == OMCI_BRIDGE_TP_VEIP     ? OUT_VEIP :
	       tp == OMCI_BRIDGE_TP_IP_HOST  ? OUT_IPHOST :
					       OUT_OTHER;
}

/*
 * Whether host VID @vid is taken: by a rule of the MIB, a person's move, or
 * a rule gathered so far, a move this gathering made among them.
 */
static bool host_taken(uint16_t vid, const struct omci_hw_vlan_xlate *rules,
		       size_t n)
{
	if (g_host_used[vid / 8u] & (1u << (vid % 8u)))
		return true;
	for (size_t k = 0; k < n; k++)
		if (rules[k].match_cvid == vid)
			return true;
	return false;
}

/*
 * A free host VID for the service of fibre VID @fibre that clashes: its own
 * when free, else the first free from 2 up; 0 when none is.
 */
static uint16_t free_host(uint16_t fibre,
			  const struct omci_hw_vlan_xlate *rules, size_t n)
{
	uint16_t v = fibre;

	if (host_taken(v, rules, n))
		for (v = 2; v < 4095u && host_taken(v, rules, n); v++)
			;
	return v < 4095u ? v : 0u;
}

/*
 * The services of an IP host (G.988 9.4.1), voice as a rule, reach a device
 * behind the stick on the lane the UNIs share, where a rule that tags its
 * untagged frames would take the untagged frames of every service. Such a
 * rule becomes a translation from a host VLAN of its own: a person's move's,
 * or the fibre's when free, else the first free one. Return: whether it did.
 */
static bool own_host_vlan(struct omci_hw_vlan_xlate *c,
			  const struct omci_svc_rule *sr, uint16_t fibre,
			  const struct omci_hw_vlan_xlate *rules, size_t n)
{
	const uint16_t host = sr != NULL && sr->action == OMCI_SVC_MOVE ?
				      sr->host :
				      free_host(fibre, rules, n);

	c->kind = host != 0u ? OMCI_HW_VLAN_CVID_XLATE : c->kind;
	c->match_cvid = host != 0u ? host : c->match_cvid;
	return host != 0u;
}

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
	case OMCI_VLAN_FATE_LOCAL:
		return "local";
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
		     enum omci_vlan_fate f,
		     const struct omci_hw_vlan_xlate *rule, uint16_t net_vid,
		     const struct omci_hw_vlan_xlate *rival)
{
	if (!record)
		return;
	if (g_fate_n >= OMCI_VLAN_FATE_MAX) {
		g_fate_over++;
		return;
	}

	static_assert(OMCI_MIB_TABLE_ENTRIES <= 0x100u,
		      "a row index must fit struct omci_vlan_fate_rec");

	g_fate_key[g_fate_n] = omci_hw_vlan_key(rule);

	struct omci_vlan_fate_rec *r = &g_fate[g_fate_n++];

	r->instance = instance;
	r->row = (uint8_t)row;
	r->fate = (uint8_t)f;
	r->match_cvid = rule->match_cvid;
	r->net_vid = net_vid;
	r->rival_match = rival != NULL ? rival->match_cvid : 0u;
	r->rival_net = rival != NULL ? net_vid_of(rival) : 0u;
}

/* Say why a rule whose frames an earlier rule takes is left out. */
static void warn_shadowed(const struct omci_hw_vlan_xlate *r,
			  const struct omci_hw_vlan_xlate *rival)
{
	if (!gives_net(r))
		OMCI_WARN(
			"me171: the discard of C-VID %u comes after the %u<->%u rule, which takes its frames; ignoring it",
			r->match_cvid, rival->match_cvid, net_vid_of(rival));
	else if (!gives_net(rival))
		OMCI_WARN(
			"me171: C-VID %u<->%u comes after the discard of C-VID %u, which takes its frames; ignoring it",
			r->match_cvid, net_vid_of(r), rival->match_cvid);
	else
		OMCI_WARN(
			"me171: C-VID %u<->%u conflicts with the existing %u<->%u rule (a VID would match two entries in one direction); ignoring it",
			r->match_cvid, net_vid_of(r), rival->match_cvid,
			net_vid_of(rival));
}

/* A row of a rule table, and its place in the table. */
struct listed_row {
	uint8_t bytes[16];
	size_t index;
};

/*
 * Whether a row is one of the three default rules, of the untagged, the
 * single-tagged and the double-tagged frames: a filter that names neither
 * tag's VID, no EtherType and no extended criteria, with the priority of no
 * tag or of the default (G.988 9.3.13). The ONU takes them as a last resort,
 * after every other rule.
 */
static bool default_row(const uint8_t *row)
{
	const uint32_t w0 = omci_be32(&row[0]);
	const uint32_t w1 = omci_be32(&row[4]);
	const unsigned int p0 = filter_pri(w0), p1 = filter_pri(w1);

	if (filter_vid(w0) != VLAN_VID_WILDCARD ||
	    filter_vid(w1) != VLAN_VID_WILDCARD || (w1 & 0xFFFu) != 0u)
		return false;
	return (p0 == VLAN_PRI_NO_TAG &&
		(p1 == VLAN_PRI_NO_TAG || p1 == VLAN_PRI_DEFAULT)) ||
	       (p0 == VLAN_PRI_DEFAULT && p1 == VLAN_PRI_DEFAULT);
}

/*
 * The list order of two rows: on their first eight bytes, the default rules
 * after all others.
 */
static int list_cmp(const uint8_t *a, const uint8_t *b)
{
	const bool da = default_row(a), db = default_row(b);

	if (da != db)
		return da ? 1 : -1;
	return memcmp(a, b, 8);
}

/* Put @r among the @n rows of @out, which are in list order. */
static void insert_listed(struct listed_row *out, size_t n,
			  const struct listed_row *r)
{
	size_t at = n;

	while (at > 0 && list_cmp(out[at - 1].bytes, r->bytes) > 0) {
		out[at] = out[at - 1];
		at--;
	}
	out[at] = *r;
}

/*
 * The rows of an instance's rule table in list order, which sorts them on
 * their first eight bytes, the filter (G.988 9.3.13), whatever order the OLT
 * set them in, but for the default rules, which come last.
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
 * Whether rule @d of row @j of the @count rows of @listed, which leaves the
 * untagged frames of an EtherType as they are and so has no rule of the
 * switch, comes ahead of a rule that tags those frames: the switch would give
 * them to that rule, and no rule of the switch found yet leaves a frame as it
 * is ahead of one that tags it.
 */
static bool passes_ahead_of_tag(const struct listed_row *listed, size_t count,
				size_t j, const struct omci_vlan_rule *d)
{
	if (d->filter_ethertype == OMCI_HW_ETHERTYPE_ANY ||
	    d->filter_outer_pri != VLAN_PRI_NO_TAG ||
	    d->filter_inner_pri != VLAN_PRI_NO_TAG ||
	    d->tags_to_remove != VLAN_TAGS_REMOVE_0)
		return false;

	for (size_t k = j + 1; k < count; k++) {
		struct omci_vlan_rule later;

		(void)omci_vlan_decode(listed[k].bytes, sizeof(listed[k].bytes),
				       &later);
		if ((later.op == OMCI_VLAN_OP_CVID_PUSH ||
		     later.op == OMCI_VLAN_OP_SCVID_PUSH) &&
		    (later.filter_ethertype == OMCI_HW_ETHERTYPE_ANY ||
		     later.filter_ethertype == d->filter_ethertype))
			return true;
	}
	return false;
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
		g_pass_refused_n = 0;
	}
	g_making_n = 0;
	g_phoning[0] = 0;
	g_phoning[1] = 0;

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

				if (d.op == OMCI_VLAN_OP_NONE &&
				    passes_ahead_of_tag(listed, count, j, &d)) {
					(void)refuse(
						&d,
						OMCI_VLAN_REJECT_ETHERTYPE_PASS);
					if (record &&
					    g_pass_refused_n <
						    ARRAY_SIZE(g_pass_refused))
						memcpy(g_pass_refused
							       [g_pass_refused_n++],
						       row, 8);
				}

				struct omci_hw_vlan_xlate cand = hw_rule_of(&d);
				const uint16_t fibre = net_vid_of(&cand);
				const struct omci_svc_rule *sr =
					fibre != 0u ?
						omci_svc_find(g_svc, g_svc_n,
							      fibre) :
						NULL;

				if (sr != NULL && sr->action == OMCI_SVC_MOVE &&
				    cand.kind == OMCI_HW_VLAN_CVID_XLATE)
					cand.match_cvid = sr->host;

				const bool phone =
					fibre != 0u &&
					out_of(mib, class_id, cur.instance) ==
						OUT_IPHOST;
				const bool own =
					phone &&
					cand.kind == OMCI_HW_VLAN_CVID_PUSH &&
					own_host_vlan(&cand, sr, fibre, rules,
						      n);

				if (prefer_in_force) {
					if (!programmable(d.op)) {
						if (pass == 0u)
							continue;
					} else if ((pass == 0u) !=
						   was_in_force(&cand)) {
						continue;
					}
				}

				if (sr != NULL && sr->action != OMCI_SVC_MOVE) {
					const enum out_kind k = out_of(
						mib, class_id, cur.instance);

					if (sr->action == OMCI_SVC_OFF ||
					    (sr->action == OMCI_SVC_ONLY_UNI &&
					     k != OUT_UNI && k != OUT_OTHER) ||
					    (sr->action == OMCI_SVC_ONLY_VEIP &&
					     k != OUT_VEIP && k != OUT_OTHER)) {
						fate_put(record, cur.instance,
							 i,
							 OMCI_VLAN_FATE_LOCAL,
							 &cand, fibre, NULL);
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
						 &cand, 0u, NULL);
					continue;
				}
				if (d.op == OMCI_VLAN_OP_NONE) {
					fate_put(record, cur.instance, i,
						 OMCI_VLAN_FATE_INERT, &cand,
						 0u, NULL);
					continue;
				}

				const uint16_t net_vid = net_vid_of(&cand);

				size_t rival = 0;
				enum slot_verdict v =
					slot_for(rules, n, &cand, &rival);

				if (v == SLOT_CONFLICT &&
				    (sr == NULL ||
				     sr->action != OMCI_SVC_MOVE) &&
				    cand.kind == OMCI_HW_VLAN_CVID_XLATE &&
				    rules[rival].match_cvid ==
					    cand.match_cvid &&
				    g_rule_inst[rival] != cur.instance &&
				    g_making_n < ARRAY_SIZE(g_making) &&
				    out_of(mib, class_id, cur.instance) !=
					    OUT_OTHER &&
				    out_of(mib, class_id, g_rule_inst[rival]) !=
					    OUT_OTHER) {
					struct omci_hw_vlan_xlate moved = cand;
					size_t other = 0;

					moved.match_cvid =
						free_host(fibre, rules, n);
					if (moved.match_cvid != 0u &&
					    slot_for(rules, n, &moved,
						     &other) == SLOT_NEW) {
						if (warn)
							OMCI_INFO(
								"me171: C-VID %u<->%u clashes with %u<->%u of another output: it goes to the host as C-VID %u",
								cand.match_cvid,
								fibre,
								rules[rival]
									.match_cvid,
								net_vid_of(
									&rules[rival]),
								moved.match_cvid);
						g_making[g_making_n++] =
							(struct omci_svc_rule){
								.fibre = fibre,
								.host = moved.match_cvid,
								.action =
									OMCI_SVC_MOVE,
								.made = true,
							};
						cand = moved;
						v = SLOT_NEW;
					}
				}

				if (v == SLOT_DUP || v == SLOT_CONFLICT) {
					if (v == SLOT_CONFLICT && warn)
						warn_shadowed(&cand,
							      &rules[rival]);
					fate_put(
						record, cur.instance, i,
						v == SLOT_DUP ?
							OMCI_VLAN_FATE_COLLAPSED :
							OMCI_VLAN_FATE_SHADOWED,
						&cand, net_vid, &rules[rival]);
					continue;
				}

				/*
				 * A discard of any priority leaves the frames
				 * of the priorities that earlier rules take from
				 * its VID to them, and takes none when they take
				 * them all.
				 */
				if (cand.kind == OMCI_HW_VLAN_DISCARD_VID &&
				    !cand.match_has_pri) {
					cand.keep_pri = taken_pri(
						rules, n, &cand, &rival);
					if (cand.keep_pri == 0xffu) {
						if (warn)
							OMCI_WARN(
								"me171: the discard of C-VID %u takes no frame, as earlier rules take each of its priorities; ignoring it",
								d.match_cvid);
						fate_put(
							record, cur.instance, i,
							OMCI_VLAN_FATE_SHADOWED,
							&cand, 0u,
							&rules[rival]);
						continue;
					}
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
						 &cand, net_vid, NULL);
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

				if (own &&
				    (sr == NULL ||
				     sr->action != OMCI_SVC_MOVE) &&
				    g_making_n < ARRAY_SIZE(g_making))
					g_making[g_making_n++] =
						(struct omci_svc_rule){
							.fibre = fibre,
							.host = cand.match_cvid,
							.action = OMCI_SVC_MOVE,
							.made = true,
						};
				if (phone && g_phoning[0] == 0u) {
					g_phoning[0] = fibre;
					g_phoning[1] =
						cand.kind == OMCI_HW_VLAN_CVID_XLATE ?
							cand.match_cvid :
							0u;
				}
				rules[n] = cand;
				g_rule_inst[n] = cur.instance;
				fate_put(record, cur.instance, i,
					 OMCI_VLAN_FATE_PROGRAMMED, &cand,
					 net_vid, NULL);
				n++;
			}
		}
	}

	if (record) {
		memcpy(g_made, g_making, g_making_n * sizeof(g_making[0]));
		g_made_n = g_making_n;
		g_phone[0] = g_phoning[0];
		g_phone[1] = g_phoning[1];
	}
	if (bad_out != NULL)
		*bad_out = bad;
	if (over_out != NULL)
		*over_out = over;
	return n;
}

/*
 * Ready the rules of the services for a programming: a person's, from the
 * backend, and the host VIDs taken, those the MIB's rules match and those
 * the person's moves give.
 */
static void svc_ready(const struct omci_mib *mib, uint16_t class_id,
		      uint8_t attr_no)
{
	struct omci_mib_cursor cur;

	g_svc_n = omci_hw()->service_rules != NULL ?
			  omci_hw()->service_rules(g_svc, ARRAY_SIZE(g_svc)) :
			  0u;
	memset(g_host_used, 0, sizeof(g_host_used));
	for (size_t k = 0; k < g_svc_n; k++)
		if (g_svc[k].action == OMCI_SVC_MOVE)
			g_host_used[g_svc[k].host / 8u] |=
				(uint8_t)(1u << (g_svc[k].host % 8u));
	for (enum omci_status w = omci_mib_walk_first(mib, &cur); w == OMCI_OK;
	     w = omci_mib_walk_next(mib, &cur)) {
		if (cur.class_id != class_id)
			continue;
		const size_t count = omci_mib_table_count(
			mib, class_id, cur.instance, attr_no);

		for (size_t j = 0; j < count; j++) {
			uint8_t row[16] = { 0 };
			struct omci_vlan_rule d;

			(void)!omci_mib_table_get(mib, class_id, cur.instance,
						  attr_no, j, row, sizeof(row));
			(void)omci_vlan_decode(row, sizeof(row), &d);
			if (d.op == OMCI_VLAN_OP_CVID_XLATE)
				g_host_used[d.match_cvid / 8u] |=
					(uint8_t)(1u << (d.match_cvid % 8u));
		}
	}
}

size_t omci_vlan_services(const struct omci_svc_rule **person,
			  const struct omci_svc_rule **made, size_t *made_n)
{
	*person = g_svc;
	*made = g_made;
	*made_n = g_made_n;
	return g_svc_n;
}

bool omci_vlan_phone(uint16_t *fibre, uint16_t *host)
{
	*fibre = g_phone[0];
	*host = g_phone[1];
	return g_phone[0] != 0u;
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

	svc_ready(mib, class_id, attr_no);

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
		    omci_hw_reject_has(&rejected, g_fate_key[i]))
			g_fate[i].fate = OMCI_VLAN_FATE_REFUSED;

	g_in_force_n = 0;
	for (size_t i = 0; i < n; i++) {
		if (omci_hw_reject_has(&rejected,
				       omci_hw_vlan_key(&rules[i]))) {
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

	if (row == NULL || len < 16)
		return false;
	for (size_t i = 0; i < g_pass_refused_n; i++)
		if (memcmp(g_pass_refused[i], row, 8) == 0)
			return true;
	if (g_rejected.n == 0)
		return false;
	(void)omci_vlan_decode(row, len, &d);

	const struct omci_hw_vlan_xlate r = hw_rule_of(&d);

	return omci_hw_reject_has(&g_rejected, omci_hw_vlan_key(&r));
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
