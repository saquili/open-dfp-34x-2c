// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What the OLT set up, read from the managed entities omcid holds (pond's
 * value "omci"), and put together the way a person thinks of it: the
 * services, each a VLAN on the fibre with the channels (GEM ports) that
 * carry it, the bandwidth they get, the VLAN the host uses for it, and
 * multicast; the rules the OLT gave for the frames from the host; the unit
 * as the OLT sees it. The attribute layouts are those of ITU-T G.988.
 */

import type { OmciValue, ServiceRule } from "../api/types";

export interface Me {
  cls: number;
  id: number;
  /** The value of each attribute; null for one pond does not give (a password). */
  attrs: Map<number, Uint8Array | null>;
  /** The attributes that are tables: their value is the rows end to end. */
  tables: Set<number>;
  /** omcid could not dump it (2 gone, 3 too big). */
  error?: number;
}

function bytesOf(hex: string): Uint8Array {
  const out = new Uint8Array(hex.length >> 1);
  for (let i = 0; i < out.length; i++) out[i] = Number.parseInt(hex.slice(2 * i, 2 * i + 2), 16);
  return out;
}

export class Mib {
  readonly byClass = new Map<number, Map<number, Me>>();

  constructor(v: OmciValue | null | undefined) {
    for (const m of v?.mes ?? []) {
      const attrs = new Map<number, Uint8Array | null>();
      for (const [k, hex] of Object.entries(m.attrs ?? {}))
        attrs.set(Number(k), hex == null ? null : bytesOf(hex));
      const me: Me = {
        cls: m.class,
        id: m.instance,
        attrs,
        tables: new Set(m.tables ?? []),
        error: m.error,
      };
      let c = this.byClass.get(me.cls);
      if (!c) {
        c = new Map();
        this.byClass.set(me.cls, c);
      }
      c.set(me.id, me);
    }
  }

  get(cls: number, id: number): Me | undefined {
    return this.byClass.get(cls)?.get(id);
  }

  all(cls: number): Me[] {
    return [...(this.byClass.get(cls)?.values() ?? [])].sort((a, z) => a.id - z.id);
  }

  get empty(): boolean {
    return this.byClass.size === 0;
  }
}

/* Numbers and text from an attribute, big-endian as on the wire. */
export function num(me: Me | undefined, attr: number, at = 0, size?: number): number | null {
  const b = me?.attrs.get(attr);
  if (!b) return null;
  const n = size ?? b.length - at;
  if (n <= 0 || at + n > b.length || n > 6) return null;
  let v = 0;
  for (let i = 0; i < n; i++) v = v * 256 + (b[at + i] ?? 0);
  return v;
}

export function text(me: Me | undefined, attr: number): string | null {
  const b = me?.attrs.get(attr);
  if (!b) return null;
  let s = "";
  for (const c of b) {
    if (c === 0) break;
    s += c >= 32 && c < 127 ? String.fromCharCode(c) : "·";
  }
  return s.trim();
}

export function hex(b: Uint8Array | null | undefined): string {
  return b ? [...b].map((x) => x.toString(16).padStart(2, "0")).join("") : "";
}

/** The rows of a table attribute, each @size bytes. */
export function rows(me: Me | undefined, attr: number, size: number): Uint8Array[] {
  const b = me?.attrs.get(attr);
  if (!b) return [];
  const out: Uint8Array[] = [];
  for (let i = 0; i + size <= b.length; i += size) out.push(b.subarray(i, i + size));
  return out;
}

const NONE = new Set([0xffff, 0]);

/* --- The classes, by number. --- */
export const ONU_G = 256;
export const ONU2_G = 257;
export const SW_IMAGE = 7;
export const CIRCUIT_PACK = 6;
export const OLT_G = 131;
export const PPTP_ETH = 11;
export const VEIP = 329;
export const IP_HOST = 134;
export const BRIDGE = 45;
export const BRIDGE_PORT = 47;
export const MAPPER = 130;
export const VLAN_FILTER = 84;
export const EXT_VLAN = 171;
export const TCONT = 262;
export const GEM_CTP = 268;
export const GEM_IW = 266;
export const MCAST_IW = 281;
export const TRAFFIC_DESC = 280;
export const PRIORITY_QUEUE = 277;
export const MCAST_PROFILE = 309;
export const MCAST_SUBSCRIBER = 310;

