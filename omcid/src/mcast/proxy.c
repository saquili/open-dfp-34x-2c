// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The multicast proxy of proxy.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mcast/message.h"
#include "mcast/proxy.h"
#include "omcid/array_size.h"

/* What a profile's timer of zero stands for (G.988 9.3.27, RFC 3376 8). */
#define DEFAULT_ROBUSTNESS 2u
#define DEFAULT_INTERVAL 125u
#define DEFAULT_RESPONSE 100u
#define DEFAULT_LAST_MEMBER 10u

/* The longest frame the proxy makes, and the seed of its jitter. */
#define FRAME_MAX 512u
#define RNG_SEED 2463534242u

/* The IPv4 and IPv6 sides of the tables that have both. */
enum { V4 = 0, V6 = 1 };

static bool is_v6(const uint8_t *group)
{
	return !omci_mcast_addr_v4(group);
}

static bool held(const struct omci_mcast_item *it)
{
	return it->any || it->n != 0u;
}

static bool zero(const uint8_t *a)
{
	static const uint8_t none[OMCI_MCAST_ADDR_LEN];

	return memcmp(a, none, sizeof(none)) == 0;
}

static unsigned int robustness_of(const struct omci_mcast_proxy_params *pr)
{
	return pr->robustness != 0u ? pr->robustness : DEFAULT_ROBUSTNESS;
}

static unsigned int last_member_of(const struct omci_mcast_proxy_params *pr)
{
	return pr->last_member != 0u ? pr->last_member : DEFAULT_LAST_MEMBER;
}

/* Units of 0.1 s as whole seconds, at least one. */
static uint64_t seconds_of(unsigned int tenths)
{
	return tenths < 10u ? 1u : (tenths + 9u) / 10u;
}

static uint32_t next_random(struct omci_mcast_proxy *p)
{
	p->rng ^= p->rng << 13;
	p->rng ^= p->rng >> 17;
	p->rng ^= p->rng << 5;
	return p->rng;
}

static void forget(struct omci_mcast_proxy *p)
{
	const uint32_t rng = p->rng;
	const bool seeded = p->seeded;

	memset(p, 0, sizeof(*p));
	p->rng = rng;
	p->seeded = seeded;
}

void omci_mcast_proxy_reset(struct omci_mcast_proxy *p)
{
	memset(p, 0, sizeof(*p));
	p->rng = RNG_SEED;
}

/* The version the messages of one family are in: the profile's, or the highest. */
static uint8_t version_for(const struct omci_mcast_proxy_params *pr, bool v6)
{
	if (v6)
		return pr->version >= 16u ? pr->version : 17u;
	return pr->version >= 1u && pr->version <= 3u ? pr->version : 3u;
}

/* Who the messages of one family are from; false without an Ethernet address. */
static bool env_for(const struct omci_mcast_proxy_params *pr,
		    const struct omci_mcast_proxy_ops *ops, bool v6,
		    struct omci_mcast_env *e)
{
	memset(e, 0, sizeof(*e));
	e->version = version_for(pr, v6);
	if (ops->mac == NULL || !ops->mac(ops->ctx, e->mac))
		return false;
	if (v6) {
		/* The link-local address of the interface identifier made of the MAC address. */
		e->source[0] = 0xfeu;
		e->source[1] = 0x80u;
		e->source[8] = e->mac[0] ^ 0x02u;
		e->source[9] = e->mac[1];
		e->source[10] = e->mac[2];
		e->source[11] = 0xffu;
		e->source[12] = 0xfeu;
		memcpy(&e->source[13], &e->mac[3], 3);
	} else if (pr->querier[0] != 0u || pr->querier[1] != 0u ||
		   pr->querier[2] != 0u || pr->querier[3] != 0u) {
		omci_mcast_addr_set_v4(e->source, pr->querier);
	}
	return true;
}

/* ---- What the OLT was told ------------------------------------------ */

