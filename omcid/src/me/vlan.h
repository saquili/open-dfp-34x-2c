/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * VLAN rules of the extended VLAN tagging operation (class 171, G.988
 * 9.3.13): decoding a rule of its received frame table, and programming
 * the hardware with every rule of every instance.
 */

#ifndef OMCI_ME_VLAN_H
#define OMCI_ME_VLAN_H

#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "hw/hw.h"
#include "omcid/attrs.h"
#include "omcid/ctl_proto.h"
#include "mib/store.h"

#include "omcid/status.h"

/**
 * enum omci_vlan_op - What a rule asks of the hardware.
 * @OMCI_VLAN_OP_NONE: nothing: a default rule, or one that passes frames on
 *	as they are
 * @OMCI_VLAN_OP_CVID_XLATE: replace the VID of a single-tagged frame
 * @OMCI_VLAN_OP_SVID_PUSH: add an outer tag in front of the frame's tag
 * @OMCI_VLAN_OP_UNSUPPORTED: something the hardware cannot do
 * @OMCI_VLAN_OP_CVID_PUSH: give an untagged frame a tag
 * @OMCI_VLAN_OP_DISCARD_TAGGED: drop the single-tagged frames that no other
 *	rule takes: the default rule of such frames, when it discards them
 * @OMCI_VLAN_OP_DISCARD_VID: drop the single-tagged frames of one VID, and of
 *	one priority when the filter names it
 * @OMCI_VLAN_OP_SCVID_PUSH: give an untagged frame a C-tag and an S-tag in
 *	front of it
 */
enum omci_vlan_op {
	OMCI_VLAN_OP_NONE = 0,

	OMCI_VLAN_OP_CVID_XLATE,

	OMCI_VLAN_OP_SVID_PUSH,

	OMCI_VLAN_OP_UNSUPPORTED,

	OMCI_VLAN_OP_CVID_PUSH,

	OMCI_VLAN_OP_DISCARD_TAGGED,

	OMCI_VLAN_OP_DISCARD_VID,

	OMCI_VLAN_OP_SCVID_PUSH,
};

/**
 * enum omci_vlan_reject - Why a rule cannot be programmed.
 * @OMCI_VLAN_REJECT_NONE: it can
 * @OMCI_VLAN_REJECT_ETHERTYPE: it filters on an Ethertype, and is a rule on a
 *	double-tagged frame or a discard, which take none here
 * @OMCI_VLAN_REJECT_PCP_REMARK: it sets a priority that is not carried: one
 *	taken from a tag the frame does not have, or from its DSCP
 * @OMCI_VLAN_REJECT_SHAPE: its combination of filter and treatment has no
 *	counterpart in the hardware
 * @OMCI_VLAN_REJECT_VID_RANGE: a VID outside 1 to 4094
 * @OMCI_VLAN_REJECT_DISCARD: it discards the double-tagged frames of a VID;
 *	no rule of the switch found yet takes those alone
 * @OMCI_VLAN_REJECT_ETHERTYPE_CODE: its filter on the Ethertype has a code
 *	G.988 reserves
 * @OMCI_VLAN_REJECT_ETHERTYPE_PASS: it leaves the untagged frames of an
 *	Ethertype as they are, ahead of a rule that tags them; no rule of the
 *	switch found yet leaves a frame as it is ahead of one that tags it
 */
enum omci_vlan_reject {
	OMCI_VLAN_REJECT_NONE = 0,
	OMCI_VLAN_REJECT_ETHERTYPE,
	OMCI_VLAN_REJECT_PCP_REMARK,
	OMCI_VLAN_REJECT_SHAPE,
	OMCI_VLAN_REJECT_VID_RANGE,
	OMCI_VLAN_REJECT_DISCARD,
	OMCI_VLAN_REJECT_ETHERTYPE_CODE,
	OMCI_VLAN_REJECT_ETHERTYPE_PASS,
};

