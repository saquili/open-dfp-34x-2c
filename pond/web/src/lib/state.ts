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
    handsOn: x.hands_on,
    up: x.ppp != null,
    ipv4: x.ipv4,
    ipv6: x.ipv6,
    dev: x.dev,
    gemDev: x.gem_dev,
    ppp: x.ppp,
    prefix: x.prefix,
    prefixHost: x.prefix_host,
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
        vlan: [...new Set(on.map((x) => x.vlan))].join(", "),
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
  vlan: string;
  gateway: string;
  lease: string | null;
}

export function hostLink(live: Live | undefined): LinkView {
  const sfp = port(live, "sfp");
  const s = live?.settings;
  return {
    up: !!sfp && sfp.carrier === 1 && sfp.operstate === "up",
    speedMbit: sfp && sfp.speed_mbit > 0 ? sfp.speed_mbit : null,
    vlan: s?.settings.LAN_VLAN ?? "",
    gateway: s?.settings.PASS_GATEWAY ?? "",
    lease: s?.pass?.address ?? null,
  };
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
