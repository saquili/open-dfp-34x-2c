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
  columns: { id: string; label: string; side: "host" | "stick" | "operator" }[];
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
    ...gems.map((g) => ({ id: g.name, label: `GEM ${g.port_id}`, side: "operator" as const })),
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
