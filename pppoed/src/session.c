// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The PPP negotiation: LCP, PAP or CHAP-MD5, IPCP (RFC 1661, 1334, 1994, 1332). */

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

/* Whether the peer's Reject of our IPCP request names the name servers. */
static bool ipcp_rejects_dns(const uint8_t *opts, size_t olen)
{
	for (size_t off = 0; off + 2 <= olen;) {
		const uint8_t t = opts[off], ol = opts[off + 1];

		if (ol < 2 || off + ol > olen)
			break;
		if (t == IPCP_OPT_DNS1 || t == IPCP_OPT_DNS2)
			return true;
		off += ol;
	}
	return false;
}

/* Open LCP: our request acknowledged, and the peer's. */
static int lcp_negotiate(struct ppp_link *l)
{
	bool ours = false, theirs = false, want_mru = true, want_magic = true;
	unsigned int tries = 0;
	uint8_t id = 0;

	l->magic = random_u32();

	while (tries < MAX_TRIES) {
		uint8_t body[PPP_CTRL_MAX];
		size_t len = 0;
		uint16_t proto;
		int n;

		if (!ours) {
			if (want_mru) {
				uint8_t v[2];

				pppoe_put16(v, PPPOE_MTU);
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
		}

		n = ppp_recv(l, &proto, body, sizeof(body), RESTART_MS);
		if (n == -ETIMEDOUT)
			continue;
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

		if (proto == PPP_PROTO_LCP && in[0] == CP_ECHO_REQ) {
			echo_reply(l, in[1]);
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
		if (n < 0)
			return n;
		if (n < CP_HDR_LEN)
			continue;

		if (proto == PPP_PROTO_LCP && in[0] == CP_ECHO_REQ) {
			echo_reply(l, in[1]);
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

/*
 * Open IPCP. The address and name servers are asked for as zero, and
 * the peer's Nak gives them. IPv6CP is not done, and is rejected.
 */
static int ipcp_negotiate(struct ppp_link *l)
{
	bool ours = false, theirs = false, want_dns = true;
	unsigned int tries = 0;
	uint8_t id = 0;

	while (tries < MAX_TRIES) {
		uint8_t body[PPP_CTRL_MAX];
		size_t len = 0;
		uint16_t proto;
		int n;

		if (!ours) {
			len = opt_put32(body, len, IPCP_OPT_ADDRESS,
					ntohl(l->local_ip));
			if (want_dns) {
				len = opt_put32(body, len, IPCP_OPT_DNS1,
						ntohl(l->dns1));
				len = opt_put32(body, len, IPCP_OPT_DNS2,
						ntohl(l->dns2));
			}
			if (!id)
				id = next_id();
			cp_send(l, PPP_PROTO_IPCP, CP_CONF_REQ, id, body, len);
			PPPOED_DEBUG("IPCP configure-request %u sent, try %u",
				     id, tries + 1);
			tries++;
		}

		n = ppp_recv(l, &proto, body, sizeof(body), RESTART_MS);
		if (n == -ETIMEDOUT)
			continue;
		if (n < 0)
			return n;
		if (n < CP_HDR_LEN)
			continue;

		if (proto == PPP_PROTO_LCP) {
			if (body[0] == CP_ECHO_REQ)
				echo_reply(l, body[1]);
			else if (body[0] == CP_TERM_REQ) {
				cp_send(l, PPP_PROTO_LCP, CP_TERM_ACK, body[1],
					NULL, 0);
				PPPOED_ERR(
					"the peer terminated the link during IPCP");
				return -ECONNRESET;
			}
			continue;
		}
		if (proto == PPP_PROTO_IPV6CP) {
			uint8_t rej[PPP_CTRL_MAX];

			pppoe_put16(rej, PPP_PROTO_IPV6CP);
			memcpy(rej + 2, body,
			       (size_t)n < sizeof(rej) - 2 ? (size_t)n :
							     sizeof(rej) - 2);
			cp_send(l, PPP_PROTO_LCP, CP_PROTO_REJ, next_id(), rej,
				2 + (size_t)n);
			PPPOED_DEBUG("IPv6CP protocol-rejected");
			continue;
		}
		if (proto != PPP_PROTO_IPCP)
			continue;

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
				if (ipcp_answer_req(l, rid, opts, olen) == 0)
					theirs = true;
				break;

			case CP_CONF_ACK:
				if (rid == id) {
					ours = true;
					PPPOED_DEBUG("IPCP configure-ack %u",
						     rid);
				}
				break;

			case CP_CONF_NAK: {
				if (rid != id) {
					PPPOED_DEBUG(
						"stale IPCP nak for id %u, waiting on %u",
						rid, id);
					break;
				}
				ipcp_take_nak(l, opts, olen);
				id = next_id();
				ours = false;
				tries = 0;
				break;
			}

			case CP_CONF_REJ: {
				if (rid != id) {
					PPPOED_DEBUG(
						"stale IPCP reject for id %u, waiting on %u",
						rid, id);
					break;
				}
				if (ipcp_rejects_dns(opts, olen)) {
					want_dns = false;
					PPPOED_DEBUG(
						"the peer gives no name servers");
				}
				id = next_id();
				ours = false;
				tries = 0;
				break;
			}

			default:
				PPPOED_DEBUG("IPCP code %u ignored", code);
				break;
			}
		}

		if (ours && theirs) {
			struct in_addr a = { .s_addr = l->local_ip };
			struct in_addr b = { .s_addr = l->peer_ip };
			char local[INET_ADDRSTRLEN], peer[INET_ADDRSTRLEN];

			snprintf(local, sizeof(local), "%s", inet_ntoa(a));
			snprintf(peer, sizeof(peer), "%s", inet_ntoa(b));
			PPPOED_INFO("IPCP up, %s peer %s", local, peer);
			return 0;
		}
	}

	PPPOED_ERR("IPCP did not come up in %u attempts", MAX_TRIES);
	return -ETIMEDOUT;
}

int ppp_negotiate(struct ppp_link *l, const char *user, const char *secret)
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

	return ipcp_negotiate(l);
}