/**
 * struct omci_vlan_rule - A decoded rule.
 * @op: what it asks of the hardware
 * @reject: why it cannot be programmed, when @op is unsupported
 * @match_cvid: the VID the rule matches, 0 for an untagged frame; of a
 *	discard, the VID of the frames it drops
 * @assign_cvid: the VID that replaces it, for a translation, or of the C-tag a
 *	C-VID push or a C- and S-VID push adds
 * @assign_svid: the VID of the added outer tag, for an S-VID push or a C- and
 *	S-VID push
 * @assign_pri: the priority of the tag written, the outer one of two, 0 to 7,
 *	%OMCI_HW_VLAN_PRI_KEEP or %OMCI_HW_VLAN_PRI_COPY
 * @match_has_pri: the rule takes only the frames whose tag has priority
 *	@match_pri, which its filter names
 * @match_pri: that priority, 0 to 7
 * @inner_has_pri: the rule gives the C-tag behind its S-tag priority
 *	@inner_pri: the C-tag a C- and S-VID push adds, or the frame's own
 *	C-tag where an S-VID push changes its priority
 * @inner_pri: that priority, 0 to 7
 * @filter_outer_pri: the fields of the rule, as the row holds them
 * @filter_outer_vid: see @filter_outer_pri
 * @filter_outer_tpid: see @filter_outer_pri
 * @filter_inner_pri: see @filter_outer_pri
 * @filter_inner_vid: see @filter_outer_pri
 * @filter_inner_tpid: see @filter_outer_pri
 * @filter_ethertype: see @filter_outer_pri; an &enum omci_hw_ethertype
 * @tags_to_remove: see @filter_outer_pri
 * @treat_outer_pri: see @filter_outer_pri
 * @treat_outer_vid: see @filter_outer_pri
 * @treat_outer_tpid: see @filter_outer_pri
 * @treat_inner_pri: see @filter_outer_pri
 * @treat_inner_vid: see @filter_outer_pri
 * @treat_inner_tpid: see @filter_outer_pri
 */
struct omci_vlan_rule {
	enum omci_vlan_op op;
	enum omci_vlan_reject reject;

	uint16_t match_cvid;
	uint16_t assign_cvid;
	uint16_t assign_svid;
	uint8_t assign_pri;
	bool match_has_pri;
	uint8_t match_pri;
	bool inner_has_pri;
	uint8_t inner_pri;

	uint8_t filter_outer_pri;
	uint16_t filter_outer_vid;
	uint8_t filter_outer_tpid;
	uint8_t filter_inner_pri;
	uint16_t filter_inner_vid;
	uint8_t filter_inner_tpid;
	uint8_t filter_ethertype;

	uint8_t tags_to_remove;
	uint8_t treat_outer_pri;
	uint16_t treat_outer_vid;
	uint8_t treat_outer_tpid;
	uint8_t treat_inner_pri;
	uint16_t treat_inner_vid;
	uint8_t treat_inner_tpid;
};

/**
 * omci_vlan_decode() - Decode a rule of the received frame table.
 * @row: the 16 bytes of the rule
 * @len: their number
 * @out: the rule
 *
 * Return: false when the row is too short to be a rule.
 */
bool omci_vlan_decode(const uint8_t *row, size_t len,
		      struct omci_vlan_rule *out);

/**
 * omci_vlan_reject_name() - Name a reason, for a log line.
 * @r: the reason
 *
 * Return: a constant string.
 */
__must_check const char *omci_vlan_reject_name(enum omci_vlan_reject r);

/**
 * omci_vlan_program() - Program the hardware with every rule of the MIB.
 * @mib: the MIB
 * @class_id: the class whose instances hold the rules (171)
 * @attr_no: the attribute of the rule table (6)
 * @refused: the number of rules left out; may be NULL
 *
 * Rules that the hardware cannot express, or that would match the same
 * VID as an earlier rule, are left out. When the rules outnumber the
 * places of the classifier, the rules already in force keep theirs, as
 * long as that does not program fewer rules than taking them in order.
 *
 * Return: the status of the hardware.
 */
