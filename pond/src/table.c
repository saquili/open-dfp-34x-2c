// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The providers: every value the API serves, by group. */

#include "prov.h"
#include "providers.h"

#include <stddef.h>

const struct prov pond_provs[] = {

	{ "uptime", "System", "Uptime and idle time", prov_uptime, NULL, 0 },
	{ "memory", "System", "Memory", prov_memory, NULL, 0 },
	{ "cpu", "System", "Processor: time in each state, load, model",
	  prov_cpu, NULL, PROV_VOLATILE },
	{ "hostname", "System", "The name the stick goes by", prov_hostname,
	  prov_hostname_write, 0 },
	{ "product", "System",
	  "The name the panel shows for the system, which the files it hands "
	  "out are named after", prov_product, prov_product_write, 0 },
	{ "clock", "System",
	  "The stick's clock: its time, the source that sets it and how near "
	  "it is, and what NTP, PTP and the OLT gave",
	  prov_clock, NULL, PROV_VOLATILE },
	{ "sfp_lane", "System",
	  "The SFP lane's setting, the one it runs in, and a change on trial",
	  prov_sfp_lane, prov_sfp_lane_write, PROV_VOLATILE },
	{ "mgmt", "System",
	  "The management VLAN and address, and a new pair that waits to be "
	  "confirmed from it", prov_mgmt, NULL, PROV_VOLATILE },
	{ "tls", "System",
	  "The panel's HTTPS: an own certificate, and the certificate of each "
	  "address it answers on, by its SHA-256", prov_tls, NULL, 0 },
	{ "ssh", "System",
	  "The ssh server: its host key, the public keys root logs in with, and "
	  "whether it takes them alone", prov_ssh, NULL, 0 },
	{ "kernel", "System", "Kernel", prov_kernel, NULL, 0 },
	{ "image", "System", "Image", prov_image, NULL, 0 },
	{ "model", "System", "Board", prov_model, NULL, 0 },
	{ "log", "System", "Kernel log", prov_log, NULL, PROV_SLOW },

	{ "identity", "GPON", "How the stick introduces itself to the OLT",
	  prov_identity, prov_identity_write, 0 },
	{ "session", "Mode", "A PPPoE session of the stick's own: add, change or remove",
	  NULL, prov_session_write, 0 },
	{ "ipoe", "Mode",
	  "A connection with an address typed in or from DHCP: add, change or remove",
	  NULL, prov_ipoe_write, 0 },
	{ "lan", "Mode",
	  "A local network the stick routes out by a connection: add, change or remove",
	  NULL, prov_lan_write, 0 },
	{ "fixed", "Mode",
	  "A device's fixed address in a local network: add, change or remove",
	  NULL, prov_fixed_write, 0 },
	{ "leases", "Mode",
	  "The devices that have an address from a local network's DHCP server, "
	  "with the time each has left", prov_leases, NULL, PROV_VOLATILE },
	{ "forward", "Mode",
	  "A port forwarded from the internet to a device of a local network: "
	  "add, change or remove", NULL, prov_forward_write, 0 },
	{ "dmz", "Mode",
	  "A connection's DMZ, the device that takes what no forward takes: "
	  "set or remove", NULL, prov_dmz_write, 0 },
	{ "rule", "Mode",
	  "A rule of the firewall: add, change, remove or move one place", NULL,
	  prov_rule_write, 0 },
	{ "fw", "Mode",
	  "What the firewall does where no rule decides: to the internet and "
	  "between the local networks; and whether the stick answers a ping from "
	  "the internet and from the local networks", NULL, prov_fw_write, 0 },
	{ "settings", "Mode",
	  "The data path: the sessions and where they hand their addresses down, "
	  "kept in the configuration store",
	  prov_settings, prov_settings_write, 0 },

	{ "ports", "Data path", "Interfaces: link, speed and CPU-path counts",
	  prov_ports, NULL, 0 },
	{ "port_counters", "Data path", "Switch MIB counters for sfp and pon",
	  prov_port_counters, NULL, 0 },

	{ "network", "Data path",
	  "Interfaces with their VLANs and addresses, routes and neighbours",
	  prov_network, NULL, PROV_SLOW },
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
	{ "firmware", "Flash",
	  "The two banks, the one that runs, the next start and an update",
	  prov_firmware, NULL, PROV_VOLATILE },

	{ "onu", "GPON", "Activation state, identifiers and alarms", prov_onu,
	  NULL, 0 },

	{ "tconts", "GPON", "T-CONTs and the Alloc-IDs the OLT bound to them",
	  prov_tconts, NULL, 0 },

	{ "optics", "Optics",
	  "Temperature, supply, laser bias, Tx and Rx power", prov_optics, NULL,
	  0 },

	{ "gem_counters", "Data path", "Per-GEM-port counters from the PON MAC",
	  prov_gem_counters, NULL, 0 },
	{ "service_rules", "GPON",
	  "The stick's rules for the OLT's services: moved to another VLAN, off, "
	  "or kept to one output; a person's, and those it made for clashes",
	  prov_service_rules, NULL, PROV_VOLATILE },
	{ "service_rule", "Mode",
	  "A rule of a service the OLT set up: moved, off, one output alone, or "
	  "the stick's own way", NULL, prov_service_rule_write, 0 },
	{ "omci", "OMCI",
	  "What the OLT set up: the managed entities omcid holds, and its "
	  "counts of the frames", prov_omci, NULL, PROV_SLOW },
	{ "omci_frames", "OMCI",
	  "The conversation with the OLT: the last frames omcid received and "
	  "sent, with their times, from the number since=N on",
	  prov_omci_frames, NULL, PROV_SLOW | PROV_VOLATILE },
};

const size_t pond_nprovs = ARRAY_SIZE(pond_provs);

const struct prov_from pond_provs_from[] = {
	{ "omci_frames", prov_omci_frames_from },
};

const size_t pond_nprovs_from = ARRAY_SIZE(pond_provs_from);
