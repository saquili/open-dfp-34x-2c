/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The stick's own table of nf_tables, "inet onu": the NAT the local networks
 * go out through, the protection that keeps the internet's new connections
 * from them, and the ports forwarded to their devices, as one batch of
 * nf_tables requests, which the kernel takes whole or not at all (ADR 0014).
 * nft would make the same table of
 *
 *   table inet onu {
 *     flowtable ft {
 *       hook ingress priority 0; devices = { sfp, WAN... }; flags offload;
 *     }
 *     chain forward {
 *       type filter hook forward priority 0; policy accept;
 *       oifname OUT tcp flags syn tcp option maxseg size set rt mtu
 *                                                        for each way out
 *       iifname LAN meta l4proto tcp flow add @ft       for each network
 *       iifname LAN meta l4proto udp flow add @ft       for each network
 *       oifname LAN meta l4proto tcp flow add @ft       for each network
 *       oifname LAN meta l4proto udp flow add @ft       for each network
 *       ct state established,related accept
 *       ct status dnat accept                           with forwards
 *       iifname OUT oifname LAN ct state new drop       for each network
 *                                                        and way out
 *     }
 *     chain postrouting {
 *       type nat hook postrouting priority 100; policy accept;
 *       oifname OUT ip saddr NET masquerade             for each network
 *                                                        and way out
 *     }
 *     chain prerouting {                                with forwards
 *       type nat hook prerouting priority -100; policy accept;
 *       iifname IN ip daddr ADDR tcp dport PORT dnat ip to DEV:TO
 *       iifname IN ip daddr ADDR tcp dport FROM-LAST dnat ip to DEV
 *       iifname IN ip daddr ADDR tcp dport FROM-LAST dnat ip to
 *         DEV:tcp dport map { FROM : TO, FROM+1 : TO+1, ..., LAST : ... }
 *                                                        for each forward
 *       iifname IN ip daddr ADDR udp dport 68 accept
 *       iifname IN ip daddr ADDR meta l4proto != icmp dnat ip to DEV
 *                                                        for each DMZ
 *     }
 *   }
 *
 * The networks go out by the stick's connections: PPPoE sessions, whose
 * interfaces are pppN ("ppp*"), and the IPoE connections, each on the
 * interface of its VLAN (wanVID). The flowtable hands their flows to the
 * switch, which carries them in its NAPT engine, on the host link, sfp, and
 * on the connections' GEM interfaces, WAN. A connection's SYN, which the CPU
 * carries, has its maximum segment size cut to what the way out takes.
 * A batch replaces the table there is, in the same transaction.
 *
 * A forward takes a new connection that comes in by a connection's interface
 * IN to its address there, ADDR, of TCP or UDP, to a port or a range of
 * them, to a device's address: a port to another port, a range to the same
 * ports, or to as many others from TO. netfilter then lets it through to the
 * network, and hands its flow to the switch as any other.
 *
 * nf_tables' NAT translates to one port, or keeps the port within a range,
 * but moves no range by an offset: nft_nat has no base port to count from
 * (iptables' DNAT has one, nft's not). A range moved to other ports takes a
 * map of its own, as nft makes one for "dnat ... map": an anonymous set,
 * constant, of each port of the range to its own of the device's, which the
 * rule looks the port up in. A port moved costs an element of 28 bytes in
 * the batch and some 32 of the kernel's memory, and a new connection one
 * lookup in a hash table; a rule for each port would cost 640 bytes, and a
 * rule more that each new connection goes through, the devices' own going
 * out among them. A DMZ takes what no forward takes, but ICMP,
 * which the stick answers, and DHCP's answers to the client's port, 68, which
 * a connection by DHCP takes itself; answers to what went out are no new
 * connections, and never reach the chain.
 *
 * The networks, the ways out, the WANs, the forwards and the DMZs are lines
 * of a file, which onu-apply writes:
 *
 *   lan IFNAME ADDRESS/LEN   a network: its interface and its addresses
 *   out IFNAME               an interface the networks go out by, or with
 *                            a '*' at its end every one whose name begins so
 *   wan IFNAME               a connection's GEM interface
 *   offload off              no flowtable: the CPU carries every flow
 *   fwd IFNAME ADDR PROTO PORTS DEV PORT
 *                            a forward: PROTO is tcp or udp, PORTS a port
 *                            or FROM-LAST, PORT the device's port, for a
 *                            range the first of as many
 *   dmz IFNAME ADDR DEV      a DMZ
 */

#ifndef ONU_UTILS_FWMSG_H
#define ONU_UTILS_FWMSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FW_LANS_MAX 8
#define FW_WANS_MAX 8
/*
 * The sessions' "ppp*" and an interface of each IPoE connection: seven at
 * most, as the connections are.
 */
