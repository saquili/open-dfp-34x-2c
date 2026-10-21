// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The multicast agent of agent.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hw/hw.h"
#include "mcast/acl.h"
#include "mcast/agent.h"
#include "mcast/message.h"
#include "mcast/proxy.h"
#include "mcast/report.h"
#include "me/bridge.h"
#include "me/gem.h"
#include "me/vlan.h"
#include "omcid/array_size.h"
#include "omcid/byteorder.h"
#include "sys/log.h"
#include "sys/publish.h"

/*
 * The active groups, one per group, source and client; the VLANs of the
 * latest joins; the preview counts; the static groups. A service package
 * of none is the profile pointer's.
 */
#define MAX_ACTIVE 64u
#define MAX_SEEN 32u
#define MAX_COUNTS 32u
#define MAX_STATIC 32u
#define NO_PACKAGE 0xffffu
#define ADDR OMCI_MCAST_ADDR_LEN

/* A day, after which the preview counts start again. */
#define DAY_SEC 86400u

/* The most messages one service reads. */
#define FRAMES_PER_SERVICE 64u

/*
 * The seconds a group the bridge forwards may go without a frame downstream
 * before it is lost.
 */
#define LOST_SEC 10u

/*
 * The latest events and refused joins kept for the stick's panel
 * (omci_mcast_publish()), and the most seconds a test of a group lasts
 * (omci_mcast_test_start()), beyond which the agent ends it itself when
 * nobody did after a few seconds more.
 */
#define MAX_EVENTS 32u
#define MAX_REFUSED 16u
#define TEST_MAX_SEC 30u
#define TEST_GRACE_SEC 5u

/*
 * The sizes of the rows of the active group lists (G.988 9.3.29) and of the
 * lost groups list (9.3.27).
 */
enum { ROW_V4 = 24, ROW_V6 = 58, ROW_LOST = 10 };

/* The VLAN of a join: known when it came in a message. */
struct asker {
	bool known;
	bool tagged;
	uint16_t vid;
	uint8_t pcp;
};

/*
 * An active group: what the bridge forwards for a client, the VLAN of its
 * joins, the time of the first and of the latest, the imputed bandwidth and
 * the package, and when a preview ends; @held while the bridge has it.
 */
struct active {
	struct asker asker;
	uint8_t group[ADDR];
	uint8_t source[ADDR];
	uint8_t client[ADDR];
	uint64_t since;
	uint64_t joined;
	uint32_t bandwidth;
	uint16_t package;
	bool preview;
	uint64_t preview_end;
	bool held;
	/*
	 * The VLAN the OLT sends the group in, when its row names one, and
	 * whether the agent tried to file the group in the bridge under it,
	 * and did (see alias_sync()).
	 */
	uint16_t olt_vlan;
	uint16_t mdb_vid;
	bool alias_tried;
	bool aliased;
};

/* The VLAN of the latest join of a group from a source. */
struct seen {
	uint8_t group[ADDR];
	uint8_t source[ADDR];
	struct asker asker;
};

/* How often a group was previewed, the latest start, and when the count starts again. */
struct count {
	uint8_t group[ADDR];
	uint16_t n;
	uint64_t last;
	uint64_t reset_at;
};

/*
 * A group whose downstream bytes the backend counts: the count last read and
 * when, the bits a second between the last two reads, when the count last
 * grew or the counting began, and whether the group is lost.
 */
struct watch {
	uint8_t group[ADDR];
	uint64_t bytes;
	uint64_t read_at;
	uint64_t rate;
	bool rated;
	uint64_t since;
	bool lost;
};

/* What happened, for the stick's panel. */
enum event_kind { EV_JOIN, EV_LEAVE, EV_REJECT, EV_QUERY, EV_LOST, EV_BACK };

static const char *const g_event[] = { "join",	"leave", "reject",
				       "query", "lost",	 "back" };

/* An event: when, its group (all zero for none) and VLAN (0 for none). */
struct event {
	uint64_t at;
	uint8_t group[ADDR];
	uint16_t vlan;
	uint8_t kind;
};

/* A join the policy refused: when, the group, who asked, its VLAN, the verdict. */
struct refusal {
	uint64_t at;
	uint8_t group[ADDR];
	uint8_t client[ADDR];
	uint16_t vlan;
	uint8_t verdict;
};

struct omci_mcast {
	struct omci_mib *mib;
	uint64_t now;

	struct active active[MAX_ACTIVE];
	size_t n_active;
	struct seen seen[MAX_SEEN];
	size_t seen_next;
	struct count counts[MAX_COUNTS];
	size_t n_counts;
	struct omci_hw_mcast_group statics[MAX_STATIC];
	size_t n_statics;

	struct omci_hw_mcast_port port;
	bool port_set;
	uint64_t minute_at;

	/* The active entries the limits count: all, or while rechecking those before. */
	size_t upto;

	/*
	 * Whether the switch traps the subscriber's IGMP and MLD for the agent
	 * to send upstream, and the messages sent in the current second.
	 */
	bool forwarding;
	uint64_t rate_sec;
	uint32_t rate_used;

	/* The downstream tags of the multicast VLANs last set. */
	struct omci_hw_mcast_ds ds[OMCI_HW_MCAST_DS_MAX];
	size_t n_ds;
	bool ds_set;

	/* The groups counted, and whether a group could not be. */
	struct watch watches[OMCI_HW_MCAST_WATCH_MAX];
	size_t n_watches;
	bool watch_short;

	/*
	 * The proxy of IGMP function 1 and 2, and the function the profile
	 * asked for and the one in force, last told.
	 */
	struct omci_mcast_proxy proxy;
	int function_asked;
	int function_in_force;

	/*
	 * What the stick's panel shows (omci_mcast_publish()): the latest
	 * events and refused joins, in rings; the counts since the start, of
	 * new joins, leaves, refusals, the OLT's queries and the joins that
	 * answered them; the OLT's latest query and its version.
	 */
	struct event events[MAX_EVENTS];
	size_t event_next;
	size_t n_events;
	struct refusal refused[MAX_REFUSED];
	size_t refused_next;
	size_t n_refused;
	struct {
		uint32_t joins;
		uint32_t leaves;
		uint32_t rejected;
		uint32_t queries;
		uint32_t reports;
	} tally;
	bool queried;
	uint64_t query_at;
	uint8_t query_version;

	/*
	 * The priority the agent last gave a message upstream, -1 before
	 * one, and the messages that went with the subscriber's priority
	 * where the operator's rules took another (upstream_frame()).
	 */
	int pcp_given;
	uint32_t pcp_dropped;

	/*
	 * A test of a group (omci_mcast_test_start()): whether one runs, its
	 * group and VLAN, when the agent ends it, the count of the group's
	 * bytes when it began, the bytes it counted when it ended, and
	 * whether the agent started counting the group for it.
	 */
	struct {
		bool on;
		uint8_t group[ADDR];
		uint16_t vid;
		uint64_t end;
		uint64_t base;
		uint64_t bytes;
		bool counted;
	} test;
};

static struct omci_mcast g_mcast;
static bool g_mcast_taken;

/* What a join gets: its bandwidth and package, and a preview's length. */
struct grant {
	uint32_t bandwidth;
	uint16_t package;
	bool preview;
	uint16_t preview_length;
	bool preview_resets;
	/* The VLAN the row that allowed the join names: where the OLT sends the group. */
	uint16_t vlan;
};

/*
 * What becomes of a join: allowed, allowed although over the bandwidth, or
 * refused, for the access control lists, the groups, the bandwidth or the
 * previews.
 */
enum verdict {
	ALLOWED = 0,
	ALLOWED_OVER,
	DENIED_ACL,
	DENIED_GROUPS,
	DENIED_BANDWIDTH,
	DENIED_PREVIEW,
};

static const char *const g_verdict[] = {
	"allowed",
	"allowed over the bandwidth",
	"not in the access control lists",
	"over the most groups",
	"over the bandwidth",
	"previewed too often",
};

/* The words of the refusals, as the stick's panel takes them. */
static const char *const g_reason[] = {
	"", "", "not_allowed", "max_groups", "max_bandwidth", "preview_over",
};

/*
 * How a join is judged: as a new group, as one already active, which is
 * not counted against the limits again, or a check of an active group
 * after the policy changed, which is but whose preview goes on.
 */
enum judging { JUDGE_NEW, JUDGE_RENEW, JUDGE_RECHECK };

static uint32_t attr_be(const struct omci_mib *mib, uint16_t class_id,
			uint16_t instance, uint8_t attr_no, size_t size)
{
	uint8_t b[4] = { 0 };
	size_t len = 0;
	uint32_t v = 0;

	(void)!omci_mib_attr_get(mib, class_id, instance, attr_no, b, size,
				 &len);
	for (size_t i = 0; i < size; i++)
		v = v << 8 | b[i];
	return v;
}

/*
 * Whether an instance of the multicast subscriber config info or monitor
 * is linked to the subscriber side: through its ME type, to a bridge port
 * that leads to the UNI or a VEIP, or to a mapper.
 */
static bool linked(const struct omci_mib *mib, uint16_t class_id,
		   uint16_t instance)
{
	if (attr_be(mib, class_id, instance, 1, 1) != 0u)
		return omci_mib_exists(mib, OMCI_BRIDGE_MAPPER_CLASS_ID,
				       instance);
	if (!omci_mib_exists(mib, OMCI_BRIDGE_PORT_CLASS_ID, instance))
		return false;

	const uint32_t tp = attr_be(mib, OMCI_BRIDGE_PORT_CLASS_ID, instance,
				    OMCI_BRIDGE_PORT_ATTR_TPTYPE, 1);

	return tp == OMCI_BRIDGE_TP_PPTP_ETH || tp == OMCI_BRIDGE_TP_VEIP;
}

/* The multicast subscriber config info of the subscriber side, if any. */
static bool subscriber(const struct omci_mib *mib, uint16_t *out)
{
	struct omci_mib_cursor cur;

	for (enum omci_status rc = omci_mib_walk_first(mib, &cur);
	     rc == OMCI_OK; rc = omci_mib_walk_next(mib, &cur))
		if (cur.class_id == OMCI_MCAST_SUBSCRIBER &&
		    linked(mib, cur.class_id, cur.instance)) {
			*out = cur.instance;
			return true;
		}
	return false;
}

/* Whether the VID of a service package or preview group takes a join. */
static bool vid_takes(uint16_t vid, const struct asker *a)
{
	if (!a->known || vid == OMCI_MCAST_VID_ANY)
		return true;
	if (vid == OMCI_MCAST_VID_UNTAGGED)
		return !a->tagged;
	if (vid == OMCI_MCAST_VID_TAGGED)
		return a->tagged;
	return a->tagged && a->vid == vid;
}

/*
 * The service packages a join may fall in, in row key order: those whose
 * VID takes it; without packages, the one that the profile pointer makes.
 */
struct candidates {
	size_t n;
	struct omci_mcast_package pkg[OMCI_MCAST_SUBSCRIBER_ROWS];
};

static void candidates_of(const struct omci_mib *mib, uint16_t sub,
			  const struct asker *a, struct candidates *c)
{
	struct omci_mcast_package all[OMCI_MCAST_SUBSCRIBER_ROWS];
	const size_t n =
		omci_mcast_packages_read(mib, sub, all, ARRAY_SIZE(all));

	c->n = 0;
	if (n == 0u) {
		memset(&c->pkg[0], 0, sizeof(c->pkg[0]));
		c->pkg[0].key = NO_PACKAGE;
		c->pkg[0].profile =
			(uint16_t)attr_be(mib, OMCI_MCAST_SUBSCRIBER, sub,
					  OMCI_MCAST_SUBSCRIBER_PROFILE, 2);
		c->n = 1;
		return;
	}
	for (size_t i = 0; i < n; i++)
		if (vid_takes(all[i].vid, a))
			c->pkg[c->n++] = all[i];
}

/*
 * The first package whose profile's dynamic list has a row for the group:
 * a row of groups authorized in full, or of groups for preview only. c->n
 * when none has.
 */
static size_t find_row(const struct omci_mib *mib, const struct candidates *c,
		       const uint8_t *g, const uint8_t *s, bool preview,
		       struct omci_mcast_acl_row *row)
{
	struct omci_mcast_acl_row rows[OMCI_MCAST_ACL_ROWS];

	for (size_t k = 0; k < c->n; k++) {
		const size_t n = omci_mcast_acl_read(
			mib, c->pkg[k].profile, OMCI_MCAST_PROFILE_DYNAMIC_ACL,
			rows, ARRAY_SIZE(rows));

		for (size_t i = 0; i < n; i++)
			if ((rows[i].preview_length != 0u) == preview &&
			    omci_mcast_acl_matches(&rows[i], g, s)) {
				*row = rows[i];
				return k;
			}
	}
	return c->n;
}