/** A channel as the OLT set it up. */
export interface Channel {
  /** The GEM port network CTP's instance. */
  ctp: number;
  portId: number;
  direction: "up" | "down" | "both" | null;
  tcont: number | null;
  /** The upstream allocation of its T-CONT; null while the OLT bound none. */
  allocId: number | null;
  encrypted: boolean;
  /** The priorities of the frames from the host that it carries (802.1p mapper). */
  pbits: number[];
  multicast: boolean;
  /** The priority and weight of its upstream queue. */
  queue: { priority: number; weight: number | null } | null;
  /** Its traffic descriptors' rates, bytes per second. */
  up: { cir: number; pir: number } | null;
  down: { cir: number; pir: number } | null;
}

function descriptor(mib: Mib, id: number | null): { cir: number; pir: number } | null {
  if (id == null || NONE.has(id)) return null;
  const td = mib.get(TRAFFIC_DESC, id);
  const cir = num(td, 1);
  const pir = num(td, 2);
  return cir == null && pir == null ? null : { cir: cir ?? 0, pir: pir ?? 0 };
}

export function channel(mib: Mib, ctp: number, pbits: number[] = []): Channel | null {
  const c = mib.get(GEM_CTP, ctp);
  if (!c) return null;
  const tcont = num(c, 2);
  const t = tcont != null ? mib.get(TCONT, tcont) : undefined;
  const alloc = num(t, 1);
  const dir = num(c, 3);
  const q = num(c, 4);
  const pq = q != null && !NONE.has(q) ? mib.get(PRIORITY_QUEUE, q) : undefined;
  return {
    ctp,
    portId: num(c, 1) ?? ctp,
    direction: dir === 1 ? "up" : dir === 2 ? "down" : dir === 3 ? "both" : null,
    tcont: t ? (tcont ?? null) : null,
    allocId: alloc != null && alloc !== 0xff && alloc !== 0xffff ? alloc : null,
    encrypted: (num(c, 8) ?? 0) !== 0,
    pbits,
    multicast: mib.all(MCAST_IW).some((m) => num(m, 1) === ctp),
    queue: pq ? { priority: num(pq, 6, 2, 2) ?? 0, weight: num(pq, 8) } : null,
    up: descriptor(mib, num(c, 5)),
    down: descriptor(mib, num(c, 9)),
  };
}

/** A port of a bridge: the host side (UNI, VEIP, IP host) or the fibre side. */
export interface BridgePort {
  id: number;
  bridge: number;
  side: "host" | "fibre" | "other";
  /** What it is, by the termination point type of G.988 9.3.4. */
  kind: "uni" | "veip" | "iphost" | "mapper" | "gem" | "multicast" | "other";
  /** The instance of what it stands for: of a UNI, slot and port, 0x0101 for port 1. */
  tp: number;
  channels: Channel[];
  /** The VLANs the OLT lets through it; null when it filters none. */
  vlans: number[] | null;
  /** What it does with the rest: forward, or discard frames of other VLANs. */
  forward: number | null;
}

const TP_KIND: Record<number, BridgePort["kind"]> = {
  1: "uni",
  11: "veip",
  4: "iphost",
  3: "mapper",
  5: "gem",
  6: "multicast",
};

export function bridgePorts(mib: Mib): BridgePort[] {
  return mib.all(BRIDGE_PORT).map((p) => {
    const kind = TP_KIND[num(p, 3) ?? -1] ?? "other";
    const tp = num(p, 4) ?? 0;
    const channels: Channel[] = [];
    if (kind === "mapper") {
      const m = mib.get(MAPPER, tp);
      const byCtp = new Map<number, number[]>();
      for (let pbit = 0; pbit < 8; pbit++) {
        const iw = num(m, 2 + pbit);
        if (iw == null || NONE.has(iw)) continue;
        const ctp = num(mib.get(GEM_IW, iw), 1);
        if (ctp == null) continue;
        byCtp.set(ctp, [...(byCtp.get(ctp) ?? []), pbit]);
      }
      for (const [ctp, pbits] of byCtp) {
        const ch = channel(mib, ctp, pbits);
        if (ch) channels.push(ch);
      }
    } else if (kind === "gem" || kind === "multicast") {
      const iw = mib.get(kind === "gem" ? GEM_IW : MCAST_IW, tp);
      const ch = channel(mib, num(iw, 1) ?? -1);
      if (ch) channels.push(ch);
    }
    const f = mib.get(VLAN_FILTER, p.id);
    const count = num(f, 3) ?? 0;
    const list = f?.attrs.get(1);
    const vlans: number[] = [];
    for (let i = 0; i < count && list && 2 * i + 1 < list.length; i++)
      vlans.push((((list[2 * i] ?? 0) << 8) | (list[2 * i + 1] ?? 0)) & 0xfff);
    return {
      id: p.id,
      bridge: num(p, 1) ?? 0,
      side:
        kind === "uni" || kind === "veip" || kind === "iphost"
          ? "host"
          : kind === "other"
            ? "other"
            : "fibre",
      kind,
      tp,
      channels: channels.sort((a, z) => a.portId - z.portId),
      vlans: f ? vlans : null,
      forward: num(f, 2),
    };
  });
}

