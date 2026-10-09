// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What the figures mean: the state of the fibre, of the stick's own session,
 * of the link to the host. Pages show these; they never read raw keys.
 */

import type { Family, GemPort, Live, Port } from "../api/types";
import type { Lang } from "../i18n";
import { dbm } from "./format";

export type Tone = "good" | "warn" | "bad" | "info" | "neutral";

/* A GPON receiver works from -8 dBm (overload) to -28 dBm (sensitivity). */
export const RX_MAX_DBM = -8;
export const RX_MIN_DBM = -28;
export const RX_WEAK_DBM = -25;

export type LightQuality = "good" | "weak" | "tooWeak" | "tooStrong" | "none";

export function lightQuality(rxDbm: number | null): LightQuality {
  if (rxDbm == null) return "none";
  if (rxDbm > RX_MAX_DBM) return "tooStrong";
  if (rxDbm >= RX_WEAK_DBM) return "good";
  if (rxDbm >= RX_MIN_DBM) return "weak";
  return "tooWeak";
}

export const LIGHT_TONE: Record<LightQuality, Tone> = {
  good: "good",
  weak: "warn",
  tooWeak: "bad",
  tooStrong: "warn",
  none: "bad",
};

export interface FibreView {
  present: boolean;
  state: string;
  stateKey: "o1" | "o2" | "o3" | "o4" | "o5" | "o6" | "o7" | "noPon";
  tone: Tone;
  connected: boolean;
  onuId: number | null;
  alarms: string[];
  rxDbm: number | null;
  txDbm: number | null;
  quality: LightQuality;
  temperature: number | null;
}

const STATE_TONE: Record<string, Tone> = {
  o1: "warn",
  o2: "warn",
  o3: "warn",
  o4: "warn",
  o5: "good",
  o6: "bad",
  o7: "bad",
};

export function fibre(live: Live | undefined): FibreView {
  const onu = live?.onu ?? null;
  const optics = live?.optics ?? null;
  const rxDbm = dbm(optics?.rx_power_uw);
  const state = onu ? String(onu.state).toLowerCase() : "noPon";
  const known = state in STATE_TONE;
  return {
    present: onu !== null,
    state,
    stateKey: (known ? state : "noPon") as FibreView["stateKey"],
    tone: onu ? (STATE_TONE[state] ?? "warn") : "bad",
    connected: state === "o5",
    onuId: state === "o5" && onu ? onu.onu_id : null,
    alarms: onu?.alarms ?? [],
    rxDbm,
    txDbm: dbm(optics?.tx_power_uw),
    quality: lightQuality(rxDbm),
    temperature: optics?.temperature_c ?? null,
  };
}

/** The most PPPoE sessions the stick holds, as many as the switch has rows for. */
export const MAX_SESSIONS = 7;

export interface SessionView {
  id: number;
  user: string;
  secretSet: boolean;
  vlan: string;
  mac: string;
  gem: string;
  family: Family;
  /** The session hands its address down to the host, rather than keep it. */
  pass: boolean;
  /** The VLAN towards the host as set: a VID, "untagged", or "" for the session's own. */
  hostVlan: string;
  handsOn: boolean;
  /** pppoed brought the link up. */
  up: boolean;
  ipv4: string | null;
  ipv6: string | null;
  dev: string | null;
  gemDev: string | null;
  ppp: string | null;
  prefix: string | null;
  prefixHost: string | null;
  /** The gateway the host is given, the concentrator's, while the address is down. */
  gateway: string | null;
  dns: string[];
}

/**
 * The VLAN a session runs in on the fibre: that of its interface (wanVID),
 * which differs from the one it was given when the OLT translates that one,
 * as an operator does from the VLAN a router behind the stick would use.
 */
export function fibreVlan(s: { dev: string | null; vlan: string }): string {
  return /^wan(\d+)$/.exec(s.dev ?? "")?.[1] ?? s.vlan;
}

/**
 * The VLAN a session hands its address down in, towards the host: the one
 * set, else its own, the VID the operator names its service with; "" for
 * none.
 */
export function downVlan(s: Pick<SessionView, "vlan" | "hostVlan">): string {
  return s.hostVlan === "untagged" ? "" : s.hostVlan || s.vlan;
}