/* Whether an allowed preview group of the subscriber takes the join. */
static bool preview_allowed(const struct omci_mib *mib, uint16_t sub,
			    const uint8_t *g, const uint8_t *s,
			    const struct asker *a)
{
	struct omci_mcast_preview pv[OMCI_MCAST_SUBSCRIBER_ROWS];
	const size_t n = omci_mcast_previews_read(mib, sub, pv, ARRAY_SIZE(pv));

	for (size_t i = 0; i < n; i++)
		if (memcmp(pv[i].group, g, ADDR) == 0 &&
		    (pv[i].any_source || memcmp(pv[i].source, s, ADDR) == 0) &&
		    vid_takes(pv[i].vlan_uni == 0u ? OMCI_MCAST_VID_UNTAGGED :
						     pv[i].vlan_uni,
			      a))
			return true;
	return false;
}

/* The preview count of a group; with @make, a new one, in the oldest's place when full. */
static struct count *count_of(struct omci_mcast *m, const uint8_t *g, bool make)
{
	size_t oldest = 0;

	for (size_t i = 0; i < m->n_counts; i++) {
		if (memcmp(m->counts[i].group, g, ADDR) == 0)
			return &m->counts[i];
		if (m->counts[i].last < m->counts[oldest].last)
			oldest = i;
	}
	if (!make)
		return NULL;
	if (m->n_counts < MAX_COUNTS)
		oldest = m->n_counts++;
	memset(&m->counts[oldest], 0, sizeof(m->counts[0]));
	memcpy(m->counts[oldest].group, g, ADDR);
	return &m->counts[oldest];
}

/* Whether a group for preview only may be previewed again (G.988 9.3.27). */
static bool preview_due(struct omci_mcast *m,
			const struct omci_mcast_acl_row *row, const uint8_t *g)
{
	const struct count *c = count_of(m, g, false);

	if (c == NULL || c->n == 0u)
		return true;
	if (row->preview_repeat_count != 0u &&
	    c->n >= row->preview_repeat_count)
		return false;
	return m->now - c->last >= row->preview_repeat_time;
}

static bool same_channel(const struct active *x, const uint8_t *g,
			 const uint8_t *s)
{
	return memcmp(x->group, g, ADDR) == 0 &&
	       memcmp(x->source, s, ADDR) == 0;
}

/*
 * The groups in use and their bandwidth, each group and source once: of
 * every package, or of one; the group @g from @s left out, and while the
 * groups are rechecked, those not rechecked yet.
 */
static void in_use(const struct omci_mcast *m, bool every, uint16_t package,
		   const uint8_t *g, const uint8_t *s, unsigned int *groups,
		   uint64_t *bandwidth)
{
	*groups = 0;
	*bandwidth = 0;
	for (size_t i = 0; i < m->n_active && i < m->upto; i++) {
		const struct active *x = &m->active[i];
		bool first = true;

		if ((!every && x->package != package) || same_channel(x, g, s))
			continue;
		for (size_t j = 0; j < i; j++)
			if (same_channel(&m->active[j], x->group, x->source))
				first = false;
		if (!first)
			continue;
		(*groups)++;
		*bandwidth += x->bandwidth;
	}
}

/* The limits of the subscriber and of the package (G.988 9.3.28). */
static enum verdict limits(const struct omci_mcast *m,
			   const struct omci_mib *mib, uint16_t sub,
			   const struct omci_mcast_package *pkg,
			   const uint8_t *g, const uint8_t *s,
			   const struct grant *gr)
{
	const uint32_t max_groups = attr_be(mib, OMCI_MCAST_SUBSCRIBER, sub,
					    OMCI_MCAST_SUBSCRIBER_MAX_GROUPS,
					    2);
	const uint32_t max_bw = attr_be(mib, OMCI_MCAST_SUBSCRIBER, sub,
					OMCI_MCAST_SUBSCRIBER_MAX_BANDWIDTH, 4);
	unsigned int groups;
	uint64_t bw;
	bool over;

	in_use(m, true, 0, g, s, &groups, &bw);
	if (max_groups != 0u && groups >= max_groups)
		return DENIED_GROUPS;
	over = max_bw != 0u && bw + gr->bandwidth > max_bw;

	in_use(m, false, gr->package, g, s, &groups, &bw);
	if (pkg->max_groups != 0u && groups >= pkg->max_groups)
		return DENIED_GROUPS;
	over = over || (pkg->max_bandwidth != 0u &&
			bw + gr->bandwidth > pkg->max_bandwidth);

	if (!over)
		return ALLOWED;
	return attr_be(mib, OMCI_MCAST_SUBSCRIBER, sub,
		       OMCI_MCAST_SUBSCRIBER_ENFORCE, 1) != 0u ?
		       DENIED_BANDWIDTH :
		       ALLOWED_OVER;
}

/*
 * Judge a join of group @g from source @s. Without a multicast subscriber
 * config info of the subscriber side every join is allowed (G.988 9.3.28).
 * The search goes through the groups the profiles authorize in full, then
 * the allowed preview groups, then the groups for preview only.
 */
static enum verdict judge(struct omci_mcast *m, const struct omci_mib *mib,
			  const uint8_t *g, const uint8_t *s,
			  const struct asker *a, enum judging how,
			  struct grant *out)
{
	static const struct omci_mcast_package none = { .key = NO_PACKAGE };
	const struct omci_mcast_package *pkg;
	struct omci_mcast_acl_row row;
	struct candidates c;
	uint16_t sub;
	size_t k;

	memset(out, 0, sizeof(*out));
	out->package = NO_PACKAGE;
	if (!subscriber(mib, &sub))
		return ALLOWED;

	candidates_of(mib, sub, a, &c);
	k = find_row(mib, &c, g, s, false, &row);
	if (k == c.n) {
		const size_t p = find_row(mib, &c, g, s, true, &row);

		if (preview_allowed(mib, sub, g, s, a)) {
			k = p;
			if (p == c.n)
				row.bandwidth = 0;
		} else if (p < c.n) {
			if (how == JUDGE_NEW && !preview_due(m, &row, g))
				return DENIED_PREVIEW;
			k = p;
			out->preview = true;
			out->preview_length = row.preview_length;
			out->preview_resets = row.preview_reset_time >= 1u &&
					      row.preview_reset_time <= 24u;
		} else {
			return DENIED_ACL;
		}
	}

	/* An allowed preview group in no package's list has no package. */
	pkg = k < c.n ? &c.pkg[k] : &none;
	out->bandwidth = row.bandwidth;
	out->package = pkg->key;
	out->vlan = 0u;
	if (k < c.n && row.vlan >= 1u && row.vlan <= 4094u)
		out->vlan = row.vlan;
	return how == JUDGE_RENEW ? ALLOWED :
				    limits(m, mib, sub, pkg, g, s, out);
}

/*
 * Add @add to a counter (@attr 3 or 4) of each multicast subscriber monitor
 * of the subscriber side, or with @set write the current bandwidth.
 */
static void monitors(struct omci_mib *mib, uint8_t attr, uint32_t value,
		     bool set)
{
	struct omci_mib_cursor cur;

	for (enum omci_status rc = omci_mib_walk_first(mib, &cur);
	     rc == OMCI_OK; rc = omci_mib_walk_next(mib, &cur)) {
		uint8_t b[4];

		if (cur.class_id != OMCI_MCAST_MONITOR ||
		    !linked(mib, cur.class_id, cur.instance))
			continue;
		omci_be32_store(b, set ? value :
					 attr_be(mib, cur.class_id,
						 cur.instance, attr, 4) +
						   value);
		(void)!omci_mib_attr_set(mib, cur.class_id, cur.instance, attr,
					 b, sizeof(b));
	}
}

/* The bandwidth of the active groups, each group and source once. */
static void report_bandwidth(struct omci_mcast *m)
{
	static const uint8_t none[ADDR];
	unsigned int groups;
	uint64_t bw;

	in_use(m, true, 0, none, none, &groups, &bw);
	monitors(m->mib, OMCI_MCAST_MONITOR_BANDWIDTH,
		 bw > UINT32_MAX ? UINT32_MAX : (uint32_t)bw, true);
}

static void note_vlan(struct omci_mcast *m, const uint8_t *g, const uint8_t *s,
		      const struct asker *a)
{
	struct seen *e = &m->seen[m->seen_next];

	m->seen_next = (m->seen_next + 1u) % MAX_SEEN;
	memcpy(e->group, g, ADDR);
	memcpy(e->source, s, ADDR);
	e->asker = *a;
}

/*
 * The VLAN of the latest join of a group from a source, or when none was
 * from that source, of a join of the group from any: a bridge that snoops a
 * source-specific join holds the group alone, and the join it came of named
 * the source. Unknown when none was heard.
 */
static struct asker vlan_noted(const struct omci_mcast *m, const uint8_t *g,
			       const uint8_t *s)
{
	struct asker a = { .known = false }, any = { .known = false };

	for (size_t k = 0; k < MAX_SEEN; k++) {
		const struct seen *e = &m->seen[(m->seen_next + k) % MAX_SEEN];

		if (!e->asker.known || memcmp(e->group, g, ADDR) != 0)
			continue;
		any = e->asker;
		if (memcmp(e->source, s, ADDR) == 0)
			a = e->asker;
	}
	return a.known ? a : any;
}

/* The VLAN of a join, for the panel: its VID when tagged, 0 otherwise. */
static uint16_t vlan_of(const struct asker *a)
{
	return a->known && a->tagged ? a->vid : 0u;
}

/* Keep an event; @g NULL for none. */
static void note(struct omci_mcast *m, enum event_kind kind, const uint8_t *g,
		 uint16_t vlan)
{
	struct event *e = &m->events[m->event_next];

	m->event_next = (m->event_next + 1u) % MAX_EVENTS;
	if (m->n_events < MAX_EVENTS)
		m->n_events++;
	memset(e, 0, sizeof(*e));
	e->at = m->now;
	e->kind = (uint8_t)kind;
	e->vlan = vlan;
	if (g != NULL)
		memcpy(e->group, g, ADDR);
}

/* Keep a join the policy refused, from @client (all zero when not known). */
static void refuse(struct omci_mcast *m, const uint8_t *g,
		   const uint8_t *client, uint16_t vlan, enum verdict v)
{
	struct refusal *r = &m->refused[m->refused_next];

	m->refused_next = (m->refused_next + 1u) % MAX_REFUSED;
	if (m->n_refused < MAX_REFUSED)
		m->n_refused++;
	r->at = m->now;
	memcpy(r->group, g, ADDR);
	memcpy(r->client, client, ADDR);
	r->vlan = vlan;
	r->verdict = (uint8_t)v;
	m->tally.rejected++;
	note(m, EV_REJECT, g, vlan);
}

/*
 * Whether a refusal of @g was kept this second: the bridge snoops a join the
 * agent refused as it heard it, and takes it back at once (look()).
 */
static bool refused_now(const struct omci_mcast *m, const uint8_t *g)
{
	bool found = false;

	for (size_t k = 0; k < m->n_refused; k++)
		found = found || (m->refused[k].at == m->now &&
				  memcmp(m->refused[k].group, g, ADDR) == 0);
	return found;
}

/*
 * The entry filed under the VLAN the OLT sends a channel in (alias_sync())
 * goes with the channel's last active entry.
 */
static void alias_del(struct omci_mcast *m, size_t i)
{
	const struct active *x = &m->active[i];
	struct omci_hw_mcast_group g = { .vid = x->olt_vlan,
					 .permanent = true,
					 .alias = true };

	if (!x->aliased)
		return;
	for (size_t j = 0; j < m->n_active; j++)
		if (j != i && m->active[j].aliased &&
		    same_channel(&m->active[j], x->group, x->source))
			return;
	memcpy(g.group, x->group, ADDR);
	memcpy(g.source, x->source, ADDR);
	(void)!omci_hw()->mcast_group_del(&g);
}

static void drop_entry(struct omci_mcast *m, size_t i)
{
	alias_del(m, i);
	memmove(&m->active[i], &m->active[i + 1u],
		(m->n_active - i - 1u) * sizeof(m->active[0]));
	m->n_active--;
}

/*
 * Note a join that was allowed: refresh the client's entry, or make one, a
 * group already active taking its bandwidth, package and preview from the
 * others. A new preview is counted.
 */
