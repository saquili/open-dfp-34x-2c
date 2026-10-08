// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { LIVE } from "../mocks/fixture";
import {
  fibre,
  fibreVlan,
  gems,
  hostLink,
  lightQuality,
  linkSpeed,
  memory,
  type SessionView,
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
    const g = gems(live).find((x) => x.use === "session");
    expect(g?.vlan).toBe("1410");
  });
});

describe("the gateway the host is given", () => {
  it("is the concentrator's address while a session hands its address on", () => {
    const link = hostLink(LIVE);
    expect(link.peer).toBe(true);
    expect(link.hostGateway).toBe("198.51.100.1");
    expect(link.gateway).toBe("169.254.0.1");
  });

  it("is the stick's own address when told so, or without the concentrator's", () => {
    const live = structuredClone(LIVE);
    if (!live.settings?.pass) throw new Error("a handed address");
    live.settings.settings.PASS_PEER = "off";
    live.settings.pass.peer_gateway = null;
    expect(hostLink(live).peer).toBe(false);
    expect(hostLink(live).hostGateway).toBe("169.254.0.1");
  });

  it("is none while nothing is handed on", () => {
    const live = structuredClone(LIVE);
    if (!live.settings) throw new Error("settings");
    live.settings.pass = null;
    expect(hostLink(live).hostGateway).toBeNull();
  });
});
