// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Performance monitoring of pm.h. */

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "hw/hw.h"
#include "me/bridge.h"
#include "me/pm.h"
#include "me/table.h"
#include "omcid/array_size.h"
#include "omcid/byteorder.h"
#include "sys/log.h"

#define ME_PPTP_ETH_UNI 11u
#define ME_ETH_PM 24u
#define ME_BRIDGE_PORT 47u
#define ME_BRIDGE_PORT_PM 52u
#define ME_ETH_PM_2 89u
#define ME_IP_HOST_PM 135u
#define ME_GEM_IW_TP 266u
#define ME_GEM_CTP 268u
#define ME_THRESHOLD_1 273u
#define ME_THRESHOLD_2 274u
#define ME_MCAST_GEM_IW_TP 281u
#define ME_ETH_PM_3 296u
#define ME_TCP_UDP_PM 342u
#define ME_IP_HOST_PM_2 458u
#define ME_FEC_PM 312u
#define ME_ETH_FRAME_PM_DS 321u
#define ME_ETH_FRAME_PM_US 322u
#define ME_ETH_FRAME_EXT_PM 334u
#define ME_GEM_CTP_PM 341u

/* The attributes read here: of a bridge port, a GEM port and a PM entity. */
#define BRIDGE_PORT_TP_TYPE 3u
#define GEM_CTP_PORT_ID 1u
#define PM_INTERVAL_END 1u
#define PM_CONTROL 2u

/* The switch ports counted: the UNI and the PON side. */
#define UNI_SWITCH_PORT 0u
#define PON_SWITCH_PORT 2u

/* The counters of the switch, by their number. */
enum {
	C_RX_OCTETS = 0,
	C_RX_UCAST = 1,
	C_RX_MCAST = 2,
	C_RX_BCAST = 3,
	C_TX_OCTETS = 5,
	C_TX_DISCARDS = 6,
	C_TX_UCAST = 7,
	C_TX_MCAST = 8,
	C_TX_BCAST = 9,
	C_RX_FWD_DISCARDS = 11,
	C_TX_SINGLE_COLLISIONS = 18,
	C_TX_MULTIPLE_COLLISIONS = 19,
	C_TX_DEFERRED = 20,
	C_TX_LATE_COLLISIONS = 21,
	C_TX_EXCESSIVE_COLLISIONS = 22,
	C_RX_DROPS = 26,
	C_RX_FRAGMENTS = 32,
	C_RX_JABBERS = 33,
	C_RX_CRC = 35,
	C_TX_UNDERSIZE = 43,
	C_TX_OVERSIZE = 44,
	C_TX_64 = 45,
	C_TX_65_127 = 46,
	C_TX_128_255 = 47,
	C_TX_256_511 = 48,
	C_TX_512_1023 = 49,
	C_TX_1024_1518 = 50,
	C_RX_UNDERSIZE = 57,
	C_RX_OVERSIZE = 59,
	C_RX_64 = 60,
	C_RX_65_127 = 61,
	C_RX_128_255 = 62,
	C_RX_256_511 = 63,
	C_RX_512_1023 = 64,
	C_RX_1024_1518 = 65,
};

/* Where the count of an attribute comes from. */
enum pm_source {
	SRC_NONE,
	SRC_PORT,
	SRC_GEM_TX_FRAMES,
	SRC_GEM_RX_FRAMES,
	SRC_GEM_TX_PAYLOAD,
	SRC_GEM_RX_PAYLOAD,
	SRC_FEC_BYTES,
	SRC_FEC_CORRECTED,
	SRC_FEC_UNCORRECTABLE,
	SRC_FEC_CODEWORDS,
	SRC_FEC_SECONDS,
};

#define PM_MAX_SOURCES 6u

/*
 * An attribute and what counts it: the sum of counters of the switch port
 * the instance counts at (SRC_PORT), a count of its GEM port or of the PON
 * receiver, or nothing (SRC_NONE), when it reads 0.
 */
struct pm_attr {
	uint8_t attr_no;
	uint8_t source;
	uint8_t nsrc;
	uint8_t idx[PM_MAX_SOURCES];
	const char *why;
};

#define NONE(no, w) { (no), SRC_NONE, 0u, { 0 }, (w) }
#define FROM(no, src, w) { (no), (src), 0u, { 0 }, (w) }
#define PORT(no, w, ...)                               \
	{ (no),                                        \
	  SRC_PORT,                                    \
	  (uint8_t)sizeof((uint8_t[]){ __VA_ARGS__ }), \
	  { __VA_ARGS__ },                             \
	  (w) }

/*
 * A threshold crossing alert: the attribute held against a threshold, the
 * alert's alarm number, and the threshold value attribute that holds it,
 * 1 to 7 of the threshold data 1, 8 to 14 of the threshold data 2.
 */
struct pm_tca {
	uint8_t attr_no;
	uint8_t bit;
	uint8_t threshold;
};

/*
 * The Ethernet frame counts (G.988 9.3.30 to 9.3.32) of what a switch port
 * receives. A packet counts with its bad ones, which the switch counts
 * apart from the good, taken to be disjoint. A frame the switch drops for
 * want of room where it is going is a drop event of the port it came in
 * on: the switch counts it there, among the frames forwarding discarded,
 * and not where it would have gone out.
 */
static const struct pm_attr g_eth_rx[] = {
	PORT(3u, "etherStatsDropEvents + dot1dTpPortInDiscards", C_RX_DROPS,
	     C_RX_FWD_DISCARDS),
	PORT(4u, "ifInOctets", C_RX_OCTETS),
	PORT(5u,
	     "ifInUcastPkts + ifInMulticastPkts + ifInBroadcastPkts + etherStatsCRCAlignErrors + etherStatsFragments + etherStatsJabbers",
	     C_RX_UCAST, C_RX_MCAST, C_RX_BCAST, C_RX_CRC, C_RX_FRAGMENTS,
	     C_RX_JABBERS),
	PORT(6u, "ifInBroadcastPkts", C_RX_BCAST),
	PORT(7u, "ifInMulticastPkts", C_RX_MCAST),
	PORT(8u, "etherStatsCRCAlignErrors", C_RX_CRC),
	PORT(9u, "etherStatsRxUndersizePkts", C_RX_UNDERSIZE),
	PORT(10u, "etherStatsRxOversizePkts", C_RX_OVERSIZE),
	PORT(11u, "etherStatsRxPkts64Octets", C_RX_64),
	PORT(12u, "etherStatsRxPkts65to127Octets", C_RX_65_127),
	PORT(13u, "etherStatsRxPkts128to255Octets", C_RX_128_255),
	PORT(14u, "etherStatsRxPkts256to511Octets", C_RX_256_511),
	PORT(15u, "etherStatsRxPkts512to1023Octets", C_RX_512_1023),
	PORT(16u, "etherStatsRxPkts1024to1518Octets", C_RX_1024_1518),
};

