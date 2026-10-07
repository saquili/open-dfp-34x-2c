// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The PPP negotiation: LCP, PAP or CHAP-MD5, then IPCP and IPv6CP (RFC 1661,
 * 1334, 1994, 1332, 5072).
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "pppoed.h"

/* The control packet header, the restart timer and the most attempts. */
#define CP_HDR_LEN 4
#define RESTART_MS 3000
#define MAX_TRIES 10

static uint8_t next_id(void)
{
	static uint8_t id;

	return ++id;
}

/* A random magic number. */
static uint32_t random_u32(void)
{
	uint32_t v = 0;
	int fd = open("/dev/urandom", O_RDONLY);

	if (fd >= 0) {
		if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
			v = 0;
		close(fd);
	}

	if (!v)
		v = 0xbadc0de5;
	return v;
}

static size_t opt_put(uint8_t *p, size_t off, uint8_t type, const void *val,
		      size_t len)
{
	p[off] = type;
	p[off + 1] = (uint8_t)(len + 2);
	if (len)
		memcpy(p + off + 2, val, len);
	return off + 2 + len;
}

static size_t opt_put32(uint8_t *p, size_t off, uint8_t type, uint32_t v)
{
	uint8_t be[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
			  (uint8_t)(v >> 8), (uint8_t)v };

	return opt_put(p, off, type, be, sizeof(be));
}

static uint32_t opt_get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

static int cp_send(struct ppp_link *l, uint16_t proto, uint8_t code, uint8_t id,
		   const void *body, size_t len)
{
	uint8_t buf[PPP_CTRL_MAX];

	if (len + CP_HDR_LEN > sizeof(buf))
		return -EMSGSIZE;

	buf[0] = code;
	buf[1] = id;
	pppoe_put16(buf + 2, (uint16_t)(len + CP_HDR_LEN));
	if (len)
		memcpy(buf + CP_HDR_LEN, body, len);

	return ppp_send(l, proto, buf, len + CP_HDR_LEN);
}

static void echo_reply(struct ppp_link *l, uint8_t id)
{
	uint8_t v[4] = { (uint8_t)(l->magic >> 24), (uint8_t)(l->magic >> 16),
			 (uint8_t)(l->magic >> 8), (uint8_t)l->magic };

	cp_send(l, PPP_PROTO_LCP, CP_ECHO_REP, id, v, sizeof(v));
}

/*
 * Answer the peer's Configure-Request: reject the options this client does
 * not know, ask for CHAP-MD5 instead of an authentication it cannot do,
 * and acknowledge the rest.
 *
 * Return: 0 when the request was acknowledged, 1 when it was rejected or
 * refused, or a negative errno.
 */
static int lcp_answer_req(struct ppp_link *l, uint8_t id, const uint8_t *opts,
			  size_t len)
{
	uint8_t rej[PPP_CTRL_MAX], nak[PPP_CTRL_MAX];
	size_t rejlen = 0, naklen = 0, off = 0;

	while (off + 2 <= len) {
		uint8_t type = opts[off];
		uint8_t olen = opts[off + 1];
		const uint8_t *val = opts + off + 2;

		if (olen < 2 || off + olen > len)
			return -EPROTO;

		switch (type) {
		case LCP_OPT_MRU:
			if (olen == 4) {
				l->mru = pppoe_get16(val);
				PPPOED_DEBUG("peer MRU %u", l->mru);
				break;
			}
			memcpy(rej + rejlen, opts + off, olen);
			rejlen += olen;
			break;

		case LCP_OPT_MAGIC:
			if (olen == 6) {
				l->peer_magic = opt_get32(val);
				break;
			}
			memcpy(rej + rejlen, opts + off, olen);
			rejlen += olen;
			break;

		case LCP_OPT_AUTH:
			if (olen >= 4) {
				uint16_t proto = pppoe_get16(val);

				if (proto == PPP_PROTO_PAP && olen == 4) {
					l->auth = PPP_PROTO_PAP;
					PPPOED_INFO("the peer asks for PAP");
					break;
				}
				if (proto == PPP_PROTO_CHAP && olen == 5 &&
				    val[2] == CHAP_MD5) {
					l->auth = PPP_PROTO_CHAP;
					l->chap_alg = CHAP_MD5;
					PPPOED_INFO(
						"the peer asks for CHAP-MD5");
					break;
				}

				{
					uint8_t v[3] = {
						(uint8_t)(PPP_PROTO_CHAP >> 8),
						(uint8_t)PPP_PROTO_CHAP,
						CHAP_MD5
					};

					PPPOED_WARN(
						"the peer asks for authentication %04x, which this client does not do; offering CHAP-MD5",
						proto);
					naklen = opt_put(nak, naklen,
							 LCP_OPT_AUTH, v,
							 sizeof(v));
				}
				break;
			}
			memcpy(rej + rejlen, opts + off, olen);
			rejlen += olen;
			break;

		default:

			PPPOED_DEBUG("rejecting LCP option %u (%u bytes)", type,
				     olen);
			memcpy(rej + rejlen, opts + off, olen);
			rejlen += olen;
			break;
		}
		off += olen;
	}

	if (rejlen)
		return cp_send(l, PPP_PROTO_LCP, CP_CONF_REJ, id, rej, rejlen) ?:
			       1;
	if (naklen)
		return cp_send(l, PPP_PROTO_LCP, CP_CONF_NAK, id, nak, naklen) ?:
			       1;
	return cp_send(l, PPP_PROTO_LCP, CP_CONF_ACK, id, opts, len);
}