static struct active *admit(struct omci_mcast *m, const uint8_t *g,
			    const uint8_t *s, const uint8_t *client,
			    const struct asker *a, const struct grant *gr)
{
	struct active *x = NULL, *peer = NULL;

	for (size_t i = 0; i < m->n_active; i++) {
		if (!same_channel(&m->active[i], g, s))
			continue;
		peer = &m->active[i];
		if (memcmp(peer->client, client, ADDR) == 0)
			x = peer;
	}
	if (x == NULL) {
		if (m->n_active == MAX_ACTIVE) {
			OMCI_WARN(
				"mcast: %u active groups already; one more is not listed",
				MAX_ACTIVE);
			return NULL;
		}
		x = &m->active[m->n_active++];
		memset(x, 0, sizeof(*x));
		memcpy(x->group, g, ADDR);
		memcpy(x->source, s, ADDR);
		memcpy(x->client, client, ADDR);
		x->since = m->now;
		if (peer != NULL) {
			x->bandwidth = peer->bandwidth;
			x->package = peer->package;
			x->preview = peer->preview;
			x->preview_end = peer->preview_end;
			x->held = peer->held;
			x->olt_vlan = peer->olt_vlan;
			x->mdb_vid = peer->mdb_vid;
			x->alias_tried = peer->alias_tried;
			x->aliased = peer->aliased;
		} else {
			x->olt_vlan = gr->vlan;
			x->bandwidth = gr->bandwidth;
			x->package = gr->package;
			x->preview = gr->preview;
			x->preview_end = m->now + gr->preview_length;
		}
		if (peer == NULL && gr->preview) {
			struct count *c = count_of(m, g, true);

			c->n++;
			c->last = m->now;
			if (gr->preview_resets && c->reset_at == 0u)
				c->reset_at = m->now + DAY_SEC;
		}
	}
	x->asker = *a;
	x->joined = m->now;
	return x;
}

/*
 * A client leaves a group, from one source or from all: its entries go,
 * but the last of a group stays, without a client, while the bridge still
 * forwards it.
 */
static void leave(struct omci_mcast *m, const uint8_t *g, const uint8_t *s,
		  const uint8_t *client)
{
	static const uint8_t none[ADDR];
	const bool every = memcmp(s, none, ADDR) == 0;

	for (size_t i = m->n_active; i-- > 0;) {
		struct active *x = &m->active[i];
		bool others = false;

		if (memcmp(x->group, g, ADDR) != 0 ||
		    memcmp(x->client, client, ADDR) != 0 ||
		    (!every && memcmp(x->source, s, ADDR) != 0))
			continue;
		for (size_t j = 0; j < m->n_active; j++)
			if (j != i &&
			    same_channel(&m->active[j], x->group, x->source))
				others = true;
		if (others)
			drop_entry(m, i);
		else
			memset(x->client, 0, ADDR);
	}
}

/* The text of an address, for the log. */
static void addr_text(const uint8_t *a, char out[static 40])
{
	static const char hex[] = "0123456789abcdef";
	char *p = out;

	if (omci_mcast_addr_v4(a)) {
		for (unsigned int i = 12; i < ADDR; i++) {
			unsigned int v = a[i], d = 100;

			while (d > 1u && v < d)
				d /= 10u;
			for (; d > 0u; d /= 10u)
				*p++ = (char)('0' + v / d % 10u);
			*p++ = i + 1u < ADDR ? '.' : '\0';
		}
		return;
	}
	for (unsigned int i = 0; i < ADDR; i++) {
		*p++ = hex[a[i] >> 4];
		*p++ = hex[a[i] & 0xfu];
		if (i % 2u == 1u)
			*p++ = i + 1u < ADDR ? ':' : '\0';
	}
}

/*
 * The profile whose settings the bridge port and the messages upstream
 * take: of the first service package, or the profile pointer's. False
 * without a subscriber or profile.
 */
static bool port_profile(const struct omci_mib *mib, uint16_t *out);

/*
 * Whether a message's version is one the profile takes: IGMP up to the
 * profile's IGMP version, MLD up to its MLD version (G.988 9.3.27); a
 * profile of the other protocol leaves it free.
 */
static bool version_taken(const struct omci_mcast *m,
			  const struct omci_mcast_report *r)
{
	uint16_t p;
	uint32_t v;

	if (!port_profile(m->mib, &p))
		return true;
	v = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
		    OMCI_MCAST_PROFILE_IGMP_VERSION, 1);
	if ((v >= 16u) != (r->version >= 16u))
		return true;
	return r->version <= v;
}

/*
 * Count and note each record of a message of the subscriber side. Return:
 * whether the message goes upstream: one that leaves a group or has a join
 * that was allowed; one whose joins were all refused only when the profile
 * forwards an unauthorized join request (G.988 9.3.27); none of a version
 * the profile does not take. @unauthorized says that this is the only reason.
 */
static bool heard(struct omci_mcast *m, const struct omci_mcast_report *r,
		  bool *unauthorized)
{
	const struct asker a = {
		.known = true, .tagged = r->tagged, .vid = r->vid, .pcp = r->pcp
	};
	bool joined = false, refused = false, left = false;
	uint16_t p;

	*unauthorized = false;
	if (!version_taken(m, r)) {
		OMCI_INFO(
			"mcast: a message of version %u, which the profile does not take, dropped",
			(unsigned int)r->version);
		return false;
	}
	for (uint8_t i = 0; i < r->n; i++) {
		const struct omci_mcast_record *rec = &r->rec[i];
		bool renew = false, again = false;
		struct grant gr;
		enum verdict v;
		char text[40];

		if (!rec->join) {
			leave(m, rec->group, rec->source, r->client);
			m->tally.leaves++;
			note(m, EV_LEAVE, rec->group, vlan_of(&a));
			left = true;
			continue;
		}
		note_vlan(m, rec->group, rec->source, &a);
		for (size_t k = 0; k < m->n_active; k++) {
			const bool same = same_channel(&m->active[k],
						       rec->group, rec->source);

			renew = renew || same;
			again = again || (same && memcmp(m->active[k].client,
							 r->client, ADDR) == 0);
		}
		v = judge(m, m->mib, rec->group, rec->source, &a,
			  renew ? JUDGE_RENEW : JUDGE_NEW, &gr);
		if (v == ALLOWED_OVER || v == DENIED_BANDWIDTH)
			monitors(m->mib, OMCI_MCAST_MONITOR_EXCEEDED, 1, false);
		if (v != ALLOWED && v != ALLOWED_OVER) {
			addr_text(rec->group, text);
			OMCI_INFO("mcast: join of %s refused: %s", text,
				  g_verdict[v]);
			refuse(m, rec->group, r->client, vlan_of(&a), v);
			refused = true;
			continue;
		}
		monitors(m->mib, OMCI_MCAST_MONITOR_JOINS, 1, false);
		if (again) {
			m->tally.reports++;
		} else {
			m->tally.joins++;
			note(m, EV_JOIN, rec->group, vlan_of(&a));
		}
		(void)admit(m, rec->group, rec->source, r->client, &a, &gr);
		joined = true;
	}
	*unauthorized = !left && !joined && refused &&
			port_profile(m->mib, &p) &&
			attr_be(m->mib, OMCI_MCAST_PROFILE, p,
				OMCI_MCAST_PROFILE_UNAUTHORIZED, 1) != 0u;
	return left || joined || *unauthorized;
}

/* The outer tag of a frame: its TPID, or 0 for an untagged frame, and its TCI. */
static unsigned int outer_tag(const uint8_t *f, unsigned int *tci)
{
	const unsigned int tpid = omci_be16(&f[12]);

	if (tpid != 0x8100u && tpid != 0x88a8u)
		return 0;
	*tci = omci_be16(&f[14]);
	return tpid;
}

/* Put a tag of @tpid and @tci in front of the others; the new length. */
static size_t push_tag(uint8_t *f, size_t len, unsigned int tpid,
		       unsigned int tci)
{
	memmove(&f[16], &f[12], len - 12u);
	omci_be16_store(&f[12], tpid);
	omci_be16_store(&f[14], tci);
	return len + 4u;
}

/* The EtherType of a frame, behind its tags. */
static unsigned int frame_ethertype(const uint8_t *f, size_t len)
{
	size_t off = 12;
	unsigned int type = omci_be16(&f[off]);

	while ((type == 0x8100u || type == 0x88a8u) && off + 6u <= len) {
		off += 4;
		type = omci_be16(&f[off]);
	}
	return type;
}

/* Whether a rule takes frames of EtherType @type. */
static bool rule_takes_ethertype(const struct omci_hw_vlan_xlate *r,
				 unsigned int type)
{
	uint16_t et[OMCI_HW_ETHERTYPES_MAX];
	size_t n;

	if (r->ethertype == OMCI_HW_ETHERTYPE_ANY)
		return true;
	n = omci_hw_ethertypes(r->ethertype, et);
	for (size_t i = 0; i < n; i++)
		if (et[i] == type)
			return true;
	return false;
}

/*
 * Whether one of the @n rules @r discards the frames of C-VID @vid and
 * priority @pcp. The switch's ACL drops them before its classifier applies
 * the other rules, whatever their order, but for the priorities that the
 * rules before the discard take from its VID.
 */
static bool vid_discarded(const struct omci_hw_vlan_xlate *r, size_t n,
			  unsigned int vid, unsigned int pcp)
{
	for (size_t i = 0; i < n; i++)
		if (r[i].kind == OMCI_HW_VLAN_DISCARD_VID &&
		    r[i].match_cvid == vid &&
		    (!r[i].match_has_pri || r[i].match_pri == pcp) &&
		    !((r[i].keep_pri >> pcp) & 1u))
			return true;
	return false;
}

/*
 * The upstream tag the extended VLAN tagging operation gives a frame: none
 * for a frame that a discard of its VID drops, which decides first, as in
 * the switch; otherwise that of the first rule in force that matches it, as
 * the switch takes them, or none.
 * Returns the frame's new length, or 0 for a frame that a rule discards, or
 * a tagged frame that no rule takes while the rules discard such frames.
 */
static size_t tag_by_rules(uint8_t *f, size_t len)
{
	const struct omci_hw_vlan_xlate *r;
	const size_t n = omci_vlan_in_force(&r);
	unsigned int tci = 0;
	const bool ctag = outer_tag(f, &tci) == 0x8100u;
	const unsigned int vid = tci & 0x0fffu, pcp = tci >> 13;
	const unsigned int type = frame_ethertype(f, len);
	bool discard = false;

	if (ctag && vid_discarded(r, n, vid, pcp))
		return 0;
	for (size_t i = 0; i < n; i++) {
		const unsigned int pri =
			r[i].assign_pri <= 7u ? r[i].assign_pri : pcp;

		if (r[i].kind == OMCI_HW_VLAN_DISCARD_VID ||
		    !rule_takes_ethertype(&r[i], type))
			continue;
		if (r[i].kind == OMCI_HW_VLAN_DISCARD_TAGGED) {
			discard = true;
			continue;
		}
		if (r[i].kind == OMCI_HW_VLAN_CVID_PUSH) {
			if (outer_tag(f, &tci) != 0u)
				continue;
			return push_tag(
				f, len, 0x8100u,
				(r[i].assign_pri <= 7u ? r[i].assign_pri : 0u)
						<< 13 |
					r[i].assign_cvid);
		}
		if (r[i].kind == OMCI_HW_VLAN_SCVID_PUSH) {
			if (outer_tag(f, &tci) != 0u)
				continue;
			len = push_tag(f, len, 0x8100u,
				       (unsigned int)r[i].inner_pri << 13 |
					       r[i].assign_cvid);
			return push_tag(f, len, 0x88a8u,
					(unsigned int)r[i].assign_pri << 13 |
						r[i].assign_svid);
		}
		if (!ctag || vid != r[i].match_cvid ||
		    (r[i].match_has_pri && pcp != r[i].match_pri))
			continue;
		if (r[i].kind == OMCI_HW_VLAN_CVID_XLATE) {
			omci_be16_store(&f[14], pri << 13 | (tci & 0x1000u) |
							r[i].assign_cvid);
			return len;
		}
		if (r[i].inner_has_pri)
			omci_be16_store(&f[14],
					(unsigned int)r[i].inner_pri << 13 |
						(tci & 0x1fffu));
		return push_tag(f, len, 0x88a8u, pri << 13 | r[i].assign_svid);
	}
	return ctag && discard ? 0u : len;
}

/*
 * The GEM port a frame goes up: of the UNI's bridge, one that carries the
 * frame's VLAN, or every VLAN, upstream, and takes its priority, as the
 * switch and the mapper choose (gem.h, bridge.h). False without one.
 */