/* The same of what a switch port sends. */
static const struct pm_attr g_eth_tx[] = {
	PORT(3u, "ifOutDiscards", C_TX_DISCARDS),
	PORT(4u, "ifOutOctets", C_TX_OCTETS),
	PORT(5u, "ifOutUcastPkts + ifOutMulticastPkts + ifOutBroadcastPkts",
	     C_TX_UCAST, C_TX_MCAST, C_TX_BCAST),
	PORT(6u, "ifOutBroadcastPkts", C_TX_BCAST),
	PORT(7u, "ifOutMulticastPkts", C_TX_MCAST),
	NONE(8u, "none: a CRC error is counted where frames are received"),
	PORT(9u, "etherStatsTxUndersizePkts", C_TX_UNDERSIZE),
	PORT(10u, "etherStatsTxOversizePkts", C_TX_OVERSIZE),
	PORT(11u, "etherStatsTxPkts64Octets", C_TX_64),
	PORT(12u, "etherStatsTxPkts65to127Octets", C_TX_65_127),
	PORT(13u, "etherStatsTxPkts128to255Octets", C_TX_128_255),
	PORT(14u, "etherStatsTxPkts256to511Octets", C_TX_256_511),
	PORT(15u, "etherStatsTxPkts512to1023Octets", C_TX_512_1023),
	PORT(16u, "etherStatsTxPkts1024to1518Octets", C_TX_1024_1518),
};

static const struct pm_tca g_eth_frame_tca[] = {
	{ 3u, 0u, 1u },
	{ 8u, 1u, 2u },
	{ 9u, 2u, 3u },
	{ 10u, 3u, 4u },
};

/* The extended PM numbers the same alerts from 1 (G.988 9.3.32). */
static const struct pm_tca g_eth_ext_tca[] = {
	{ 3u, 1u, 1u },
	{ 8u, 2u, 2u },
	{ 9u, 3u, 3u },
	{ 10u, 4u, 4u },
};

/* Ethernet PM history data (G.988 9.5.2), of the UNI. */
static const struct pm_attr g_eth_pm[] = {
	PORT(3u,
	     "etherStatsCRCAlignErrors: the switch counts FCS and alignment errors together",
	     C_RX_CRC),
	PORT(4u, "dot3StatsExcessiveCollisions", C_TX_EXCESSIVE_COLLISIONS),
	PORT(5u, "dot3StatsLateCollisions", C_TX_LATE_COLLISIONS),
	PORT(6u, "etherStatsRxOversizePkts", C_RX_OVERSIZE),
	PORT(7u, "etherStatsDropEvents + dot1dTpPortInDiscards", C_RX_DROPS,
	     C_RX_FWD_DISCARDS),
	PORT(8u, "ifOutDiscards", C_TX_DISCARDS),
	PORT(9u, "dot3StatsSingleCollisionFrames", C_TX_SINGLE_COLLISIONS),
	PORT(10u, "dot3StatsMultipleCollisionFrames", C_TX_MULTIPLE_COLLISIONS),
	NONE(11u, "none: a full-duplex link has no SQE test"),
	PORT(12u, "dot3StatsDeferredTransmissions", C_TX_DEFERRED),
	NONE(13u,
	     "none: the switch keeps no count of internal transmit errors"),
	NONE(14u, "none: the switch keeps no count of carrier sense errors"),
	NONE(15u, "none: counted with the FCS errors, attribute 3"),
	NONE(16u, "none: the switch keeps no count of internal receive errors"),
};

static const struct pm_tca g_eth_pm_tca[] = {
	{ 3u, 0u, 1u },	   { 4u, 1u, 2u },    { 5u, 2u, 3u },
	{ 6u, 3u, 4u },	   { 7u, 4u, 5u },    { 8u, 5u, 6u },
	{ 9u, 6u, 7u },	   { 10u, 7u, 8u },   { 11u, 8u, 9u },
	{ 12u, 9u, 10u },  { 13u, 10u, 11u }, { 14u, 11u, 12u },
	{ 15u, 12u, 13u }, { 16u, 13u, 14u },
};

/* Ethernet PM history data 2 (G.988 9.5.3), of the UNI. */
static const struct pm_attr g_eth_pm_2[] = {
	NONE(3u, "none: the UNI's PPPoE filter is not in force"),
};

static const struct pm_tca g_eth_pm_2_tca[] = {
	{ 3u, 0u, 1u },
};

/* Ethernet PM history data 3 (G.988 9.5.4), of what the UNI receives. */
static const struct pm_attr g_eth_pm_3[] = {
	PORT(3u, "etherStatsDropEvents + dot1dTpPortInDiscards", C_RX_DROPS,
	     C_RX_FWD_DISCARDS),
	PORT(4u, "ifInOctets", C_RX_OCTETS),
	PORT(5u,
	     "ifInUcastPkts + ifInMulticastPkts + ifInBroadcastPkts + etherStatsCRCAlignErrors + etherStatsFragments + etherStatsJabbers",
	     C_RX_UCAST, C_RX_MCAST, C_RX_BCAST, C_RX_CRC, C_RX_FRAGMENTS,
	     C_RX_JABBERS),
	PORT(6u, "ifInBroadcastPkts", C_RX_BCAST),
	PORT(7u, "ifInMulticastPkts", C_RX_MCAST),
	PORT(8u, "etherStatsRxUndersizePkts", C_RX_UNDERSIZE),
	PORT(9u, "etherStatsFragments", C_RX_FRAGMENTS),
	PORT(10u, "etherStatsJabbers", C_RX_JABBERS),
	PORT(11u, "etherStatsRxPkts64Octets", C_RX_64),
	PORT(12u, "etherStatsRxPkts65to127Octets", C_RX_65_127),
	PORT(13u, "etherStatsRxPkts128to255Octets", C_RX_128_255),
	PORT(14u, "etherStatsRxPkts256to511Octets", C_RX_256_511),
	PORT(15u, "etherStatsRxPkts512to1023Octets", C_RX_512_1023),
	PORT(16u, "etherStatsRxPkts1024to1518Octets", C_RX_1024_1518),
};

static const struct pm_tca g_eth_pm_3_tca[] = {
	{ 3u, 0u, 1u },
	{ 8u, 1u, 2u },
	{ 9u, 2u, 3u },
	{ 10u, 3u, 4u },
};

