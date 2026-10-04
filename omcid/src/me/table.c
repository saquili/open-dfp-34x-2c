// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
/*
 * The managed entity classes of table.h.
 *
 * Made by tools/gen_me_table.py from data/g988-me-min.tsv: change the
 * table, not this file, then run python3 tools/gen_me_table.py --write.
 *
 * The table follows ITU-T G.988 (11/2022). sha256 of the table:
 * eb527ec96478c7d1fd887b0f9190d2905452bdb55522c48ae2b37b97e5d6319c
 * 30 classes, 286 attributes.
 */

#include "me/table.h"
#include "omcid/array_size.h"

/* Class 2: ONU data */
static const struct omci_me_attr attrs_class_2[] = {
	{ "MIB data sync", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
};

/* Class 5: Cardholder */
static const struct omci_me_attr attrs_class_5[] = {
	{ "Actual plug-in unit type", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_AVC }, /* 1 */
	{ "Expected plug-in unit type", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Expected port count", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 3 */
	{ "Expected equipment ID", 20, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 4 */
	{ "Actual equipment ID", 20, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_AVC }, /* 5 */
	{ "Protection profile pointer", 1, OMCI_ME_ACC_READ, 0 }, /* 6 */
	{ "Invoke protection switch", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 7 */
	{ "ARC", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_AVC }, /* 8 */
	{ "ARC interval", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 9 */
};

/* Class 6: Circuit pack */
static const struct omci_me_attr attrs_class_6[] = {
	{ "Type", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Number of ports", 1, OMCI_ME_ACC_READ, 0 }, /* 2 */
	{ "Serial number", 8, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Version", 14, OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "Vendor ID", 4, OMCI_ME_ACC_READ, 0 }, /* 5 */
	{ "Administrative state", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 6 */
	{ "Operational state", 1, OMCI_ME_ACC_READ, OMCI_ME_ATTR_AVC }, /* 7 */
	{ "Bridged or IP ind", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 8 */
	{ "Equipment ID", 20, OMCI_ME_ACC_READ, 0 }, /* 9 */
	{ "Card configuration", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 10 */
	{ "Total T-CONT buffer number", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 11 */
	{ "Total priority queue number", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 12 */
	{ "Total traffic scheduler number", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 13 */
	{ "Power shed override", 4, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 14 */
};

/* Class 7: Software image */
static const struct omci_me_attr attrs_class_7[] = {
	{ "Version", 14, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_AVC }, /* 1 */
	{ "Is committed", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_AVC }, /* 2 */
	{ "Is active", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_AVC }, /* 3 */
	{ "Is valid", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_AVC }, /* 4 */
	{ "Product code", 25, OMCI_ME_ACC_READ, OMCI_ME_ATTR_AVC }, /* 5 */
	{ "Image hash", 16, OMCI_ME_ACC_READ, OMCI_ME_ATTR_AVC }, /* 6 */
};

/* Class 11: Physical path termination point Ethernet UNI */
static const struct omci_me_attr attrs_class_11[] = {
	{ "Expected type", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Sensed type", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_AVC }, /* 2 */
	{ "Auto detection configuration", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Ethernet loopback configuration", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "Administrative state", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 5 */
	{ "Operational state", 1, OMCI_ME_ACC_READ, OMCI_ME_ATTR_AVC }, /* 6 */
	{ "Configuration ind", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "Max frame size", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 8 */
	{ "DTE or DCE ind", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 9 */
	{ "Pause time", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 10 */
	{ "Bridged or IP ind", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 11 */
	{ "ARC", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_AVC }, /* 12 */
	{ "ARC interval", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 13 */
	{ "PPPoE filter", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 14 */
	{ "Power control", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 15 */
};

/* Class 45: MAC bridge service profile */
static const struct omci_me_attr attrs_class_45[] = {
	{ "Spanning tree ind", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Learning ind", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Port bridging ind", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Priority", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "Max age", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 5 */
	{ "Hello time", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 6 */
	{ "Forward delay", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "Unknown MAC address discard", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 8 */
	{ "MAC learning depth", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 9 */
	{ "Dynamic filtering ageing time", 4,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 10 */
};

/* Class 47: MAC bridge port configuration data */
static const struct omci_me_attr attrs_class_47[] = {
	{ "Bridge ID pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Port num", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "TP type", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "TP pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "Port priority", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 5 */
	{ "Port path cost", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 6 */
	{ "Port spanning tree ind", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "Deprecated 1", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 8 */
	{ "Deprecated 2", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 9 */
	{ "Port MAC address", 6, OMCI_ME_ACC_READ, 0 }, /* 10 */
	{ "Outbound TD pointer", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 11 */
	{ "Inbound TD pointer", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 12 */
	{ "MAC learning depth", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 13 */
	{ "LASP ID pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 14 */
};

/* Class 84: VLAN tagging filter data */
static const struct omci_me_attr attrs_class_84[] = {
	{ "VLAN filter list", 24,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Forward operation", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Number of entries", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
};

/* Class 130: IEEE 802.1p mapper service profile */
static const struct omci_me_attr attrs_class_130[] = {
	{ "TP pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Interwork TP pointer for P-bit priority 0", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Interwork TP pointer for P-bit priority 1", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Interwork TP pointer for P-bit priority 2", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "Interwork TP pointer for P-bit priority 3", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 5 */
	{ "Interwork TP pointer for P-bit priority 4", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 6 */
	{ "Interwork TP pointer for P-bit priority 5", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "Interwork TP pointer for P-bit priority 6", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 8 */
	{ "Interwork TP pointer for P-bit priority 7", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 9 */
	{ "Unmarked frame option", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 10 */
	{ "DSCP to P-bit mapping", 24, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 11 */
	{ "Default P-bit assumption", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 12 */
	{ "TP type", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 13 */
};

/* Class 131: OLT-G */
static const struct omci_me_attr attrs_class_131[] = {
	{ "OLT vendor ID", 4, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Equipment ID", 20, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Version", 14, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Time of day information", 14, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 4 */
};

/* Class 134: IP host config data */
static const struct omci_me_attr attrs_class_134[] = {
	{ "IP options", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "MAC address", 6, OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "ONU identifier", 25, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "IP address", 4, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "Mask", 4, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 5 */
	{ "Gateway", 4, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 6 */
	{ "Primary DNS", 4, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "Secondary DNS", 4, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 8 */
	{ "Current address", 4, OMCI_ME_ACC_READ, OMCI_ME_ATTR_AVC }, /* 9 */
	{ "Current mask", 4, OMCI_ME_ACC_READ, OMCI_ME_ATTR_AVC }, /* 10 */
	{ "Current gateway", 4, OMCI_ME_ACC_READ, OMCI_ME_ATTR_AVC }, /* 11 */
	{ "Current primary DNS", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_AVC }, /* 12 */
	{ "Current secondary DNS", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_AVC }, /* 13 */
	{ "Domain name", 25, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_AVC }, /* 14 */
	{ "Host name", 25, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_AVC }, /* 15 */
	{ "Relay agent options", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_AVC }, /* 16 */
};

/* Class 171: Extended VLAN tagging operation configuration data */
static const struct omci_me_attr attrs_class_171[] = {
	{ "Association type", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Received frame VLAN tagging operation table max size", 2,
	  OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Input TPID", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Output TPID", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "Downstream mode", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 5 */
	{ "Received frame VLAN tagging operation table", 16,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_TABLE }, /* 6 */
	{ "Associated ME pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "DSCP to P-bit mapping", 24, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 8 */
	{ "Enhanced mode", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 9 */
	{ "Enhanced received frame classification and processing table", 28,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, OMCI_ME_ATTR_TABLE }, /* 10 */
};

/* Class 256: ONU-G */
static const struct omci_me_attr attrs_class_256[] = {
	{ "Vendor ID", 4, OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Version", 14, OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Serial number", 8, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Traffic management option", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "Deprecated", 1, OMCI_ME_ACC_READ, 0 }, /* 5 */
	{ "Battery backup", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 6 */
	{ "Administrative state", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "Operational state", 1, OMCI_ME_ACC_READ, OMCI_ME_ATTR_AVC }, /* 8 */
	{ "ONU survival time", 1, OMCI_ME_ACC_READ, 0 }, /* 9 */
	{ "Logical ONU ID", 24, OMCI_ME_ACC_READ, OMCI_ME_ATTR_AVC }, /* 10 */
	{ "Logical password", 12, OMCI_ME_ACC_READ, OMCI_ME_ATTR_AVC }, /* 11 */
	{ "Credentials status", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 12 */
	{ "Extended TC-layer options", 2, OMCI_ME_ACC_READ, 0 }, /* 13 */
};

/* Class 257: ONU2-G */
static const struct omci_me_attr attrs_class_257[] = {
	{ "Equipment ID", 20, OMCI_ME_ACC_READ, 0 }, /* 1 */
	{ "OMCC version", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_AVC }, /* 2 */
	{ "Vendor product code", 2, OMCI_ME_ACC_READ, 0 }, /* 3 */
	{ "Security capability", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "Security mode", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 5 */
	{ "Total priority queue number", 2, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 6 */
	{ "Total traffic scheduler number", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "Deprecated", 1, OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 8 */
	{ "Total GEM port-ID number", 2, OMCI_ME_ACC_READ, 0 }, /* 9 */
	{ "SysUpTime", 4, OMCI_ME_ACC_READ, 0 }, /* 10 */
	{ "Connectivity capability", 2, OMCI_ME_ACC_READ, 0 }, /* 11 */
	{ "Current connectivity mode", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 12 */
	{ "Quality of service (QoS) configuration flexibility", 2,
	  OMCI_ME_ACC_READ, 0 }, /* 13 */
	{ "Priority queue scale factor", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 14 */
};

/* Class 262: T-CONT */
static const struct omci_me_attr attrs_class_262[] = {
	{ "Alloc-ID", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Deprecated", 1, OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Policy", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
};

/* Class 263: ANI-G */
static const struct omci_me_attr attrs_class_263[] = {
	{ "SR indication", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Total T-CONT number", 2, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "GEM block length", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Piggyback DBA reporting", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "Deprecated", 1, OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 5 */
	{ "Signal fail (SF) threshold", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 6 */
	{ "Signal degrade (SD) threshold", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "ARC", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_AVC }, /* 8 */
	{ "ARC interval", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 9 */
	{ "Optical signal level", 2, OMCI_ME_ACC_READ, 0 }, /* 10 */
	{ "Lower optical threshold", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 11 */
	{ "Upper optical threshold", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 12 */
	{ "ONU response time", 2, OMCI_ME_ACC_READ, 0 }, /* 13 */
	{ "Transmit optical level", 2, OMCI_ME_ACC_READ, 0 }, /* 14 */
	{ "Lower transmit power threshold", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 15 */
	{ "Upper transmit power threshold", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 16 */
};

/* Class 264: UNI-G */
static const struct omci_me_attr attrs_class_264[] = {
	{ "Deprecated", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Administrative state", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Management capability", 1, OMCI_ME_ACC_READ, 0 }, /* 3 */
	{ "Non-OMCI management identifier", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 4 */
	{ "Relay agent options", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 5 */
};

/* Class 266: GEM interworking termination point */
static const struct omci_me_attr attrs_class_266[] = {
	{ "GEM port network CTP connectivity pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Interworking option", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Service profile pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Interworking termination point pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "PPTP counter", 1, OMCI_ME_ACC_READ, 0 }, /* 5 */
	{ "Operational state", 1, OMCI_ME_ACC_READ, OMCI_ME_ATTR_AVC }, /* 6 */
	{ "GAL profile pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "GAL loopback configuration", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 8 */
};

/* Class 268: GEM port network CTP */
static const struct omci_me_attr attrs_class_268[] = {
	{ "Port-ID", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "T-CONT pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Direction", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Traffic management pointer for upstream", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "Traffic descriptor profile pointer for upstream", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 5 */
	{ "UNI counter", 1, OMCI_ME_ACC_READ, 0 }, /* 6 */
	{ "Priority queue pointer for downstream", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "Encryption state", 1, OMCI_ME_ACC_READ, 0 }, /* 8 */
	{ "Traffic descriptor profile pointer for downstream", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 9 */
	{ "Encryption key ring", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 10 */
};

/* Class 272: GAL Ethernet profile */
static const struct omci_me_attr attrs_class_272[] = {
	{ "Maximum GEM payload size", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
};

/* Class 277: Priority queue */
static const struct omci_me_attr attrs_class_277[] = {
	{ "Queue configuration option", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Maximum queue size", 2, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Allocated queue size", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Discard-block counter reset interval", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 4 */
	{ "Threshold value for discarded blocks due to buffer overflow", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 5 */
	{ "Related port", 4, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 6 */
	{ "Traffic scheduler pointer", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "Weight", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 8 */
	{ "Back pressure operation", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 9 */
	{ "Back pressure time", 4, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 10 */
	{ "Back pressure occur queue threshold", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 11 */
	{ "Back pressure clear queue threshold", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 12 */
	{ "Packet drop queue thresholds", 8,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 13 */
	{ "Packet drop max_p", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 14 */
	{ "Queue drop w_q", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 15 */
	{ "Drop precedence colour marking", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 16 */
};

/* Class 278: Traffic scheduler */
static const struct omci_me_attr attrs_class_278[] = {
	{ "T-CONT pointer", 2, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Traffic scheduler pointer", 2, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Policy", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Priority/weight", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY }, /* 4 */
};

/* Class 280: Traffic descriptor */
static const struct omci_me_attr attrs_class_280[] = {
	{ "CIR", 4,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 1 */
	{ "PIR", 4,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 2 */
	{ "CBS", 4,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 3 */
	{ "PBS", 4,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 4 */
	{ "Colour mode", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 5 */
	{ "Ingress colour marking", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 6 */
	{ "Egress colour marking", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 7 */
	{ "Meter type", 1, OMCI_ME_ACC_READ | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 8 */
};

/* Class 281: Multicast GEM interworking termination point */
static const struct omci_me_attr attrs_class_281[] = {
	{ "GEM port network CTP connectivity pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Interworking option", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Service profile pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Not used 1", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "PPTP counter", 1, OMCI_ME_ACC_READ, 0 }, /* 5 */
	{ "Operational state", 1, OMCI_ME_ACC_READ, OMCI_ME_ATTR_AVC }, /* 6 */
	{ "GAL profile pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "Not used 2", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 8 */
	{ "IPv4 multicast address table", 12,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_TABLE }, /* 9 */
	{ "IPv6 multicast address table", 24,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, OMCI_ME_ATTR_TABLE }, /* 10 */
};

/* Class 287: OMCI */
static const struct omci_me_attr attrs_class_287[] = {
	{ "ME type table", 2, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_TABLE }, /* 1 */
	{ "Message type table", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_TABLE }, /* 2 */
};

/* Class 309: Multicast operations profile */
static const struct omci_me_attr attrs_class_309[] = {
	{ "IGMP version", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "IGMP function", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Immediate leave", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Upstream IGMP TCI", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 4 */
	{ "Upstream IGMP tag control", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 5 */
	{ "Upstream IGMP rate", 4,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 6 */
	{ "Dynamic access control list table", 24,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_TABLE }, /* 7 */
	{ "Static access control list table", 24,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_TABLE }, /* 8 */
	{ "Lost groups list table", 10, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_TABLE }, /* 9 */
	{ "Robustness", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 10 */
	{ "Querier IP address", 4,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 11 */
	{ "Query interval", 4,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 12 */
	{ "Query max response time", 4,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 13 */
	{ "Last member query interval", 4, OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  0 }, /* 14 */
	{ "Unauthorized join request behaviour", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, 0 }, /* 15 */
	{ "Downstream IGMP and multicast TCI", 3,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 16 */
};

/* Class 310: Multicast subscriber config info */
static const struct omci_me_attr attrs_class_310[] = {
	{ "ME type", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Multicast operations profile pointer", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Max simultaneous groups", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 3 */
	{ "Max multicast bandwidth", 4,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 4 */
	{ "Bandwidth enforcement", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  0 }, /* 5 */
	{ "Multicast service package table", 20,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE, OMCI_ME_ATTR_TABLE }, /* 6 */
	{ "Allowed preview groups table", 22,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_TABLE }, /* 7 */
};

/* Class 311: Multicast subscriber monitor */
static const struct omci_me_attr attrs_class_311[] = {
	{ "ME type", 1,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Current multicast bandwidth", 4, OMCI_ME_ACC_READ, 0 }, /* 2 */
	{ "Join messages counter", 4, OMCI_ME_ACC_READ, 0 }, /* 3 */
	{ "Bandwidth exceeded counter", 4, OMCI_ME_ACC_READ, 0 }, /* 4 */
	{ "IPv4 active group list table", 24, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY | OMCI_ME_ATTR_TABLE }, /* 5 */
	{ "IPv6 active group list table", 58, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_TABLE }, /* 6 */
};

/* Class 321: Ethernet frame performance monitoring history data downstream */
static const struct omci_me_attr attrs_class_321[] = {
	{ "Interval end time", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Threshold data 1/2 ID", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Drop events", 4, OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Octets", 4, OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "Packets", 4, OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 5 */
	{ "Broadcast packets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 6 */
	{ "Multicast packets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "CRC errored packets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 8 */
	{ "Undersize packets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 9 */
	{ "Oversize packets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 10 */
	{ "Packets 64 octets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 11 */
	{ "Packets 65 to 127 octets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 12 */
	{ "Packets 128 to 255 octets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 13 */
	{ "Packets 256 to 511 octets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 14 */
	{ "Packets 512 to 1023 octets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 15 */
	{ "Packets 1024 to 1518 octets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 16 */
};

/* Class 322: Ethernet frame performance monitoring history data upstream */
static const struct omci_me_attr attrs_class_322[] = {
	{ "Interval end time", 1, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 1 */
	{ "Threshold data 1/2 ID", 2,
	  OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE,
	  OMCI_ME_ATTR_MANDATORY }, /* 2 */
	{ "Drop events", 4, OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 3 */
	{ "Octets", 4, OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 4 */
	{ "Packets", 4, OMCI_ME_ACC_READ, OMCI_ME_ATTR_MANDATORY }, /* 5 */
	{ "Broadcast packets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 6 */
	{ "Multicast packets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 7 */
	{ "CRC errored packets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 8 */
	{ "Undersize packets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 9 */
	{ "Oversize packets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 10 */
	{ "Packets 64 octets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 11 */
	{ "Packets 65 to 127 octets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 12 */
	{ "Packets 128 to 255 octets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 13 */
	{ "Packets 256 to 511 octets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 14 */
	{ "Packets 512 to 1023 octets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 15 */
	{ "Packets 1024 to 1518 octets", 4, OMCI_ME_ACC_READ,
	  OMCI_ME_ATTR_MANDATORY }, /* 16 */
};

const struct omci_me_class omci_me_classes[] = {
	{
		.class_id = 2,
		.name = "ONU data",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Set, Get, Get All Alarms, Get All Alarms Next, MIB Upload, MIB Upload Next, MIB Reset */
		.actions = 0x0000fb00u,
		.alarms = 0x00000000u,
		.attr_count = 1,
		.attrs = attrs_class_2,
	},
	{
		.class_id = 5,
		.name = "Cardholder",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Set, Get */
		.actions = 0x00000300u,
		.alarms = 0x0000001fu,
		.attr_count = 9,
		.attrs = attrs_class_5,
	},
	{
		.class_id = 6,
		.name = "Circuit pack",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Create, Delete, Set, Get */
		.actions = 0x00000350u,
		.alarms = 0x0000003fu,
		.attr_count = 14,
		.attrs = attrs_class_6,
	},
	{
		.class_id = 7,
		.name = "Software image",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Get, Start Software Download, Download Section, End Software Download, Activate Software, Commit Software */
		.actions = 0x00f80200u,
		.alarms = 0x00000000u,
		.attr_count = 6,
		.attrs = attrs_class_7,
	},
	{
		.class_id = 11,
		.name = "Physical path termination point Ethernet UNI",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Set, Get */
		.actions = 0x00000300u,
		.alarms = 0x00000001u,
		.attr_count = 15,
		.attrs = attrs_class_11,
	},
	{
		.class_id = 45,
		.name = "MAC bridge service profile",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get */
		.actions = 0x00000350u,
		.alarms = 0x00000000u,
		.attr_count = 10,
		.attrs = attrs_class_45,
	},
	{
		.class_id = 47,
		.name = "MAC bridge port configuration data",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get */
		.actions = 0x00000350u,
		.alarms = 0x00000001u,
		.attr_count = 14,
		.attrs = attrs_class_47,
	},
	{
		.class_id = 84,
		.name = "VLAN tagging filter data",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get */
		.actions = 0x00000350u,
		.alarms = 0x00000000u,
		.attr_count = 3,
		.attrs = attrs_class_84,
	},
	{
		.class_id = 130,
		.name = "IEEE 802.1p mapper service profile",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get */
		.actions = 0x00000350u,
		.alarms = 0x00000000u,
		.attr_count = 13,
		.attrs = attrs_class_130,
	},
	{
		.class_id = 131,
		.name = "OLT-G",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Set, Get */
		.actions = 0x00000300u,
		.alarms = 0x00000000u,
		.attr_count = 4,
		.attrs = attrs_class_131,
	},
	{
		.class_id = 134,
		.name = "IP host config data",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Set, Get, Test */
		.actions = 0x00040300u,
		.alarms = 0x00000000u,
		.attr_count = 16,
		.attrs = attrs_class_134,
	},
	{
		.class_id = 171,
		.name = "Extended VLAN tagging operation configuration data",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get, Get Next, Set Table */
		.actions = 0x24000350u,
		.alarms = 0x00000000u,
		.attr_count = 10,
		.attrs = attrs_class_171,
	},
	{
		.class_id = 256,
		.name = "ONU-G",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Set, Get, Test, Synchronize Time, Reboot */
		.actions = 0x03040300u,
		.alarms = 0x0000ffffu,
		.attr_count = 13,
		.attrs = attrs_class_256,
	},
	{
		.class_id = 257,
		.name = "ONU2-G",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Set, Get */
		.actions = 0x00000300u,
		.alarms = 0x00000000u,
		.attr_count = 14,
		.attrs = attrs_class_257,
	},
	{
		.class_id = 262,
		.name = "T-CONT",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Set, Get */
		.actions = 0x00000300u,
		.alarms = 0x00000000u,
		.attr_count = 3,
		.attrs = attrs_class_262,
	},
	{
		.class_id = 263,
		.name = "ANI-G",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Set, Get, Test */
		.actions = 0x00040300u,
		.alarms = 0x0000007fu,
		.attr_count = 16,
		.attrs = attrs_class_263,
	},
	{
		.class_id = 264,
		.name = "UNI-G",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Set, Get */
		.actions = 0x00000300u,
		.alarms = 0x00000000u,
		.attr_count = 5,
		.attrs = attrs_class_264,
	},
	{
		.class_id = 266,
		.name = "GEM interworking termination point",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get */
		.actions = 0x00000350u,
		.alarms = 0x00000001u,
		.attr_count = 8,
		.attrs = attrs_class_266,
	},
	{
		.class_id = 268,
		.name = "GEM port network CTP",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get */
		.actions = 0x00000350u,
		.alarms = 0x00000020u,
		.attr_count = 10,
		.attrs = attrs_class_268,
	},
	{
		.class_id = 272,
		.name = "GAL Ethernet profile",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get */
		.actions = 0x00000350u,
		.alarms = 0x00000000u,
		.attr_count = 1,
		.attrs = attrs_class_272,
	},
	{
		.class_id = 277,
		.name = "Priority queue",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Set, Get */
		.actions = 0x00000300u,
		.alarms = 0x00000001u,
		.attr_count = 16,
		.attrs = attrs_class_277,
	},
	{
		.class_id = 278,
		.name = "Traffic scheduler",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Set, Get */
		.actions = 0x00000300u,
		.alarms = 0x00000000u,
		.attr_count = 4,
		.attrs = attrs_class_278,
	},
	{
		.class_id = 280,
		.name = "Traffic descriptor",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get */
		.actions = 0x00000350u,
		.alarms = 0x00000000u,
		.attr_count = 8,
		.attrs = attrs_class_280,
	},
	{
		.class_id = 281,
		.name = "Multicast GEM interworking termination point",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get, Get Next, Set Table */
		.actions = 0x24000350u,
		.alarms = 0x00000001u,
		.attr_count = 10,
		.attrs = attrs_class_281,
	},
	{
		.class_id = 287,
		.name = "OMCI",
		.creator = OMCI_ME_CREATED_BY_ONU,
		/* Get, Get Next */
		.actions = 0x04000200u,
		.alarms = 0x00000000u,
		.attr_count = 2,
		.attrs = attrs_class_287,
	},
	{
		.class_id = 309,
		.name = "Multicast operations profile",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get, Get Next, Set Table */
		.actions = 0x24000350u,
		.alarms = 0x00000000u,
		.attr_count = 16,
		.attrs = attrs_class_309,
	},
	{
		.class_id = 310,
		.name = "Multicast subscriber config info",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get, Get Next, Set Table */
		.actions = 0x24000350u,
		.alarms = 0x00000000u,
		.attr_count = 7,
		.attrs = attrs_class_310,
	},
	{
		.class_id = 311,
		.name = "Multicast subscriber monitor",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get, Get Next */
		.actions = 0x04000350u,
		.alarms = 0x00000000u,
		.attr_count = 6,
		.attrs = attrs_class_311,
	},
	{
		.class_id = 321,
		.name = "Ethernet frame performance monitoring history data downstream",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get, Get Current Data */
		.actions = 0x10000350u,
		.alarms = 0x0000000fu,
		.attr_count = 16,
		.attrs = attrs_class_321,
	},
	{
		.class_id = 322,
		.name = "Ethernet frame performance monitoring history data upstream",
		.creator = OMCI_ME_CREATED_BY_OLT,
		/* Create, Delete, Set, Get, Get Current Data */
		.actions = 0x10000350u,
		.alarms = 0x0000000fu,
		.attr_count = 16,
		.attrs = attrs_class_322,
	},
};

const size_t omci_me_class_count = ARRAY_SIZE(omci_me_classes);

const struct omci_me_class *omci_me_class_by_id(uint16_t class_id)
{
	for (size_t i = 0; i < omci_me_class_count; i++)
		if (omci_me_classes[i].class_id == class_id)
			return &omci_me_classes[i];
	return NULL;
}

const struct omci_me_attr *omci_me_attr_get(const struct omci_me_class *cls,
					    uint8_t attr_no)
{
	if (cls == NULL || attr_no == 0u || attr_no > cls->attr_count)
		return NULL;
	return &cls->attrs[attr_no - 1u];
}