enum omci_status omci_vlan_program(const struct omci_mib *mib,
				   uint16_t class_id, uint8_t attr_no,
				   unsigned int *refused);

struct omci_svc_rule;

/**
 * omci_vlan_services() - The rules of the services at the last programming.
 * @person: set to a person's rules, as the backend gave them
 * @made: set to the moves the programming made for clashes of two outputs
 * @made_n: set to their number
 *
 * Return: the number of a person's rules.
 */
size_t omci_vlan_services(const struct omci_svc_rule **person,
			  const struct omci_svc_rule **made, size_t *made_n);

/**
 * omci_vlan_phone() - The VLAN of an IP host's service, voice as a rule, at
 *	the last programming: of its first rule in force.
 * @fibre: set to its VLAN on the fibre, 0 without one
 * @host: set to its VLAN towards the host, 0 for untagged
 *
 * A rule of an IP host that tags its untagged frames is given a host VLAN of
 * its own, the fibre's when free (me/svcrule.h), which the moves made list.
 *
 * Return: whether there is one.
 */
bool omci_vlan_phone(uint16_t *fibre, uint16_t *host);

/**
 * omci_vlan_row_refused() - Whether the hardware refused a rule at the last
 *	programming, or the programming left it out as one that the hardware
 *	cannot carry where it stands in the list.
 * @row: the rule
 * @len: its size
 *
 * Return: true when it did.
 */
__must_check bool omci_vlan_row_refused(const uint8_t *row, size_t len);

/**
 * omci_vlan_has_room() - Whether a new rule would find a place.
 * @mib: the MIB
 * @class_id: the class of the rules
 * @attr_no: the attribute of the rule table
 * @row: the new rule
 * @len: its size
 *
 * Return: false only for a programmable rule that needs a place of its own
 * when the classifier is full.
 */
__must_check bool omci_vlan_has_room(const struct omci_mib *mib,
				     uint16_t class_id, uint8_t attr_no,
				     const uint8_t *row, size_t len);

/**
 * enum omci_vlan_fate - What became of a rule at the last programming.
 * @OMCI_VLAN_FATE_PROGRAMMED: it is in the hardware
 * @OMCI_VLAN_FATE_COLLAPSED: an earlier rule does the same thing
 * @OMCI_VLAN_FATE_SHADOWED: an earlier rule matches its VID or assigns the
 *	same one, so a frame would match two rules in one direction
 * @OMCI_VLAN_FATE_OVER_BUDGET: the classifier had no place left
 * @OMCI_VLAN_FATE_UNSUPPORTED: the hardware cannot express it
 * @OMCI_VLAN_FATE_INERT: it needs nothing from the hardware
 * @OMCI_VLAN_FATE_REFUSED: the hardware would not take it
 * @OMCI_VLAN_FATE_LOCAL: a person's rule of the stick keeps its service off
 *	the host, or off this output (me/svcrule.h)
 *
 * The values are the ones of the control protocol.
 */
enum omci_vlan_fate {
	OMCI_VLAN_FATE_PROGRAMMED = 0,
	OMCI_VLAN_FATE_COLLAPSED = 1,
	OMCI_VLAN_FATE_SHADOWED = 2,
	OMCI_VLAN_FATE_OVER_BUDGET = 3,
	OMCI_VLAN_FATE_UNSUPPORTED = 4,
	OMCI_VLAN_FATE_INERT = 5,
	OMCI_VLAN_FATE_REFUSED = 6,
	OMCI_VLAN_FATE_LOCAL = 7,
};

static_assert((int)OMCI_VLAN_FATE_PROGRAMMED ==
		      (int)OMCI_CTL_VLAN_FATE_PROGRAMMED,
	      "wire");
