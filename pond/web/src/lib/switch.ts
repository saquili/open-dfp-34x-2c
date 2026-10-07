// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What the switch is doing, in the stick's own terms: which interfaces sit
 * on which port, what each one is for, whether anything is bridged at L2
 * between the SFP port and the fibre, and which addresses the switch knows.
 */

import type { FdbEntry, Link, Live, Network, Route } from "../api/types";

/** The side of the stick a port faces. */
export type Side = "host" | "operator";

/** What an interface is for. */
export type Role = "management" | "handover" | "wan" | "session" | "bridged" | "channel" | "unused";

export interface IfaceView {
  link: Link;
  role: Role;
  /** The interfaces that run over this one. */
  children: IfaceView[];
}

export interface PortView {
  link: Link;
  side: Side;
  ifaces: IfaceView[];
}

export interface BridgeView {
  link: Link;
  members: { link: Link; side: Side | null }[];
  /** It joins the SFP port and the fibre. */
  spans: boolean;
}

export interface SwitchView {
  conduit: Link | null;
  ports: PortView[];
  bridges: BridgeView[];
  /** The stick's own PPPoE session: the host VLAN, the WAN VLAN, the GEM port. */
  routed: { lanVlan: number | null; wanVlan: number | null; gem: string | null } | null;
  /** The interfaces the stick can be reached on, and their addresses. */
  management: Link[];
}

const SIDE: Record<string, Side> = { sfp: "host", pon: "operator" };

function byName(links: Link[]) {
  return new Map(links.map((l) => [l.name, l]));
}

/** The switch port an interface ends up on, following the devices it runs over. */
export function portOf(links: Link[], name: string | null): Link | null {
  const all = byName(links);
  let at = name ? all.get(name) : undefined;
  for (let hops = 0; at && hops < 8; hops++) {
    if (at.kind === "dsa") return at;
    at = at.lower ? all.get(at.lower) : undefined;
  }
  return null;
}

export function sideOf(links: Link[], name: string | null): Side | null {
  const port = portOf(links, name);
  return port ? (SIDE[port.name] ?? null) : null;
}

const globalV4 = (l: Link) => l.addresses.some((a) => a.family === "ipv4" && !a.link_local);

export function roleOf(link: Link, live: Live | undefined): Role {
  const s = live?.settings;
  if (link.kind === "ppp") return "session";
  if (s?.sessions?.some((x) => x.dev === link.name) || (s?.wan && link.name === s.wan.dev))
    return "wan";
  if (s?.pass?.lan && link.name === s.pass.lan) return "handover";
  if (link.master) return "bridged";
  if (link.kind === "gem") return "channel";
  if (globalV4(link)) return "management";
  return "unused";
}

export function switchView(net: Network | undefined, live: Live | undefined): SwitchView {
  const links = net?.links ?? [];
  const s = live?.settings;
  const ports = links.filter((l) => l.kind === "dsa");
  const conduit = links.find((l) => l.name === ports[0]?.lower) ?? null;

  /* A PPP link runs over its session's VLAN, though no netlink attribute says so. */
  const over = (l: Link) =>
    l.kind === "ppp"
      ? (s?.sessions?.find((x) => x.ppp === l.name)?.dev ??
        (s?.wan && l.name === s.wan.out ? s.wan.dev : null))
      : l.lower;

  const tree = (parent: string): IfaceView[] =>
    links
      .filter((l) => l.kind !== "dsa" && l.kind !== "bridge" && over(l) === parent)
      .sort((a, z) => (a.vlan ?? 0) - (z.vlan ?? 0) || a.name.localeCompare(z.name))
      .map((link) => ({ link, role: roleOf(link, live), children: tree(link.name) }));

  const bridges = links
    .filter((l) => l.kind === "bridge")
    .map((link) => {
      const members = links
        .filter((m) => m.master === link.name)
        .map((m) => ({ link: m, side: sideOf(links, m.name) }));
      const sides = new Set(members.map((m) => m.side));
      return { link, members, spans: sides.has("host") && sides.has("operator") };
    });

  const main = s?.sessions?.find((x) => x.hands_on);
  const wan = main ? links.find((l) => l.name === main.dev) : undefined;
  const lan = s?.pass?.lan ? links.find((l) => l.name === s.pass?.lan) : undefined;
  const routed =
    s?.pass && main
      ? {
          lanVlan: lan?.vlan ?? null,
          wanVlan: wan?.vlan ?? null,
          gem: main.gem_dev ?? wan?.lower ?? null,
        }
      : null;

  return {
    conduit,
    ports: ports
      .map((link) => ({ link, side: SIDE[link.name] ?? "host", ifaces: tree(link.name) }))
      .sort((a, z) => (a.side === z.side ? 0 : a.side === "host" ? -1 : 1)),
    bridges,
    routed,
    /* Only what sits on a switch port: the loopback is no way in. */
    management: links.filter(
      (l) => sideOf(links, l.name) && globalV4(l) && roleOf(l, live) === "management",
    ),
  };
}

/** Entries the stick keeps for itself: multicast groups and its own addresses. */
export function isSystemEntry(e: FdbEntry): boolean {
  const mc = e.addr != null && (e.addr.startsWith("33:33:") || e.addr.startsWith("01:00:5e:"));
  return mc || e.state === "permanent";
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