/*
 * Take the peer's Nak or Reject of our LCP request: an MRU it names is no
 * longer asked for, and a magic number it naks is drawn again.
 */
static void lcp_take_refusal(struct ppp_link *l, uint8_t code,
			     const uint8_t *opts, size_t olen, bool *want_mru,
			     bool *want_magic)
{
	for (size_t off = 0; off + 2 <= olen;) {
		const uint8_t t = opts[off], ol = opts[off + 1];

		if (ol < 2 || off + ol > olen)
			break;
		if (t == LCP_OPT_MRU) {
			*want_mru = false;
		} else if (t == LCP_OPT_MAGIC) {
			if (code == CP_CONF_NAK && ol == 6)
				l->magic = random_u32();
			else
				*want_magic = false;
		}
		off += ol;
	}
}

/* Take the peer's Nak of our IPCP request: the addresses it gives. */
static void ipcp_take_nak(struct ppp_link *l, const uint8_t *opts, size_t olen)
{
	for (size_t off = 0; off + 2 <= olen;) {
		const uint8_t t = opts[off], ol = opts[off + 1];

		if (ol < 2 || off + ol > olen)
			break;
		if (ol == 6) {
			const uint32_t v = htonl(opt_get32(opts + off + 2));

			if (t == IPCP_OPT_ADDRESS)
				l->local_ip = v;
			else if (t == IPCP_OPT_DNS1)
				l->dns1 = v;
			else if (t == IPCP_OPT_DNS2)
				l->dns2 = v;
		}
		off += ol;
	}
}

/*
 * Open LCP: our request acknowledged, and the peer's. Our request goes out
 * when the restart timer runs out and after a refusal, never because a frame
 * came in: a peer whose LCP is open takes a second Configure-Request as the
 * start of a new negotiation (RFC 1661, state Opened), and a concentrator of
 * an operator's ended the session at that, 10 ms after it had sent its CHAP
 * challenge (measured).
 */
