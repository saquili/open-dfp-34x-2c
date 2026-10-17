// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* What the switch's pages show of its tables: the MAC table and the routes. */

import type { FdbEntry, GemPort, Route } from "../api/types";

/** The family of the multicast group an address stands for; null for a device's address. */
export function multicastOf(addr: string): "ipv4" | "ipv6" | null {
  const a = addr.toLowerCase();
  return a.startsWith("33:33:") ? "ipv6" : a.startsWith("01:00:5e:") ? "ipv4" : null;
}

/** Entries the stick keeps for itself: multicast groups and its own addresses. */
export function isSystemEntry(e: FdbEntry): boolean {
  return (e.addr != null && multicastOf(e.addr) != null) || e.state === "permanent";
}

/** Where the switch sees an address: on the host's side, the stick's own, the operator's. */
export interface MacPlace {
  side: "host" | "stick" | "operator";
  /** The Port-ID of the GEM channel it comes through, on the operator's side. */
  gem: number | null;
}

/** An address on a port of the switch, with every VLAN it is seen in there. */
export interface MacRow {
  mac: string;
  place: MacPlace;
  vlans: number[];
  learned: boolean;
  offloaded: boolean;
  /** Its entries are all ones the stick keeps for itself. */
  system: boolean;
}

/* Where a port of the switch leads: the SFP port to the host, the PON port and its channels to the operator, the rest is the stick's. */
function placeOf(dev: string, gems: Pick<GemPort, "name" | "port_id">[]): MacPlace {
  if (dev === "sfp") return { side: "host", gem: null };
  if (dev === "pon") return { side: "operator", gem: null };
  const g = gems.find((x) => x.name === dev);
  return g ? { side: "operator", gem: g.port_id } : { side: "stick", gem: null };
}

const SIDE: Record<MacPlace["side"], number> = { host: 0, stick: 1, operator: 2 };

/*
 * The MAC table as rows: an address on a port is one row, whatever VLANs it
 * is seen in there; the host's side first, then the stick, then the
 * operator's, by channel and VLAN.
 */
export function macRows(fdb: FdbEntry[], gems: Pick<GemPort, "name" | "port_id">[]): MacRow[] {
  const rows = new Map<string, MacRow>();
  for (const e of fdb) {
    if (!e.addr) continue;
    const key = `${e.addr} ${e.dev ?? ""}`;
    let r = rows.get(key);
    if (!r) {
      r = {
        mac: e.addr,
        place: placeOf(e.dev ?? "", gems),
        vlans: [],
        learned: false,
        offloaded: false,
        system: true,
      };
      rows.set(key, r);
    }
    if (e.vlan != null && !r.vlans.includes(e.vlan)) r.vlans.push(e.vlan);
    r.learned ||= e.learned;
    r.offloaded ||= e.offloaded;
    r.system &&= isSystemEntry(e);
  }
  const out = [...rows.values()];
  for (const r of out) r.vlans.sort((a, z) => a - z);
  return out.sort(
    (a, z) =>
      SIDE[a.place.side] - SIDE[z.place.side] ||
      (a.place.gem ?? -1) - (z.place.gem ?? -1) ||
      (a.vlans[0] ?? 0) - (z.vlans[0] ?? 0) ||
      a.mac.localeCompare(z.mac),
  );
}

/** Routes people care about: not the link-local ones every IPv6 interface has. */
export function shownRoutes(routes: Route[]): Route[] {
  return routes
    .filter((r) => !r.destination.toLowerCase().startsWith("fe80:"))
    .sort((a, z) =>
      a.destination === "default"
        ? -1
        : z.destination === "default"
          ? 1
          : a.family.localeCompare(z.family),
    );
}