/* MAC bridge port PM history data (G.988 9.3.9), of the port's side. */
static const struct pm_attr g_bridge_port_pm[] = {
	PORT(3u, "ifOutUcastPkts + ifOutMulticastPkts + ifOutBroadcastPkts",
	     C_TX_UCAST, C_TX_MCAST, C_TX_BCAST),
	NONE(4u, "none: the switch discards no frame for its delay"),
	PORT(5u, "etherStatsRxOversizePkts: frames over the port's frame limit",
	     C_RX_OVERSIZE),
	PORT(6u,
	     "ifInUcastPkts + ifInMulticastPkts + ifInBroadcastPkts + etherStatsCRCAlignErrors + etherStatsFragments + etherStatsJabbers",
	     C_RX_UCAST, C_RX_MCAST, C_RX_BCAST, C_RX_CRC, C_RX_FRAGMENTS,
	     C_RX_JABBERS),
	PORT(7u,
	     "etherStatsCRCAlignErrors + etherStatsFragments + etherStatsJabbers",
	     C_RX_CRC, C_RX_FRAGMENTS, C_RX_JABBERS),
};

static const struct pm_tca g_bridge_port_pm_tca[] = {
	{ 4u, 1u, 1u },
	{ 5u, 2u, 2u },
	{ 7u, 4u, 3u },
};

/* FEC PM history data (G.988 9.2.9), of the PON receiver. */
static const struct pm_attr g_fec_pm[] = {
	FROM(3u, SRC_FEC_BYTES, "the bytes FEC corrected"),
	FROM(4u, SRC_FEC_CORRECTED, "the codewords FEC corrected"),
	FROM(5u, SRC_FEC_UNCORRECTABLE, "the codewords FEC could not correct"),
	FROM(6u, SRC_FEC_CODEWORDS, "the codewords received"),
	FROM(7u, SRC_FEC_SECONDS,
	     "the seconds in which the uncorrectable codewords grew"),
};

static const struct pm_tca g_fec_pm_tca[] = {
	{ 3u, 0u, 1u },
	{ 4u, 1u, 2u },
	{ 5u, 2u, 3u },
	{ 7u, 4u, 4u },
};

/* GEM port network CTP PM history data (G.988 9.2.13), of its GEM port. */
static const struct pm_attr g_gem_pm[] = {
	FROM(3u, SRC_GEM_TX_FRAMES, "the GEM frames sent"),
	FROM(4u, SRC_GEM_RX_FRAMES, "the GEM frames received"),
	FROM(5u, SRC_GEM_RX_PAYLOAD, "the payload bytes received"),
	FROM(6u, SRC_GEM_TX_PAYLOAD, "the payload bytes sent"),
	NONE(7u, "none: encryption key indexes are of G.987, not G-PON"),
};

static const struct pm_tca g_gem_pm_tca[] = {
	{ 7u, 1u, 1u },
};

/*
 * IP host PM history data (G.988 9.4.2), of the IP host it is linked to. The
 * host is a Linux stack, which keeps none of these counts.
 */
static const struct pm_attr g_ip_host_pm[] = {
	NONE(3u, "none: the stack keeps no count of ICMP errors by this host"),
	NONE(4u, "none: the stack keeps no count of DNS errors"),
	NONE(5u, "none: the stack keeps no count of DHCP timeouts"),
	NONE(6u, "none: the stack keeps no count of address conflicts"),
	NONE(7u, "none: the stack keeps no count of failed allocations"),
	NONE(8u, "none: the stack keeps no count of internal errors"),
};

static const struct pm_tca g_ip_host_pm_tca[] = {
	{ 3u, 1u, 1u }, { 4u, 2u, 2u }, { 5u, 3u, 3u },
	{ 6u, 4u, 4u }, { 7u, 5u, 5u }, { 8u, 6u, 6u },
};

/* IP host PM history data part 2 (G.988 9.4.6): the DHCP exchanges. */
static const struct pm_attr g_ip_host_pm_2[] = {
	NONE(3u, "none: the DHCP client keeps no count of its attempts"),
	NONE(4u,
	     "none: the DHCP client keeps no count of the ACKs it received"),
	NONE(5u,
	     "none: the DHCP client keeps no count of the NAKs it received"),
	NONE(6u, "none: the DHCP client keeps no count of faulty responses"),
	NONE(7u,
	     "none: the DHCP client keeps no count of incomplete responses"),
};

static const struct pm_tca g_ip_host_pm_2_tca[] = {
	{ 6u, 0u, 1u },
	{ 7u, 1u, 2u },
};

/* TCP/UDP PM history data (G.988 9.4.4), of the TCP/UDP config data. */
static const struct pm_attr g_tcp_udp_pm[] = {
	NONE(3u, "none: the sockets are not counted when they fail to open"),
	NONE(4u, "none: the sockets are not counted when listening fails"),
	NONE(5u, "none: the sockets are not counted when binding fails"),
	NONE(6u, "none: the sockets are not counted when accepting fails"),
	NONE(7u, "none: the sockets are not counted when selecting fails"),
};

static const struct pm_tca g_tcp_udp_pm_tca[] = {
	{ 3u, 1u, 1u }, { 4u, 2u, 2u }, { 5u, 3u, 3u },
	{ 6u, 4u, 4u }, { 7u, 5u, 5u },
};

/* How an instance finds what it counts. */
enum pm_where {
	AT_UNI,
	AT_BRIDGE_PORT,
	AT_BRIDGE_PORT_US,
	AT_BRIDGE_PORT_DS,
	AT_EXTENDED,
	AT_GEM_PORT,
	AT_PON,
	AT_HOST,
};

/*
 * A class: where its instances count, its attributes, of what a port
 * receives (@rx) and sends (@tx) when the instance chooses, and its alerts.
 */
struct pm_class {
	uint16_t class_id;
	uint8_t where;
	const struct pm_attr *rx;
	const struct pm_attr *tx;
	size_t n;
	const struct pm_tca *tca;
	size_t ntca;
};

static_assert(ARRAY_SIZE(g_eth_rx) == ARRAY_SIZE(g_eth_tx),
	      "both directions count the same attributes");

