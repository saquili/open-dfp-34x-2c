// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* What the switch's pages show of its tables: the MAC table and the routes. */

import type { FdbEntry, Route } from "../api/types";

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