static bool gem_for(const uint8_t *f, uint16_t *out)
{
	const struct omci_hw_bridge_port *bp;
	const struct omci_hw_gem_port *gp;
	const size_t nb = omci_bridge_last(&bp), ng = omci_gem_last(&gp);
	unsigned int tci = 0;
	const bool tagged = outer_tag(f, &tci) != 0u;
	const uint16_t vid = (uint16_t)(tci & 0x0fffu);
	const unsigned int pcp = tagged ? tci >> 13 : 0u;
	size_t uni = 0;

	while (uni < nb && bp[uni].port_id != OMCI_HW_BRIDGE_UNI)
		uni++;
	for (size_t i = 0; i < nb && uni < nb; i++) {
		bool vlan = bp[i].num_vlan == 0u;

		if (bp[i].bridge != bp[uni].bridge || i == uni)
			continue;
		for (uint8_t v = 0; v < bp[i].num_vlan; v++)
			vlan = vlan || (tagged && bp[i].vlan[v] == vid);
		for (size_t j = 0; j < ng && vlan; j++)
			if (gp[j].port_id == bp[i].port_id &&
			    !gp[j].multicast &&
			    (gp[j].direction & OMCI_HW_GEM_DIR_UPSTREAM) &&
			    (gp[j].pbits & (1u << pcp))) {
				*out = gp[j].port_id;
				return true;
			}
	}
	return false;
}

/* The upstream IGMP tag control of the profile and its TCI; 0 without a profile. */
static unsigned int us_control(const struct omci_mcast *m, unsigned int *tci)
{
	uint16_t p;

	*tci = 0;
	if (!port_profile(m->mib, &p))
		return 0;
	*tci = attr_be(m->mib, OMCI_MCAST_PROFILE, p, OMCI_MCAST_PROFILE_US_TCI,
		       2);
	return attr_be(m->mib, OMCI_MCAST_PROFILE, p,
		       OMCI_MCAST_PROFILE_US_TAG_CONTROL, 1);
}

/*
 * The frame that goes upstream: the tag the receive path took off put back,
 * the priority of the subscriber's C-tag made @pcp unless it is negative,
 * then the upstream IGMP tag control of the profile (G.988 9.3.27): 0 the
 * extended VLAN tagging operation's rules, 1 a tag added, 2 the outer tag
 * replaced, 3 its VID replaced; with no tag to replace, one is added.
 * Returns its length, 0 when the rules discard it.
 */
static size_t tag_up(const struct omci_mcast *m,
		     const struct omci_hw_mcast_frame *f, int pcp, uint8_t *out)
{
	size_t len = f->len;
	unsigned int control, tci, have = 0;

	memcpy(out, f->data, len);
	if (f->tagged)
		len = push_tag(out, len, 0x8100u, f->tci);
	if (pcp >= 0 && outer_tag(out, &have) == 0x8100u)
		omci_be16_store(&out[14],
				(unsigned int)pcp << 13 | (have & 0x1fffu));
	control = us_control(m, &tci);
	if (control == 0u)
		return tag_by_rules(out, len);
	if (control == 1u || outer_tag(out, &have) == 0u)
		return push_tag(out, len, 0x8100u, tci);
	if (control == 3u)
		tci = (have & 0xf000u) | (tci & 0x0fffu);
	omci_be16_store(&out[14], tci);
	return len;
}

/*
 * The priorities to try for a message of the subscriber's C-tag of VID @vid
 * and priority @own: its own, then those the rules in force name for the
 * VID, in their order, then the others from 0; all eight, each once.
 */
static void priorities(unsigned int vid, unsigned int own, uint8_t out[8])
{
	const struct omci_hw_vlan_xlate *r;
	const size_t n = omci_vlan_in_force(&r);
	unsigned int seen = 1u << own;
	size_t k = 0;

	out[k++] = (uint8_t)own;
	for (size_t i = 0; i < n; i++)
		if (r[i].kind != OMCI_HW_VLAN_DISCARD_VID &&
		    r[i].kind != OMCI_HW_VLAN_DISCARD_TAGGED &&
		    r[i].match_has_pri && r[i].match_cvid == vid &&
		    !((seen >> r[i].match_pri) & 1u)) {
			seen |= 1u << r[i].match_pri;
			out[k++] = r[i].match_pri;
		}
	for (unsigned int p = 0; p < 8u; p++)
		if (!((seen >> p) & 1u)) {
			seen |= 1u << p;
			out[k++] = (uint8_t)p;
		}
}

/* The frame for priority @pcp, when the rules and a GEM port take it: its length, or 0. */
static size_t taken(const struct omci_mcast *m,
		    const struct omci_hw_mcast_frame *f, int pcp, uint8_t *out,
		    uint16_t *port)
{
	const size_t len = tag_up(m, f, pcp, out);

	return len != 0u && gem_for(out, port) ? len : 0u;
}

/*
 * The frame that goes upstream (tag_up()) and the GEM port that carries it
 * (gem_for()). Where the profile leaves the priority to the subscriber
 * (control 0 and 3) and the message has a C-tag, a priority the operator's
 * rules or the GEM ports do not take would lose it on the way: the agent
 * gives it its own priority when they take that, or else the first they
 * take of priorities(), unless a person chose the subscriber's own
 * (igmp_host_priority), when such a message is counted as dropped. Returns
 * the frame's length, 0 when none goes, @discarded saying that the rules
 * discard it rather than that no GEM port carries it.
 */
static size_t upstream_frame(struct omci_mcast *m,
			     const struct omci_hw_mcast_frame *f, uint8_t *out,
			     uint16_t *port, bool *discarded)
{
	const struct omci_hw_ops *hw = omci_hw();
	unsigned int control, tci = 0, own = 0;
	uint8_t pri[8];
	bool host, ctag;
	size_t len;

	control = us_control(m, &tci);
	if (f->tagged)
		own = f->tci;
	ctag = f->tagged ||
	       (f->len >= 18u && outer_tag(f->data, &own) == 0x8100u);
	len = taken(m, f, -1, out, port);
	*discarded = len == 0u && tag_up(m, f, -1, out) == 0u;
	if ((control != 0u && control != 3u) || !ctag)
		return len;
	host = hw->igmp_host_priority != NULL && hw->igmp_host_priority();
	if (len != 0u) {
		if (!host)
			m->pcp_given = (int)(own >> 13);
		return len;
	}
	priorities(own & 0x0fffu, own >> 13, pri);
	for (size_t k = 1; k < sizeof(pri); k++) {
		len = taken(m, f, pri[k], out, port);
		if (len == 0u)
			continue;
		if (host) {
			m->pcp_dropped++;
			return 0;
		}
		m->pcp_given = pri[k];
		return len;
	}
	return 0;
}

/*
 * Send a message upstream, within the profile's upstream IGMP rate, in
 * messages a second (G.988 9.3.27): those over it are dropped. Return:
 * whether it was sent. The proxy, which tries again, makes no noise of a
 * message that did not go (@warn).
 */
static bool forward(struct omci_mcast *m, const struct omci_hw_mcast_frame *f,
		    bool warn)
{
	uint8_t out[OMCI_HW_MCAST_FRAME_MAX + 8u];
	bool discarded;
	uint32_t rate = 0;
	uint16_t p, port;
	size_t len;

	len = upstream_frame(m, f, out, &port, &discarded);
	if (len == 0u) {
		if (discarded)
			OMCI_DEBUG(
				"mcast: the VLAN rules discard a message in a VLAN they do not take");
		else if (warn)
			OMCI_WARN(
				"mcast: no GEM port carries the message upstream; dropped");
		return false;
	}
	if (port_profile(m->mib, &p))
		rate = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
			       OMCI_MCAST_PROFILE_US_RATE, 4);
	if (m->rate_sec != m->now) {
		m->rate_sec = m->now;
		m->rate_used = 0;
	}
	if (rate != 0u && m->rate_used >= rate)
		return false;
	m->rate_used++;

	if (omci_hw()->mcast_send(port, out, len) != OMCI_OK) {
		if (warn)
			OMCI_WARN(
				"mcast: a message to GEM port %u could not be sent",
				(unsigned int)port);
		return false;
	}
	return true;
}

/* End a group and source: out of the bridge and the active list. */
static void end_channel(struct omci_mcast *m, const struct active *x,
			const char *why)
{
	struct omci_hw_mcast_group g = { .vid = x->mdb_vid };
	uint8_t group[ADDR], source[ADDR];
	char text[40];

	memcpy(group, x->group, ADDR);
	memcpy(source, x->source, ADDR);
	memcpy(g.group, group, ADDR);
	memcpy(g.source, source, ADDR);
	(void)!omci_hw()->mcast_group_del(&g);
	for (size_t i = m->n_active; i-- > 0;)
		if (same_channel(&m->active[i], group, source))
			drop_entry(m, i);
	addr_text(group, text);
	OMCI_INFO("mcast: %s ended: %s", text, why);
}

/*
 * File each group the bridge holds for the subscriber under the VLAN the
 * OLT sends it in as well, when the bridge holds it under another, the
 * VLAN of the join. The switch looks the downstream frames of a group up by
 * the VID they arrive with, 1420 say, and a translation to the subscriber's
 * VLAN, 421, changes the tag but not the key: an entry under 421 never
 * matches them, the switch takes the stream for unregistered multicast and
 * drops it. The entry under the OLT's VLAN follows the bridge's: it goes
 * when the active entry does (alias_del()). Tried once for an entry.
 */
static void alias_sync(struct omci_mcast *m)
{
	for (size_t i = 0; i < m->n_active; i++) {
		struct active *x = &m->active[i];
		struct omci_hw_mcast_group g = { .vid = x->olt_vlan,
						 .permanent = true,
						 .alias = true };

		if (!x->held || x->alias_tried || x->olt_vlan == 0u ||
		    x->olt_vlan == x->mdb_vid)
			continue;
		memcpy(g.group, x->group, ADDR);
		memcpy(g.source, x->source, ADDR);
		for (size_t j = 0; j < m->n_active; j++)
			if (same_channel(&m->active[j], x->group, x->source))
				m->active[j].alias_tried = true;
		if (omci_hw()->mcast_group_add(&g) != OMCI_OK) {
			char text[40];

			addr_text(x->group, text);
			OMCI_WARN("mcast: %s could not be filed under VLAN %u",
				  text, (unsigned int)x->olt_vlan);
			continue;
		}
		for (size_t j = 0; j < m->n_active; j++)
			if (same_channel(&m->active[j], x->group, x->source))
				m->active[j].aliased = true;
	}
}

/*
 * A permanent group of the bridge under a VLAN that is no static group and
 * no entry under an OLT's VLAN of an active channel is a leftover, of an
 * agent that ended with its channels active: the stream it lets through goes
 * on with nobody to watch it. It goes. (A bridge that does not filter VLANs
 * holds every group under VID 0, and none is taken for a leftover there.)
 */
static bool a_static(const struct omci_mcast *m,
		     const struct omci_hw_mcast_group *g)
{
	for (size_t k = 0; k < m->n_statics; k++) {
		const struct omci_hw_mcast_group *s = &m->statics[k];

		if (g->vid == s->vid && memcmp(g->group, s->group, ADDR) == 0 &&
		    memcmp(g->source, s->source, ADDR) == 0)
			return true;
	}
	return false;
}

static bool an_alias(const struct omci_mcast *m,
		     const struct omci_hw_mcast_group *g)
{
	for (size_t k = 0; k < m->n_active; k++) {
		const struct active *x = &m->active[k];

		if (x->aliased && x->olt_vlan == g->vid &&
		    same_channel(x, g->group, g->source))
			return true;
	}
	return false;
}

static void leftovers(const struct omci_mcast *m,
		      const struct omci_hw_mcast_group *g, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		const bool known = !g[i].permanent || g[i].vid == 0u ||
				   a_static(m, &g[i]) || an_alias(m, &g[i]);

		if (!known)
			(void)!omci_hw()->mcast_group_del(&g[i]);
	}
}

/*
 * Look at the groups of the bridge: one that the agent does not know is
 * judged, and removed unless allowed; an active group the bridge no longer
 * has is gone; a static group that went missing comes back. Return: false
 * when the bridge's groups could not be read.
 */