static int lcp_negotiate(struct ppp_link *l)
{
	bool ours = false, theirs = false, want_mru = true, want_magic = true;
	unsigned int tries = 0;
	long due = pppoed_now_ms();
	uint8_t id = 0;

	l->magic = random_u32();

	while (tries < MAX_TRIES) {
		uint8_t body[PPP_CTRL_MAX];
		size_t len = 0;
		uint16_t proto;
		long wait;
		int n;

		if (!ours && pppoed_now_ms() >= due) {
			if (want_mru) {
				uint8_t v[2];

				pppoe_put16(v, l->max_payload);
				len = opt_put(body, len, LCP_OPT_MRU, v,
					      sizeof(v));
			}
			if (want_magic)
				len = opt_put32(body, len, LCP_OPT_MAGIC,
						l->magic);
			if (!id)
				id = next_id();
			cp_send(l, PPP_PROTO_LCP, CP_CONF_REQ, id, body, len);
			PPPOED_DEBUG("LCP configure-request %u sent, try %u",
				     id, tries + 1);
			tries++;
			due = pppoed_now_ms() + RESTART_MS;
		}

		wait = ours ? RESTART_MS : due - pppoed_now_ms();
		n = ppp_recv(l, &proto, body, sizeof(body),
			     wait > 0 ? (int)wait : 0);
		if (n == -ETIMEDOUT) {
			/* Waiting on the peer's request alone counts too. */
			if (ours)
				tries++;
			continue;
		}
		if (n < 0)
			return n;
		if (n < CP_HDR_LEN)
			continue;

		if (proto != PPP_PROTO_LCP) {
			PPPOED_DEBUG("protocol %04x during LCP, not yet",
				     proto);
			continue;
		}

		{
			uint8_t code = body[0], rid = body[1];
			uint16_t flen = pppoe_get16(body + 2);
			const uint8_t *opts = body + CP_HDR_LEN;
			size_t olen;

			if (flen < CP_HDR_LEN || (size_t)flen > (size_t)n)
				continue;
			olen = (size_t)flen - CP_HDR_LEN;

			switch (code) {
			case CP_CONF_REQ:
				if (lcp_answer_req(l, rid, opts, olen) == 0)
					theirs = true;
				break;

			case CP_CONF_ACK:
				if (rid == id) {
					ours = true;
					PPPOED_DEBUG("LCP configure-ack %u",
						     rid);
				}
				break;

			case CP_CONF_NAK:
			case CP_CONF_REJ: {
				if (rid != id) {
					PPPOED_DEBUG(
						"stale LCP %s for id %u, waiting on %u",
						code == CP_CONF_NAK ? "nak" :
								      "reject",
						rid, id);
					break;
				}

				lcp_take_refusal(l, code, opts, olen, &want_mru,
						 &want_magic);
				id = next_id();
				ours = false;
				tries = 0;
				due = pppoed_now_ms();
				break;
			}

			case CP_ECHO_REQ:
				echo_reply(l, rid);
				break;

			case CP_TERM_REQ:
				cp_send(l, PPP_PROTO_LCP, CP_TERM_ACK, rid,
					NULL, 0);
				PPPOED_ERR(
					"the peer terminated the link during LCP");
				return -ECONNRESET;

			case CP_CODE_REJ:
				PPPOED_WARN(
					"the peer rejected an LCP code we sent");
				break;

			default:
				PPPOED_DEBUG("LCP code %u ignored", code);
				break;
			}
		}

		if (ours && theirs) {
			PPPOED_INFO("LCP up, MRU %u, peer magic %08x%s", l->mru,
				    l->peer_magic,
				    l->auth == PPP_PROTO_PAP ?
					    ", PAP" :
				    l->auth == PPP_PROTO_CHAP ?
					    ", CHAP-MD5" :
					    ", no authentication");
			return 0;
		}
	}

	PPPOED_ERR("LCP did not come up in %u attempts", MAX_TRIES);
	return -ETIMEDOUT;
}

/*
 * An LCP frame while the peer authenticates us: an Echo-Request is answered;
 * a Terminate-Request, the way a peer ends a link it does not accept, is
 * acknowledged and reported with the reason it carries; anything else is
 * passed over. Returns 0 to go on, or -ECONNRESET for a terminated link.
 */
static int auth_lcp(struct ppp_link *l, const uint8_t *in, int n,
		    const char *stage)
{
	uint16_t flen = pppoe_get16(in + 2);
	char why[96] = "";
	size_t len = 0;

	if (in[0] == CP_ECHO_REQ) {
		echo_reply(l, in[1]);
		return 0;
	}
	if (in[0] != CP_TERM_REQ) {
		PPPOED_DEBUG("LCP code %u passed over during %s", in[0], stage);
		return 0;
	}
	if (flen > CP_HDR_LEN && flen <= (size_t)n)
		len = flen - CP_HDR_LEN;
	if (len > sizeof(why) - 1)
		len = sizeof(why) - 1;
	for (size_t i = 0; i < len; i++)
		why[i] = isprint(in[CP_HDR_LEN + i]) ? (char)in[CP_HDR_LEN + i] :
						       '.';
	cp_send(l, PPP_PROTO_LCP, CP_TERM_ACK, in[1], NULL, 0);
	PPPOED_ERR("the peer terminated the link during %s%s%s", stage,
		   why[0] ? ": " : "", why);
	return -ECONNRESET;
}

