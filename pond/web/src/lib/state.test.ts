// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { LIVE } from "../mocks/fixture";
import { fibre, lightQuality, linkSpeed, memory, session } from "./state";

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
    const none = {
      ...LIVE,
      settings: { ...LIVE.settings, pppoe: { user: "", secret_set: false } },
    };
    expect(session(none).configured).toBe(false);
    expect(session(none).up).toBe(false);
  });

  it("puts the line rate and memory in plain terms", () => {
    expect(linkSpeed(2500, "en")).toBe("2.5 Gb/s");
    expect(linkSpeed(1000, "pl")).toBe("1 Gb/s");
    expect(memory(LIVE)?.share).toBeGreaterThan(0);
  });
});
