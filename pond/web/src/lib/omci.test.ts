// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { OMCI } from "../mocks/omci";
import {
  bridgePorts,
  isDefault,
  Mib,
  multicast,
  services,
  tagsMatched,
  translation,
  unit,
  vlanRule,
  vlanRules,
} from "./omci";

const mib = new Mib(OMCI);

describe("the managed entities", () => {
  it("are kept by class and instance", () => {
    expect(mib.get(256, 0)?.attrs.get(1)).toEqual(new Uint8Array([0x5a, 0x5a, 0x5a, 0x5a]));
    expect(mib.all(262).map((t) => t.id)).toEqual([
      0x8000, 0x8001, 0x8002, 0x8003, 0x8004, 0x8005, 0x8006, 0x8007,
    ]);
  });

  it("keep a password pond leaves out as null", () => {
    expect(mib.get(256, 0)?.attrs.get(11)).toBeNull();
  });

  it("are empty without a value", () => {
    expect(new Mib(null).empty).toBe(true);
  });
});

describe("a rule of an extended VLAN tagging operation", () => {
  /* Single-tagged VLAN 410, priority 0 -> VLAN 100, the priority kept. */
  const row = new Uint8Array([
    0xf8, 0x00, 0x00, 0x00, 0x00, 0xcd, 0x00, 0x00, 0x40, 0x0f, 0x00, 0x00, 0x00, 0x08, 0x03, 0x20,
  ]);

  it("is read field by field", () => {
    const r = vlanRule(row);
    expect(r.outer.prio).toBe(15);
    expect(r.inner).toEqual({ prio: 0, vid: 410, tpid: 0 });
    expect(r.remove).toBe(1);
    expect(r.addOuter.prio).toBe(15);
    expect(r.addInner).toEqual({ prio: 8, vid: 100, tpid: 0 });
  });

  it("says which VLAN goes to which", () => {
    const r = vlanRule(row);
    expect(tagsMatched(r)).toBe(1);
    expect(translation(r)).toEqual({ from: 410, to: 100 });
  });

  it("drops what no other rule takes", () => {
    const rules = vlanRules(mib)[0]?.rules ?? [];
    const defaults = rules.filter((r) => isDefault(r) || tagsMatched(r) === 0);
    expect(defaults.length).toBe(3);
    expect(defaults.every((r) => r.remove === 3 && translation(r) === null)).toBe(true);
  });
});

describe("the services", () => {
  const list = services(mib);

  it("are the VLANs on the fibre, each with the host's VLAN", () => {
    expect(list.map((s) => [s.vlan, s.hostVlans])).toEqual([
      [100, [410]],
      [200, [420]],
      [300, [421]],
    ]);
  });

  it("carry internet on two channels by priority, and broadcast on a third", () => {
    const internet = list[0];
    const channels = internet?.ports.flatMap((p) => p.channels) ?? [];
    expect(channels.map((c) => [c.portId, c.pbits, c.allocId])).toEqual([
      [278, [0], 271],
      [279, [6, 7], 272],
      [2046, [], null],
    ]);
    expect(channels[0]?.encrypted).toBe(true);
    expect(channels[0]?.queue).toEqual({ priority: 0, weight: 1 });
  });

  it("put the multicast channel with the television's VLAN", () => {
    const tv = list[2];
    expect(tv?.multicast?.version).toBe(2);
    expect(tv?.multicast?.downstreamVid).toBe(421);
    expect(tv?.multicast?.acl).toEqual([
      { portId: 2047, vlan: 300, from: "224.0.0.0", to: "239.255.255.255" },
    ]);
    expect(tv?.ports.flatMap((p) => p.channels).some((c) => c.portId === 2047 && c.multicast)).toBe(
      true,
    );
  });
});

describe("the unit", () => {
  it("is what the OLT reads", () => {
    const u = unit(mib);
    expect(u.vendorId).toBe("ZZZZ");
    expect(u.equipment).toBe("EXAMPLE-ONT");
    expect(u.omcc).toBe(0xa3);
    /* An ONU-G with no LOID holds an empty one, which the OLT reads as such. */
    expect(u.loid).toBe("");
    expect(u.packVersion).toBe("V1.0");
    expect(u.images.map((i) => [i.id, i.version, i.committed, i.active])).toEqual([
      [0, "V1.0.0", true, true],
      [1, "V1.0.0", false, false],
    ]);
    expect(u.olt?.equipment).toBe("EXAMPLE-OLT");
  });

  it("has a host port and the fibre ports on one bridge", () => {
    const ports = bridgePorts(mib);
    expect(ports.filter((p) => p.side === "host").map((p) => p.kind)).toEqual(["uni"]);
    expect(new Set(ports.map((p) => p.bridge))).toEqual(new Set([1]));
    expect(multicast(mib)).toHaveLength(1);
  });
});