static const struct pm_class g_class[] = {
	{
		.class_id = ME_ETH_PM,
		.where = AT_UNI,
		.rx = g_eth_pm,
		.n = ARRAY_SIZE(g_eth_pm),
		.tca = g_eth_pm_tca,
		.ntca = ARRAY_SIZE(g_eth_pm_tca),
	},
	{
		.class_id = ME_BRIDGE_PORT_PM,
		.where = AT_BRIDGE_PORT,
		.rx = g_bridge_port_pm,
		.n = ARRAY_SIZE(g_bridge_port_pm),
		.tca = g_bridge_port_pm_tca,
		.ntca = ARRAY_SIZE(g_bridge_port_pm_tca),
	},
	{
		.class_id = ME_ETH_PM_2,
		.where = AT_UNI,
		.rx = g_eth_pm_2,
		.n = ARRAY_SIZE(g_eth_pm_2),
		.tca = g_eth_pm_2_tca,
		.ntca = ARRAY_SIZE(g_eth_pm_2_tca),
	},
	{
		.class_id = ME_ETH_PM_3,
		.where = AT_UNI,
		.rx = g_eth_pm_3,
		.n = ARRAY_SIZE(g_eth_pm_3),
		.tca = g_eth_pm_3_tca,
		.ntca = ARRAY_SIZE(g_eth_pm_3_tca),
	},
	{
		.class_id = ME_FEC_PM,
		.where = AT_PON,
		.rx = g_fec_pm,
		.n = ARRAY_SIZE(g_fec_pm),
		.tca = g_fec_pm_tca,
		.ntca = ARRAY_SIZE(g_fec_pm_tca),
	},
	{
		.class_id = ME_ETH_FRAME_PM_DS,
		.where = AT_BRIDGE_PORT_DS,
		.rx = g_eth_rx,
		.tx = g_eth_tx,
		.n = ARRAY_SIZE(g_eth_rx),
		.tca = g_eth_frame_tca,
		.ntca = ARRAY_SIZE(g_eth_frame_tca),
	},
	{
		.class_id = ME_ETH_FRAME_PM_US,
		.where = AT_BRIDGE_PORT_US,
		.rx = g_eth_rx,
		.tx = g_eth_tx,
		.n = ARRAY_SIZE(g_eth_rx),
		.tca = g_eth_frame_tca,
		.ntca = ARRAY_SIZE(g_eth_frame_tca),
	},
	{
		.class_id = ME_ETH_FRAME_EXT_PM,
		.where = AT_EXTENDED,
		.rx = g_eth_rx,
		.tx = g_eth_tx,
		.n = ARRAY_SIZE(g_eth_rx),
		.tca = g_eth_ext_tca,
		.ntca = ARRAY_SIZE(g_eth_ext_tca),
	},
	{
		.class_id = ME_IP_HOST_PM,
		.where = AT_HOST,
		.rx = g_ip_host_pm,
		.n = ARRAY_SIZE(g_ip_host_pm),
		.tca = g_ip_host_pm_tca,
		.ntca = ARRAY_SIZE(g_ip_host_pm_tca),
	},
	{
		.class_id = ME_TCP_UDP_PM,
		.where = AT_HOST,
		.rx = g_tcp_udp_pm,
		.n = ARRAY_SIZE(g_tcp_udp_pm),
		.tca = g_tcp_udp_pm_tca,
		.ntca = ARRAY_SIZE(g_tcp_udp_pm_tca),
	},
	{
		.class_id = ME_IP_HOST_PM_2,
		.where = AT_HOST,
		.rx = g_ip_host_pm_2,
		.n = ARRAY_SIZE(g_ip_host_pm_2),
		.tca = g_ip_host_pm_2_tca,
		.ntca = ARRAY_SIZE(g_ip_host_pm_2_tca),
	},
	{
		.class_id = ME_GEM_CTP_PM,
		.where = AT_GEM_PORT,
		.rx = g_gem_pm,
		.n = ARRAY_SIZE(g_gem_pm),
		.tca = g_gem_pm_tca,
		.ntca = ARRAY_SIZE(g_gem_pm_tca),
	},
};

/* The counters of the switch that feed no attribute, and why. */
static const struct {
	uint16_t idx;
	const char *why;
} g_unmapped[] = {
	{ 13u,
	  "dot3InPauseFrames: these entities have no pause frame attribute" },
	{ 14u,
	  "dot3OutPauseFrames: these entities have no pause frame attribute" },
	{ 24u,
	  "dot3StatsSymbolErrors: these entities have no symbol error attribute" },
	{ 25u,
	  "dot3ControlInUnknownOpcodes: these entities have no such attribute" },
	{ 34u,
	  "etherStatsCollisions: the collisions are counted by their kind" },
	{ 51u,
	  "etherStatsTxPkts1519toMaxOctets: the size buckets end at 1518 bytes" },
	{ 52u,
	  "etherStatsTxBroadcastPkts: ifOutBroadcastPkts feeds the attribute" },
	{ 53u,
	  "etherStatsTxMulticastPkts: ifOutMulticastPkts feeds the attribute" },
	{ 66u,
	  "etherStatsRxPkts1519toMaxOctets: the size buckets end at 1518 bytes" },
	{ 67u, "inOamPduPkts: an EPON counter, which no GPON entity reports" },
	{ 68u, "outOamPduPkts: an EPON counter, which no GPON entity reports" },
};

static const struct pm_class *class_of(uint16_t class_id)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(g_class); i++)
		if (g_class[i].class_id == class_id)
			return &g_class[i];
	return NULL;
}

bool omci_pm_owns(uint16_t class_id)
{
	return class_of(class_id) != NULL;
}

uint16_t omci_pm_class_at(size_t i)
{
	return i < ARRAY_SIZE(g_class) ? g_class[i].class_id : 0u;
}

/* Whether a set of attributes has a switch counter, and what it feeds. */
static bool feeds(const struct pm_attr *attrs, size_t n, unsigned int idx,
		  const char **why)
{
	for (size_t a = 0; attrs != NULL && a < n; a++)
		for (unsigned int k = 0; k < attrs[a].nsrc; k++)
			if (attrs[a].idx[k] == idx) {
				*why = attrs[a].why;
				return true;
			}
	return false;
}

bool omci_pm_counter_accounted(unsigned int idx, const char **why)
{
	const char *what = NULL;
	bool found = false;

	for (unsigned int c = 0; c < ARRAY_SIZE(g_class) && !found; c++)
		found = feeds(g_class[c].rx, g_class[c].n, idx, &what) ||
			feeds(g_class[c].tx, g_class[c].n, idx, &what);

	for (unsigned int i = 0; i < ARRAY_SIZE(g_unmapped) && !found; i++)
		if (g_unmapped[i].idx == idx) {
			what = g_unmapped[i].why;
			found = true;
		}

	if (why != NULL)
		*why = what;
	return found;
}

/* The attributes 1 to 16 of an entity, by their number. */
#define PM_MAX_ATTR 17u

/* What an instance counts. */
enum pm_target_kind {
	T_NONE,
	T_PORT,
	T_GEM,
	T_PON,
};

/*
 * @kind; @port, the switch port, and @rx, its counts of what it receives
 * rather than sends, for T_PORT; @port_id for T_GEM.
 */
struct pm_target {
	uint8_t kind;
	uint8_t port;
	bool rx;
	uint16_t port_id;
};

/*
 * The counts of an instance: what it counts, the reading of each
 * attribute's counters at the last sample, the counts of the interval in
 * progress and of the last closed one, the alerts that stand, as bits of
 * their alarm numbers, and the last second counted as a FEC second.
 */
