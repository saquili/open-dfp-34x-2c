// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Which VLAN goes through which port of the switch, as a matrix: a row per
 * VLAN, a column per port (the SFP port, the processor, each GEM channel),
 * and in each cell whether the VLAN's frames cross that port tagged,
 * untagged, or not at all. It is put together from the VLAN interfaces the
 * stick runs (management, the host's address, its sessions), the VLANs of
 * its bridges, and the translations the OLT asked for.
 */

import type { Bridge, Live, Network } from "../api/types";
import { sessions } from "./state";

/** How a VLAN crosses a port: tagged, untagged, untagged and the port's own (PVID). */
export type Member = "tagged" | "untagged" | "pvid";

export type VlanRole = "management" | "handover" | "session" | "bridged" | "other";

export interface VlanRow {
  vid: number;
  role: VlanRole;
  /** Membership by column: "sfp", "cpu" or a GEM interface's name. */
  cells: Record<string, Member>;
  /** The VLAN it is translated to on the other side, when the OLT asks for one. */
  peer: number | null;
}

export interface VlanMatrix {
  /** The columns: the SFP port, the processor, then the GEM channels by Port-ID. */
  columns: { id: string; label: string; side: "host" | "stick" | "operator"; port?: number }[];
  rows: VlanRow[];
}

export function vlanMatrix(
  net: Network | undefined,
  bridge: Bridge | undefined,
  live: Live | undefined,
  translations: { host: number | null; fibre: number | null }[] = [],
): VlanMatrix {
  const gems = (live?.gem_ports ?? []).slice().sort((a, z) => a.port_id - z.port_id);
  const columns: VlanMatrix["columns"] = [
    { id: "sfp", label: "SFP", side: "host" },
    { id: "cpu", label: "CPU", side: "stick" },
    ...gems.map((g) => ({
      id: g.name,
      label: `GEM ${g.port_id}`,
      side: "operator" as const,
      port: g.port_id,
    })),
  ];
  const rows = new Map<number, VlanRow>();
  const row = (vid: number, role: VlanRole) => {
    let r = rows.get(vid);
    if (!r) {
      r = { vid, role, cells: {}, peer: null };
      rows.set(vid, r);
    } else if (r.role === "other" || r.role === "bridged") r.role = role;
    return r;
  };

  /* A VLAN interface of the stick's own: its VLAN ends on the processor. */
  const s = live?.settings;
  const sessionDevs = new Set(sessions(live).map((x) => x.dev));
  for (const l of net?.links ?? []) {
    if (l.kind !== "vlan" || l.vlan == null || !l.lower) continue;
    const role: VlanRole =
      l.name === s?.pass?.lan
        ? "handover"
        : sessionDevs.has(l.name)
          ? "session"
          : l.addresses.some((a) => a.family === "ipv4" && !a.link_local)
            ? "management"
            : "other";
    const r = row(l.vlan, role);
    r.cells[l.lower] = "tagged";
    r.cells.cpu = "tagged";
  }

  /* The bridges: each member port carries its VLANs. */
  for (const v of bridge?.vlans ?? []) {
    const r = row(v.vlan, "bridged");
    r.cells[v.dev] = v.pvid ? "pvid" : v.egress_untagged ? "untagged" : "tagged";
  }

  for (const t of translations) {
    if (t.host == null || t.fibre == null || t.host === t.fibre) continue;
    const a = rows.get(t.host);
    const b = rows.get(t.fibre);
    if (a) a.peer = t.fibre;
    if (b) b.peer = t.host;
  }

  return { columns, rows: [...rows.values()].sort((a, z) => a.vid - z.vid) };
}

/** A VLAN's way through the stick: from the host on the SFP port to the operator on the fibre. */
export interface VlanPath {
  /** The VLAN as the host and the stick name it. */
  vid: number;
  /** The VLAN on the fibre, when the OLT translates it to another. */
  fibre: number | null;
  role: VlanRole;
  /** How it crosses the SFP port to the host; null when it does not. */
  host: Member | null;
  /** It ends on the stick's processor. */
  ends: boolean;
  /** The Port-IDs of the GEM channels that carry it. */
  gems: number[];
}

/* Which of two rows' roles a way takes: the one that says more. */
const SAYS: Record<VlanRole, number> = {
  other: 0,
  bridged: 1,
  session: 2,
  handover: 2,
  management: 2,
};

/*
 * The matrix's rows as ways through the stick. A VLAN and the one the OLT
 * translates it to on the fibre are one way, under the host's VID; the SFP
 * port is taken from the host's VID's row, the channels from both.
 */
export function vlanPaths(
  m: VlanMatrix,
  translations: { host: number | null; fibre: number | null }[] = [],
): VlanPath[] {
  const hostOf = new Map<number, number>();
  const fibreOf = new Map<number, number>();
  for (const t of translations) {
    if (t.host == null || t.fibre == null || t.host === t.fibre) continue;
    hostOf.set(t.fibre, t.host);
    fibreOf.set(t.host, t.fibre);
  }
  const paths = new Map<number, VlanPath>();
  for (const r of m.rows) {
    const vid = hostOf.get(r.vid) ?? r.vid;
    let p = paths.get(vid);
    if (!p) {
      p = { vid, fibre: fibreOf.get(vid) ?? null, role: r.role, host: null, ends: false, gems: [] };
      paths.set(vid, p);
    } else if (SAYS[r.role] > SAYS[p.role]) p.role = r.role;
    for (const c of m.columns) {
      const mem = r.cells[c.id];
      if (!mem) continue;
      if (c.side === "host") p.host = r.vid === vid ? mem : (p.host ?? mem);
      else if (c.side === "stick") p.ends = true;
      else if (c.port != null && !p.gems.includes(c.port)) p.gems.push(c.port);
    }
  }
  const out = [...paths.values()].sort((a, z) => a.vid - z.vid);
  for (const p of out) p.gems.sort((a, z) => a - z);
  return out;
}
