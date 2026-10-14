// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { BRIDGE, LIVE, NETWORK } from "../mocks/fixture";
import { gems } from "./state";
import { isSystemEntry, shownRoutes } from "./switch";

describe("what the switch is doing", () => {
  it("leaves the stick's own MAC entries out unless asked", () => {
    const shown = BRIDGE.fdb.filter((e) => !isSystemEntry(e));
    expect(shown.every((e) => !e.addr?.startsWith("33:33:"))).toBe(true);
    expect(shown.some((e) => e.dev === "gem278")).toBe(false);
    expect(shown.length).toBe(4);
  });

  it("lists the default route first and hides the IPv6 link-local ones", () => {
    const r = shownRoutes(NETWORK.routes);
    expect(r[0]?.destination).toBe("default");
    expect(r.some((x) => x.destination.startsWith("fe80:"))).toBe(false);
  });

  it("gives each channel its allocation and its use", () => {
    expect(gems(LIVE).map((g) => [g.gem.port_id, g.allocId, g.use])).toEqual([
      [278, 271, "session"],
      [279, 272, "ends"],
      [280, 282, "bridged"],
      [281, 282, "bridged"],
      [2046, null, "bridged"],
      [2047, null, "bridged"],
    ]);
  });
});