static bool look(struct omci_mcast *m)
{
	struct omci_hw_mcast_group g[OMCI_HW_MCAST_MAX_GROUPS];
	static const uint8_t none[ADDR];
	size_t n = 0;

	if (omci_hw()->mcast_groups(g, ARRAY_SIZE(g), &n) != OMCI_OK)
		return false;

	for (size_t i = 0; i < m->n_active; i++)
		m->active[i].held = false;
	for (size_t i = 0; i < n; i++) {
		bool held = false;
		struct asker a;
		struct grant gr;
		enum verdict v;
		char text[40];

		if (g[i].permanent)
			continue;
		for (size_t j = 0; j < m->n_active; j++)
			if (same_channel(&m->active[j], g[i].group,
					 g[i].source)) {
				m->active[j].held = true;
				m->active[j].mdb_vid = g[i].vid;
				held = true;
			}
		if (held)
			continue;

		a = vlan_noted(m, g[i].group, g[i].source);
		v = judge(m, m->mib, g[i].group, g[i].source, &a, JUDGE_NEW,
			  &gr);
		if (v == ALLOWED || v == ALLOWED_OVER) {
			struct active *x = admit(m, g[i].group, g[i].source,
						 none, &a, &gr);

			if (x != NULL)
				x->held = true;
			m->tally.joins++;
			note(m, EV_JOIN, g[i].group, vlan_of(&a));
			continue;
		}
		(void)!omci_hw()->mcast_group_del(&g[i]);
		addr_text(g[i].group, text);
		OMCI_INFO("mcast: %s taken back from the bridge: %s", text,
			  g_verdict[v]);
		if (!refused_now(m, g[i].group))
			refuse(m, g[i].group, none, vlan_of(&a), v);
	}
	for (size_t i = m->n_active; i-- > 0;)
		if (!m->active[i].held)
			drop_entry(m, i);

	for (size_t k = 0; k < m->n_statics; k++) {
		bool there = false;

		for (size_t i = 0; i < n; i++)
			there = there ||
				(g[i].permanent &&
				 g[i].vid == m->statics[k].vid &&
				 memcmp(g[i].group, m->statics[k].group,
					ADDR) == 0 &&
				 memcmp(g[i].source, m->statics[k].source,
					ADDR) == 0);
		if (!there &&
		    omci_hw()->mcast_group_add(&m->statics[k]) != OMCI_OK)
			OMCI_WARN("mcast: a static group could not be added");
	}
	alias_sync(m);
	leftovers(m, g, n);
	return true;
}

static bool port_profile(const struct omci_mib *mib, uint16_t *out)
{
	const struct asker any = { .known = false };
	struct candidates c;
	uint16_t sub;

	if (!subscriber(mib, &sub))
		return false;
	candidates_of(mib, sub, &any, &c);
	*out = c.pkg[0].profile;
	return omci_mib_exists(mib, OMCI_MCAST_PROFILE, *out);
}

/*
 * The bridge port's leave and timers, from the profile or the bridge's own,
 * set each time, and tried again each second until they are.
 */
static void set_port(struct omci_mcast *m)
{
	struct omci_hw_mcast_port cfg = { .fast_leave = false };
	uint16_t p;

	/* The agent forwards the messages under a policy it can send them for. */
	cfg.trap = omci_hw()->mcast_send != NULL && subscriber(m->mib, &p);
	if (port_profile(m->mib, &p)) {
		cfg.fast_leave = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
					 OMCI_MCAST_PROFILE_IMMEDIATE_LEAVE,
					 1) != 0u;
		cfg.robustness = (uint8_t)attr_be(m->mib, OMCI_MCAST_PROFILE, p,
						  OMCI_MCAST_PROFILE_ROBUSTNESS,
						  1);
		cfg.query_interval = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
					     OMCI_MCAST_PROFILE_QUERY_INTERVAL,
					     4);
		cfg.query_response = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
					     OMCI_MCAST_PROFILE_QUERY_RESPONSE,
					     4);
		cfg.last_member = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
					  OMCI_MCAST_PROFILE_LAST_MEMBER, 4);
	}
	if (omci_hw()->mcast_port(&cfg) != OMCI_OK) {
		if (m->port_set)
			OMCI_WARN(
				"mcast: the bridge port's leave and timers could not be set; trying again");
		m->port_set = false;
		return;
	}
	m->forwarding = cfg.trap;
	if (m->port_set && memcmp(&cfg, &m->port, sizeof(cfg)) == 0)
		return;
	m->port = cfg;
	m->port_set = true;
	OMCI_INFO(
		"mcast: leave %s, robustness %u, query interval %u s, response %u, last member %u (0: the bridge's own)",
		cfg.fast_leave ? "at once" : "after a query",
		(unsigned int)cfg.robustness, cfg.query_interval,
		cfg.query_response, cfg.last_member);
}

/*
 * The static groups of the profiles of the subscriber's packages: each
 * group of each row, its source, and the VLAN that carries it downstream,
 * as many as there is room for.
 */
static size_t statics_of(const struct omci_mib *mib,
			 struct omci_hw_mcast_group *out)
{
	const struct asker any = { .known = false };
	struct omci_mcast_acl_row rows[OMCI_MCAST_ACL_ROWS];
	struct candidates c;
	uint16_t sub;
	size_t n = 0;

	if (!subscriber(mib, &sub))
		return 0;
	candidates_of(mib, sub, &any, &c);
	for (size_t k = 0; k < c.n; k++) {
		const size_t nr = omci_mcast_acl_read(
			mib, c.pkg[k].profile, OMCI_MCAST_PROFILE_STATIC_ACL,
			rows, ARRAY_SIZE(rows));

		for (size_t r = 0; r < nr; r++) {
			const uint32_t first = omci_be32(&rows[r].first[12]);
			const uint32_t last = omci_be32(&rows[r].last[12]);

			for (uint64_t a = first; a <= last && n < MAX_STATIC;
			     a++) {
				struct omci_hw_mcast_group *g = &out[n++];

				memset(g, 0, sizeof(*g));
				g->vid = rows[r].vlan >= 1u &&
							 rows[r].vlan <= 4094u ?
						 rows[r].vlan :
						 0u;
				memcpy(g->group, rows[r].first, ADDR);
				omci_be32_store(&g->group[12], (uint32_t)a);
				if (!rows[r].any_source)
					memcpy(g->source, rows[r].source, ADDR);
				g->permanent = true;
			}
		}
	}
	return n;
}

static bool same_group(const struct omci_hw_mcast_group *a,
		       const struct omci_hw_mcast_group *b)
{
	return a->vid == b->vid && memcmp(a->group, b->group, ADDR) == 0 &&
	       memcmp(a->source, b->source, ADDR) == 0;
}

/* Remove the static groups the profiles no longer have; look() adds the new. */
static void set_statics(struct omci_mcast *m)
{
	struct omci_hw_mcast_group want[MAX_STATIC];
	const size_t n = statics_of(m->mib, want);

	for (size_t k = 0; k < m->n_statics; k++) {
		bool kept = false;

		for (size_t i = 0; i < n; i++)
			kept = kept || same_group(&want[i], &m->statics[k]);
		if (!kept)
			(void)!omci_hw()->mcast_group_del(&m->statics[k]);
	}
	memcpy(m->statics, want, n * sizeof(want[0]));
	m->n_statics = n;
}

/* Whether an extended VLAN tagging rule in force gives unicast frames VLAN @vid. */
static bool unicast_vlan(uint16_t vid)
{
	const struct omci_hw_vlan_xlate *r;
	const size_t n = omci_vlan_in_force(&r);

	for (size_t i = 0; i < n; i++) {
		if (r[i].kind == OMCI_HW_VLAN_DISCARD_TAGGED ||
		    r[i].kind == OMCI_HW_VLAN_DISCARD_VID)
			continue;
		if ((r[i].kind == OMCI_HW_VLAN_SVID_PUSH ||
				     r[i].kind == OMCI_HW_VLAN_SCVID_PUSH ?
			     r[i].assign_svid :
			     r[i].assign_cvid) == vid)
			return true;
	}
	return false;
}

/*
 * The downstream tag a profile's downstream IGMP and multicast TCI gives
 * (G.988 9.3.27): @tci its three bytes, @pkg_vid the VID (UNI) of the
 * service package. Types 5 to 7 take the package's VID, and with an
 * untagged one strip the tag, but add the attribute's tag; with an
 * unspecified one they take the attribute's tag. False for type 0.
 */
static bool ds_tag(const uint8_t *tci, uint16_t pkg_vid,
		   struct omci_hw_mcast_ds *out)
{
	static const uint8_t base[8] = { 0,
					 OMCI_HW_MCAST_DS_STRIP,
					 OMCI_HW_MCAST_DS_ADD,
					 OMCI_HW_MCAST_DS_REPLACE,
					 OMCI_HW_MCAST_DS_REPLACE_VID,
					 OMCI_HW_MCAST_DS_ADD,
					 OMCI_HW_MCAST_DS_REPLACE,
					 OMCI_HW_MCAST_DS_REPLACE_VID };
	const unsigned int control = tci[0] & 7u;
	const unsigned int own = omci_be16(&tci[1]);

	if (control == 0u)
		return false;
	out->action = base[control];
	out->tci = (uint16_t)own;
	if (control < 5u || pkg_vid == OMCI_MCAST_VID_ANY ||
	    pkg_vid == OMCI_MCAST_VID_TAGGED)
		return true;
	if (pkg_vid == OMCI_MCAST_VID_UNTAGGED) {
		if (control != 5u)
			out->action = OMCI_HW_MCAST_DS_STRIP;
		return true;
	}
	out->tci = (uint16_t)((own & 0xf000u) | pkg_vid);
	return true;
}

/*
 * The downstream tags of the multicast VLANs: for each VLAN a row of a
 * profile's access control lists names, that profile's downstream IGMP and
 * multicast TCI. The switch tells multicast from unicast by the VLAN alone,
 * so a VLAN that the extended VLAN tagging rules also give unicast frames,
 * and rows of untagged groups or of any VLAN, keep the rules' treatment.
 */
static void set_ds(struct omci_mcast *m)
{
	const struct asker any = { .known = false };
	struct omci_hw_mcast_ds want[OMCI_HW_MCAST_DS_MAX];
	struct omci_mcast_acl_row rows[OMCI_MCAST_ACL_ROWS];
	struct candidates c = { .n = 0 };
	size_t n = 0;
	uint16_t sub;

	if (omci_hw()->mcast_ds == NULL)
		return;
	if (subscriber(m->mib, &sub))
		candidates_of(m->mib, sub, &any, &c);
	for (size_t k = 0; k < c.n; k++) {
		uint8_t tci[3] = { 0 };
		struct omci_hw_mcast_ds d;
		size_t len = 0;

		(void)!omci_mib_attr_get(m->mib, OMCI_MCAST_PROFILE,
					 c.pkg[k].profile,
					 OMCI_MCAST_PROFILE_DS_TCI, tci,
					 sizeof(tci), &len);
		if (!ds_tag(tci,
			    c.pkg[k].key == NO_PACKAGE ? OMCI_MCAST_VID_ANY :
							 c.pkg[k].vid,
			    &d))
			continue;
		for (uint8_t attr = OMCI_MCAST_PROFILE_DYNAMIC_ACL;
		     attr <= OMCI_MCAST_PROFILE_STATIC_ACL; attr++) {
			const size_t nr = omci_mcast_acl_read(m->mib,
							      c.pkg[k].profile,
							      attr, rows,
							      ARRAY_SIZE(rows));

			for (size_t r = 0; r < nr; r++) {
				bool dup = false;

				for (size_t i = 0; i < n; i++)
					dup = dup ||
					      want[i].vid == rows[r].vlan;
				if (dup || rows[r].vlan < 1u ||
				    rows[r].vlan > 4094u)
					continue;
				if (unicast_vlan(rows[r].vlan)) {
					OMCI_WARN(
						"mcast: VLAN %u carries unicast too; its multicast keeps the VLAN rules' downstream tag",
						(unsigned int)rows[r].vlan);
					continue;
				}
				if (n == OMCI_HW_MCAST_DS_MAX)
					continue;
				d.vid = rows[r].vlan;
				want[n++] = d;
			}
		}
	}
	if (m->ds_set && n == m->n_ds &&
	    memcmp(want, m->ds, n * sizeof(want[0])) == 0)
		return;
	if (omci_hw()->mcast_ds(want, n) != OMCI_OK) {
		OMCI_WARN(
			"mcast: the downstream tags of the multicast VLANs could not be set");
		m->ds_set = false;
		return;
	}
	memcpy(m->ds, want, n * sizeof(want[0]));
	m->n_ds = n;
	m->ds_set = true;
	OMCI_INFO(
		"mcast: %zu multicast VLAN(s) with a downstream tag of their own",
		n);
}

/*
 * Judge every active group again, in the order they came, each against the
 * limits with those before it: as a new group, but a preview under way goes
 * on. A group that is left for preview only, its allowed preview group
 * gone, ends (G.988 9.3.28).
 */
static void recheck(struct omci_mcast *m)
{
	for (size_t i = 0; i < m->n_active;) {
		const struct active *x = &m->active[i];
		bool judged = false;
		struct grant gr;
		enum verdict v;

		for (size_t j = 0; j < i; j++)
			judged = judged || same_channel(&m->active[j], x->group,
							x->source);
		m->upto = i;
		v = judge(m, m->mib, x->group, x->source, &x->asker,
			  JUDGE_RECHECK, &gr);
		m->upto = SIZE_MAX;
		if (judged || ((v == ALLOWED || v == ALLOWED_OVER) &&
			       gr.preview == x->preview)) {
			i++;
			continue;
		}
		end_channel(m, x,
			    v == ALLOWED || v == ALLOWED_OVER ?
				    "its allowed preview group is gone" :
				    g_verdict[v]);
	}
}