static_assert((int)OMCI_VLAN_FATE_COLLAPSED ==
		      (int)OMCI_CTL_VLAN_FATE_COLLAPSED,
	      "wire");
static_assert((int)OMCI_VLAN_FATE_SHADOWED == (int)OMCI_CTL_VLAN_FATE_SHADOWED,
	      "wire");
static_assert((int)OMCI_VLAN_FATE_OVER_BUDGET ==
		      (int)OMCI_CTL_VLAN_FATE_OVER_BUDGET,
	      "wire");
static_assert((int)OMCI_VLAN_FATE_UNSUPPORTED ==
		      (int)OMCI_CTL_VLAN_FATE_UNSUPPORTED,
	      "wire");
static_assert((int)OMCI_VLAN_FATE_INERT == (int)OMCI_CTL_VLAN_FATE_INERT,
	      "wire");
static_assert((int)OMCI_VLAN_FATE_REFUSED == (int)OMCI_CTL_VLAN_FATE_REFUSED,
	      "wire");
static_assert((int)OMCI_VLAN_FATE_LOCAL == (int)OMCI_CTL_VLAN_FATE_LOCAL,
	      "wire");

/**
 * omci_vlan_fate_name() - Name a fate, for a log line.
 * @f: the fate
 *
 * Return: a constant string.
 */
__must_check const char *omci_vlan_fate_name(enum omci_vlan_fate f);

/**
 * struct omci_vlan_fate_rec - The fate of one rule.
 * @instance: the instance that holds it
 * @row: its place in the table
 * @fate: an &enum omci_vlan_fate
 * @match_cvid: the VID it matches
 * @net_vid: the VID it gives the frame towards the network
 * @rival_match: for a collapsed or shadowed rule, the earlier rule's match
 * @rival_net: and its network VID
 */
struct omci_vlan_fate_rec {
	uint16_t instance;
	uint8_t row;
	uint8_t fate;
	uint16_t match_cvid;
	uint16_t net_vid;
	uint16_t rival_match;
	uint16_t rival_net;
};

/* Room for the fates of eight instances full of rules. */
#define OMCI_VLAN_FATE_INSTANCES 8u
#define OMCI_VLAN_FATE_MAX (OMCI_VLAN_FATE_INSTANCES * OMCI_HW_VLAN_MAX_RULES)

/**
 * omci_vlan_fates() - The fates of the last programming.
 * @out: the records
 * @overflow: the records that did not fit; may be NULL
 *
 * Return: the number of records.
 */
size_t omci_vlan_fates(const struct omci_vlan_fate_rec **out,
		       unsigned int *overflow);

/**
 * omci_vlan_in_force() - The rules in force, in the order the hardware
 *	takes them: a frame is changed by the first that matches it.
 * @out: the rules
 *
 * Return: how many there are.
 */
size_t omci_vlan_in_force(const struct omci_hw_vlan_xlate **out);

/**
 * omci_vlan_per_pri_stale() - Whether the rules in force take the priorities
 *	apart where the GEM ports now divide them.
 *
 * A translation that keeps any priority, into a VLAN whose GEM ports divide
 * its priorities, takes each priority apart (struct omci_hw_vlan_xlate,
 * @per_pri). Programming the GEM ports or the bridges can change which
 * VLANs those are; the rules are then to be programmed again.
 *
 * Return: true when a rule in force takes them apart and no longer needs
 * to, or needs to and does not.
 */
bool omci_vlan_per_pri_stale(void);

/**
 * omci_vlan_forget_in_force() - Forget which rules are in force, as after a MIB reset.
 */
void omci_vlan_forget_in_force(void);

/**
 * omci_vlan_last_reconcile() - How the last programming chose its rules.
 * @yielded: how often the rules in force had to give way; may be NULL
 * @preferred: the rules programmed when those in force were kept; may be NULL
 *
 * Return: the rules programmed when they were taken in order.
 */
size_t omci_vlan_last_reconcile(unsigned int *yielded, size_t *preferred);

#endif