static bool same_tag(const struct omci_mcast_tag *a,
		     const struct omci_mcast_tag *b)
{
	return a->tagged == b->tagged && (!a->tagged || a->vid == b->vid);
}

static struct omci_mcast_told *told_of(struct omci_mcast_proxy *p,
				       const uint8_t *group,
				       const struct omci_mcast_tag *tag)
{
	for (size_t i = 0; i < p->n_told; i++)
		if (memcmp(p->told[i].item.group, group, OMCI_MCAST_ADDR_LEN) ==
			    0 &&
		    same_tag(&p->told[i].tag, tag))
			return &p->told[i];
	return NULL;
}

static bool has_source(const struct omci_mcast_item *it, const uint8_t *source)
{
	for (unsigned int i = 0; i < it->n; i++)
		if (memcmp(it->source[i], source, OMCI_MCAST_ADDR_LEN) == 0)
			return true;
	return false;
}

/* The same state: from every source, or from the same sources in any order. */
static bool same_state(const struct omci_mcast_item *a,
		       const struct omci_mcast_item *b)
{
	if (a->any != b->any)
		return false;
	if (a->any)
		return true;
	if (a->n != b->n)
		return false;
	for (unsigned int i = 0; i < a->n; i++)
		if (!has_source(b, a->source[i]))
			return false;
	return true;
}

/* Record what a report told: the group held in its tag, or gone. */
static void commit(struct omci_mcast_proxy *p, const struct omci_mcast_told *e)
{
	struct omci_mcast_told *t = told_of(p, e->item.group, &e->tag);

	if (!held(&e->item)) {
		if (t != NULL)
			*t = p->told[--p->n_told];
		return;
	}
	if (t == NULL && p->n_told < ARRAY_SIZE(p->told))
		t = &p->told[p->n_told++];
	if (t != NULL)
		*t = *e;
}

/*
 * What the subscribers hold, a group once in each tag, from every source or
 * from some.
 */
static size_t held_groups(const struct omci_mcast_chan *chans, size_t n,
			  struct omci_mcast_told *out, size_t cap)
{
	size_t count = 0;

	for (size_t i = 0; i < n; i++) {
		struct omci_mcast_told *e = NULL;

		for (size_t k = 0; k < count; k++)
			if (memcmp(out[k].item.group, chans[i].group,
				   OMCI_MCAST_ADDR_LEN) == 0 &&
			    same_tag(&out[k].tag, &chans[i].tag))
				e = &out[k];
		if (e == NULL) {
			if (count == cap)
				continue;
			e = &out[count++];
			memset(e, 0, sizeof(*e));
			memcpy(e->item.group, chans[i].group,
			       OMCI_MCAST_ADDR_LEN);
			e->tag = chans[i].tag;
		}
		if (zero(chans[i].source)) {
			e->item.any = true;
			e->item.n = 0;
		} else if (!e->item.any &&
			   !has_source(&e->item, chans[i].source) &&
			   e->item.n < OMCI_MCAST_ITEM_SOURCES) {
			memcpy(e->item.source[e->item.n++], chans[i].source,
			       OMCI_MCAST_ADDR_LEN);
		}
	}
	return count;
}

/*
 * Send the entries of one family, a frame in each tag for as many of its
 * entries as fit. With @change the entries are changes, and each frame that
 * went is noted as told; without, they answer a query. A frame that could not
 * be sent ends the entries of its tag, which go again at the next run.
 */