#define FW_OUTS_MAX 7
#define FW_IFNAMSIZ 16
/* The panel's 96 forwards, each of TCP and of UDP; a DMZ for each connection. */
#define FW_FWDS_MAX (2 * 96)
#define FW_DMZS_MAX 7
/*
 * The most ports of ranges moved to other ports, of every forward and each
 * protocol apart, an element of a map each: what the settings take
 * (SETTINGS_MOVED_MAX). Ranges of a thousand ports moved, beside the most
 * of everything else, keep the batch near 255 KiB and the maps' elements
 * near 32 KiB of the 24 MiB the stick has; a rule for each port would have
 * taken 640 KiB of batch.
 */
#define FW_MOVED_MAX 1024
/*
 * The longest batch, of the most of everything, is 255 KiB: about 640 bytes
 * for each forward's rule, 280 more for a range moved, which takes a map,
 * and 28 for each of its ports. The buffer for it is twice as large, for
 * the request that goes past it (mnl_nlmsg_batch_start()).
 */
#define FW_BATCH_MAX (320 * 1024)

/**
 * struct fw_lan - A local network.
 * @ifname: its interface
 * @net: its first address, in host order
 * @len: the length of its prefix, 8 to 32
 */
struct fw_lan {
	char ifname[FW_IFNAMSIZ];
	uint32_t net;
	uint8_t len;
};

/**
 * struct fw_out - An interface the networks go out by.
 * @ifname: its name, or the beginning of the names when @prefix
 * @prefix: whether it stands for every interface whose name begins so
 */
struct fw_out {
	char ifname[FW_IFNAMSIZ];
	bool prefix;
};

/**
 * struct fw_fwd - A forward.
 * @ifname: the interface it comes in by, a connection's
 * @addr: the connection's address there, in host order
 * @proto: IPPROTO_TCP or IPPROTO_UDP
 * @from: the first port it takes
 * @last: the last, @from for one
 * @dev: the device's address, in host order
 * @port: the device's port, for a range the first of as many
 */
struct fw_fwd {
	char ifname[FW_IFNAMSIZ];
	uint32_t addr;
	uint8_t proto;
	uint16_t from, last;
	uint32_t dev;
	uint16_t port;
};

/* Whether @f is a range moved to other ports, which takes a map. */
static inline bool fw_fwd_moved(const struct fw_fwd *f)
{
	return f->last > f->from && f->port != f->from;
}

/**
 * struct fw_dmz - A connection's DMZ.
 * @ifname: the connection's interface
 * @addr: its address there, in host order
 * @dev: the device's address, in host order
 */
struct fw_dmz {
	char ifname[FW_IFNAMSIZ];
	uint32_t addr;
	uint32_t dev;
};

/**
 * struct fw_spec - What the table is made of.
 * @lan: the networks, @nlans of them
 * @out: the interfaces they go out by, @nouts of them
 * @wan: the connections' GEM interfaces, @nwans of them
 * @fwd: the forwards, @nfwds of them
 * @nmoved: the ports of those of them moved to other ports, FW_MOVED_MAX
 *          at most
 * @dmz: the DMZs, @ndmzs of them, after the forwards
 * @offload: with the flowtable
 */
struct fw_spec {
	struct fw_lan lan[FW_LANS_MAX];
	int nlans;
	struct fw_out out[FW_OUTS_MAX];
	int nouts;
	char wan[FW_WANS_MAX][FW_IFNAMSIZ];
	int nwans;
	struct fw_fwd fwd[FW_FWDS_MAX];
	int nfwds;
	int nmoved;
	struct fw_dmz dmz[FW_DMZS_MAX];
	int ndmzs;
	bool offload;
};

/* An empty spec: no network, no way out, no WAN, no forward, with the flowtable. */
void fw_spec_init(struct fw_spec *s);

/*
 * Take a line of the file over @s: 0, or -1 with why in @err for a line
 * that is no setting of it. An empty line and a line that begins with '#'
 * are none, and are taken.
 */
int fw_spec_line(struct fw_spec *s, const char *line, char *err, size_t errcap);

/*
 * The batch that puts the table of @s in place of the one there is, in @buf
 * of @cap bytes, its first request numbered @seq: its length, or 0 when it
 * does not fit. The last request but the batch's end asks for an
 * acknowledgement, which comes after the errors of any request that failed;
 * @last is its number.
 */
size_t fw_batch(const struct fw_spec *s, char *buf, size_t cap, uint32_t seq,
		uint32_t *last);

/* The batch that takes the table away, as fw_batch(). */
size_t fw_batch_clear(char *buf, size_t cap, uint32_t seq, uint32_t *last);

#endif /* ONU_UTILS_FWMSG_H */