struct pm_row {
	bool used;
	bool seen;
	uint16_t class_id;
	uint16_t instance;
	const struct pm_class *cls;
	struct pm_target target;
	bool have_target;
	uint32_t last_ok;
	uint64_t last[PM_MAX_ATTR];
	uint64_t cur[PM_MAX_ATTR];
	uint64_t hist[PM_MAX_ATTR];
	uint16_t tca;
	bool fec_counted;
	uint64_t fec_second;
};

/* The readings of one sample, made when first needed. */
struct pm_snapshot {
	bool port_read[2];
	bool port_ok[2];
	uint64_t port[2][OMCI_HW_PORT_COUNTERS];
	bool pon_read;
	bool pon_ok;
	struct omci_hw_pon pon;
};

/*
 * The state: the instances, the readings of the sample in progress, the
 * second of the last sample, when the interval in progress ends and the
 * number of the last one that closed.
 */
struct omci_pm {
	struct omci_mib *mib;
	struct pm_row row[OMCI_PM_MAX_INSTANCES];
	struct pm_snapshot snap;
	bool started;
	bool sampled;
	bool full_told;
	uint64_t now_sec;
	uint64_t next_end;
	uint8_t interval_end_time;
};

static struct omci_pm g_pm;
static bool g_pm_taken;

enum omci_status omci_pm_init(struct omci_pm **out, struct omci_mib *mib)
{
	if (out == NULL || mib == NULL)
		return OMCI_ERR_INVAL;
	if (g_pm_taken)
		return OMCI_ERR_BUSY;

	memset(&g_pm, 0, sizeof(g_pm));
	g_pm.mib = mib;
	g_pm_taken = true;
	*out = &g_pm;
	return OMCI_OK;
}

void omci_pm_fini(struct omci_pm *pm)
{
	if (pm == NULL)
		return;
	memset(pm, 0, sizeof(*pm));
	g_pm_taken = false;
}

static bool attr_bytes(const struct omci_mib *mib, uint16_t class_id,
		       uint16_t instance, uint8_t attr_no, uint8_t *out,
		       size_t len)
{
	size_t got = 0;

	return omci_mib_attr_get(mib, class_id, instance, attr_no, out, len,
				 &got) == OMCI_OK &&
	       got == len;
}

static bool attr_u16(const struct omci_mib *mib, uint16_t class_id,
		     uint16_t instance, uint8_t attr_no, uint16_t *out)
{
	uint8_t b[2];

	if (!attr_bytes(mib, class_id, instance, attr_no, b, sizeof(b)))
		return false;
	*out = (uint16_t)omci_be16(b);
	return true;
}

/*
 * The control block of an extended PM (G.988 9.3.32): the threshold data,
 * the parent, the disables, the control fields and the TCI.
 */
struct ext_control {
	uint16_t threshold_id;
	uint16_t parent_class;
	uint16_t parent_instance;
	uint16_t accumulation_off;
	uint16_t tca_off;
	uint16_t fields;
	uint16_t tci;
};

#define EXT_CONTROL_LEN 16u
/* Accumulation disable: clear everything (an action), and stop counting. */
#define EXT_CLEAR 0x8000u
#define EXT_OFF 0x4000u
/* Control fields: count continuously, count downstream, match the TCI. */
#define EXT_CONTINUOUS 0x0001u
#define EXT_DOWNSTREAM 0x0002u
#define EXT_MATCH_PBITS 0x4000u
#define EXT_MATCH_VID 0x8000u

/* Read the control block: false, and all of it 0, when it is not there. */
static bool ext_control(const struct omci_mib *mib, uint16_t instance,
			struct ext_control *out)
{
	uint8_t b[EXT_CONTROL_LEN] = { 0 };
	const bool ok = attr_bytes(mib, ME_ETH_FRAME_EXT_PM, instance,
				   PM_CONTROL, b, sizeof(b));

	out->threshold_id = (uint16_t)omci_be16(&b[0]);
	out->parent_class = (uint16_t)omci_be16(&b[2]);
	out->parent_instance = (uint16_t)omci_be16(&b[4]);
	out->accumulation_off = (uint16_t)omci_be16(&b[6]);
	out->tca_off = (uint16_t)omci_be16(&b[8]);
	out->fields = (uint16_t)omci_be16(&b[10]);
	out->tci = (uint16_t)omci_be16(&b[12]);
	return ok;
}

/* Whether an instance counts on without intervals (an extended PM's choice). */
static bool continuous(const struct omci_pm *pm, const struct pm_row *r)
{
	struct ext_control ec;

	return r->class_id == ME_ETH_FRAME_EXT_PM &&
	       ext_control(pm->mib, r->instance, &ec) &&
	       (ec.fields & EXT_CONTINUOUS) != 0u;
}

/* The side of the bridge a bridge port is on. */
enum side {
	SIDE_NONE,
	SIDE_UNI,
	SIDE_ANI,
};

static enum side bridge_port_side(const struct omci_mib *mib, uint16_t instance)
{
	uint8_t tp_type = 0;

	if (!attr_bytes(mib, ME_BRIDGE_PORT, instance, BRIDGE_PORT_TP_TYPE,
			&tp_type, 1u))
		return SIDE_NONE;
	switch (tp_type) {
	case OMCI_BRIDGE_TP_PPTP_ETH:
		return SIDE_UNI;
	case OMCI_BRIDGE_TP_MAPPER:
	case OMCI_BRIDGE_TP_GEM_IW:
	case OMCI_BRIDGE_TP_MCAST_GEM:
		return SIDE_ANI;
	default:
		return SIDE_NONE;
	}
}

/*
 * Ethernet frames through a side of the bridge: upstream, what the UNI
 * receives or the PON side sends; downstream, the reverse. The PON side's
 * switch port carries every GEM port, so every instance on that side
 * counts all of them.
 */
static void eth_frames(enum side s, bool downstream, struct pm_target *t)
{
	if (s == SIDE_NONE)
		return;
	t->kind = T_PORT;
	t->port = s == SIDE_UNI ? UNI_SWITCH_PORT : PON_SWITCH_PORT;
	t->rx = (s == SIDE_UNI) != downstream;
}

/* What an extended PM counts: the entity its control block names. */
static void ext_target(const struct omci_pm *pm, const struct pm_row *r,
		       struct pm_target *t)
{
	struct ext_control ec;

	if (!ext_control(pm->mib, r->instance, &ec) ||
	    (ec.fields & (EXT_MATCH_PBITS | EXT_MATCH_VID)) != 0u)
		return;

	const bool down = (ec.fields & EXT_DOWNSTREAM) != 0u;

	switch (ec.parent_class) {
	case ME_PPTP_ETH_UNI:
		eth_frames(SIDE_UNI, down, t);
		break;
	case ME_BRIDGE_PORT:
		eth_frames(bridge_port_side(pm->mib, ec.parent_instance), down,
			   t);
		break;
	case ME_GEM_IW_TP:
	case ME_MCAST_GEM_IW_TP:
		eth_frames(SIDE_ANI, down, t);
		break;
	default:
		break;
	}
}

