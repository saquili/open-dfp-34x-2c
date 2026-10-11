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
  /** Absent from a stick whose pond predates them. */
  buffers_kb?: number;
  cached_kb?: number;
}

/** The processor: clock ticks in each state since the start, the load, the model. */
export interface Cpu {
  ticks: {
    user: number;
    nice: number;
    system: number;
    idle: number;
    iowait: number;
    irq: number;
    softirq: number;
    steal: number;
  };
  load: [number, number, number];
  running: number;
  threads: number;
  model: string;
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

/** A PPPoE session of the stick's own, and what onu-apply brought up for it. */
export interface SessionInfo {
  id: number;
  user: string;
  secret_set: boolean;
  /** The GEM port's Port-ID, or "auto". */
  gem: string;
  vlan: string;
  mac: string;
  family: Family;
  /** Whether the session hands its address down to the host ("on") or keeps it. */
  pass: "on" | "off";
  /** The VLAN towards the host: a VID, "untagged", or "" for the session's own. */
  host_vlan: string;
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
  /** The link it hands down by, the gateway it gives there, its name servers. */
  lan?: string | null;
  gateway?: string | null;
  dns?: string | null;
}

/**
 * A connection that takes its address on a service as a router does: typed
 * in (static) or from the operator's DHCP. The stick keeps the address and
 * is the router of what stands behind it.
 */
export interface IpoeInfo {
  id: number;
  kind: "static" | "dhcp";
  /** The service's VID, as a session's, and its GEM port's Port-ID or "auto". */
  vlan: string;
  gem: string;
  mac: string;
  /** The families DHCP asks for; a static connection has those it is given. */
  family: Family;
  /** As typed, for a static connection: IPv4 with its length, its gateway, the name servers, and IPv6. */
  address: string;
  gateway: string;
  dns: string;
  address6: string;
  gateway6: string;
  prefix6: string;
  /** What is in effect: the interface, the addresses with their lengths, the gateway, the name servers. */
  dev: string | null;
  gem_dev: string | null;
  up: boolean;
  ipv4: string | null;
  router: string | null;
  dns_now: string | null;
  ipv6: string | null;
  /** The IPv6 prefix for the network behind the stick: delegated, or typed. */
  prefix: string | null;
  /** Seconds left of the DHCP lease. */
  lease: number | null;
}

/**
 * A network of the stick's towards the devices behind it, out through one
 * connection that makes the stick their router.
 */
export interface LanInfo {
  id: number;
  /** Its name, given by hand ("Dom", "Goście"); none from a stick of before. */
  name?: string;
  /** The connection it goes out through: "pppoe:N" for a session, "ipoe:N" for the others. */
  conn: string;
  /** The VID it goes in to the SFP port; "" for none. */
  vlan: string;
  /** The stick's address in it, with its length. */
  address: string;
  /** Its DHCP server, and the addresses it gives, first and last, for how long, seconds. */
  dhcp: "on" | "off";
  pool_from: string;
  pool_to: string;
  lease_s: number;
  /**
   * IPv6 for its devices, by SLAAC: the /64 network @ipv6_subnet (hex) of
   * the prefix its connection has from the operator. None from a stick of
   * before.
   */
  ipv6?: "on" | "off";
  ipv6_subnet?: string;
}

/** A device behind the stick that has an address from its DHCP server. */
export interface LeaseInfo {
  /** The network it is in. */
  lan: number;
  mac: string;
  ip: string;
  /** The name it gave itself, if any. */
  name: string | null;
  /** Seconds left of its lease. */
  left_s: number;
  /** The global IPv6 addresses the stick sees it use (its neighbours), the ones it made by SLAAC. */
  ipv6?: string[];
}

/**
 * A port forwarded from the internet, by the NAT of a router connection, to
 * a device of its local network: by its MAC, whose fixed address it goes to.
 */
export interface ForwardInfo {
  id: number;
  /** A name for it given by hand; empty for none. */
  name: string;
  /** The connection it comes in by: "pppoe:N" or "ipoe:N". */
  conn: string;
  proto: "tcp" | "udp" | "both";
  /** The ports it comes in on, first and last (the same for one). */
  ext_from: number;
  ext_to: number;
  /** The device: its network and MAC, and the first of the ports it goes to. */
  lan: number;
  mac: string;
  int_port: number;
  on: boolean;
}

/**
 * A firewall rule: what it lets through or blocks, from where to where.
 * The first rule that traffic matches decides; what none matches, the
 * defaults (FwDefaults) do.
 */
export interface RuleInfo {
  id: number;
  /** A name for it given by hand; empty for none. */
  name: string;
  action: "allow" | "block";
  /**
   * Where it comes from and goes to: "internet", or a part of it as
   * "internet:ADDR/LEN"; "lans", every local network; "lan:N", one; or
   * "dev:N:MAC", a device of network N by its MAC.
   */
  from: string;
  to: string;
  proto: "any" | "tcp" | "udp" | "both" | "icmp";
  /** The ports it goes to, of TCP or UDP: "" for every one, "443", "8000-8100". */
  ports: string;
  family: "any" | "ipv4" | "ipv6";
  on: boolean;
}

/**
 * What the firewall does where no rule decides: from the local networks to
 * the internet, and between them. From the internet in it always blocks,
 * but for the forwards and the DMZ (NAT).
 */
export interface FwDefaults {
  out: "allow" | "block";
  between: "allow" | "block";
}

/** A connection's DMZ: the device that takes what comes in that no forward takes. */
export interface DmzInfo {
  conn: string;
  lan: number;
  mac: string;
}

/** A device's fixed address: in its network, its MAC always gets the same IP. */
export interface FixedInfo {
  /** The network it is in. */
  lan: number;
  mac: string;
  ip: string;
  /** A name for it given by hand, which the panel shows before its own; empty for none. */
  name: string;
}

/** A change on trial (ADR 0014). */
export interface Trial {
  name: "settings" | "lane" | "mgmt";
  until_s: number;
  /** The management pair's new address, with its prefix. */
  address?: string;
}

export interface Settings {
  settings: Record<string, string>;
  sessions: SessionInfo[];
  /** The connections that take their address as a router does; absent on a stick of before. */
  ipoe?: IpoeInfo[];
  /** The networks behind the stick, and the devices in them; absent on a stick of before. */
  lans?: LanInfo[];
  leases?: LeaseInfo[];
  fixed?: FixedInfo[];
  forwards?: ForwardInfo[];
  dmz?: DmzInfo[];
  /** The firewall's rules, in their order. */
  rules?: RuleInfo[];
  fw?: FwDefaults;
  mode: string | null;
  /** The bench's router mode: its WAN. */
  wan: { gem: string; dev: string; out: string } | null;
  dns: string | null;
  /** peer_gateway: the concentrator's address, when the router has it as its gateway. */
  pass: { address: string; lan: string | null; peer_gateway?: string | null } | null;
  store: "loaded" | "defaults" | "saved" | "failed" | "none" | null;
  /**
   * The changes on trial: each goes back at until_s, in seconds of the
   * stick's uptime, unless the panel confirms it; the management pair's
   * only from a panel opened at its new address. Absent on a stick of before.
   */
  trials?: Trial[];
  applying: boolean;
  last_apply_status: number | null;
}

/** The panel's HTTPS: the certificate of each address it answers on (pond's "tls"). */
export interface Tls {
  https: boolean;
  certificates: {
    address: string;
    port: number;
    /** The SHA-256 of the certificate, as browsers show it: "AB:CD:...". */
    sha256: string;
    not_before: string;
    not_after: string;
  }[];
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

/** A bank of the flash as onu-update tells it. */
export interface FirmwareBank {
  bank: number;
  /** valid, incomplete (a write cut short) or empty */
  state: string | null;
  version: string | null;
  /** the time of its kernel image, in seconds since 1970 */
  time: number | null;
  kernel_size_b: number | null;
  rootfs_size_b: number | null;
  /** What its kernel image and root filesystem use; absent from an older stick. */
  kernel_used_b?: number | null;
  rootfs_used_b?: number | null;
}

/** The two banks, the starts, and an upload under way (pond's value "firmware"). */
export interface Firmware {
  running: number | null;
  committed: number | null;
  next: number | null;
  /** the bank that runs on trial, not committed yet */
  trial: number | null;
  /** a bank whose trial did not come up */
  failed: number | null;
  banks: FirmwareBank[];
  update: {
    state: "idle" | "receiving" | "writing" | "done" | "failed";
    received_b: number;
    expected_b: number;
    bank: number | null;
    version: string | null;
    message: string | null;
  };
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
  /** null from a stick whose pond does not serve them yet. */
  cpu: Cpu | null;
  hostname: string | null;
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
  "cpu",
  "hostname",
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
  /** Absent from a stick whose pond predates them. */
  loid?: string;
  loid_password_set?: boolean;
  sw_version?: string;
  sw_version0?: string;
  sw_version1?: string;
  pack_version?: string;
  source: "store" | "factory" | null;
}

/** The modes of the SFP lane: the driver's, and the chip's that are to come. */
export type LaneMode = "auto" | "2500basex" | "1000basex-an" | "1000basex" | "sgmii" | "hsgmii";

/**
 * The SFP lane (pond's value "sfp_lane"): the mode chosen, the one the lane
 * runs in now (null while it looks for the host), and a change on trial,
 * which the stick undoes at until_s (seconds of uptime) unless it is kept.
 */
export interface SfpLane {
  choice: LaneMode;
  active: LaneMode | null;
  trial: { mode: LaneMode; previous: LaneMode; until_s: number } | null;
}

/**
 * The management VLAN and address (pond's value "mgmt"), and a new pair
 * added beside them that waits to be confirmed from a panel opened at it.
 */
export interface Mgmt {
  vlan: number | null;
  address: string | null;
  /** An address on the SFP port itself, without a VLAN; empty in production. */
  data_address: string | null;
  pending: { vlan: number; address: string; until_s: number } | null;
}

/**
 * What the OLT set up, as omcid holds it: each instance of the classes pond
 * reads, every attribute in hexadecimal (null for a password), and which of
 * them are tables, whose value is the rows end to end.
 */
export interface OmciValue {
  mes: {
    class: number;
    instance: number;
    attrs?: Record<string, string | null>;
    tables?: number[];
    /** omcid could not dump it. */
    error?: number;
  }[];
  truncated: boolean;
  logical_password_set: boolean;
  frames: { handled: number; rejected: number; answered: number } | null;
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
