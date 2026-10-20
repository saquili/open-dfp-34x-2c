/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The stick's own table of nf_tables, "inet onu": the NAT the local networks
 * go out through, the protection that keeps the internet's new connections
 * from them, the ports forwarded to their devices, and the firewall's
 * defaults with the rules that let through what a default refuses, as one
 * batch of nf_tables requests, which the kernel takes whole or not at all
 * (ADR 0011). nft would make the same table of
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
 *                                                        and way out, or
 *         jump allow_in in its place                    with rules in
 *       iifname LAN oifname OUT ct state new drop       with out blocked,
 *         or jump allow_out                              as above
 *       iifname LAN oifname LAN2 ct state new drop      with between
 *         or jump allow_between                          blocked, for each
 *                                                        two networks
 *     }
 *     chain allow_in {                                  with rules of it
 *       meta nfproto ipv4 [iifname IN] [ether saddr MAC] [oifname LAN]
 *         [ip saddr NET] [ip daddr NET] [meta l4proto tcp|udp|icmp]
 *         [tcp|udp dport PORTS] accept                   for each rule
 *       drop
 *     }
 *     chain allow_out { ... }                           as allow_in
 *     chain allow_between { ... }                       as allow_in
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
 *       iifname IN ip daddr ADDR dnat ip to DEV          for each DMZ
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
 * out among them.
 *
 * A DMZ takes what no forward takes, a ping as well, but DHCP's answers to
 * the client's port, 68, which a connection by DHCP takes itself. The chain
 * sees the first packet of a connection only: what answers the stick's own
 * traffic, and an error of ICMP about it, belongs to a connection there is,
 * and stays with the stick. A session's PPP is no IP, and no chain of the
 * table sees it.
 *
 * Where no rule decides, the firewall drops a new connection from the
 * internet to a network (in), and by its defaults one from a network to
 * the internet (out) and one between two networks (between). The rules
 * that block are the switch's ACL's alone, which judges a frame as it
 * enters a port, before netfilter sees it (ADR 0019); the rules that allow
 * are the ACL's too, and are here as well where the default of their way
 * would drop what the ACL let through: each is a rule of the chain of its
 * way, which a new connection that way jumps to, and which drops what none
 * of them accepts. The ways are told apart by the interfaces, so the
 * defaults hold for IPv6 as well; a rule is of IPv4 alone, as the stick
 * routes no IPv6 to the networks' devices yet.
 *
 * The networks, the ways out, the WANs, the forwards, the DMZs and the
 * firewall's are lines of a file, which onu-apply writes:
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
 *   default out|between block|allow
 *                            what becomes of a new connection that way where
 *                            no rule decides: allow when there is no line
 *   allow in|out|between [iif IFNAME] [ether MAC] [oif IFNAME] [saddr NET]
 *         [daddr NET] [tcp|udp|icmp [PORTS]]
 *                            a rule that lets a new connection of IPv4
 *                            through that way: from an interface, and a MAC
 *                            address there, to an interface, from an address
 *                            or a network ADDR/LEN, to one, of a protocol
 *                            and a port or FROM-LAST of TCP or UDP; what it
 *                            does not name it takes any of
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
 * The longest batch, of the most of everything, is some 490 KiB: about 640
 * bytes for each forward's rule, 280 more for a range moved, which takes a
 * map, and 28 for each of its ports; up to 900 for each rule of the
 * firewall, and 300 for each jump to the chain of a way, of each network
 * by each way out or into each other network. The buffer for it is twice
 * as large, for the request that goes past it (mnl_nlmsg_batch_start()).
 */
#define FW_BATCH_MAX (512 * 1024)

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

/* The ways of a new connection that the firewall judges. */
enum fw_way {
	FW_IN,
	FW_OUT,
	FW_BETWEEN,
	FW_WAYS
};

/* The panel's 96 rules, a rule of both TCP and UDP two of them. */
#define FW_RULES_MAX (2 * 96)

/**
 * struct fw_rule - A rule that lets a new connection of IPv4 through.
 * @way: %FW_IN, %FW_OUT or %FW_BETWEEN
 * @iif: the interface it comes in by, "" for any
 * @oif: the interface it goes out by, "" for any
 * @ether: whether it comes from @mac
 * @mac: the MAC address it comes from
 * @saddr: whether it comes from @snet
 * @snet: the first address of the network it comes from, in host order
 * @slen: the length of its prefix
 * @daddr: whether it goes to @dnet
 * @dnet: the first address of the network it goes to, in host order
 * @dlen: the length of its prefix
 * @proto: IPPROTO_TCP, IPPROTO_UDP or IPPROTO_ICMP; 0 for any
 * @from: the first port it goes to, of TCP or UDP; 0 for any
 * @last: the last, @from for one
 */