static void resolve(const struct omci_pm *pm, const struct pm_row *r,
		    struct pm_target *t)
{
	memset(t, 0, sizeof(*t));
	switch (r->cls->where) {
	case AT_UNI:
		t->kind = T_PORT;
		t->port = UNI_SWITCH_PORT;
		break;
	case AT_BRIDGE_PORT: {
		const enum side s = bridge_port_side(pm->mib, r->instance);

		if (s != SIDE_NONE) {
			t->kind = T_PORT;
			t->port = s == SIDE_UNI ? UNI_SWITCH_PORT :
						  PON_SWITCH_PORT;
		}
		break;
	}
	case AT_BRIDGE_PORT_US:
	case AT_BRIDGE_PORT_DS:
		eth_frames(bridge_port_side(pm->mib, r->instance),
			   r->cls->where == AT_BRIDGE_PORT_DS, t);
		break;
	case AT_EXTENDED:
		ext_target(pm, r, t);
		break;
	case AT_GEM_PORT:
		if (attr_u16(pm->mib, ME_GEM_CTP, r->instance, GEM_CTP_PORT_ID,
			     &t->port_id))
			t->kind = T_GEM;
		break;
	case AT_HOST:
		break;
	default:
		t->kind = T_PON;
		break;
	}
}

/* The attributes an instance counts with what it counts now. */
static const struct pm_attr *attrs_of(const struct pm_row *r)
{
	return r->cls->tx != NULL && r->target.kind == T_PORT && !r->target.rx ?
		       r->cls->tx :
		       r->cls->rx;
}

static uint8_t attr_size(uint16_t class_id, uint8_t attr_no)
{
	const struct omci_me_attr *a =
		omci_me_attr_get(omci_me_class_by_id(class_id), attr_no);

	return a != NULL ? (uint8_t)a->size : 4u;
}

static bool port_counters(struct omci_pm *pm, uint8_t port,
			  const uint64_t **out)
{
	const unsigned int s = port == UNI_SWITCH_PORT ? 0u : 1u;

	if (!pm->snap.port_read[s]) {
		pm->snap.port_read[s] = true;
		pm->snap.port_ok[s] =
			omci_hw()->port_counters(port, pm->snap.port[s]) ==
			OMCI_OK;
	}
	*out = pm->snap.port[s];
	return pm->snap.port_ok[s];
}

static bool pon_state(struct omci_pm *pm, const struct omci_hw_pon **out)
{
	if (!pm->snap.pon_read) {
		pm->snap.pon_read = true;
		pm->snap.pon_ok = omci_hw()->pon != NULL &&
				  omci_hw()->pon(&pm->snap.pon) == OMCI_OK;
	}
	*out = &pm->snap.pon;
	return pm->snap.pon_ok;
}

static uint64_t fec_count(const struct omci_hw_pon *p, uint8_t source)
{
	if (!p->fec_valid)
		return 0;
	switch (source) {
	case SRC_FEC_BYTES:
		return p->fec_corrected_bytes;
	case SRC_FEC_CORRECTED:
		return p->fec_corrected_codewords;
	case SRC_FEC_CODEWORDS:
		return p->fec_codewords;
	default:
		return p->fec_uncorrectable_codewords;
	}
}

static uint64_t gem_count(const struct omci_hw_gem_counts *g, uint8_t source)
{
	switch (source) {
	case SRC_GEM_TX_FRAMES:
		return g->tx_frames;
	case SRC_GEM_RX_FRAMES:
		return g->rx_frames;
	case SRC_GEM_TX_PAYLOAD:
		return g->tx_payload;
	default:
		return g->rx_payload;
	}
}

/*
 * The reading of an attribute's counters, from what the instance counts:
 * 0 for an attribute that nothing counts or an instance whose entity counts
 * nothing. False when the hardware could not be read.
 */
static bool reading(struct omci_pm *pm, const struct pm_row *r,
		    const struct pm_attr *a,
		    const struct omci_hw_gem_counts *gem, bool gem_ok,
		    uint64_t *out)
{
	*out = 0;
	switch (a->source) {
	case SRC_NONE:
		return true;
	case SRC_PORT: {
		const uint64_t *c;

		if (r->target.kind != T_PORT)
			return true;
		if (!port_counters(pm, r->target.port, &c))
			return false;
		for (unsigned int k = 0; k < a->nsrc; k++)
			*out += c[a->idx[k]];
		return true;
	}
	case SRC_GEM_TX_FRAMES:
	case SRC_GEM_RX_FRAMES:
	case SRC_GEM_TX_PAYLOAD:
	case SRC_GEM_RX_PAYLOAD:
		if (r->target.kind != T_GEM)
			return true;
		*out = gem_count(gem, a->source);
		return gem_ok;
	default: {
		const struct omci_hw_pon *p;

		if (!pon_state(pm, &p))
			return false;
		*out = fec_count(p, a->source);
		return true;
	}
	}
}

/* The most an attribute of @size bytes holds. */
static uint64_t attr_max(uint8_t size)
{
	return size >= 8u ? UINT64_MAX : (1ull << (8u * size)) - 1u;
}

/*
 * Add to a count: within an interval it stops at the most its attribute
 * holds (G.988 I.4); counting continuously, it wraps.
 */
static void add(uint64_t *count, uint64_t delta, uint8_t size, bool wrap)
{
	const uint64_t max = attr_max(size);

	if (wrap)
		*count = (*count + delta) & max;
	else
		*count = delta > max - *count ? max : *count + delta;
}

/*
 * Take the counters of an instance: each attribute grows by what its
 * counters grew since the last sample, unless the instance's control block
 * stops it from counting. A counter that went back, as the counters of a
 * GEM port made again do, counts from zero; an instance that counts
 * something else now starts from the readings. A FEC second is a second
 * of the clock in which the uncorrectable codewords grew.
 *
 * Return: false when a counter could not be read.
 */