/* --- Extended VLAN tagging operation (G.988 9.3.13), attribute 6. --- */

export interface Tag {
  /** 0..7, or 8 "any", 14 "default rule", 15 "no such tag". */
  prio: number;
  /** 0..4094, or 4096 "any". */
  vid: number;
  tpid: number;
}

export interface VlanRule {
  outer: Tag;
  inner: Tag;
  ethertype: number;
  /** 0..2 tags taken off, 3 = discard the frame. */
  remove: number;
  /** Treatment: prio 0..7, 8 copy inner, 9 copy outer, 10 from DSCP, 15 no tag; vid 0..4094, 4096 copy inner, 4097 copy outer. */
  addOuter: Tag;
  addInner: Tag;
}

export function vlanRule(r: Uint8Array): VlanRule {
  const w = (i: number) =>
    (((r[i] ?? 0) << 24) | ((r[i + 1] ?? 0) << 16) | ((r[i + 2] ?? 0) << 8) | (r[i + 3] ?? 0)) >>>
    0;
  const w1 = w(0);
  const w2 = w(4);
  const w3 = w(8);
  const w4 = w(12);
  return {
    outer: { prio: w1 >>> 28, vid: (w1 >>> 15) & 0x1fff, tpid: (w1 >>> 12) & 7 },
    inner: { prio: w2 >>> 28, vid: (w2 >>> 15) & 0x1fff, tpid: (w2 >>> 12) & 7 },
    ethertype: w2 & 0xf,
    remove: w3 >>> 30,
    addOuter: { prio: (w3 >>> 16) & 0xf, vid: (w3 >>> 3) & 0x1fff, tpid: w3 & 7 },
    addInner: { prio: (w4 >>> 16) & 0xf, vid: (w4 >>> 3) & 0x1fff, tpid: w4 & 7 },
  };
}

/** How many tags a rule's frames carry: 0, 1, 2. */
export function tagsMatched(r: VlanRule): number {
  return r.outer.prio === 15 ? (r.inner.prio === 15 ? 0 : 1) : 2;
}

/** A default rule: for the frames with that many tags that no other rule took. */
export function isDefault(r: VlanRule): boolean {
  return r.outer.prio === 14 || r.inner.prio === 14;
}

/**
 * One of the three rules every table holds from its start (G.988 9.3.13):
 * for the untagged frames, and the default ones for one tag and for two.
 * The OLT may change what they do, never take them away.
 */
export function isDefaultEntry(r: VlanRule): boolean {
  return isDefault(r) || (tagsMatched(r) === 0 && r.ethertype === 0);
}

export interface VlanRules {
  me: number;
  /** What the rules sit on: 2 a UNI, 10 a VEIP, 0 a bridge port… */
  association: number | null;
  rules: VlanRule[];
}

export function vlanRules(mib: Mib): VlanRules[] {
  return mib.all(EXT_VLAN).map((m) => ({
    me: m.id,
    association: num(m, 1),
    rules: rows(m, 6, 16).map(vlanRule),
  }));
}

/** The VLAN a rule's frames come in with, and the VLAN they go out in; null for none. */
export function translation(r: VlanRule): { from: number | null; to: number | null } | null {
  if (r.remove === 3) return null;
  const tags = tagsMatched(r);
  const from = tags === 0 ? null : tags === 1 ? r.inner.vid : r.outer.vid;
  const added = r.addInner.prio !== 15 ? r.addInner : r.addOuter.prio !== 15 ? r.addOuter : null;
  let to: number | null;
  if (added) to = added.vid === 4096 ? r.inner.vid : added.vid === 4097 ? r.outer.vid : added.vid;
  else to = r.remove >= tags ? null : from;
  return { from: from === 4096 ? null : from, to: to === 4096 ? null : to };
}

/* --- Multicast (G.988 9.3.27, 9.3.28). --- */

export interface Multicast {
  me: number;
  /** 1, 2 or 3; 16 MLDv1, 17 MLDv2. */
  version: number | null;
  /** 0 transparent snooping, 1 snooping with proxy reporting, 2 IGMP proxy. */
  fn: number | null;
  immediateLeave: boolean;
  /** Upstream IGMP messages a second; 0 for no limit. */
  rate: number | null;
  robustness: number | null;
  queryInterval: number | null;
  /** The VLAN the downstream multicast is put in towards the host, if any. */
  downstreamVid: number | null;
  /** The groups the host may join: channel, VLAN, address range. */
  acl: { portId: number; vlan: number; from: string; to: string }[];
}