static void send_entries(struct omci_mcast_proxy *p,
			 const struct omci_mcast_proxy_params *pr,
			 const struct omci_mcast_proxy_ops *ops,
			 const struct omci_mcast_told *e, size_t n, bool v6,
			 bool change)
{
	struct omci_mcast_item items[2u * OMCI_MCAST_PROXY_GROUPS];
	size_t index[2u * OMCI_MCAST_PROXY_GROUPS];
	bool done[2u * OMCI_MCAST_PROXY_GROUPS] = { false };
	struct omci_mcast_env env;

	if (!env_for(pr, ops, v6, &env))
		return;
	for (size_t i = 0; i < n; i++) {
		const struct omci_mcast_tag tag = e[i].tag;
		size_t ni = 0, off = 0;

		if (done[i])
			continue;
		for (size_t k = i; k < n; k++) {
			if (done[k] || !same_tag(&e[k].tag, &tag))
				continue;
			items[ni] = e[k].item;
			index[ni++] = k;
			done[k] = true;
		}
		while (off < ni) {
			uint8_t frame[FRAME_MAX];
			size_t used = 0;
			const size_t len = omci_mcast_build_report(
				frame, sizeof(frame), &env, change, &items[off],
				ni - off, &used);

			if (used == 0u ||
			    (len != 0u && !ops->up(ops->ctx, frame, len, &tag)))
				break;
			for (size_t k = off; change && k < off + used; k++)
				commit(p, &e[index[k]]);
			off += used;
		}
	}
}

/* A group whose state changed: the state now and whether it was held. */
struct change {
	struct omci_mcast_told after;
	bool was_held;
};

/*
 * Tell the OLT the changes of one family. A change a version before 3 cannot
 * tell, the sources of a group that stays held, is only noted.
 */
static void tell(struct omci_mcast_proxy *p,
		 const struct omci_mcast_proxy_params *pr,
		 const struct omci_mcast_proxy_ops *ops,
		 const struct change *chg, size_t n, bool v6)
{
	struct omci_mcast_told batch[2u * OMCI_MCAST_PROXY_GROUPS];
	size_t nb = 0;
	bool v3;

	v3 = version_for(pr, v6) == 3u || version_for(pr, v6) == 17u;
	for (size_t i = 0; i < n; i++) {
		if (is_v6(chg[i].after.item.group) != v6)
			continue;
		if (!v3 && chg[i].was_held && held(&chg[i].after.item))
			commit(p, &chg[i].after);
		else
			batch[nb++] = chg[i].after;
	}
	send_entries(p, pr, ops, batch, nb, v6, true);
}

/* Tell the OLT what the subscribers hold, now compared with what it was told. */
static void report(struct omci_mcast_proxy *p,
		   const struct omci_mcast_proxy_params *pr,
		   const struct omci_mcast_proxy_ops *ops,
		   const struct omci_mcast_chan *chans, size_t n_chans)
{
	struct omci_mcast_told want[OMCI_MCAST_PROXY_GROUPS];
	struct change chg[2u * OMCI_MCAST_PROXY_GROUPS];
	const size_t n_want =
		held_groups(chans, n_chans, want, ARRAY_SIZE(want));
	size_t n = 0, room;

	/* The groups no longer held go first. */
	for (size_t i = 0; i < p->n_told; i++) {
		bool still = false;

		for (size_t k = 0; k < n_want; k++)
			still = still ||
				(memcmp(want[k].item.group,
					p->told[i].item.group,
					OMCI_MCAST_ADDR_LEN) == 0 &&
				 same_tag(&want[k].tag, &p->told[i].tag));
		if (still)
			continue;
		memset(&chg[n].after, 0, sizeof(chg[n].after));
		memcpy(chg[n].after.item.group, p->told[i].item.group,
		       OMCI_MCAST_ADDR_LEN);
		chg[n].after.tag = p->told[i].tag;
		chg[n++].was_held = true;
	}
	/*
	 * A group new to the OLT needs a place to be noted in. Those that are
	 * let go give theirs only once the OLT has been told, at the next run.
	 */
	room = ARRAY_SIZE(p->told) - p->n_told;
	for (size_t k = 0; k < n_want; k++) {
		const struct omci_mcast_told *t =
			told_of(p, want[k].item.group, &want[k].tag);

		if (t != NULL && same_state(&t->item, &want[k].item))
			continue;
		if (t == NULL && room-- == 0u)
			continue;
		chg[n].after = want[k];
		chg[n++].was_held = t != NULL;
	}
	if (n == 0u)
		return;
	tell(p, pr, ops, chg, n, V4);
	tell(p, pr, ops, chg, n, V6);
}