static bool sample_row(struct omci_pm *pm, struct pm_row *r)
{
	struct pm_target t;

	resolve(pm, r, &t);
	if (!r->have_target || memcmp(&t, &r->target, sizeof(t)) != 0) {
		r->target = t;
		r->have_target = true;
		r->last_ok = 0;
	}

	struct ext_control ec = { 0 };
	const bool ext = r->class_id == ME_ETH_FRAME_EXT_PM &&
			 ext_control(pm->mib, r->instance, &ec);
	const bool wrap = ext && (ec.fields & EXT_CONTINUOUS) != 0u;

	struct omci_hw_gem_counts gem = { 0 };
	const bool gem_ok = t.kind == T_GEM && omci_hw()->gem_counts != NULL &&
			    omci_hw()->gem_counts(t.port_id, &gem) == OMCI_OK;

	const struct pm_attr *attrs = attrs_of(r);
	bool all = true;

	for (size_t i = 0; i < r->cls->n; i++) {
		const struct pm_attr *a = &attrs[i];
		const uint8_t no = a->attr_no;
		uint64_t now = 0, delta = 0;

		if (!reading(pm, r, a, &gem, gem_ok, &now)) {
			all = false;
			continue;
		}
		if ((r->last_ok & (1u << no)) != 0u)
			delta = now >= r->last[no] ? now - r->last[no] : now;
		r->last[no] = now;
		r->last_ok |= 1u << no;

		if (ext && (ec.accumulation_off &
			    (EXT_OFF | (1u << (no - PM_CONTROL - 1u)))) != 0u)
			continue;
		if (a->source == SRC_FEC_SECONDS) {
			if (delta == 0u ||
			    (r->fec_counted && r->fec_second == pm->now_sec))
				continue;
			r->fec_counted = true;
			r->fec_second = pm->now_sec;
			delta = 1u;
		}
		add(&r->cur[no], delta, attr_size(r->class_id, no), wrap);
	}
	return all;
}

/* Begin a sample: the readings are made afresh. */
static void snapshot_drop(struct omci_pm *pm)
{
	memset(&pm->snap, 0, sizeof(pm->snap));
}

static struct pm_row *row_find(struct omci_pm *pm, uint16_t class_id,
			       uint16_t instance)
{
	for (unsigned int i = 0; i < OMCI_PM_MAX_INSTANCES; i++)
		if (pm->row[i].used && pm->row[i].class_id == class_id &&
		    pm->row[i].instance == instance)
			return &pm->row[i];
	return NULL;
}

/* Start counting for an instance, from the readings of now. */
static struct pm_row *row_new(struct omci_pm *pm, uint16_t class_id,
			      uint16_t instance)
{
	for (unsigned int i = 0; i < OMCI_PM_MAX_INSTANCES; i++) {
		struct pm_row *r = &pm->row[i];

		if (r->used)
			continue;
		memset(r, 0, sizeof(*r));
		r->used = true;
		r->seen = true;
		r->class_id = class_id;
		r->instance = instance;
		r->cls = class_of(class_id);
		(void)sample_row(pm, r);
		return r;
	}
	if (!pm->full_told)
		OMCI_WARN(
			"pm: no room to count %u/%u; %u instances are counted",
			class_id, instance, OMCI_PM_MAX_INSTANCES);
	pm->full_told = true;
	return NULL;
}

static struct pm_row *row_for(struct omci_pm *pm, uint16_t class_id,
			      uint16_t instance)
{
	struct pm_row *r = row_find(pm, class_id, instance);

	return r != NULL ? r : row_new(pm, class_id, instance);
}

/* Count for every instance the MIB holds, and for no other. */
static void reconcile(struct omci_pm *pm)
{
	struct omci_mib_cursor cur;

	for (unsigned int i = 0; i < OMCI_PM_MAX_INSTANCES; i++)
		pm->row[i].seen = false;

	for (enum omci_status rc = omci_mib_walk_first(pm->mib, &cur);
	     rc == OMCI_OK; rc = omci_mib_walk_next(pm->mib, &cur)) {
		if (!omci_pm_owns(cur.class_id))
			continue;

		struct pm_row *r = row_for(pm, cur.class_id, cur.instance);

		if (r != NULL)
			r->seen = true;
	}

	for (unsigned int i = 0; i < OMCI_PM_MAX_INSTANCES; i++)
		if (pm->row[i].used && !pm->row[i].seen)
			pm->row[i].used = false;
}

static bool sample_all(struct omci_pm *pm)
{
	bool all = true;

	for (unsigned int i = 0; i < OMCI_PM_MAX_INSTANCES; i++)
		if (pm->row[i].used && !sample_row(pm, &pm->row[i]))
			all = false;
	return all;
}

/* A threshold of 0 or of all ones does not hold (G.988 I.4). */
static bool threshold_holds(uint32_t v)
{
	return v != 0u && v != 0xFFFFu && v != 0xFFFFFFFFu;
}

/*
 * The thresholds an instance is held against, 1 to 14, from the threshold
 * data its control block points at; bit n of the result is set when
 * threshold n was read.
 */
static uint16_t thresholds(const struct omci_pm *pm, uint16_t id,
			   uint32_t value[15])
{
	uint16_t have = 0;

	for (uint8_t k = 1; k <= 14u; k++) {
		uint8_t b[4];
		const uint16_t cls = k <= 7u ? ME_THRESHOLD_1 : ME_THRESHOLD_2;
		const uint8_t no = k <= 7u ? k : (uint8_t)(k - 7u);

		if (!attr_bytes(pm->mib, cls, id, no, b, sizeof(b)))
			continue;
		value[k] = (uint32_t)omci_be32(b);
		have |= (uint16_t)(1u << k);
	}
	return have;
}

/*
 * Hold the counts of an instance against its thresholds: an alert is
 * raised when a count reaches its threshold, and stands until the interval
 * ends; one whose threshold no longer holds, or whose alerts the control
 * block disables, clears.
 */
static void judge_row(struct omci_pm *pm, struct pm_row *r)
{
	uint16_t id = 0, off = 0;
	bool pointed;

	if (r->class_id == ME_ETH_FRAME_EXT_PM) {
		struct ext_control ec;

		pointed = ext_control(pm->mib, r->instance, &ec);
		id = ec.threshold_id;
		off = ec.tca_off;
	} else {
		pointed = attr_u16(pm->mib, r->class_id, r->instance,
				   PM_CONTROL, &id);
	}

	uint32_t value[15] = { 0 };
	const uint16_t have = pointed ? thresholds(pm, id, value) : 0u;

	for (size_t i = 0; i < r->cls->ntca; i++) {
		const struct pm_tca *e = &r->cls->tca[i];
		const uint16_t bit = (uint16_t)(1u << e->bit);
		const bool on =
			(have & (1u << e->threshold)) != 0u &&
			threshold_holds(value[e->threshold]) &&
			(off & (EXT_OFF | (1u << (e->threshold - 1u)))) == 0u;

		if (!on)
			r->tca &= (uint16_t)~bit;
		else if (r->cur[e->attr_no] >= value[e->threshold])
			r->tca |= bit;
	}
}

/*
 * Close the interval: each instance's counts become its history, and its
 * alerts clear (G.988 I.4); an instance that counts continuously keeps
 * both.
 */
static void close_interval(struct omci_pm *pm, unsigned int intervals)
{
	for (unsigned int i = 0; i < OMCI_PM_MAX_INSTANCES; i++) {
		struct pm_row *r = &pm->row[i];

		if (!r->used || continuous(pm, r))
			continue;
		memcpy(r->hist, r->cur, sizeof(r->hist));
		memset(r->cur, 0, sizeof(r->cur));
		r->tca = 0;
	}
	pm->interval_end_time = (uint8_t)(pm->interval_end_time + intervals);
}