/*
 * The profile whose dynamic access control list has the row that takes an
 * active group, found as judge() finds it, and the VLAN that the row
 * carries the group in downstream. False when no such row takes it: a
 * group without a policy, or an allowed preview group in no package's list.
 */
static bool row_of(const struct omci_mcast *m, const struct active *x,
		   uint16_t *profile, uint16_t *vlan)
{
	struct omci_mcast_acl_row row;
	struct candidates c;
	uint16_t sub;
	size_t k;

	if (!subscriber(m->mib, &sub))
		return false;
	candidates_of(m->mib, sub, &x->asker, &c);
	k = find_row(m->mib, &c, x->group, x->source, false, &row);
	if (k == c.n)
		k = find_row(m->mib, &c, x->group, x->source, true, &row);
	if (k == c.n)
		return false;
	*profile = c.pkg[k].profile;
	*vlan = row.vlan;
	return true;
}

/* The count of a group; NULL when it is not counted. */
static const struct watch *watch_of(const struct omci_mcast *m,
				    const uint8_t *g)
{
	for (size_t i = 0; i < m->n_watches; i++)
		if (memcmp(m->watches[i].group, g, ADDR) == 0)
			return &m->watches[i];
	return NULL;
}

static bool group_lost(const struct omci_mcast *m, const uint8_t *g)
{
	const struct watch *w = watch_of(m, g);

	return w != NULL && w->lost;
}

/* Whether a test of a group runs for @g. */
static bool tested(const struct omci_mcast *m, const uint8_t *g)
{
	return m->test.on && memcmp(m->test.group, g, ADDR) == 0;
}

/*
 * Whether a group is to be counted: the bridge forwards it for an active
 * entry that a row of a dynamic access control list took, or a test of it
 * runs.
 */
static bool wanted(const struct omci_mcast *m, const uint8_t *g)
{
	uint16_t p, v;

	if (tested(m, g))
		return true;
	for (size_t i = 0; i < m->n_active; i++)
		if (m->active[i].held &&
		    memcmp(m->active[i].group, g, ADDR) == 0 &&
		    row_of(m, &m->active[i], &p, &v))
			return true;
	return false;
}

/* The VLAN of the joins of a group, for the panel; 0 when not known. */
static uint16_t group_vlan(const struct omci_mcast *m, const uint8_t *g)
{
	size_t i = 0;

	while (i < m->n_active && memcmp(m->active[i].group, g, ADDR) != 0)
		i++;
	return i < m->n_active ? vlan_of(&m->active[i].asker) : 0u;
}

/* Note whether a counted group is lost, and say when that changes. */
static void set_lost(struct omci_mcast *m, struct watch *w, bool lost)
{
	char text[40];

	if (w->lost == lost)
		return;
	w->lost = lost;
	note(m, lost ? EV_LOST : EV_BACK, w->group, group_vlan(m, w->group));
	addr_text(w->group, text);
	if (lost)
		OMCI_INFO("mcast: %s lost: no frame downstream for %u s", text,
			  LOST_SEC);
	else
		OMCI_INFO("mcast: %s flows again", text);
}

/*
 * Count the downstream bytes of the groups to be counted, as many as the
 * backend can, with the bits a second between two reads, and judge a group
 * lost when none came for LOST_SEC (G.988 9.3.27); not one under test,
 * whose stream the test is to find out about. A backend that cannot count
 * loses no group.
 */
static void watch(struct omci_mcast *m)
{
	const struct omci_hw_ops *hw = omci_hw();
	bool short_now = false;

	if (hw->mcast_watch == NULL || hw->mcast_seen == NULL)
		return;
	for (size_t i = m->n_watches; i-- > 0;)
		if (!wanted(m, m->watches[i].group) &&
		    hw->mcast_watch(m->watches[i].group, false) == OMCI_OK)
			m->watches[i] = m->watches[--m->n_watches];
	for (size_t i = 0; i < m->n_active; i++) {
		const uint8_t *g = m->active[i].group;
		struct watch *w;

		if (watch_of(m, g) != NULL || !wanted(m, g))
			continue;
		if (m->n_watches == OMCI_HW_MCAST_WATCH_MAX ||
		    hw->mcast_watch(g, true) != OMCI_OK) {
			short_now = true;
			continue;
		}
		w = &m->watches[m->n_watches++];
		memset(w, 0, sizeof(*w));
		memcpy(w->group, g, ADDR);
		w->since = m->now;
	}
	if (short_now && !m->watch_short)
		OMCI_WARN(
			"mcast: a group cannot be counted; whether its stream flows is not known");
	m->watch_short = short_now;
	for (size_t i = 0; i < m->n_watches; i++) {
		struct watch *w = &m->watches[i];
		uint64_t bytes = 0;

		if (tested(m, w->group) ||
		    hw->mcast_seen(w->group, &bytes) != OMCI_OK)
			continue;
		if (w->read_at != 0u && m->now > w->read_at &&
		    bytes >= w->bytes) {
			w->rate =
				(bytes - w->bytes) * 8u / (m->now - w->read_at);
			w->rated = true;
		}
		w->read_at = m->now;
		if (bytes != w->bytes) {
			w->bytes = bytes;
			w->since = m->now;
			set_lost(m, w, false);
		} else if (m->now - w->since >= LOST_SEC) {
			set_lost(m, w, true);
		}
	}
}

/*
 * A join or a leave of the group under test, from the agent itself, as the
 * proxy speaks (message.h): from the stick's address and the profile's
 * upstream IGMP address, in its IGMP version (2 when it names none of IGMP),
 * tagged with the test's VLAN, if any, and sent upstream as a subscriber's
 * message would be. Return: whether it was sent.
 */
static bool test_message(struct omci_mcast *m, bool join)
{
	struct omci_hw_mcast_frame f = { .tagged = m->test.vid != 0u,
					 .tci = m->test.vid };
	struct omci_mcast_env env = { .version = 2 };
	struct omci_mcast_item it = { .any = join };
	uint8_t querier[4];
	size_t used;
	uint16_t p;

	if (omci_hw()->mcast_mac(env.mac) != OMCI_OK)
		return false;
	if (port_profile(m->mib, &p)) {
		const uint32_t v = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
					   OMCI_MCAST_PROFILE_IGMP_VERSION, 1);

		if (v >= 1u && v <= 3u)
			env.version = (uint8_t)v;
		omci_be32_store(querier,
				attr_be(m->mib, OMCI_MCAST_PROFILE, p,
					OMCI_MCAST_PROFILE_QUERIER, 4));
		if (omci_be32(querier) != 0u)
			omci_mcast_addr_set_v4(env.source, querier);
	}
	memcpy(it.group, m->test.group, ADDR);
	f.len = (uint16_t)omci_mcast_build_report(f.data, sizeof(f.data), &env,
						  true, &it, 1, &used);
	return f.len != 0u && forward(m, &f, true);
}

enum omci_status omci_mcast_test_start(struct omci_mcast *m,
				       const uint8_t group[static ADDR],
				       uint16_t vid, unsigned int seconds,
				       const char **why)
{
	static const uint8_t none[ADDR];
	const struct omci_hw_ops *hw = omci_hw();
	const struct asker a = { .known = true,
				 .tagged = vid != 0u,
				 .vid = vid };
	struct watch *w;
	struct grant gr;
	enum verdict v;

	*why = "";
	if (m == NULL || m->mib == NULL || !omci_mcast_addr_v4(group) ||
	    (group[12] & 0xf0u) != 0xe0u || vid > 4094u || seconds == 0u ||
	    seconds > TEST_MAX_SEC)
		return OMCI_ERR_INVAL;
	if (m->test.on)
		return OMCI_ERR_BUSY;
	if (hw->mcast_send == NULL || hw->mcast_mac == NULL ||
	    hw->mcast_watch == NULL || hw->mcast_seen == NULL)
		return OMCI_ERR_IO;
	v = judge(m, m->mib, group, none, &a, JUDGE_NEW, &gr);
	if (v != ALLOWED && v != ALLOWED_OVER) {
		*why = g_reason[v];
		return OMCI_DECLINED;
	}
	memset(&m->test, 0, sizeof(m->test));
	memcpy(m->test.group, group, ADDR);
	m->test.vid = vid;
	m->test.end = m->now + seconds + TEST_GRACE_SEC;
	if (watch_of(m, group) == NULL) {
		if (m->n_watches == OMCI_HW_MCAST_WATCH_MAX ||
		    hw->mcast_watch(group, true) != OMCI_OK)
			return OMCI_ERR_NOMEM;
		w = &m->watches[m->n_watches++];
		memset(w, 0, sizeof(*w));
		memcpy(w->group, group, ADDR);
		w->since = m->now;
		m->test.counted = true;
	}
	(void)!hw->mcast_seen(group, &m->test.base);
	m->test.on = true;
	if (test_message(m, true))
		return OMCI_OK;
	omci_mcast_test_stop(m);
	return OMCI_ERR_HW;
}

enum omci_status omci_mcast_test_read(struct omci_mcast *m, uint64_t *bytes,
				      bool *running)
{
	uint64_t now = 0;

	*bytes = 0;
	*running = false;
	if (m == NULL || (!m->test.on && m->test.end == 0u))
		return OMCI_DECLINED;
	*running = m->test.on;
	if (!m->test.on) {
		*bytes = m->test.bytes;
		return OMCI_OK;
	}
	if (omci_hw()->mcast_seen(m->test.group, &now) != OMCI_OK)
		return OMCI_ERR_HW;
	*bytes = now >= m->test.base ? now - m->test.base : 0u;
	return OMCI_OK;
}

void omci_mcast_test_stop(struct omci_mcast *m)
{
	bool running;

	if (m == NULL || !m->test.on)
		return;
	(void)!omci_mcast_test_read(m, &m->test.bytes, &running);
	(void)test_message(m, false);
	m->test.on = false;
	for (size_t i = 0; i < m->n_watches && m->test.counted; i++)
		if (memcmp(m->watches[i].group, m->test.group, ADDR) == 0 &&
		    !wanted(m, m->test.group) &&
		    omci_hw()->mcast_watch(m->test.group, false) == OMCI_OK)
			m->watches[i] = m->watches[--m->n_watches];
}

/*
 * The proxy of IGMP function 1 and 2 (proxy.h). It takes the place of the
 * subscribers towards the OLT, which needs the messages of the subscribers
 * held back for the agent, a way to send, and an Ethernet address to send
 * from: where it has not, the profile's function is carried out as 0.
 */
static const char *const g_function[] = {
	"transparent snooping",
	"snooping with proxy reporting",
	"IGMP proxy",
};

static void proxy_params(struct omci_mcast *m,
			 struct omci_mcast_proxy_params *pr)
{
	const struct omci_hw_ops *hw = omci_hw();
	unsigned int asked = OMCI_MCAST_FUNCTION_SNOOPING;
	uint16_t p;

	memset(pr, 0, sizeof(*pr));
	if (port_profile(m->mib, &p)) {
		const uint32_t querier = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
						 OMCI_MCAST_PROFILE_QUERIER, 4);

		asked = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
				OMCI_MCAST_PROFILE_IGMP_FUNCTION, 1);
		if (asked > OMCI_MCAST_FUNCTION_PROXY)
			asked = OMCI_MCAST_FUNCTION_PROXY;
		pr->version = (uint8_t)attr_be(m->mib, OMCI_MCAST_PROFILE, p,
					       OMCI_MCAST_PROFILE_IGMP_VERSION,
					       1);
		for (unsigned int i = 0; i < 4u; i++)
			pr->querier[i] = (uint8_t)(querier >> (24u - 8u * i));
		pr->robustness = (uint8_t)attr_be(m->mib, OMCI_MCAST_PROFILE, p,
						  OMCI_MCAST_PROFILE_ROBUSTNESS,
						  1);
		pr->query_interval = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
					     OMCI_MCAST_PROFILE_QUERY_INTERVAL,
					     4);
		pr->query_response = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
					     OMCI_MCAST_PROFILE_QUERY_RESPONSE,
					     4);
		pr->last_member = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
					  OMCI_MCAST_PROFILE_LAST_MEMBER, 4);
		pr->fast_leave = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
					 OMCI_MCAST_PROFILE_IMMEDIATE_LEAVE,
					 1) != 0u;
	}
	pr->function = (uint8_t)asked;
	if (asked != OMCI_MCAST_FUNCTION_SNOOPING &&
	    !(m->forwarding && hw->mcast_send != NULL && hw->mcast_mac != NULL))
		pr->function = OMCI_MCAST_FUNCTION_SNOOPING;

	if ((int)asked == m->function_asked &&
	    (int)pr->function == m->function_in_force)
		return;
	m->function_asked = (int)asked;
	m->function_in_force = pr->function;
	if (pr->function == asked)
		OMCI_INFO("mcast: IGMP function %u: %s", asked,
			  g_function[asked]);
	else
		OMCI_INFO(
			"mcast: IGMP function %u (%s) cannot be done here: the messages of the subscribers are not held for the agent; %s instead",
			asked, g_function[asked], g_function[pr->function]);
}