static int pap_authenticate(struct ppp_link *l, const char *user,
			    const char *secret)
{
	size_t ulen = strlen(user), slen = strlen(secret);
	uint8_t body[PPP_CTRL_MAX];
	uint8_t id = next_id();
	unsigned int tries;

	if (ulen > 255 || slen > 255 || ulen + slen + 2 > sizeof(body))
		return -EINVAL;

	body[0] = (uint8_t)ulen;
	memcpy(body + 1, user, ulen);
	body[1 + ulen] = (uint8_t)slen;
	memcpy(body + 2 + ulen, secret, slen);

	for (tries = 0; tries < MAX_TRIES; tries++) {
		uint8_t in[PPP_CTRL_MAX];
		uint16_t proto;
		int n;

		cp_send(l, PPP_PROTO_PAP, PAP_AUTH_REQ, id, body,
			2 + ulen + slen);
		PPPOED_DEBUG("PAP request sent for %s, try %u", user,
			     tries + 1);

		n = ppp_recv(l, &proto, in, sizeof(in), RESTART_MS);
		if (n == -ETIMEDOUT)
			continue;
		if (n < 0)
			return n;
		if (n < CP_HDR_LEN)
			continue;

		if (proto == PPP_PROTO_LCP) {
			int r = auth_lcp(l, in, n, "PAP");

			if (r)
				return r;
			continue;
		}
		if (proto != PPP_PROTO_PAP)
			continue;

		{
			size_t mlen = (size_t)n > CP_HDR_LEN ? in[CP_HDR_LEN] :
							       0;
			char msg[128] = "";

			if (mlen && CP_HDR_LEN + 1 + mlen <= (size_t)n) {
				if (mlen > sizeof(msg) - 1)
					mlen = sizeof(msg) - 1;
				memcpy(msg, in + CP_HDR_LEN + 1, mlen);
			}

			if (in[0] == PAP_AUTH_ACK) {
				PPPOED_INFO("PAP accepted%s%s",
					    msg[0] ? ": " : "", msg);
				return 0;
			}
			if (in[0] == PAP_AUTH_NAK) {
				PPPOED_ERR("PAP refused%s%s",
					   msg[0] ? ": " : "", msg);
				return -EACCES;
			}
		}
	}
	return -ETIMEDOUT;
}

static int chap_authenticate(struct ppp_link *l, const char *user,
			     const char *secret)
{
	unsigned int waited = 0;

	while (waited < 30000) {
		uint8_t in[PPP_CTRL_MAX];
		uint16_t proto;
		int n;

		n = ppp_recv(l, &proto, in, sizeof(in), RESTART_MS);
		if (n == -ETIMEDOUT) {
			waited += RESTART_MS;
			continue;
		}
		if (n == -ENOTCONN)
			PPPOED_ERR("the concentrator ended the session during CHAP, with no answer to it");
		if (n < 0)
			return n;
		if (n < CP_HDR_LEN)
			continue;

		if (proto == PPP_PROTO_LCP) {
			int r = auth_lcp(l, in, n, "CHAP");

			if (r)
				return r;
			continue;
		}
		if (proto != PPP_PROTO_CHAP)
			continue;

		if (in[0] == CHAP_CHALLENGE) {
			uint8_t id = in[1];
			uint16_t flen = pppoe_get16(in + 2);
			uint8_t vsize;
			const uint8_t *chal;
			uint8_t digest[16], resp[PPP_CTRL_MAX];
			size_t rlen, ulen = strlen(user);

			if (flen < CP_HDR_LEN + 1 || (size_t)flen > (size_t)n)
				continue;
			vsize = in[CP_HDR_LEN];
			chal = in + CP_HDR_LEN + 1;
			if (CP_HDR_LEN + 1 + vsize > flen)
				continue;

			md5_three(&id, 1, secret, strlen(secret), chal, vsize,
				  digest);

			resp[0] = sizeof(digest);
			memcpy(resp + 1, digest, sizeof(digest));
			rlen = 1 + sizeof(digest);
			if (ulen > sizeof(resp) - rlen)
				ulen = sizeof(resp) - rlen;
			memcpy(resp + rlen, user, ulen);
			rlen += ulen;

			cp_send(l, PPP_PROTO_CHAP, CHAP_RESPONSE, id, resp,
				rlen);
			PPPOED_DEBUG("CHAP challenge %u of %u bytes answered",
				     id, vsize);
			continue;
		}

		if (in[0] == CHAP_SUCCESS || in[0] == CHAP_FAILURE) {
			uint16_t flen = pppoe_get16(in + 2);
			char msg[128] = "";
			size_t mlen = 0;

			if (flen > CP_HDR_LEN && (size_t)flen <= (size_t)n)
				mlen = (size_t)flen - CP_HDR_LEN;
			if (mlen) {
				if (mlen > sizeof(msg) - 1)
					mlen = sizeof(msg) - 1;
				memcpy(msg, in + CP_HDR_LEN, mlen);
			}
			if (in[0] == CHAP_SUCCESS) {
				PPPOED_INFO("CHAP accepted%s%s",
					    msg[0] ? ": " : "", msg);
				return 0;
			}
			PPPOED_ERR("CHAP refused%s%s", msg[0] ? ": " : "", msg);
			return -EACCES;
		}
	}
	PPPOED_ERR("no CHAP challenge in 30 s");
	return -ETIMEDOUT;
}