bool omci_pm_tick(struct omci_pm *pm, uint64_t now_sec)
{
	if (pm == NULL)
		return false;

	if (!pm->started) {
		pm->started = true;
		pm->next_end = now_sec + OMCI_PM_INTERVAL_SEC;
	}
	if (pm->sampled && now_sec == pm->now_sec)
		return false;
	pm->sampled = true;
	pm->now_sec = now_sec;

	snapshot_drop(pm);
	reconcile(pm);
	(void)sample_all(pm);

	if (now_sec < pm->next_end) {
		for (unsigned int i = 0; i < OMCI_PM_MAX_INSTANCES; i++)
			if (pm->row[i].used)
				judge_row(pm, &pm->row[i]);
		return false;
	}

	/* A daemon that stood still over a boundary closes the missed ones too. */
	const uint64_t n = (now_sec - pm->next_end) / OMCI_PM_INTERVAL_SEC + 1u;

	close_interval(pm, (unsigned int)(n & 0xFFu));
	pm->next_end += n * OMCI_PM_INTERVAL_SEC;
	return true;
}

void omci_pm_synchronize(struct omci_pm *pm, uint64_t now_sec,
			 unsigned int offset_sec)
{
	if (pm == NULL)
		return;

	pm->sampled = true;
	pm->now_sec = now_sec;
	snapshot_drop(pm);
	reconcile(pm);
	(void)sample_all(pm);

	for (unsigned int i = 0; i < OMCI_PM_MAX_INSTANCES; i++) {
		struct pm_row *r = &pm->row[i];

		if (!r->used || continuous(pm, r))
			continue;
		memset(r->cur, 0, sizeof(r->cur));
		memset(r->hist, 0, sizeof(r->hist));
		r->tca = 0;
	}
	pm->interval_end_time = 0;
	pm->started = true;
	pm->next_end = now_sec + OMCI_PM_INTERVAL_SEC -
		       offset_sec % OMCI_PM_INTERVAL_SEC;
}

void omci_pm_schedule_get(const struct omci_pm *pm,
			  struct omci_pm_schedule *out)
{
	out->started = pm->started;
	out->next_end = pm->next_end;
	out->interval_end_time = pm->interval_end_time;
}

void omci_pm_schedule_set(struct omci_pm *pm, const struct omci_pm_schedule *in)
{
	pm->started = in->started;
	pm->next_end = in->next_end;
	pm->interval_end_time = in->interval_end_time;
}

void omci_pm_changed(struct omci_pm *pm, uint16_t class_id, uint16_t instance)
{
	if (pm == NULL || !omci_pm_owns(class_id) ||
	    !omci_mib_exists(pm->mib, class_id, instance))
		return;

	snapshot_drop(pm);

	struct pm_row *r = row_for(pm, class_id, instance);
	struct ext_control ec;

	if (r == NULL || class_id != ME_ETH_FRAME_EXT_PM ||
	    !ext_control(pm->mib, instance, &ec) ||
	    (ec.accumulation_off & EXT_CLEAR) == 0u)
		return;

	/* Clearing is an action: it is done, and the bit reads 0 again. */
	uint8_t b[EXT_CONTROL_LEN];

	if (attr_bytes(pm->mib, class_id, instance, PM_CONTROL, b, sizeof(b))) {
		omci_be16_store(&b[6], ec.accumulation_off & ~EXT_CLEAR);
		(void)!omci_mib_attr_set(pm->mib, class_id, instance,
					 PM_CONTROL, b, sizeof(b));
	}
	memset(r->cur, 0, sizeof(r->cur));
	memset(r->hist, 0, sizeof(r->hist));
	r->tca = 0;
}

static void store_count(struct omci_mib *mib, uint16_t class_id,
			uint16_t instance, uint8_t attr_no, uint64_t value)
{
	const uint8_t size = attr_size(class_id, attr_no);
	uint8_t wire[8];

	for (uint8_t k = 0; k < size; k++)
		wire[k] = (uint8_t)(value >> (8u * (size - 1u - k)));
	(void)!omci_mib_attr_set(mib, class_id, instance, attr_no, wire, size);
}

enum omci_status omci_pm_refresh(struct omci_pm *pm, uint16_t class_id,
				 uint16_t instance, bool current)
{
	if (pm == NULL)
		return OMCI_ERR_INVAL;
	if (class_of(class_id) == NULL)
		return OMCI_ERR_ME_UNKNOWN;
	if (!omci_mib_exists(pm->mib, class_id, instance))
		return OMCI_ERR_INST_UNKNOWN;

	snapshot_drop(pm);

	struct pm_row *r = row_for(pm, class_id, instance);

	if (r == NULL)
		return OMCI_ERR_NOMEM;

	enum omci_status rc = OMCI_OK;

	if (current && !sample_row(pm, r))
		rc = OMCI_ERR_HW;

	/*
	 * Counting continuously there is no interval: the end time is 0 and
	 * both Get and Get current data read the counts so far (G.988 I.4.2).
	 */
	const bool cont = continuous(pm, r);
	const uint8_t end = cont ? 0u : pm->interval_end_time;

	(void)!omci_mib_attr_set(pm->mib, class_id, instance, PM_INTERVAL_END,
				 &end, 1u);
	for (size_t i = 0; i < r->cls->n; i++) {
		const uint8_t no = r->cls->rx[i].attr_no;

		store_count(pm->mib, class_id, instance, no,
			    current || cont ? r->cur[no] : r->hist[no]);
	}
	return rc;
}

size_t omci_pm_tca(const struct omci_pm *pm, uint16_t class_id, uint8_t bit,
		   uint16_t *out, size_t cap)
{
	size_t n = 0;

	if (pm == NULL || out == NULL || bit >= OMCI_PM_TCA_BITS)
		return 0;
	for (unsigned int i = 0; i < OMCI_PM_MAX_INSTANCES && n < cap; i++) {
		const struct pm_row *r = &pm->row[i];

		if (r->used && r->class_id == class_id &&
		    (r->tca & (1u << bit)) != 0u)
			out[n++] = r->instance;
	}
	return n;
}

void omci_pm_forget(struct omci_pm *pm, uint16_t class_id, uint16_t instance)
{
	struct pm_row *r;

	if (pm == NULL)
		return;
	r = row_find(pm, class_id, instance);
	if (r != NULL)
		r->used = false;
}

void omci_pm_forget_all(struct omci_pm *pm)
{
	if (pm == NULL)
		return;
	for (unsigned int i = 0; i < OMCI_PM_MAX_INSTANCES; i++)
		pm->row[i].used = false;
	pm->full_told = false;
}
