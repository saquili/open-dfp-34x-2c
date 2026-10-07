// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What the figures mean: the state of the fibre, of the stick's own session,
 * of the link to the router. Pages show these; they never read raw keys.
 */

import type { GemPort, Live, Port } from "../api/types";
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

export interface SessionView {
  /** An account is set: the stick holds a PPPoE session. */
  configured: boolean;
  up: boolean;
  user: string;
  secretSet: boolean;
  /** The public address handed to the router. */
  address: string | null;
  dns: string[];
  vlan: string;
  mac: string;
}

export function session(live: Live | undefined): SessionView {
  const s = live?.settings;
  const user = s?.pppoe.user ?? "";
  const configured = user !== "";
  const up = configured && !!s?.wan?.out?.startsWith("ppp") && !!s?.pass;
  return {
    configured,
    up,
    user,
    secretSet: s?.pppoe.secret_set ?? false,
    address: up ? (s?.pass?.address ?? null) : null,
    dns: s?.dns ? s.dns.split(/\s+/).filter(Boolean) : [],
    vlan: s?.settings.WAN_VLAN ?? "",
    mac: s?.settings.WAN_MAC ?? "",
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
  /** The session's VLAN, for a channel that carries it. */
  vlan: string;
  /** The bridge, for a channel bridged to the SFP port. */
  bridge: string | null;
}

export function gems(live: Live | undefined): GemView[] {
  const s = live?.settings;
  return (live?.gem_ports ?? [])
    .slice()
    .sort((a, z) => a.port_id - z.port_id)
    .map((gem) => ({
      gem,
      allocId:
        gem.tcont == null
          ? null
          : (live?.tconts?.find((x) => x.index === gem.tcont)?.alloc_id ?? null),
      use: s?.wan?.gem === gem.name ? "session" : gem.master ? "bridged" : "ends",
      vlan: s?.settings.WAN_VLAN ?? "",
      bridge: gem.master,
    }));
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

export function routerLink(live: Live | undefined): LinkView {
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