export function sessions(live: Live | undefined): SessionView[] {
  return (live?.settings?.sessions ?? []).map((x) => ({
    id: x.id,
    user: x.user,
    secretSet: x.secret_set,
    vlan: x.vlan,
    mac: x.mac,
    gem: x.gem,
    family: x.family,
    pass: x.pass !== "off",
    hostVlan: x.host_vlan ?? "",
    handsOn: x.hands_on,
    up: x.ppp != null,
    ipv4: x.ipv4,
    ipv6: x.ipv6,
    dev: x.dev,
    gemDev: x.gem_dev,
    ppp: x.ppp,
    prefix: x.prefix,
    prefixHost: x.prefix_host,
    gateway: x.gateway ?? null,
    dns: x.dns ? x.dns.split(/\s+/).filter(Boolean) : [],
  }));
}

/*
 * The most connections the stick holds, of every kind together: as many as
 * the switch has PPPoE rows for, within the 8 external addresses of its
 * engine.
 */
export const MAX_CONNS = 7;

export interface IpoeView {
  id: number;
  kind: "static" | "dhcp";
  vlan: string;
  gem: string;
  mac: string;
  family: Family;
  /** As typed, for a static connection. */
  address: string;
  gateway: string;
  dnsSet: string;
  address6: string;
  gateway6: string;
  prefix6: string;
  dev: string | null;
  gemDev: string | null;
  up: boolean;
  ipv4: string | null;
  router: string | null;
  dns: string[];
  ipv6: string | null;
  prefix: string | null;
  lease: number | null;
}

export function ipoes(live: Live | undefined): IpoeView[] {
  return (live?.settings?.ipoe ?? []).map((x) => ({
    id: x.id,
    kind: x.kind,
    vlan: x.vlan,
    gem: x.gem,
    mac: x.mac,
    family: x.family,
    address: x.address,
    gateway: x.gateway,
    dnsSet: x.dns,
    address6: x.address6,
    gateway6: x.gateway6,
    prefix6: x.prefix6,
    dev: x.dev,
    gemDev: x.gem_dev,
    up: x.up,
    ipv4: x.ipv4,
    router: x.router,
    dns: x.dns_now ? x.dns_now.split(/\s+/).filter(Boolean) : [],
    ipv6: x.ipv6,
    prefix: x.prefix,
    lease: x.lease,
  }));
}

/** The stick's sessions in sum: what the host gets from them. */
export interface InternetView {
  /** The stick has accounts: it holds PPPoE sessions. */
  configured: boolean;
  /** Every session is up, and the address handed on is out. */
  up: boolean;
  count: number;
  /** The session that hands its IPv4 address to the host. */
  main: SessionView | null;
  /** The public address handed to the host. */
  address: string | null;
  /** The IPv6 prefix delegated to the host. */
  prefix: string | null;
  dns: string[];
}

export function session(live: Live | undefined): InternetView {
  const s = live?.settings;
  const all = sessions(live);
  const main = all.find((x) => x.handsOn) ?? all[0] ?? null;
  const up = all.length > 0 && all.every((x) => x.up) && (!main?.handsOn || !!s?.pass);
  return {
    configured: all.length > 0,
    up,
    count: all.length,
    main,
    address: main?.handsOn && main.up ? (s?.pass?.address ?? null) : null,
    prefix: all.find((x) => x.prefix && x.prefixHost)?.prefix ?? null,
    dns: s?.dns ? s.dns.split(/\s+/).filter(Boolean) : [],
  };
}

/**
 * What the stick does with a channel: carries its own PPPoE session on it,
 * bridges it at L2 to the SFP port, or ends it on itself.
 */
export type GemUse = "session" | "bridged" | "ends";

export interface GemView {
  gem: GemPort;
  /** The OLT's upstream allocation, for a channel that sends. */
  allocId: number | null;
  use: GemUse;
  /** The VLANs of the sessions it carries. */
  vlan: string;
  /** The bridge, for a channel bridged to the SFP port. */
  bridge: string | null;
}

