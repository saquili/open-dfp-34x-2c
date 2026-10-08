// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { BRIDGE, LIVE, NETWORK } from "../mocks/fixture";
import { gems } from "./state";
import { isSystemEntry, shownRoutes, sideOf, switchView } from "./switch";

describe("what the switch is doing", () => {
  const v = switchView(NETWORK, LIVE);

  it("puts the SFP port on the host side and the fibre on the operator side", () => {
    expect(v.ports.map((p) => [p.link.name, p.side])).toEqual([
      ["sfp", "host"],
      ["pon", "operator"],
    ]);
    expect(v.conduit?.name).toBe("cpu");
  });

  it("says what each VLAN on the SFP port is for", () => {
    const sfp = v.ports[0];
    expect(sfp?.ifaces.map((i) => [i.link.name, i.link.vlan, i.role])).toEqual([
      ["lan", 36, "handover"],
      ["mgmt", 4094, "management"],
    ]);
  });

  it("hangs the sessions under their VLAN, and the VLAN under its GEM port", () => {
    const pon = v.ports[1];
    const gem278 = pon?.ifaces.find((i) => i.link.name === "gem278");
    expect(gem278?.role).toBe("channel");
    expect(gem278?.children.map((c) => [c.link.name, c.role])).toEqual([["wan100", "wan"]]);
    expect(gem278?.children[0]?.children.map((c) => [c.link.name, c.role])).toEqual([
      ["ppp0", "session"],
      ["ppp1", "session"],
    ]);
  });

  it("knows the stick routes between the host's VLAN and its own session", () => {
    expect(v.routed).toEqual({ lanVlan: 36, wanVlan: 100, gem: "gem278" });
  });

  it("names the interfaces the stick can be reached on, and not the loopback", () => {
    expect(v.management.map((l) => l.name)).toEqual(["mgmt"]);
  });

  it("sees the bridge of the phone and television as joining both sides", () => {
    expect(v.bridges).toHaveLength(1);
    expect(v.bridges[0]?.spans).toBe(true);
    expect(v.bridges[0]?.members.map((m) => [m.link.name, m.side])).toEqual([
      ["sfp", "host"],
      ["gem2047", "operator"],
      ["gem2046", "operator"],
      ["gem280", "operator"],
      ["gem281", "operator"],
    ]);
    const gem = v.ports[1]?.ifaces.find((i) => i.link.name === "gem280");
    expect(gem?.role).toBe("bridged");
  });

  it("follows a VLAN down to the port it ends up on", () => {
    expect(sideOf(NETWORK.links, "wan100")).toBe("operator");
    expect(sideOf(NETWORK.links, "mgmt")).toBe("host");
    expect(sideOf(NETWORK.links, "ppp0")).toBe(null);
  });

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
