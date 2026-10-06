// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The providers: every value the API serves, by group. */

#include "prov.h"
#include "providers.h"

#include <stddef.h>

const struct prov pond_provs[] = {

	{ "uptime", "System", "Uptime and idle time", prov_uptime, NULL, 0 },
	{ "memory", "System", "Memory", prov_memory, NULL, 0 },
	{ "kernel", "System", "Kernel", prov_kernel, NULL, 0 },
	{ "image", "System", "Image", prov_image, NULL, 0 },
	{ "model", "System", "Board", prov_model, NULL, 0 },
	{ "log", "System", "Kernel log", prov_log, NULL, PROV_SLOW },

	{ "settings", "Mode",
	  "Bridge, router or passthrough, the WAN and the LAN, kept in the "
	  "configuration store",
	  prov_settings, prov_settings_set, 0 },

	{ "ports", "Data path", "Interfaces: link, speed and CPU-path counts",
	  prov_ports, NULL, 0 },
	{ "port_counters", "Data path", "Switch MIB counters for sfp and pon",
	  prov_port_counters, NULL, 0 },

	{ "bridge", "Data path",
	  "Forwarding database and per-port VLAN membership", prov_bridge, NULL,
	  PROV_SLOW },
	{ "gem_ports", "Data path",
	  "GEM ports: Port-ID, direction, T-CONT and multicast flag",
	  prov_gem_ports, NULL, 0 },
	{ "tc_rules", "Data path",
	  "Filters installed on the sfp and pon ingress hooks", prov_tc_rules,
	  NULL, 0 },

	{ "banks", "Flash",
	  "Partitions, the U-Boot environment and the two firmware banks",
	  prov_banks, NULL, PROV_SLOW },

	{ "onu", "GPON", "Activation state, identifiers and alarms", prov_onu,
	  NULL, 0 },

	{ "tconts", "GPON", "T-CONTs and the Alloc-IDs the OLT bound to them",
	  prov_tconts, NULL, 0 },

	{ "optics", "Optics",
	  "Temperature, supply, laser bias, Tx and Rx power", prov_optics, NULL,
	  0 },

	{ "gem_counters", "Data path", "Per-GEM-port counters from the PON MAC",
	  prov_gem_counters, NULL, 0 },
	{ "omci", "OMCI", "Session, alarms, managed entities", NULL, NULL,
	  PROV_ABSENT },
};

const size_t pond_nprovs = ARRAY_SIZE(pond_provs);