static bool proxy_up(void *ctx, const uint8_t *frame, size_t len,
		     const struct omci_mcast_tag *tag)
{
	struct omci_hw_mcast_frame f = { .len = (uint16_t)len,
					 .tagged = tag->tagged,
					 .tci = (uint16_t)(tag->pcp << 13 |
							   tag->vid) };

	memcpy(f.data, frame, len < sizeof(f.data) ? len : sizeof(f.data));
	return forward(ctx, &f, false);
}

static bool proxy_down(void *ctx, const uint8_t *frame, size_t len,
		       const struct omci_mcast_tag *tag)
{
	uint8_t out[OMCI_HW_MCAST_FRAME_MAX + 4u];

	(void)ctx;
	memcpy(out, frame, len);
	if (tag->tagged)
		len = push_tag(out, len, 0x8100u,
			       (unsigned int)tag->pcp << 13 | tag->vid);
	return omci_hw()->mcast_send(OMCI_HW_MCAST_UNI, out, len) == OMCI_OK;
}

/* Nobody on the subscriber side answered for a group: end it, whatever its sources. */
static void proxy_drop(void *ctx, const uint8_t group[static ADDR])
{
	struct omci_mcast *m = ctx;
	bool again;

	do {
		again = false;
		for (size_t i = 0; i < m->n_active && !again; i++) {
			if (memcmp(m->active[i].group, group, ADDR) != 0)
				continue;
			end_channel(
				m, &m->active[i],
				"nobody answered the query after the leave");
			again = true;
		}
	} while (again);
}

static bool proxy_mac(void *ctx, uint8_t mac[static OMCI_MCAST_MAC_LEN])
{
	(void)ctx;
	return omci_hw()->mcast_mac(mac) == OMCI_OK;
}

/*
 * A message of the subscriber side was taken in: a join ends the check of
 * the group's leave, and a leave starts one, whoever else the agent knows to
 * hold the group: the bridge shortens the group's timer at a leave and would
 * let it go before long, without a query, if the others did not answer one.
 */
static void proxy_noted(struct omci_mcast *m,
			const struct omci_mcast_proxy_params *pr,
			const struct omci_mcast_report *r)
{
	for (uint8_t i = 0; i < r->n; i++) {
		const struct omci_mcast_record *rec = &r->rec[i];
		const struct omci_mcast_tag tag = { .tagged = r->tagged,
						    .vid = r->vid,
						    .pcp = r->pcp };
		bool active = false;

		if (rec->join) {
			omci_mcast_proxy_joined(&m->proxy, rec->group);
			continue;
		}
		for (size_t k = 0; k < m->n_active; k++)
			active = active || memcmp(m->active[k].group,
						  rec->group, ADDR) == 0;
		if (active)
			omci_mcast_proxy_left(&m->proxy, pr, rec->group, &tag,
					      m->now);
	}
}

/* Run the proxy over the groups the bridge holds, after the agent has looked at them. */
static void proxy_run(struct omci_mcast *m,
		      const struct omci_mcast_proxy_params *pr)
{
	const struct omci_mcast_proxy_ops ops = {
		.up = proxy_up,
		.down = proxy_down,
		.drop = proxy_drop,
		.mac = proxy_mac,
		.ctx = m,
	};
	struct omci_mcast_chan held[MAX_ACTIVE];

	for (size_t i = 0; i < m->n_active; i++) {
		const struct asker *a = &m->active[i].asker;

		memcpy(held[i].group, m->active[i].group, ADDR);
		memcpy(held[i].source, m->active[i].source, ADDR);
		held[i].tag.tagged = a->known && a->tagged;
		held[i].tag.vid = held[i].tag.tagged ? a->vid : 0u;
		held[i].tag.pcp = held[i].tag.tagged ? a->pcp : 0u;
	}
	omci_mcast_proxy_run(&m->proxy, pr, &ops, held, m->n_active, m->now);
}

static bool ready(struct omci_mcast *m, struct omci_mib *mib, uint64_t now_sec)
{
	if (m == NULL || omci_hw()->mcast_port == NULL)
		return false;
	m->mib = mib;
	m->now = now_sec;
	return true;
}

void omci_mcast_service(struct omci_mcast *m, struct omci_mib *mib,
			uint64_t now_sec)
{
	struct omci_mcast_proxy_params pr;
	struct omci_hw_mcast_frame f;
	struct omci_mcast_report r;

	if (!ready(m, mib, now_sec))
		return;
	proxy_params(m, &pr);
	/*
	 * A message the switch trapped goes upstream only if the agent sends
	 * it: a query from the subscriber side, or what is not taken apart,
	 * never does. Under a proxy it is the proxy that tells the OLT, and a
	 * message goes upstream as it came only when it asks for a group the
	 * policy refuses and the profile forwards such requests. The OLT's
	 * queries are for the proxy to answer.
	 */
	for (unsigned int k = 0;
	     k < FRAMES_PER_SERVICE && omci_hw()->mcast_frame(&f) == OMCI_OK;
	     k++) {
		bool unauthorized;

		if (!omci_mcast_report_parse(f.data, f.len, f.tci, f.tagged,
					     &r))
			continue;
		if (f.from_pon) {
			if (r.kind != OMCI_MCAST_KIND_QUERY)
				continue;
			m->tally.queries++;
			m->queried = true;
			m->query_at = m->now;
			m->query_version = r.version;
			note(m, EV_QUERY, r.query_group, r.tagged ? r.vid : 0u);
			if (pr.function != OMCI_MCAST_FUNCTION_SNOOPING)
				omci_mcast_proxy_query(&m->proxy, &r, m->now);
			continue;
		}
		if (r.kind == OMCI_MCAST_KIND_OTHER ||
		    r.kind == OMCI_MCAST_KIND_QUERY)
			continue;
		if (!heard(m, &r, &unauthorized))
			continue;
		if (pr.function != OMCI_MCAST_FUNCTION_SNOOPING)
			proxy_noted(m, &pr, &r);
		if (m->forwarding &&
		    (pr.function == OMCI_MCAST_FUNCTION_SNOOPING ||
		     unauthorized))
			(void)forward(m, &f, true);
	}
	if (look(m))
		proxy_run(m, &pr);
	report_bandwidth(m);
}

void omci_mcast_program(struct omci_mcast *m, struct omci_mib *mib,
			uint64_t now_sec)
{
	if (!ready(m, mib, now_sec))
		return;
	set_port(m);
	set_statics(m);
	set_ds(m);
	recheck(m);
	omci_mcast_service(m, mib, now_sec);
}

void omci_mcast_tick(struct omci_mcast *m, struct omci_mib *mib,
		     uint64_t now_sec)
{
	uint16_t sub;

	if (!ready(m, mib, now_sec))
		return;
	if (!m->port_set)
		set_port(m);
	for (size_t i = 0; i < m->n_active;)
		if (m->active[i].preview && now_sec >= m->active[i].preview_end)
			end_channel(m, &m->active[i], "the preview is over");
		else
			i++;
	for (size_t k = 0; k < m->n_counts; k++)
		if (m->counts[k].reset_at != 0u &&
		    now_sec >= m->counts[k].reset_at) {
			m->counts[k].n = 0;
			m->counts[k].reset_at = 0;
		}
	if (now_sec - m->minute_at >= 60u) {
		m->minute_at = now_sec;
		if (subscriber(mib, &sub) &&
		    omci_mcast_preview_count_down(mib, sub))
			recheck(m);
	}
	omci_mcast_service(m, mib, now_sec);
	if (m->test.on && now_sec >= m->test.end)
		omci_mcast_test_stop(m);
	watch(m);
}

enum omci_status omci_mcast_init(struct omci_mcast **out)
{
	if (out == NULL)
		return OMCI_ERR_INVAL;
	if (g_mcast_taken)
		return OMCI_ERR_BUSY;
	memset(&g_mcast, 0, sizeof(g_mcast));
	omci_mcast_proxy_reset(&g_mcast.proxy);
	g_mcast.upto = SIZE_MAX;
	g_mcast.pcp_given = -1;
	g_mcast_taken = true;
	*out = &g_mcast;
	return OMCI_OK;
}

void omci_mcast_fini(struct omci_mcast *m)
{
	if (m == &g_mcast)
		g_mcast_taken = false;
}

/*
 * An address of the active group lists: IPv4 in four bytes, or sixteen. A
 * group of the IPv6 list was joined over IPv6, so its source and client
 * are IPv6 addresses, or none.
 */
static void put_addr(uint8_t *out, const uint8_t *a, bool wide)
{
	memcpy(out, wide ? a : &a[12], wide ? ADDR : 4u);
}

/* The active entry @index of a family, for a monitor linked to the subscriber side. */
static const struct active *listed(uint16_t instance, bool v6, size_t index,
				   size_t *count)
{
	const struct active *found = NULL;
	size_t n = 0;

	if (g_mcast_taken && g_mcast.mib != NULL &&
	    linked(g_mcast.mib, OMCI_MCAST_MONITOR, instance))
		for (size_t i = 0; i < g_mcast.n_active; i++) {
			const struct active *x = &g_mcast.active[i];

			if (omci_mcast_addr_v4(x->group) == v6)
				continue;
			if (n++ == index)
				found = x;
		}
	*count = n;
	return found;
}

static size_t v4_count(uint16_t instance)
{
	size_t n;

	(void)listed(instance, false, SIZE_MAX, &n);
	return n;
}

static size_t v6_count(uint16_t instance)
{
	size_t n;

	(void)listed(instance, true, SIZE_MAX, &n);
	return n;
}

/*
 * A row of an active group list (G.988 9.3.29): the VLAN of the joins, 0
 * for none; the source, 0 for any; the group; the imputed bandwidth; the
 * client; the seconds since its latest join.
 */
static bool entry(uint16_t instance, bool v6, size_t index, void *out,
		  size_t cap)
{
	const size_t size = v6 ? ROW_V6 : ROW_V4;
	const size_t a = v6 ? ADDR : 4u;
	size_t n;
	const struct active *x = listed(instance, v6, index, &n);
	uint8_t *p = out;

	if (x == NULL || cap < size)
		return false;
	memset(p, 0, size);
	omci_be16_store(p, x->asker.tagged ? x->asker.vid : 0u);
	put_addr(&p[2], x->source, v6);
	put_addr(&p[2 + a], x->group, v6);
	omci_be32_store(&p[2 + 2 * a], x->bandwidth);
	put_addr(&p[6 + 2 * a], x->client, v6);
	omci_be32_store(&p[6 + 3 * a], (uint32_t)(g_mcast.now - x->joined));
	return true;
}

static bool v4_entry(uint16_t instance, size_t index, void *out, size_t cap)
{
	return entry(instance, false, index, out, cap);
}

static bool v6_entry(uint16_t instance, size_t index, void *out, size_t cap)
{
	return entry(instance, true, index, out, cap);
}

const struct omci_me_table_source omci_mcast_ipv4_groups = {
	.count = v4_count,
	.entry = v4_entry,
};

const struct omci_me_table_source omci_mcast_ipv6_groups = {
	.count = v6_count,
	.entry = v6_entry,
};

/*
 * The lost group @index of a profile: each group and source of the active
 * list once, whose group is lost and whose row is in the profile's dynamic
 * access control list; *@vlan the VLAN of the row.
 */
static const struct active *lost_at(uint16_t profile, size_t index,
				    size_t *count, uint16_t *vlan)
{
	const struct omci_mcast *m = &g_mcast;
	const bool live = g_mcast_taken && m->mib != NULL;
	const struct active *found = NULL;
	size_t n = 0;

	for (size_t i = 0; live && i < m->n_active; i++) {
		const struct active *x = &m->active[i];
		bool first = true;
		uint16_t p, v;

		for (size_t j = 0; j < i; j++)
			first = first && !same_channel(&m->active[j], x->group,
						       x->source);
		if (!first || !group_lost(m, x->group) ||
		    !row_of(m, x, &p, &v) || p != profile)
			continue;
		if (n++ == index) {
			found = x;
			*vlan = v;
		}
	}
	*count = n;
	return found;
}

static size_t lost_count(uint16_t instance)
{
	uint16_t vlan;
	size_t n;

	(void)lost_at(instance, SIZE_MAX, &n, &vlan);
	return n;
}

/*
 * A row of the lost groups list (G.988 9.3.27): the VLAN, 0 for none; the
 * last four bytes of the source, 0 for any, and of the group.
 */