/*
 * Answer the peer's IPCP request: take its address, reject the rest.
 *
 * Return: as lcp_answer_req().
 */
static int ipcp_answer_req(struct ppp_link *l, uint8_t id, const uint8_t *opts,
			   size_t len)
{
	uint8_t rej[PPP_CTRL_MAX];
	size_t rejlen = 0, off = 0;

	while (off + 2 <= len) {
		uint8_t type = opts[off];
		uint8_t olen = opts[off + 1];

		if (olen < 2 || off + olen > len)
			return -EPROTO;

		if (type == IPCP_OPT_ADDRESS && olen == 6) {
			l->peer_ip = htonl(opt_get32(opts + off + 2));
		} else {
			PPPOED_DEBUG("rejecting IPCP option %u", type);
			memcpy(rej + rejlen, opts + off, olen);
			rejlen += olen;
		}
		off += olen;
	}

	if (rejlen)
		return cp_send(l, PPP_PROTO_IPCP, CP_CONF_REJ, id, rej, rejlen) ?:
			       1;
	return cp_send(l, PPP_PROTO_IPCP, CP_CONF_ACK, id, opts, len);
}

static bool iid_is_zero(const uint8_t *iid)
{
	static const uint8_t zero[PPP_IID_LEN];

	return !memcmp(iid, zero, PPP_IID_LEN);
}

/* A random interface identifier that is neither zero nor @other. */
static void iid_draw(uint8_t *iid, const uint8_t *other)
{
	do {
		const uint32_t hi = random_u32(), lo = random_u32();

		for (unsigned int i = 0; i < 4; i++) {
			iid[i] = (uint8_t)(hi >> (24 - 8 * i));
			iid[4 + i] = (uint8_t)(lo >> (24 - 8 * i));
		}
	} while (iid_is_zero(iid) ||
		 (other && !memcmp(iid, other, PPP_IID_LEN)));
}

/*
 * Answer the peer's IPv6CP request (RFC 5072): take its interface
 * identifier unless it is zero or ours, which is answered with a Nak that
 * offers another; reject every other option.
 *
 * Return: as lcp_answer_req().
 */
