// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { LIVE } from "../mocks/fixture";
import {
  downVlan,
  fibre,
  fibreVlan,
  gems,
  hostLink,
  lightQuality,
  linkSpeed,
  memory,
  registration,
  type SessionView,
  served,
  session,
  sessions,
} from "./state";

describe("what the figures mean", () => {
  it("rates the received light against the receiver's range", () => {
    expect(lightQuality(-7)).toBe("tooStrong");
    expect(lightQuality(-20)).toBe("good");
    expect(lightQuality(-27)).toBe("weak");
    expect(lightQuality(-30)).toBe("tooWeak");
    expect(lightQuality(null)).toBe("none");
  });

  it("reads a connected fibre as connected, with its ONU number", () => {
    const f = fibre(LIVE);
    expect(f.connected).toBe(true);
    expect(f.tone).toBe("good");
    expect(f.onuId).toBe(1);
  });

  it("knows no fibre when there is no PON device", () => {
    expect(fibre({ ...LIVE, onu: null }).stateKey).toBe("noPon");
  });

  it("sees the stick's own session only with an account and an address handed on", () => {
    expect(session(LIVE).up).toBe(true);
    expect(session(LIVE).address).toBe("203.0.113.24");
    const none = { ...LIVE, settings: { ...LIVE.settings, sessions: [] } };
    expect(session(none).configured).toBe(false);
    expect(session(none).up).toBe(false);
  });

  it("sums two sessions: up only when both are, the address from the IPv4 one", () => {
    const [v4] = LIVE.settings.sessions;
    if (!v4) throw new Error("the fixture has a session");
    const v6 = { ...v4, id: 2, family: "ipv6" as const, hands_on: false, ipv4: null, ppp: null };
    const two = { ...LIVE, settings: { ...LIVE.settings, sessions: [v4, v6] } };
    expect(sessions(two).map((x) => [x.id, x.handsOn, x.up])).toEqual([
      [1, true, true],
      [2, false, false],
    ]);
    expect(session(two).up).toBe(false);
    expect(session(two).count).toBe(2);
    expect(session(two).address).toBe("203.0.113.24");
    const both = { ...two, settings: { ...two.settings, sessions: [v4, { ...v6, ppp: "ppp1" }] } };
    expect(session(both).up).toBe(true);
  });

  it("puts the line rate and memory in plain terms", () => {
    expect(linkSpeed(2500, "en")).toBe("2.5 Gb/s");
    expect(linkSpeed(1000, "pl")).toBe("1 Gb/s");
    expect(memory(LIVE)?.share).toBeGreaterThan(0);
  });
});

describe("the VLAN of a session on the fibre", () => {
  const given = (vlan: string, dev: string | null) =>
    ({ ...sessions(LIVE)[0], vlan, dev }) as SessionView;

  it("is the VLAN of its interface, which the OLT's translation may make another", () => {
    expect(fibreVlan(given("410", "wan1410"))).toBe("1410");
    expect(fibreVlan(given("410", "wan410"))).toBe("410");
  });

  it("is the VLAN it was given while it has no interface, or runs on the GEM interface", () => {
    expect(fibreVlan(given("410", null))).toBe("410");
    expect(fibreVlan(given("", "gem278"))).toBe("");
  });

  it("is what a channel shows for the sessions it carries", () => {
    const live = structuredClone(LIVE);
    const s = live.settings?.sessions?.[0];
    if (!s) throw new Error("a session");
    s.vlan = "410";
    s.dev = "wan1410";
    for (const x of live.settings?.sessions ?? []) x.dev = "wan1410";
    const g = gems(live).find((x) => x.use === "session");
    expect(g?.vlan).toBe("1410");
  });
});

describe("the VLAN a session hands its address down in", () => {
  it("is the session's own, the service's, unless another or none is set", () => {
    expect(downVlan({ vlan: "410", hostVlan: "" })).toBe("410");
    expect(downVlan({ vlan: "410", hostVlan: "36" })).toBe("36");
    expect(downVlan({ vlan: "410", hostVlan: "untagged" })).toBe("");
  });

  it("is told to the panel by the stick, which hands down unless told not to", () => {
    const [v4] = sessions(LIVE);
    expect([v4?.pass, v4?.hostVlan]).toEqual([true, "36"]);
    const live = structuredClone(LIVE);
    const s = live.settings?.sessions?.[0];
    if (!s) throw new Error("a session");
    s.pass = "off";
    expect(sessions(live)[0]?.pass).toBe(false);
  });
});

describe("the gateway the host is given", () => {
  it("is the concentrator's address while a session hands its address on", () => {
    expect(hostLink(LIVE).hostGateway).toBe("198.51.100.1");
  });

  it("is none while nothing is handed on", () => {
    const live = structuredClone(LIVE);
    if (!live.settings) throw new Error("settings");
    live.settings.pass = null;
    expect(hostLink(live).hostGateway).toBeNull();
  });
});

describe("what the stick serves of the routing settings", () => {
  it("takes a part the stick leaves out as not served", () => {
    /* The recorded stick is one of before the routing: it serves none of the parts. */
    for (const part of ["ipoe", "lans", "forwards", "rules"] as const)
      expect(served(LIVE, part)).toBe(false);
  });

  it("takes a part the stick lists, even empty, as served", () => {
    const live = { ...LIVE, settings: { ...LIVE.settings, lans: [], rules: [] } };
    expect(served(live, "lans")).toBe(true);
    expect(served(live, "rules")).toBe(true);
    expect(served(live, "forwards")).toBe(false);
    expect(served(live, "ipoe")).toBe(false);
  });

  it("serves nothing before the first reading", () => {
    expect(served(undefined, "lans")).toBe(false);
  });
});

describe("how far the stick got in registering on the OLT", () => {
  const at = (state: string, more: Partial<Parameters<typeof registration>[0]> = {}) =>
    registration({
      state,
      alarms: [],
      omcc: true,
      ploamSet: false,
      omciFrames: 120,
      services: 3,
      ...more,
    });

  it("has every step done in O5, the password not used where none is set", () => {
    expect(at("o5")).toEqual({
      sync: "done",
      serial: "done",
      ranging: "done",
      ploam: "off",
      omcc: "done",
      mib: "done",
      services: "done",
    });
    expect(at("o5", { ploamSet: true }).ploam).toBe("done");
  });

  it("is at the serial number in O3 and at the ranging in O4", () => {
    expect(at("o3")).toMatchObject({ sync: "done", serial: "now", ranging: "wait", omcc: "wait" });
    expect(at("o4")).toMatchObject({ serial: "done", ranging: "now", ploam: "wait" });
  });

  it("waits for the OMCI channel, then for the OLT to talk on it, then for services", () => {
    expect(at("o5", { omcc: false })).toMatchObject({ omcc: "now", mib: "wait" });
    expect(at("o5", { omciFrames: 0 })).toMatchObject({ mib: "now", services: "wait" });
    expect(at("o5", { services: 0 }).services).toBe("now");
  });

  it("fails the signal on LOS in O1 and in O6, and the serial number in O7", () => {
    expect(at("o1").sync).toBe("now");
    expect(at("o1", { alarms: ["LOS"] }).sync).toBe("fail");
    expect(at("o6")).toMatchObject({ sync: "fail", serial: "done", ranging: "done", omcc: "wait" });
    expect(at("o7")).toMatchObject({ sync: "done", serial: "fail", ranging: "wait" });
  });

  it("waits at every step without a PON interface", () => {
    expect(Object.values(at("noPon")).every((s) => s === "wait")).toBe(true);
  });
});
