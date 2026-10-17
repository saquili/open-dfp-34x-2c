/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The stick's own table of nf_tables, "inet onu": the NAT the local networks
 * go out through and the protection that keeps the internet's new
 * connections from them, as one batch of nf_tables requests, which the
 * kernel takes whole or not at all (ADR 0014). nft would make the same table
 * of
 *
 *   table inet onu {
 *     flowtable ft {
 *       hook ingress priority 0; devices = { sfp, WAN... }; flags offload;
 *     }
 *     chain forward {
 *       type filter hook forward priority 0; policy accept;
 *       iifname LAN meta l4proto tcp flow add @ft       for each network
 *       iifname LAN meta l4proto udp flow add @ft       for each network
 *       ct state established,related accept
 *       iifname "ppp*" oifname LAN ct state new drop    for each network
 *     }
 *     chain postrouting {
 *       type nat hook postrouting priority 100; policy accept;
 *       oifname "ppp*" ip saddr NET masquerade          for each network
 *     }
 *   }
 *
 * The networks go out by PPPoE sessions, whose interfaces are pppN; the
 * flowtable hands their flows to the switch, which carries them in its NAPT
 * engine, on the host link, sfp, and on the sessions' GEM interfaces, WAN.
 * A batch replaces the table there is, in the same transaction.
 *
 * The networks and the WANs are lines of a file, which onu-apply writes:
 *
 *   lan IFNAME ADDRESS/LEN   a network: its interface and its addresses
 *   wan IFNAME               a session's GEM interface
 *   offload off              no flowtable: the CPU carries every flow
 */

#ifndef ONU_UTILS_FWMSG_H
#define ONU_UTILS_FWMSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FW_LANS_MAX 8
#define FW_WANS_MAX 8
#define FW_IFNAMSIZ 16

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
 * struct fw_spec - What the table is made of.
 * @lan: the networks, @nlans of them
 * @wan: the sessions' GEM interfaces, @nwans of them
 * @offload: with the flowtable
 */
struct fw_spec {
	struct fw_lan lan[FW_LANS_MAX];
	int nlans;
	char wan[FW_WANS_MAX][FW_IFNAMSIZ];
	int nwans;
	bool offload;
};

/* An empty spec: no network, no WAN, with the flowtable. */
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
 * does not fit. Each request but the batch's begin and end asks for an
 * acknowledgement; @last is the number of the last of them.
 */
size_t fw_batch(const struct fw_spec *s, char *buf, size_t cap, uint32_t seq,
		uint32_t *last);

/* The batch that takes the table away, as fw_batch(). */
size_t fw_batch_clear(char *buf, size_t cap, uint32_t seq, uint32_t *last);

#endif /* ONU_UTILS_FWMSG_H */