struct fw_rule {
	uint8_t way;
	char iif[FW_IFNAMSIZ];
	char oif[FW_IFNAMSIZ];
	bool ether;
	uint8_t mac[6];
	bool saddr;
	uint32_t snet;
	uint8_t slen;
	bool daddr;
	uint32_t dnet;
	uint8_t dlen;
	uint8_t proto;
	uint16_t from, last;
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
 * @rule: the rules that let new connections through, @nrules of them
 * @block: whether a new connection of each way that no rule takes is
 *         dropped: one from the internet always
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
	struct fw_rule rule[FW_RULES_MAX];
	int nrules;
	bool block[FW_WAYS];
	bool offload;
};

/*
 * An empty spec: no network, no way out, no WAN, no forward, no rule, new
 * connections from the internet dropped and others not, with the flowtable.
 */
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

/*
 * The guard of the stick's own programs, a table of its own, "inet
 * onu_guard", in every shape of the stick, with local networks or without:
 * the stick talks to the host over its management VLAN, and what comes in
 * to the stick itself by any other interface is dropped, but what one of its
 * programs takes there; and the stick forwards nothing to the management
 * VLAN nor from it, which is no network it routes. nft would make the table
 * of
 *
 *   table inet onu_guard {
 *     chain input {
 *       type filter hook input priority 0; policy drop;
 *       iifname "lo" accept
 *       iifname MGMT accept                       for each management interface
 *       iifname DEV ip daddr ADDR accept          with an address on the host
 *                                                  link itself
 *       ct state established,related accept
 *       icmp type destination-unreachable accept
 *       icmp type time-exceeded accept
 *       icmp type parameter-problem accept
 *       icmpv6 type destination-unreachable accept
 *       icmpv6 type packet-too-big accept
 *       icmpv6 type time-exceeded accept
 *       icmpv6 type parameter-problem accept
 *       ct state invalid drop
 *       iifname IF udp dport 546 accept           the sessions': the DHCPv6
 *       iifname IF icmpv6 type nd-router-advert accept      client's answers
 *       iifname IF udp dport 68 accept            an IPoE connection's by DHCP
 *       iifname IF udp dport 67 accept            a link's or a network's with
 *                                                  a DHCP server
 *       iifname IF udp dport 547 accept           a link's with the DHCPv6
 *       iifname IF icmpv6 type nd-router-solicit accept     server
 *       iifname IF icmpv6 type nd-neighbor-solicit accept   each interface of
 *       iifname IF icmpv6 type nd-neighbor-advert accept    IPv6
 *       iifname IF icmp type echo-request accept  where its side's ping is
 *       iifname IF icmpv6 type echo-request accept         answered
 *     }
 *     chain forward {
 *       type filter hook forward priority -10; policy accept;
 *       iifname MGMT drop                          for each management interface
 *       oifname MGMT drop
 *     }
 *   }
 *
 * The management VLAN brings in whatever it brings: the ssh server, the
 * status daemon and its panel, and timed's PTP listen there, and nowhere
 * else. So does the loopback, by which the stick's programs reach each other
 * and its own addresses. The address on the host link itself, a bench's
 * (DATA_ADDRESS of the boot script), is a management address too: what
 * comes in by the host link to it is taken, and nothing else by that link.
 *
 * From the other interfaces the stick takes what answers its own traffic,
 * which the connection tracking knows, such as a time server's answers or a
 * DHCP server's to a renewal of a lease. An error of ICMP is taken whether
 * the tracking knows the flow it is about or not, as it forgets one it saw (a
 * timeout, a full table), and the kernel matches each error to its own
 * sockets: a path's MTU is learnt all the same (RFC 4890, 4.3.1). What the
 * tracking finds wrong is dropped, and of what is new, only what a program of
 * the stick takes on that interface:
 *
 *   - the sessions' (pppN): the answers of dhcp6d's DHCPv6 client, which asks
 *     the multicast address of the servers and is answered from another, so
 *     that no connection ties the two, and the concentrator's router
 *     advertisements, from which the kernel may take an address;
 *   - an IPoE connection's (wanVID): with its address from DHCP, the DHCP
 *     client's answers, which a relay sends from its own address;
 *   - a link a session hands its address down by (lanVID, or sfp untagged):
 *     the requests to udhcpd, the DHCP server of the router behind; where a
 *     session hands its prefix down, those to dhcp6d's DHCPv6 server and the
 *     router solicitations, which dhcp6d answers;
 *   - a local network's (netN, or sfp untagged): with its DHCP server, the
 *     requests to it;
 *   - each of them that carries IPv6, a session's, an IPoE connection's or a
 *     link's: neighbour discovery, which the kernel needs on the link and
 *     takes only from it (a hop limit of 255);
 *   - an echo request, where the stick answers a ping of that side: from the
 *     operator's (the sessions and the IPoE connections), or from the host
 *     side's networks (the local networks and the links).
 *
 * The rest is dropped, every server of the stick's among it. IGMP and MLD
 * are none of the table's: omcid's multicast agent takes them, and sends its
 * own, by packet sockets, which see a frame before netfilter does; PPP and
 * PPPoE's discovery are no IP. The forward chain comes before the one of
 * "inet onu" (priority 0), so that no flow to or from the management VLAN
 * reaches that table's flowtable; what else that table does stays as it is.
 * The guard needs no more than the connection tracking: what a rule names is
 * an interface, a protocol, a port and a type.
 *
 * The interfaces are lines of a file, which onu-apply writes:
 *
 *   mgmt IFNAME             an interface of the management VLAN, or with a '*'
 *                           at its end every one whose name begins so
 *   data IFNAME ADDR        the address on the host link IFNAME itself
 *   session IFNAME          the sessions' interfaces, "ppp*"
 *   ipoe IFNAME [dhcp]      an IPoE connection's interface, with its address
 *                           from DHCP
 *   link IFNAME [dhcp] [dhcp6]
 *                           a link a session hands its address down by, with
 *                           the DHCP server, and its prefix, with the DHCPv6
 *                           server
 *   lan IFNAME [dhcp]       a local network's interface, with its DHCP server
 *   ping in|lans            a ping answered from the operator's side, or from
 *                           the host side's networks
 *
 * A guard without a management interface is none: it would keep everyone off
 * the stick.
 */

