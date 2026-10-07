// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The values pond serves, as /api/get/ID and /api/getm?ids=... return them.
 * They are the stick's own figures in its own units (bytes, microwatts,
 * seconds); the panel turns them into words and units for people.
 */

export interface Session {
  user: string;
  must_change: boolean;
  idle_timeout_s: number;
}

export interface Uptime {
  up_s: number;
  idle_s: number;
}

export interface Memory {
  total_kb: number;
  free_kb: number;
  available_kb: number;
}

export interface Image {
  name: string;
  version: string;
  build: string;
  variant: string;
}

export interface PortCpuPath {
  rx_packets: number;
  tx_packets: number;
  rx_bytes: number;
  tx_bytes: number;
  rx_missed: number;
}

export interface Port {
  name: string;
  admin_up: boolean;
  carrier: number;
  operstate: string;
  speed_mbit: number;
  mtu: number;
  mac: string;
  cpu_path: PortCpuPath;
}

/** The switch's own counters of a port. Octets are what the wire carried. */
export interface PortCounters {
  rx_octets: number;
  tx_octets: number;
  rx_unicast: number;
  tx_unicast: number;
  rx_multicast: number;
  tx_multicast: number;
  rx_broadcast: number;
  tx_broadcast: number;
  rx_crc_errors: number;
  rx_drops: number;
  rx_symbol_errors: number;
  [key: string]: number;
}

export interface GemPort {
  name: string;
  ifindex: number;
  master: string | null;
  port_id: number;
  direction: "upstream" | "downstream" | "both";
  direction_bits: number;
  tcont: number | null;
  multicast: boolean;
}

export interface GemCounters {
  pon_tx_octets: number;
  pon_tx_frames: number;
  pon_rx_frames: number;
  pon_rx_octets: number;
}

export interface OnuCounters {
  sn_requests: number;
  ranging_requests: number;
  ploam_rx: number;
  ploam_crc_errors: number;
  ploam_overflows: number;
  bwmap_rx: number;
  bwmap_crc_errors: number;
  bwmap_overflows: number;
  gem_frames: number;
  bip_errors: number;
  fec_corrected_bytes: number;
  fec_corrected_codewords: number;
  fec_uncorrectable_codewords: number;
  fec_codewords: number;
}

export type OnuState = "o1" | "o2" | "o3" | "o4" | "o5" | "o6" | "o7";

export interface Onu {
  id: number;
  num_gem_ports: number;
  num_tconts: number;
  interface: string;
  type: string;
  state: OnuState | string;
  onu_id: number;
  alloc_ids: number[];
  omcc_port_id: number;
  alarms: string[];
  counters: OnuCounters;
  serial: string;
}

export interface Tcont {
  onu: number;
  index: number;
  /** null while the OLT has bound no Alloc-ID to this T-CONT. */
  alloc_id: number | null;
}

export interface Optics {
  temperature_c: number;
  vcc_v: number;
  bias_ma: number;
  tx_power_uw: number;
  rx_power_uw: number;
}

export type Family = "ipv4" | "ipv6" | "both";

/** A PPPoE session of the stick's own, and what onu-mode brought up for it. */
export interface SessionInfo {
  id: number;
  user: string;
  secret_set: boolean;
  /** The GEM port's Port-ID, or "auto". */
  gem: string;
  vlan: string;
  mac: string;
  family: Family;
  /** The session whose IPv4 address goes to the host. */
  hands_on: boolean;
  dev: string | null;
  gem_dev: string | null;
  ppp: string | null;
  ipv4: string | null;
  ipv6: string | null;
  /** The IPv6 prefix delegated to the session, and the host it went on to. */
  prefix: string | null;
  prefix_host: string | null;
}

export interface Settings {
  settings: Record<string, string>;
  sessions: SessionInfo[];
  mode: string | null;
  /** The bench's router mode: its WAN. */
  wan: { gem: string; dev: string; out: string } | null;
  dns: string | null;
  pass: { address: string; lan: string | null } | null;
  store: "loaded" | "defaults" | "saved" | "failed" | "none" | null;
  applying: boolean;
  last_apply_status: number | null;
}

export interface Banks {
  partitions: {
    name: string;
    mtd: number;
    size_b: number;
    type: string;
    type_code: number;
    writable: boolean;
  }[];
  selection: {
    active_bank: number | null;
    commit_bank: number | null;
    trial_bank: number | null;
    next_boot_bank: number | null;
    raw: Record<string, string>;
  };
  banks: { bank: number; kernel_part: string; rootfs_part: string; version_recorded: string }[];
  rootfs: { root: string; mtd: number | null; partition: string | null; bank: number | null };
}