const ipv4 = (b: Uint8Array, at: number) => [...b.subarray(at, at + 4)].join(".");

export function multicast(mib: Mib): Multicast[] {
  return mib.all(MCAST_PROFILE).map((m) => {
    const tci = m.attrs.get(16);
    return {
      me: m.id,
      version: num(m, 1),
      fn: num(m, 2),
      immediateLeave: (num(m, 3) ?? 0) !== 0,
      rate: num(m, 6),
      robustness: num(m, 10),
      queryInterval: num(m, 12),
      downstreamVid:
        tci && tci.length >= 3 && (tci[0] ?? 0) !== 0
          ? (((tci[1] ?? 0) << 8) | (tci[2] ?? 0)) & 0xfff
          : null,
      acl: rows(m, 7, 24).map((r) => ({
        portId: ((r[2] ?? 0) << 8) | (r[3] ?? 0),
        vlan: (((r[4] ?? 0) << 8) | (r[5] ?? 0)) & 0xfff,
        from: ipv4(r, 10),
        to: ipv4(r, 14),
      })),
    };
  });
}

/* --- Services. --- */

/**
 * Where a service leaves the stick, as the OLT set it up: an Ethernet UNI by
 * its port (PPTP, G.988 9.5.1), or the virtual Ethernet interface point
 * (VEIP, 9.5.5). Each of them is the one SFP lane to the host.
 */
export type Output = { kind: "uni"; port: number } | { kind: "veip" };

/** A service: a VLAN on the fibre, and how it gets to the host. */
export interface Service {
  /** The VLAN on the fibre; null for the ports that filter none. */
  vlan: number | null;
  /** The VLANs the host uses for it, as the OLT's rules translate them. */
  hostVlans: (number | null)[];
  /** The channels that carry it, by bridge port. */
  ports: BridgePort[];
  /** Where it leaves the stick: the UNIs and the VEIP of its bridges. */
  outputs: Output[];
  multicast: Multicast | null;
  /** The stick's rule for it, once the rules are applied (withRules). */
  rule?: ServiceRule;
  /** The VLANs towards the host the OLT gave it, of a service the rule moved. */
  oltHostVlans?: (number | null)[];
  /** Kept off the host by its rule. */
  off?: boolean;
}

/**
 * The services as they reach the host, the stick's rules applied: a moved
 * one in its new VLAN, one kept to an output with that output alone, one
 * kept off marked so.
 */
export function withRules(list: Service[], rules: ServiceRule[]): Service[] {
  return list.map((s) => {
    const rule = rules.find((r) => r.fibre === s.vlan);
    if (!rule) return s;
    switch (rule.action) {
      case "move":
        return { ...s, rule, oltHostVlans: s.hostVlans, hostVlans: [rule.host ?? null] };
      case "off":
        return { ...s, rule, off: true };
      case "only_uni":
        return { ...s, rule, outputs: s.outputs.filter((o) => o.kind === "uni") };
      case "only_veip":
        return { ...s, rule, outputs: s.outputs.filter((o) => o.kind === "veip") };
      default:
        return s;
    }
  });
}

/**
 * Why a service does not stand apart on the SFP lane, which is one for all:
 * it leaves by the UNI and by the VEIP at once, so that the two are one
 * network there; or another service reaches the host with a VLAN of its own.
 */
export type Clash = { kind: "both" } | { kind: "vlan"; vid: number | null; with: number | null };

/** The VLANs by which service @s reaches the host: those the rules make, or its own. */
export const hostSide = (s: Service): (number | null)[] =>
  s.hostVlans.length ? s.hostVlans : [s.vlan];

/** The clashes of each service of @list, by its fibre VLAN. */
export function clashes(all: Service[]): Map<number | null, Clash[]> {
  const out = new Map<number | null, Clash[]>();
  const add = (s: Service, c: Clash) => out.set(s.vlan, [...(out.get(s.vlan) ?? []), c]);
  /* A service kept off the host clashes with none. */
  const list = all.filter((s) => !s.off);
  for (const s of list) {
    if (s.outputs.some((o) => o.kind === "uni") && s.outputs.some((o) => o.kind === "veip"))
      add(s, { kind: "both" });
    for (const vid of hostSide(s))
      for (const o of list)
        if (o !== s && hostSide(o).includes(vid)) add(s, { kind: "vlan", vid, with: o.vlan });
  }
  return out;
}