/* The interfaces of the guard, by what the stick's programs take there. */
enum fw_guard_kind {
	FW_G_SESSION,
	FW_G_IPOE,
	FW_G_LINK,
	FW_G_LAN,
};

/*
 * The management interfaces: the one in use and one of a pair on trial, or
 * a name that stands for both.
 */
#define FW_MGMTS_MAX 4
/* The sessions', each IPoE connection's, a link of each, each network's. */
#define FW_GUARD_IFS_MAX 32

/**
 * struct fw_gif - An interface of the guard.
 * @dev: its name, or the beginning of the names
 * @kind: %FW_G_SESSION, %FW_G_IPOE, %FW_G_LINK or %FW_G_LAN
 * @dhcp: with a DHCP server (a link's, a network's) or client (an IPoE
 *        connection's)
 * @dhcp6: with the DHCPv6 server (a link's)
 */
struct fw_gif {
	struct fw_out dev;
	uint8_t kind;
	bool dhcp;
	bool dhcp6;
};

/**
 * struct fw_guard - What the guard is made of.
 * @mgmt: the management interfaces, @nmgmt of them
 * @data_if: the host link, when @data
 * @data: the address on it, in host order; 0 for none
 * @gif: the other interfaces whose programs take something, @ngifs of them
 * @ping_in: a ping answered from the operator's side
 * @ping_lans: a ping answered from the host side's networks
 */
struct fw_guard {
	struct fw_out mgmt[FW_MGMTS_MAX];
	int nmgmt;
	char data_if[FW_IFNAMSIZ];
	uint32_t data;
	struct fw_gif gif[FW_GUARD_IFS_MAX];
	int ngifs;
	bool ping_in;
	bool ping_lans;
};

/* An empty guard: no interface, and no ping answered. */
void fw_guard_init(struct fw_guard *g);

/* Take a line of the guard's file over @g, as fw_spec_line(). */
int fw_guard_line(struct fw_guard *g, const char *line, char *err, size_t errcap);

/*
 * The batch that puts the guard of @g in place of the one there is, as
 * fw_batch(); 0 as well for a guard without a management interface.
 */
size_t fw_guard_batch(const struct fw_guard *g, char *buf, size_t cap, uint32_t seq,
		      uint32_t *last);

#endif /* ONU_UTILS_FWMSG_H */
