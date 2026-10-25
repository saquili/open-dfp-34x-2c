// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What the television page makes of the multicast agent's lists: the
 * addresses as people write them, the groups that are no channels, and the
 * same event or refusal that came again and again, as one row with its count.
 */

import type { Iptv } from "../api/types";
import { ip6, ip6s } from "./ipv6";

/** An address as it is written: an IPv6 one shortened (RFC 5952), an IPv4 one as it came. */
export function shortAddr(a: string): string {
  if (!a.includes(":")) return a;
  const n = ip6(a);
  return n == null ? a : ip6s(n);
}

/**
 * Whether a group never leaves its link: 224.0.0.0/24 of IPv4 and the
 * interface and link scopes of IPv6 (RFC 5771, 4291). They are the routers'
 * and the hosts' own, not channels that anyone watches.
 */
export function linkLocal(group: string): boolean {
  if (!group.includes(":")) return /^224\.0\.0\.\d{1,3}$/.test(group);
  const n = ip6(group);
  if (n == null) return false;
  const first = Number((n >> 112n) & 0xffffn);
  const scope = first & 0xf;
  return first >> 8 === 0xff && (scope === 1 || scope === 2);
}

/** What a group of the link is for, which says why nobody asks the stick for it as a channel. */
export type LinkKind =
  | "neighbours"
  | "all_nodes"
  | "all_routers"
  | "reports"
  | "discovery"
  | "routing"
  | "dhcp"
  | "other";

/* The solicited-node groups of IPv6 (RFC 4291, 2.7.1): ff02::1:ff00:0/104, one for each address of a host. */
const SOLICITED = (ip6("ff02::1:ff00:0") ?? 0n) >> 24n;
const LOW_112 = (1n << 112n) - 1n;

/* The IPv6 groups of the link that have a use of their own, by their last 112 bits, scope apart. */
const LINK6 = new Map<bigint, LinkKind>([
  [0x1n, "all_nodes"],
  [0x2n, "all_routers"],
  [0x5n, "routing"] /* OSPFv3 routers */,
  [0x6n, "routing"] /* OSPFv3 designated routers */,
  [0x9n, "routing"] /* RIPng */,
  [0xdn, "routing"] /* PIM */,
  [0x12n, "routing"] /* VRRP */,
  [0xcn, "discovery"] /* SSDP */,
  [0xfbn, "discovery"] /* mDNS */,
  [0x10003n, "discovery"] /* LLMNR */,
  [0x16n, "reports"] /* MLDv2 */,
  [0x10002n, "dhcp"] /* DHCPv6 servers and relay agents */,
]);

const LINK4 = new Map<string, LinkKind>([
  ["224.0.0.1", "all_nodes"],
  ["224.0.0.2", "all_routers"],
  ["224.0.0.5", "routing"] /* OSPF routers */,
  ["224.0.0.6", "routing"] /* OSPF designated routers */,
  ["224.0.0.9", "routing"] /* RIPv2 */,
  ["224.0.0.13", "routing"] /* PIM */,
  ["224.0.0.18", "routing"] /* VRRP */,
  ["224.0.0.22", "reports"] /* IGMPv3 */,
  ["224.0.0.251", "discovery"] /* mDNS */,
  ["224.0.0.252", "discovery"] /* LLMNR */,
]);

/** What a group of the link is for, or null for a group that is no group of the link. */
export function linkKind(group: string): LinkKind | null {
  if (!linkLocal(group)) return null;
  if (!group.includes(":")) return LINK4.get(group) ?? "other";
  const n = ip6(group);
  if (n == null) return null;
  if (n >> 24n === SOLICITED) return "neighbours";
  return LINK6.get(n & LOW_112) ?? "other";
}

export interface RefusedRow {
  group: string;
  vlan: number | null;
  reason: Iptv["rejected"][number]["reason"];
  count: number;
  /** The stick's clock at the latest of them, seconds. */
  lastAt: number;
  /** Who asked, each once, shortened. */
  clients: string[];
}

/** The refusals of one group, in one VLAN, for one reason, as a row; the latest first. */
export function refusedRows(list: Iptv["rejected"]): RefusedRow[] {
  const rows = new Map<string, RefusedRow>();
  for (const r of list) {
    const key = `${r.group}/${r.vlan ?? ""}/${r.reason}`;
    const row = rows.get(key) ?? {
      group: r.group,
      vlan: r.vlan ?? null,
      reason: r.reason,
      count: 0,
      lastAt: r.at_s,
      clients: [],
    };
    row.count++;
    row.lastAt = Math.max(row.lastAt, r.at_s);
    if (r.client != null && !row.clients.includes(shortAddr(r.client)))
      row.clients.push(shortAddr(r.client));
    rows.set(key, row);
  }
  return [...rows.values()].sort((a, b) => b.lastAt - a.lastAt);
}

export interface KnownRow {
  kind: LinkKind;
  /** How many refusals, and for how many groups and of how many devices. */
  count: number;
  groups: number;
  clients: number;
  /** The stick's clock at the latest of them, seconds, and the group of that one. */
  lastAt: number;
  example: string;
}

/** The refusals of the groups of the link, one row for each kind of them, the latest first. */
export function knownRows(list: Iptv["rejected"]): KnownRow[] {
  const by = new Map<LinkKind, { row: KnownRow; groups: Set<string>; clients: Set<string> }>();
  for (const r of list) {
    const kind = linkKind(r.group);
    if (kind == null) continue;
    const group = shortAddr(r.group);
    const e = by.get(kind) ?? {
      row: { kind, count: 0, groups: 0, clients: 0, lastAt: r.at_s, example: group },
      groups: new Set<string>(),
      clients: new Set<string>(),
    };
    e.row.count++;
    if (r.at_s >= e.row.lastAt) {
      e.row.lastAt = r.at_s;
      e.row.example = group;
    }
    e.groups.add(group);
    if (r.client != null) e.clients.add(shortAddr(r.client));
    e.row.groups = e.groups.size;
    e.row.clients = e.clients.size;
    by.set(kind, e);
  }
  return [...by.values()].map((e) => e.row).sort((a, b) => b.lastAt - a.lastAt);
}

export interface EventRow {
  event: Iptv["log"][number]["event"];
  group: string | null;
  vlan: number | null;
  count: number;
  lastAt: number;
}

/** The same event of the same group in the same VLAN as one row, the latest first. */
export function eventRows(log: Iptv["log"]): EventRow[] {
  const rows = new Map<string, EventRow>();
  for (const e of log) {
    const key = `${e.event}/${e.group ?? ""}/${e.vlan ?? ""}`;
    const row = rows.get(key) ?? {
      event: e.event,
      group: e.group ?? null,
      vlan: e.vlan ?? null,
      count: 0,
      lastAt: e.at_s,
    };
    row.count++;
    row.lastAt = Math.max(row.lastAt, e.at_s);
    rows.set(key, row);
  }
  return [...rows.values()].sort((a, b) => b.lastAt - a.lastAt);
}

/**
 * What @tv holds of the groups of the link, and the rest of it, with them or
 * without. The refusals stay: they are told apart by their kind, not hidden.
 */
export function withoutLinkLocal(
  tv: Iptv,
  show: boolean,
): { tv: Iptv; hidden: { events: number; watched: number } } {
  const keep = (g: string | null | undefined) => show || g == null || !linkLocal(g);
  const log = tv.log.filter((e) => keep(e.group));
  const groups = tv.groups.filter((g) => keep(g.group));
  return {
    tv: { ...tv, log, groups },
    hidden: {
      events: tv.log.length - log.length,
      watched: tv.groups.length - groups.length,
    },
  };
}