static int ipv6cp_answer_req(struct ppp_link *l, uint8_t id,
			     const uint8_t *opts, size_t len)
{
	uint8_t rej[PPP_CTRL_MAX], nak[PPP_CTRL_MAX];
	size_t rejlen = 0, naklen = 0, off = 0;

	while (off + 2 <= len) {
		uint8_t type = opts[off];
		uint8_t olen = opts[off + 1];
		const uint8_t *val = opts + off + 2;

		if (olen < 2 || off + olen > len)
			return -EPROTO;

		if (type == IPV6CP_OPT_IFACE_ID && olen == 2 + PPP_IID_LEN) {
			if (iid_is_zero(val) ||
			    !memcmp(val, l->iid_local, PPP_IID_LEN)) {
				uint8_t offer[PPP_IID_LEN];

				iid_draw(offer, l->iid_local);
				naklen = opt_put(nak, naklen,
						 IPV6CP_OPT_IFACE_ID, offer,
						 sizeof(offer));
			} else {
				memcpy(l->iid_peer, val, PPP_IID_LEN);
			}
		} else {
			PPPOED_DEBUG("rejecting IPv6CP option %u", type);
			memcpy(rej + rejlen, opts + off, olen);
			rejlen += olen;
		}
		off += olen;
	}

	if (rejlen)
		return cp_send(l, PPP_PROTO_IPV6CP, CP_CONF_REJ, id, rej, rejlen) ?:
			       1;
	if (naklen)
		return cp_send(l, PPP_PROTO_IPV6CP, CP_CONF_NAK, id, nak, naklen) ?:
			       1;
	return cp_send(l, PPP_PROTO_IPV6CP, CP_CONF_ACK, id, opts, len);
}

/*
 * Take the peer's Nak of our IPv6CP request: the identifier it offers,
 * unless that is zero or its own.
 */
static void ipv6cp_take_nak(struct ppp_link *l, const uint8_t *opts,
			    size_t olen)
{
	for (size_t off = 0; off + 2 <= olen;) {
		const uint8_t t = opts[off], ol = opts[off + 1];

		if (ol < 2 || off + ol > olen)
			break;
		if (t == IPV6CP_OPT_IFACE_ID && ol == 2 + PPP_IID_LEN) {
			const uint8_t *v = opts + off + 2;

			if (iid_is_zero(v) ||
			    !memcmp(v, l->iid_peer, PPP_IID_LEN))
				iid_draw(l->iid_local, l->iid_peer);
			else
				memcpy(l->iid_local, v, PPP_IID_LEN);
		}
		off += ol;
	}
}

/* Whether a Reject names an option of @type. */
static bool rejects(const uint8_t *opts, size_t olen, uint8_t type,
		    uint8_t type2)
{
	for (size_t off = 0; off + 2 <= olen;) {
		const uint8_t t = opts[off], ol = opts[off + 1];

		if (ol < 2 || off + ol > olen)
			break;
		if (t == type || t == type2)
			return true;
		off += ol;
	}
	return false;
}

/**
 * struct ncp - The state of one network control protocol.
 * @proto: IPCP or IPv6CP
 * @name: its name, for the log
 * @ours: the peer acknowledged our request
 * @theirs: we acknowledged the peer's
 * @dead: refused by the peer, given up, or not wanted
 * @id: the identifier of our request
 * @tries: how often our request went out without an answer
 * @sent: when it last went out, in ms, or 0 to send it now
 */
struct ncp {
	uint16_t proto;
	const char *name;
	bool ours, theirs, dead;
	uint8_t id;
	unsigned int tries;
	long sent;
};

static bool ncp_open(const struct ncp *n)
{
	return n->ours && n->theirs;
}

static size_t ncp_request(struct ppp_link *l, const struct ncp *n,
			  bool want_dns, bool want_iid, uint8_t *body)
{
	size_t len = 0;

	if (n->proto == PPP_PROTO_IPV6CP) {
		if (want_iid)
			len = opt_put(body, len, IPV6CP_OPT_IFACE_ID,
				      l->iid_local, PPP_IID_LEN);
		return len;
	}
	len = opt_put32(body, len, IPCP_OPT_ADDRESS, ntohl(l->local_ip));
	if (want_dns) {
		len = opt_put32(body, len, IPCP_OPT_DNS1, ntohl(l->dns1));
		len = opt_put32(body, len, IPCP_OPT_DNS2, ntohl(l->dns2));
	}
	return len;
}