/* ---- Answers and queries -------------------------------------------- */

/* Answer a query of the OLT: what is held of one group, or of every group of a family. */
static void answer(struct omci_mcast_proxy *p,
		   const struct omci_mcast_proxy_params *pr,
		   const struct omci_mcast_proxy_ops *ops, bool v6,
		   const uint8_t *group)
{
	struct omci_mcast_told e[OMCI_MCAST_PROXY_GROUPS];
	size_t n = 0;

	for (size_t i = 0; i < p->n_told; i++)
		if (is_v6(p->told[i].item.group) == v6 &&
		    (group == NULL || memcmp(group, p->told[i].item.group,
					     OMCI_MCAST_ADDR_LEN) == 0))
			e[n++] = p->told[i];
	send_entries(p, pr, ops, e, n, v6, false);
}

/* Send a query to the subscribers in @tag: general for no group, or about one. */
static void query_down(const struct omci_mcast_proxy_params *pr,
		       const struct omci_mcast_proxy_ops *ops, bool v6,
		       const uint8_t *group, unsigned int max_resp,
		       const struct omci_mcast_tag *tag)
{
	uint8_t frame[FRAME_MAX];
	struct omci_mcast_env e;
	size_t len;

	if (!env_for(pr, ops, v6, &e))
		return;
	len = omci_mcast_build_query(frame, sizeof(frame), &e, group, max_resp,
				     robustness_of(pr),
				     pr->query_interval != 0u ?
					     pr->query_interval :
					     DEFAULT_INTERVAL);
	if (len != 0u)
		(void)ops->down(ops->ctx, frame, len, tag);
}

/*
 * A general query of one family to the subscribers in each tag the groups they
 * hold are in, or one untagged where they hold none.
 */
static void general_queries(const struct omci_mcast_proxy *p,
			    const struct omci_mcast_proxy_params *pr,
			    const struct omci_mcast_proxy_ops *ops, bool v6,
			    unsigned int max_resp)
{
	struct omci_mcast_tag tags[OMCI_MCAST_PROXY_TAGS] = { { 0 } };
	size_t nt = 0;

	for (size_t i = 0; i < p->n_told; i++) {
		bool seen = false;

		if (is_v6(p->told[i].item.group) != v6)
			continue;
		for (size_t k = 0; k < nt; k++)
			seen = seen || same_tag(&tags[k], &p->told[i].tag);
		if (!seen && nt < ARRAY_SIZE(tags))
			tags[nt++] = p->told[i].tag;
	}
	for (size_t k = 0; k < (nt != 0u ? nt : 1u); k++)
		query_down(pr, ops, v6, NULL, max_resp, &tags[k]);
}

void omci_mcast_proxy_left(struct omci_mcast_proxy *p,
			   const struct omci_mcast_proxy_params *pr,
			   const uint8_t group[static OMCI_MCAST_ADDR_LEN],
			   const struct omci_mcast_tag *tag, uint64_t now)
{
	struct omci_mcast_check *c;

	if (pr->function == OMCI_MCAST_FUNCTION_SNOOPING || pr->fast_leave)
		return;
	for (size_t i = 0; i < p->n_check; i++)
		if (memcmp(p->check[i].group, group, OMCI_MCAST_ADDR_LEN) == 0)
			return;
	if (p->n_check == ARRAY_SIZE(p->check))
		return;
	c = &p->check[p->n_check++];
	memcpy(c->group, group, OMCI_MCAST_ADDR_LEN);
	c->tag = *tag;
	c->left = robustness_of(pr);
	c->next = now;
}

