// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The stick's connections to the operator, of every kind, in one list: its
 * PPPoE sessions and the connections whose address is typed in or comes
 * from DHCP. Each runs in a VLAN of the operator's, named by the VID the
 * operator gives the service towards the host.
 */

import type { Live } from "../../api/types";
import type { Service } from "../../lib/omci";
import { type IpoeView, ipoes, MAX_CONNS, type SessionView, sessions } from "../../lib/state";
import { hostSide } from "./shared";

export const KINDS = ["pppoe", "static", "dhcp"] as const;
export type Kind = (typeof KINDS)[number];

export type Conn =
  | { kind: "pppoe"; key: string; vlan: string; x: SessionView }
  | { kind: "static" | "dhcp"; key: string; vlan: string; x: IpoeView };

/* A VID as a number to sort by; none goes last. */
const order = (vlan: string) => (/^\d+$/.test(vlan) ? Number(vlan) : 1e4);

/** The connections, by their VLAN, then PPPoE before the others, then by number. */
export function conns(live: Live | undefined): Conn[] {
  const list: Conn[] = [
    ...sessions(live).map((x) => ({
      kind: "pppoe" as const,
      key: `pppoe:${x.id}`,
      vlan: x.vlan,
      x,
    })),
    ...ipoes(live).map((x) => ({ kind: x.kind, key: `ipoe:${x.id}`, vlan: x.vlan, x })),
  ];
  return list.sort(
    (a, z) =>
      order(a.vlan) - order(z.vlan) ||
      KINDS.indexOf(a.kind) - KINDS.indexOf(z.kind) ||
      a.x.id - z.x.id,
  );
}

/** Whether another connection can be added: seven of every kind together. */
export function room(list: Conn[]) {
  return list.length < MAX_CONNS;
}

/**
 * The service of the operator's that VLAN @vlan names, as the host knows
 * it: the one translated to it, else the one that runs in it on the fibre.
 */
export function serviceIn(list: Service[], vlan: string): Service | undefined {
  const vid = Number(vlan);
  if (!vlan || !Number.isInteger(vid)) return undefined;
  return list.find((s) => hostSide(s).includes(vid)) ?? list.find((s) => s.vlan === vid);
}

/**
 * The connection whose address is typed in or comes from DHCP in VLAN
 * @vlan, other than @self: a VLAN has one at most.
 */
export function ipoeIn(list: Conn[], vlan: string, self?: string) {
  return list.find((c) => c.kind !== "pppoe" && c.vlan === vlan && c.key !== self);
}