/* Send the requests that are due; give up on one that went unanswered. */
static void ncp_send_due(struct ppp_link *l, struct ncp *n, bool want_dns,
			 bool want_iid)
{
	uint8_t body[PPP_CTRL_MAX];
	const long now = pppoed_now_ms();
	size_t len;

	if (n->dead || n->ours || (n->sent && now - n->sent < RESTART_MS))
		return;
	if (n->tries == MAX_TRIES) {
		PPPOED_WARN("%s: no answer in %u attempts", n->name, MAX_TRIES);
		n->dead = true;
		return;
	}
	if (!n->id)
		n->id = next_id();
	len = ncp_request(l, n, want_dns, want_iid, body);
	cp_send(l, n->proto, CP_CONF_REQ, n->id, body, len);
	PPPOED_DEBUG("%s configure-request %u sent, try %u", n->name, n->id,
		     n->tries + 1);
	n->tries++;
	n->sent = now;
}

/* Our request was refused with a Nak or a Reject: send another at once. */
static void ncp_again(struct ncp *n)
{
	n->id = next_id();
	n->ours = false;
	n->tries = 0;
	n->sent = 0;
}

/* Answer an LCP packet that comes in the network phase. */
static int ncp_lcp(struct ppp_link *l, const uint8_t *body, size_t n,
		   struct ncp *v4, struct ncp *v6)
{
	if (body[0] == CP_ECHO_REQ) {
		echo_reply(l, body[1]);
	} else if (body[0] == CP_TERM_REQ) {
		cp_send(l, PPP_PROTO_LCP, CP_TERM_ACK, body[1], NULL, 0);
		PPPOED_ERR("the peer terminated the link");
		return -ECONNRESET;
	} else if (body[0] == CP_PROTO_REJ && n >= CP_HDR_LEN + 2) {
		const uint16_t proto = pppoe_get16(body + CP_HDR_LEN);
		struct ncp *dead = proto == v4->proto ? v4 :
				   proto == v6->proto ? v6 :
							NULL;

		if (dead && !dead->dead) {
			PPPOED_INFO("the peer does not do %s", dead->name);
			dead->dead = true;
		}
	}
	return 0;
}

/* Refuse a protocol this client does not do (RFC 1661, Protocol-Reject). */
static void reject_protocol(struct ppp_link *l, uint16_t proto,
			    const uint8_t *body, size_t n)
{
	uint8_t rej[PPP_CTRL_MAX];
	const size_t len = n < sizeof(rej) - 2 ? n : sizeof(rej) - 2;

	pppoe_put16(rej, proto);
	memcpy(rej + 2, body, len);
	cp_send(l, PPP_PROTO_LCP, CP_PROTO_REJ, next_id(), rej, 2 + len);
	PPPOED_DEBUG("protocol %04x rejected", proto);
}

/*
 * Open IPCP when @want_ipv4 and IPv6CP when @want_ipv6, side by side. IPCP
 * asks for its address and name servers as zero, and the peer's Nak gives
 * them; IPv6CP offers a random interface identifier. Either may be refused
 * by the peer; the link comes up when one of them is open and the other is
 * open or dead. IPv6CP gets two restart periods more once IPCP is open. A
 * protocol that is not wanted is rejected when the peer asks for it.
 */