export function services(mib: Mib): Service[] {
  const all = bridgePorts(mib);
  const ports = all.filter((p) => p.side === "fibre");
  const outputsOf = (list: BridgePort[]): Output[] => {
    const bridges = new Set(list.map((p) => p.bridge));
    const out: Output[] = [];
    for (const p of all)
      if (
        bridges.has(p.bridge) &&
        p.kind === "uni" &&
        !out.some((o) => o.kind === "uni" && o.port === (p.tp & 0xff))
      )
        out.push({ kind: "uni", port: p.tp & 0xff });
      else if (bridges.has(p.bridge) && p.kind === "veip" && !out.some((o) => o.kind === "veip"))
        out.push({ kind: "veip" });
    return out.sort((a, z) => (a.kind === "uni" ? a.port : 99) - (z.kind === "uni" ? z.port : 99));
  };
  const rules = vlanRules(mib).flatMap((r) => r.rules);
  const mc = multicast(mib);
  const byVlan = new Map<number | null, BridgePort[]>();
  for (const p of ports) {
    const keys: (number | null)[] = p.vlans?.length ? p.vlans : [null];
    for (const v of keys) byVlan.set(v, [...(byVlan.get(v) ?? []), p]);
  }
  /* The multicast channel has no VLAN filter of its own: it joins the VLAN its groups are in. */
  for (const m of mc)
    for (const a of m.acl) {
      const list = byVlan.get(a.vlan);
      const loose = byVlan.get(null)?.filter((p) => p.channels.some((c) => c.portId === a.portId));
      if (list && loose?.length) {
        byVlan.set(a.vlan, [...list, ...loose.filter((p) => !list.includes(p))]);
        byVlan.set(
          null,
          (byVlan.get(null) ?? []).filter((p) => !loose.includes(p)),
        );
        if (!byVlan.get(null)?.length) byVlan.delete(null);
      }
    }
  return [...byVlan.entries()]
    .map(([vlan, list]) => {
      const host = new Set<number | null>();
      for (const r of rules) {
        const t = translation(r);
        if (t && vlan != null && t.to === vlan) host.add(t.from);
      }
      return {
        vlan,
        hostVlans: [...host],
        /* Ports that carry several services come last in each. */
        ports: list.sort((a, z) => (a.vlans?.length ?? 0) - (z.vlans?.length ?? 0) || a.id - z.id),
        outputs: outputsOf(list),
        multicast:
          mc.find((m) => m.acl.some((a) => a.vlan === vlan) || m.downstreamVid === vlan) ?? null,
      };
    })
    .sort((a, z) => (a.vlan ?? 1e9) - (z.vlan ?? 1e9));
}

/** The unit as the OLT reads it, and the OLT as it introduced itself. */
export interface Unit {
  /** The serial number: four letters, then eight hexadecimal digits. */
  serial: string | null;
  vendorId: string | null;
  version: string | null;
  equipment: string | null;
  omcc: number | null;
  productCode: number | null;
  tmOption: number | null;
  /** Empty when the ONU-G has none, null when it has no such attribute. */
  loid: string | null;
  /** The version of the stick's circuit packs, one for them all. */
  packVersion: string | null;
  images: {
    id: number;
    version: string | null;
    committed: boolean;
    active: boolean;
    valid: boolean;
  }[];
  olt: { vendorId: string | null; equipment: string | null; version: string | null } | null;
}

export function unit(mib: Mib): Unit {
  const g = mib.get(ONU_G, 0);
  const g2 = mib.get(ONU2_G, 0);
  const olt = mib.all(OLT_G)[0];
  const sn = g?.attrs.get(3);
  return {
    serial:
      sn && sn.length === 8
        ? String.fromCharCode(...sn.subarray(0, 4)) + hex(sn.subarray(4)).toUpperCase()
        : null,
    vendorId: text(g, 1),
    version: text(g, 2),
    equipment: text(g2, 1),
    omcc: num(g2, 2),
    productCode: num(g2, 3),
    tmOption: num(g, 4),
    loid: text(g, 10),
    packVersion: text(mib.all(CIRCUIT_PACK)[0], 4),
    images: mib.all(SW_IMAGE).map((s) => ({
      id: s.id,
      version: text(s, 1),
      committed: num(s, 2) === 1,
      active: num(s, 3) === 1,
      valid: num(s, 4) === 1,
    })),
    olt: olt
      ? {
          vendorId: text(olt, 1) || null,
          equipment: text(olt, 2) || null,
          version: text(olt, 3) || null,
        }
      : null,
  };
}