static bool lost_entry(uint16_t instance, size_t index, void *out, size_t cap)
{
	uint16_t vlan = 0;
	size_t n;
	const struct active *x = lost_at(instance, index, &n, &vlan);
	uint8_t *p = out;

	if (x == NULL || cap < ROW_LOST)
		return false;
	omci_be16_store(p, vlan >= 1u && vlan <= 4095u ? vlan : 0u);
	memcpy(&p[2], &x->source[12], 4);
	memcpy(&p[6], &x->group[12], 4);
	return true;
}

const struct omci_me_table_source omci_mcast_lost_groups = {
	.count = lost_count,
	.entry = lost_entry,
};

size_t omci_mcast_lost_profiles(const struct omci_mcast *m, uint16_t *out,
				size_t cap)
{
	size_t n = 0;

	for (size_t i = 0; m != NULL && m->mib != NULL && i < m->n_active;
	     i++) {
		bool dup = false;
		uint16_t p, v;

		if (!group_lost(m, m->active[i].group) ||
		    !row_of(m, &m->active[i], &p, &v))
			continue;
		for (size_t k = 0; k < n; k++)
			dup = dup || out[k] == p;
		if (!dup && n < cap)
			out[n++] = p;
	}
	return n;
}

/* ---- What the stick's panel shows ----------------------------------- */

static uint32_t clamp32(uint64_t v)
{
	return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

/* An address as text, or none for one all zero. */
static void put_addr_text(const struct omci_publish_ops *ops, void *ctx,
			  const char *name, const uint8_t *a)
{
	static const uint8_t zero[ADDR];
	char text[40];

	if (memcmp(a, zero, ADDR) == 0) {
		ops->none(ctx, name);
		return;
	}
	addr_text(a, text);
	ops->str(ctx, name, text);
}

/* A number, or none when it is not @known. */
static void put_known(const struct omci_publish_ops *ops, void *ctx,
		      const char *name, uint32_t v, bool known)
{
	if (known)
		ops->num(ctx, name, v);
	else
		ops->none(ctx, name);
}

/* A VLAN of a row: 1 to 4094, or none. */
static void put_vlan(const struct omci_publish_ops *ops, void *ctx,
		     const char *name, uint32_t vid)
{
	put_known(ops, ctx, name, vid, vid >= 1u && vid <= 4094u);
}

/*
 * The rows of the dynamic access control lists of the subscriber's
 * packages, each profile once, as the groups the policy allows.
 */
static void put_allowed(const struct omci_mcast *m, uint16_t sub,
			const struct omci_publish_ops *ops, void *ctx)
{
	const struct asker any = { .known = false };
	struct omci_mcast_acl_row rows[OMCI_MCAST_ACL_ROWS];
	struct candidates c;

	candidates_of(m->mib, sub, &any, &c);
	ops->open(ctx, "allowed", true);
	for (size_t k = 0; k < c.n; k++) {
		bool seen = false;
		size_t n;

		for (size_t j = 0; j < k; j++)
			seen = seen || c.pkg[j].profile == c.pkg[k].profile;
		if (seen)
			continue;
		n = omci_mcast_acl_read(m->mib, c.pkg[k].profile,
					OMCI_MCAST_PROFILE_DYNAMIC_ACL, rows,
					ARRAY_SIZE(rows));
		for (size_t r = 0; r < n; r++) {
			ops->open(ctx, NULL, false);
			put_addr_text(ops, ctx, "from", rows[r].first);
			put_addr_text(ops, ctx, "to", rows[r].last);
			put_vlan(ops, ctx, "vlan", rows[r].vlan);
			put_known(ops, ctx, "bandwidth_bps",
				  clamp32((uint64_t)rows[r].bandwidth * 8u),
				  rows[r].bandwidth != 0u);
			ops->flag(ctx, "preview", rows[r].preview_length != 0u);
			ops->close(ctx, false);
		}
	}
	ops->close(ctx, true);
}

/*
 * What the OLT set in the multicast operations profile and the subscriber's
 * config info: the timers 0 leaves to the defaults of IGMP (RFC 3376), as
 * the bridge and the proxy take them; the VLAN of the groups towards the
 * host, of a profile that tags them downstream.
 */
static void put_policy(const struct omci_mcast *m,
		       const struct omci_publish_ops *ops, void *ctx)
{
	static const char *const mode[] = { "snoop", "snoop_proxy", "proxy" };
	struct omci_hw_mcast_ds d = { .vid = 0 };
	uint8_t tci[3] = { 0 };
	uint32_t fn, groups, bw, v;
	size_t len = 0;
	uint16_t p, sub;

	if (m->mib == NULL || !port_profile(m->mib, &p) ||
	    !subscriber(m->mib, &sub)) {
		ops->none(ctx, "policy");
		return;
	}
	ops->open(ctx, "policy", false);
	ops->num(ctx, "igmp_version",
		 attr_be(m->mib, OMCI_MCAST_PROFILE, p,
			 OMCI_MCAST_PROFILE_IGMP_VERSION, 1));
	fn = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
		     OMCI_MCAST_PROFILE_IGMP_FUNCTION, 1);
	ops->str(ctx, "mode", mode[fn < ARRAY_SIZE(mode) ? fn : 2u]);
	ops->flag(ctx, "fast_leave",
		  attr_be(m->mib, OMCI_MCAST_PROFILE, p,
			  OMCI_MCAST_PROFILE_IMMEDIATE_LEAVE, 1) != 0u);
	groups = attr_be(m->mib, OMCI_MCAST_SUBSCRIBER, sub,
			 OMCI_MCAST_SUBSCRIBER_MAX_GROUPS, 2);
	bw = attr_be(m->mib, OMCI_MCAST_SUBSCRIBER, sub,
		     OMCI_MCAST_SUBSCRIBER_MAX_BANDWIDTH, 4);
	put_known(ops, ctx, "max_groups", groups, groups != 0u);
	put_known(ops, ctx, "max_bandwidth_bps", clamp32((uint64_t)bw * 8u),
		  bw != 0u);
	v = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
		    OMCI_MCAST_PROFILE_ROBUSTNESS, 1);
	ops->num(ctx, "robustness", v != 0u ? v : 2u);
	v = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
		    OMCI_MCAST_PROFILE_QUERY_INTERVAL, 4);
	ops->num(ctx, "query_interval_s", v != 0u ? v : 125u);
	v = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
		    OMCI_MCAST_PROFILE_QUERY_RESPONSE, 4);
	ops->num(ctx, "query_response_ms",
		 clamp32((uint64_t)(v != 0u ? v : 100u) * 100u));
	v = attr_be(m->mib, OMCI_MCAST_PROFILE, p,
		    OMCI_MCAST_PROFILE_LAST_MEMBER, 4);
	ops->num(ctx, "last_member_ms",
		 clamp32((uint64_t)(v != 0u ? v : 10u) * 100u));
	(void)!omci_mib_attr_get(m->mib, OMCI_MCAST_PROFILE, p,
				 OMCI_MCAST_PROFILE_DS_TCI, tci, sizeof(tci),
				 &len);
	put_vlan(ops, ctx, "host_vlan",
		 ds_tag(tci, OMCI_MCAST_VID_ANY, &d) &&
				 d.action != OMCI_HW_MCAST_DS_STRIP ?
			 d.tci & 0x0fffu :
			 0u);
	put_allowed(m, sub, ops, ctx);
	ops->open(ctx, "static", true);
	for (size_t k = 0; k < m->n_statics; k++) {
		ops->open(ctx, NULL, false);
		put_addr_text(ops, ctx, "group", m->statics[k].group);
		put_vlan(ops, ctx, "vlan", m->statics[k].vid);
		ops->close(ctx, false);
	}
	ops->close(ctx, true);
	ops->close(ctx, false);
}

/* The groups the bridge forwards for the subscriber side, an entry each. */
static void put_groups(const struct omci_mcast *m,
		       const struct omci_publish_ops *ops, void *ctx)
{
	ops->open(ctx, "groups", true);
	for (size_t i = 0; i < m->n_active; i++) {
		const struct active *x = &m->active[i];
		const struct watch *w = watch_of(m, x->group);

		if (!x->held)
			continue;
		ops->open(ctx, NULL, false);
		put_addr_text(ops, ctx, "group", x->group);
		put_addr_text(ops, ctx, "source", x->source);
		put_vlan(ops, ctx, "vlan", vlan_of(&x->asker));
		put_addr_text(ops, ctx, "client", x->client);
		ops->num(ctx, "since_s", clamp32(x->since));
		put_known(ops, ctx, "rate_bps",
			  w != NULL ? clamp32(w->rate) : 0u,
			  w != NULL && w->rated);
		ops->close(ctx, false);
	}
	ops->close(ctx, true);
}

/* The refused joins and the events, the latest first. */
static void put_rings(const struct omci_mcast *m,
		      const struct omci_publish_ops *ops, void *ctx)
{
	ops->open(ctx, "rejected", true);
	for (size_t k = 1; k <= m->n_refused; k++) {
		const struct refusal *r =
			&m->refused[(m->refused_next + MAX_REFUSED - k) %
				    MAX_REFUSED];

		ops->open(ctx, NULL, false);
		ops->num(ctx, "at_s", clamp32(r->at));
		put_addr_text(ops, ctx, "group", r->group);
		put_vlan(ops, ctx, "vlan", r->vlan);
		put_addr_text(ops, ctx, "client", r->client);
		ops->str(ctx, "reason", g_reason[r->verdict]);
		ops->close(ctx, false);
	}
	ops->close(ctx, true);
	ops->open(ctx, "log", true);
	for (size_t k = 1; k <= m->n_events; k++) {
		const struct event *e =
			&m->events[(m->event_next + MAX_EVENTS - k) %
				   MAX_EVENTS];

		ops->open(ctx, NULL, false);
		ops->num(ctx, "at_s", clamp32(e->at));
		ops->str(ctx, "event", g_event[e->kind]);
		put_addr_text(ops, ctx, "group", e->group);
		put_vlan(ops, ctx, "vlan", e->vlan);
		ops->close(ctx, false);
	}
	ops->close(ctx, true);
}

/*
 * Who gives the subscriber's IGMP its priority upstream (upstream_frame()):
 * the profile's tag at control 1 and 2; at 0 and 3 the agent, with the
 * priority it last gave, or the subscriber, when a person chose that.
 */
static void put_priority(const struct omci_mcast *m,
			 const struct omci_publish_ops *ops, void *ctx)
{
	const struct omci_hw_ops *hw = omci_hw();
	const bool host = hw->igmp_host_priority != NULL &&
			  hw->igmp_host_priority();
	unsigned int control, tci;
	uint16_t p;

	if (m->mib == NULL || !port_profile(m->mib, &p)) {
		ops->none(ctx, "priority");
		return;
	}
	control = us_control(m, &tci);
	if (control > 3u)
		control = 0;
	ops->open(ctx, "priority", false);
	ops->num(ctx, "control", control);
	if (control == 1u || control == 2u) {
		ops->str(ctx, "by", "olt");
		ops->num(ctx, "pcp", tci >> 13);
	} else {
		ops->str(ctx, "by", host ? "host" : "stick");
		put_known(ops, ctx, "pcp", (uint32_t)m->pcp_given,
			  !host && m->pcp_given >= 0);
	}
	ops->flag(ctx, "auto", !host);
	ops->num(ctx, "dropped", m->pcp_dropped);
	ops->close(ctx, false);
}

void omci_mcast_publish(const struct omci_mcast *m,
			const struct omci_publish_ops *ops, void *ctx)
{
	static const struct omci_mcast idle;

	if (m == NULL)
		m = &idle;
	ops->num(ctx, "now_s", clamp32(m->now));
	put_policy(m, ops, ctx);
	put_groups(m, ops, ctx);
	put_rings(m, ops, ctx);
	ops->open(ctx, "lost", true);
	for (size_t i = 0; i < m->n_watches; i++) {
		if (!m->watches[i].lost)
			continue;
		ops->open(ctx, NULL, false);
		put_addr_text(ops, ctx, "group", m->watches[i].group);
		ops->num(ctx, "since_s", clamp32(m->watches[i].since));
		ops->close(ctx, false);
	}
	ops->close(ctx, true);
	ops->open(ctx, "counters", false);
	ops->num(ctx, "joins", m->tally.joins);
	ops->num(ctx, "leaves", m->tally.leaves);
	ops->num(ctx, "rejected", m->tally.rejected);
	ops->num(ctx, "queries", m->tally.queries);
	ops->num(ctx, "reports", m->tally.reports);
	ops->close(ctx, false);
	if (m->queried) {
		ops->open(ctx, "querier", false);
		ops->num(ctx, "last_s", clamp32(m->query_at));
		ops->num(ctx, "version", m->query_version);
		ops->close(ctx, false);
	} else {
		ops->none(ctx, "querier");
	}
	put_priority(m, ops, ctx);
}