static int ncp_negotiate(struct ppp_link *l, bool want_ipv4, bool want_ipv6)
{
	struct ncp v4 = { .proto = PPP_PROTO_IPCP,
			  .name = "IPCP",
			  .dead = !want_ipv4 };
	struct ncp v6 = { .proto = PPP_PROTO_IPV6CP,
			  .name = "IPv6CP",
			  .dead = !want_ipv6 };
	bool want_dns = true, want_iid = true;
	long v4_open_at = 0;

	if (want_ipv6)
		iid_draw(l->iid_local, NULL);

	for (;;) {
		uint8_t body[PPP_CTRL_MAX];
		uint16_t proto;
		struct ncp *ncp;
		long now;
		int n;

		ncp_send_due(l, &v4, want_dns, want_iid);
		ncp_send_due(l, &v6, want_dns, want_iid);

		now = pppoed_now_ms();
		if (ncp_open(&v4) && !v4_open_at)
			v4_open_at = now;
		if (v4_open_at && !v6.dead && !ncp_open(&v6) &&
		    now - v4_open_at >= 2 * RESTART_MS) {
			PPPOED_WARN("IPv6CP did not open with IPCP; IPv4 only");
			v6.dead = true;
		}
		if ((ncp_open(&v4) && (ncp_open(&v6) || v6.dead)) ||
		    (ncp_open(&v6) && v4.dead))
			break;
		if (v4.dead && v6.dead) {
			PPPOED_ERR("neither IPCP nor IPv6CP came up");
			return -ETIMEDOUT;
		}

		n = ppp_recv(l, &proto, body, sizeof(body), RESTART_MS / 4);
		if (n == -ETIMEDOUT)
			continue;
		if (n < 0)
			return n;
		if (n < CP_HDR_LEN)
			continue;

		if (proto == PPP_PROTO_LCP) {
			n = ncp_lcp(l, body, (size_t)n, &v4, &v6);
			if (n < 0)
				return n;
			continue;
		}
		if (proto == PPP_PROTO_IPCP && want_ipv4) {
			ncp = &v4;
		} else if (proto == PPP_PROTO_IPV6CP && want_ipv6) {
			ncp = &v6;
		} else {
			reject_protocol(l, proto, body, (size_t)n);
			continue;
		}

		{
			uint8_t code = body[0], rid = body[1];
			uint16_t flen = pppoe_get16(body + 2);
			const uint8_t *opts = body + CP_HDR_LEN;
			size_t olen;

			if (flen < CP_HDR_LEN || (size_t)flen > (size_t)n)
				continue;
			olen = (size_t)flen - CP_HDR_LEN;

			switch (code) {
			case CP_CONF_REQ:
				if (ncp == &v4)
					n = ipcp_answer_req(l, rid, opts, olen);
				else
					n = ipv6cp_answer_req(l, rid, opts,
							      olen);
				ncp->theirs = n == 0;
				break;

			case CP_CONF_ACK:
				if (rid == ncp->id) {
					ncp->ours = true;
					PPPOED_DEBUG("%s configure-ack %u",
						     ncp->name, rid);
				}
				break;

			case CP_CONF_NAK:
				if (rid != ncp->id)
					break;
				if (ncp == &v4)
					ipcp_take_nak(l, opts, olen);
				else
					ipv6cp_take_nak(l, opts, olen);
				ncp_again(ncp);
				break;

			case CP_CONF_REJ:
				if (rid != ncp->id)
					break;
				if (ncp == &v4 &&
				    rejects(opts, olen, IPCP_OPT_DNS1,
					    IPCP_OPT_DNS2)) {
					want_dns = false;
					PPPOED_DEBUG(
						"the peer gives no name servers");
				}
				if (ncp == &v6 &&
				    rejects(opts, olen, IPV6CP_OPT_IFACE_ID,
					    IPV6CP_OPT_IFACE_ID))
					want_iid = false;
				ncp_again(ncp);
				break;

			case CP_TERM_REQ:
				cp_send(l, ncp->proto, CP_TERM_ACK, rid, NULL,
					0);
				PPPOED_INFO("the peer closed %s", ncp->name);
				ncp->dead = true;
				break;

			default:
				PPPOED_DEBUG("%s code %u ignored", ncp->name,
					     code);
				break;
			}
		}
	}

	l->ipv4_up = ncp_open(&v4);
	l->ipv6_up = ncp_open(&v6);
	if (l->ipv4_up) {
		struct in_addr a = { .s_addr = l->local_ip };
		struct in_addr b = { .s_addr = l->peer_ip };
		char local[INET_ADDRSTRLEN], peer[INET_ADDRSTRLEN];

		snprintf(local, sizeof(local), "%s", inet_ntoa(a));
		snprintf(peer, sizeof(peer), "%s", inet_ntoa(b));
		PPPOED_INFO("IPCP up, %s peer %s", local, peer);
	}
	if (l->ipv6_up)
		PPPOED_INFO("IPv6CP up");
	return 0;
}

int ppp_negotiate(struct ppp_link *l, const char *user, const char *secret,
		  bool want_ipv4, bool want_ipv6)
{
	int err;

	err = lcp_negotiate(l);
	if (err)
		return err;

	if (l->auth) {
		if (!user || !*user) {
			PPPOED_ERR(
				"the peer asks for authentication and no user was given");
			return -EINVAL;
		}
		err = l->auth == PPP_PROTO_PAP ?
			      pap_authenticate(l, user, secret ? secret : "") :
			      chap_authenticate(l, user, secret ? secret : "");
		if (err)
			return err;
	}

	return ncp_negotiate(l, want_ipv4, want_ipv6);
}