void omci_mcast_proxy_joined(struct omci_mcast_proxy *p,
			     const uint8_t group[static OMCI_MCAST_ADDR_LEN])
{
	for (size_t i = 0; i < p->n_check; i++)
		if (memcmp(p->check[i].group, group, OMCI_MCAST_ADDR_LEN) ==
		    0) {
			p->check[i] = p->check[--p->n_check];
			return;
		}
}

void omci_mcast_proxy_query(struct omci_mcast_proxy *p,
			    const struct omci_mcast_report *q, uint64_t now)
{
	const unsigned int side = q->version >= 16u ? V6 : V4;
	/* A time within the response time the query allows, a second at least short of it. */
	const uint64_t span = q->max_resp / 10u;
	uint64_t at;

	if (!zero(q->query_group)) {
		for (size_t i = 0; i < p->n_asked; i++)
			if (memcmp(p->asked[i], q->query_group,
				   OMCI_MCAST_ADDR_LEN) == 0)
				return;
		if (p->n_asked == ARRAY_SIZE(p->asked))
			return;
		memcpy(p->asked[p->n_asked++], q->query_group,
		       OMCI_MCAST_ADDR_LEN);
		return;
	}
	at = now + (span > 1u ? next_random(p) % span : 0u);
	if (!p->answering[side] || at < p->answer_at[side]) {
		p->answering[side] = true;
		p->answer_at[side] = at;
	}
}

/* The checks of leaves: ask again, or let the group go. */
static void check_leaves(struct omci_mcast_proxy *p,
			 const struct omci_mcast_proxy_params *pr,
			 const struct omci_mcast_proxy_ops *ops, uint64_t now)
{
	for (size_t i = 0; i < p->n_check;) {
		struct omci_mcast_check *c = &p->check[i];
		uint8_t group[OMCI_MCAST_ADDR_LEN];

		if (now < c->next) {
			i++;
			continue;
		}
		if (c->left != 0u) {
			c->left--;
			c->next = now + seconds_of(last_member_of(pr));
			query_down(pr, ops, is_v6(c->group), c->group,
				   last_member_of(pr), &c->tag);
			i++;
			continue;
		}
		memcpy(group, c->group, sizeof(group));
		*c = p->check[--p->n_check];
		ops->drop(ops->ctx, group);
	}
}

void omci_mcast_proxy_run(struct omci_mcast_proxy *p,
			  const struct omci_mcast_proxy_params *pr,
			  const struct omci_mcast_proxy_ops *ops,
			  const struct omci_mcast_chan *chans, size_t n,
			  uint64_t now)
{
	uint8_t mac[OMCI_MCAST_MAC_LEN];

	if (pr->function == OMCI_MCAST_FUNCTION_SNOOPING) {
		forget(p);
		return;
	}
	/* The jitter differs from one ONU to the next: the address seeds it. */
	if (!p->seeded && ops->mac != NULL && ops->mac(ops->ctx, mac)) {
		for (size_t i = 0; i < sizeof(mac); i++)
			p->rng = p->rng * 31u + mac[i];
		if (p->rng == 0u)
			p->rng = RNG_SEED;
		p->seeded = true;
	}
	check_leaves(p, pr, ops, now);
	report(p, pr, ops, chans, n);

	for (unsigned int side = V4; side <= V6; side++) {
		if (p->answering[side] && now >= p->answer_at[side]) {
			p->answering[side] = false;
			answer(p, pr, ops, side == V6, NULL);
		}
	}
	for (size_t i = 0; i < p->n_asked; i++)
		answer(p, pr, ops, is_v6(p->asked[i]), p->asked[i]);
	p->n_asked = 0;

	if (pr->function == OMCI_MCAST_FUNCTION_PROXY && now >= p->query_at) {
		const unsigned int response = pr->query_response != 0u ?
						      pr->query_response :
						      DEFAULT_RESPONSE;

		p->query_at = now + (pr->query_interval != 0u ?
					     pr->query_interval :
					     DEFAULT_INTERVAL);
		general_queries(p, pr, ops, false, response);
		general_queries(p, pr, ops, true, response);
	}
}