/** The values the panel polls together, every few seconds. */
export interface Live {
  uptime: Uptime;
  memory: Memory;
  image: Image;
  model: string;
  kernel: string;
  ports: Port[];
  port_counters: { sfp: PortCounters; pon: PortCounters };
  gem_ports: GemPort[];
  gem_counters: Record<string, GemCounters>;
  onu: Onu | null;
  tconts: Tcont[];
  optics: Optics | null;
  settings: Settings;
}

export const LIVE_IDS: (keyof Live)[] = [
  "uptime",
  "memory",
  "image",
  "model",
  "kernel",
  "ports",
  "port_counters",
  "gem_ports",
  "gem_counters",
  "onu",
  "tconts",
  "optics",
  "settings",
];

/**
 * How the stick introduces itself to the OLT. The PLOAM password is never
 * served, only whether one is set. source says where it comes from: the
 * stick's own store, or the shipped firmware's configuration.
 */
export interface Identity {
  serial: string;
  ploam_set: boolean;
  ploam_format: string;
  vendor_id: string;
  equipment_id: string;
  version: string;
  equipment_serial: string;
  model: string;
  omcc_version: string;
  vendor_product_code: string;
  tm_option: string;
  source: "store" | "factory" | null;
}

export interface LinkAddress {
  family: "ipv4" | "ipv6";
  address: string;
  link_local: boolean;
}

/** One network interface: a switch port, a VLAN, a GEM port, a bridge, a PPP link. */
export interface Link {
  name: string;
  ifindex: number;
  /** "dsa" for a switch port, "vlan", "gem", "bridge", "ppp"; null for the CPU port. */
  kind: string | null;
  /** The device it runs over: a VLAN's port, a switch port's conduit. */
  lower: string | null;
  vlan: number | null;
  master: string | null;
  operstate: "up" | "down" | "unknown";
  admin_up: boolean;
  mtu: number | null;
  mac: string | null;
  addresses: LinkAddress[];
}

export interface Route {
  family: "ipv4" | "ipv6";
  /** "default", or an address with its prefix length. */
  destination: string;
  gateway: string | null;
  dev: string | null;
  protocol: "kernel" | "static" | "ra" | "dhcp" | "other";
  metric: number;
  offloaded: boolean;
}

export interface Neighbour {
  family: "ipv4" | "ipv6";
  address: string;
  mac: string | null;
  dev: string | null;
  state: "permanent" | "static" | "reachable" | "stale" | "incomplete" | "failed" | "none";
}

export interface Network {
  links: Link[];
  routes: Route[];
  neighbours: Neighbour[];
}

export interface FdbEntry {
  dev: string | null;
  ifindex: number;
  addr: string | null;
  vlan: number | null;
  master: string | null;
  state: "permanent" | "static" | "stale" | "reachable" | "none";
  learned: boolean;
  self: boolean;
  offloaded: boolean;
  ext_learned: boolean;
  sticky: boolean;
}

export interface BridgeVlan {
  dev: string;
  vlan: number;
  pvid: boolean;
  egress_untagged: boolean;
  range_begin: boolean;
  range_end: boolean;
}

export interface Bridge {
  fdb: FdbEntry[];
  vlans: BridgeVlan[];
}

export interface TcAction {
  kind: string;
  order?: number;
  options_decoded?: boolean;
  vlan_action?: "pop" | "push" | "modify" | "pop_eth" | "push_eth" | "unknown";
  vlan_id?: number | null;
  vlan_prio?: number | null;
}

export interface TcRule {
  handle: number;
  priority: number;
  ethertype: number;
  kind: string;
  chain: number | null;
  match?: Partial<{
    indev: string;
    eth_dst: string;
    eth_src: string;
    ethertype: number;
    ip_proto: number;
    ipv4_src: string;
    ipv4_src_mask: string;
    ipv4_dst: string;
    ipv4_dst_mask: string;
    vlan_id: number;
    vlan_prio: number | null;
    vlan_ethtype: number;
  }>;
  actions?: TcAction[];
  offload?: { skip_sw: boolean; skip_hw: boolean; in_hw: boolean; not_in_hw: boolean };
}

/** The rules on each port's way in: a list, or why it could not be read. */
export type TcRules = Record<string, { ingress: TcRule[] } | { error: string }>;