export function gems(live: Live | undefined): GemView[] {
  const all = sessions(live);
  return (live?.gem_ports ?? [])
    .slice()
    .sort((a, z) => a.port_id - z.port_id)
    .map((gem) => {
      const on = all.filter((x) => x.gemDev === gem.name);
      return {
        gem,
        allocId:
          gem.tcont == null
            ? null
            : (live?.tconts?.find((x) => x.index === gem.tcont)?.alloc_id ?? null),
        use: on.length ? "session" : gem.master ? "bridged" : "ends",
        vlan: [...new Set(on.map(fibreVlan))].join(", "),
        bridge: gem.master,
      };
    });
}

export function port(live: Live | undefined, name: string): Port | undefined {
  return live?.ports?.find((p) => p.name === name);
}

export interface LinkView {
  up: boolean;
  speedMbit: number | null;
  lease: string | null;
  /**
   * The gateway the host has now, while a session hands its address on: the
   * concentrator's address, as the host would have it with a session of its
   * own, which the stick takes on the link itself.
   */
  hostGateway: string | null;
}

export function hostLink(live: Live | undefined): LinkView {
  const sfp = port(live, "sfp");
  const s = live?.settings;
  return {
    up: !!sfp && sfp.carrier === 1 && sfp.operstate === "up",
    speedMbit: sfp && sfp.speed_mbit > 0 ? sfp.speed_mbit : null,
    lease: s?.pass?.address ?? null,
    hostGateway: s?.pass?.peer_gateway ?? null,
  };
}

/** The line rates of GPON (ITU-T G.984.1): 2.48832 Gb/s down, 1.24416 Gb/s up. */
export const GPON_DOWN_BPS = 2_488_320_000;
export const GPON_UP_BPS = 1_244_160_000;

/** The port between the switch and the processor carries at most a gigabit. */
export const CPU_PORT_BPS = 1_000_000_000;

/**
 * The SFP lane's mode, as its speed tells it: 2500BASE-X at 2.5 Gb/s,
 * 1000BASE-X at 1 Gb/s; null without a link.
 */
export function laneMode(live: Live | undefined): string | null {
  const speed = port(live, "sfp")?.speed_mbit;
  return speed === 2500 ? "2500BASE-X" : speed === 1000 ? "1000BASE-X" : null;
}

/** A line rate for people: 2.5 Gb/s (2,5 Gb/s in Polish), 1 Gb/s, 100 Mb/s. */
export function linkSpeed(mbit: number | null, lang: Lang): string | null {
  if (mbit == null) return null;
  const n = new Intl.NumberFormat(lang, { maximumFractionDigits: 1 });
  return mbit >= 1000 ? `${n.format(mbit / 1000)} Gb/s` : `${n.format(mbit)} Mb/s`;
}

export interface MemoryView {
  totalBytes: number;
  usedBytes: number;
  share: number;
}

export function memory(live: Live | undefined): MemoryView | null {
  const m = live?.memory;
  if (!m?.total_kb) return null;
  const used = m.total_kb - m.available_kb;
  return { totalBytes: m.total_kb * 1024, usedBytes: used * 1024, share: used / m.total_kb };
}

/** A network of the stick's towards the devices behind it, out through one connection. */
export interface LanView {
  id: number;
  /** The connection's key, as conns() names it: "pppoe:N" or "ipoe:N". */
  conn: string;
  vlan: string;
  address: string;
  dhcp: boolean;
  poolFrom: string;
  poolTo: string;
  leaseS: number;
}

export function lans(live: Live | undefined): LanView[] {
  return (live?.settings?.lans ?? []).map((x) => ({
    id: x.id,
    conn: x.conn,
    vlan: x.vlan,
    address: x.address,
    dhcp: x.dhcp === "on",
    poolFrom: x.pool_from,
    poolTo: x.pool_to,
    leaseS: x.lease_s,
  }));
}

/* An address as a number, for ordering. */
const order = (ip: string) => ip.split(".").reduce((a, o) => a * 256 + Number(o), 0);

/** The devices behind the stick with an address from it, by their address. */
export function leases(live: Live | undefined) {
  return (live?.settings?.leases ?? []).slice().sort((a, z) => order(a.ip) - order(z.ip));
}

/** The fixed addresses of the devices, by address. */
export function fixed(live: Live | undefined) {
  return (live?.settings?.fixed ?? []).slice().sort((a, z) => order(a.ip) - order(z.ip));
}
