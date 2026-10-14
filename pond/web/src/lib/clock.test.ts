// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import type { Clock } from "../api/types";
import { clockState, offsetText, ppmText, sourceKey, spanText, stale } from "./clock";

const base: Clock = {
  now_s: 1791892800,
  source: "ptp",
  state: "locked",
  offset_ns: -1200,
  updated_s: 1791892795,
  steps: 1,
  freq_ppb: 5120,
  ntp: null,
  ptp: null,
  olt: null,
};

describe("clockState", () => {
  it("names each state of timed with its tone", () => {
    expect(clockState(base)).toEqual(["system.clockLocked", "good"]);
    expect(clockState({ ...base, state: "syncing" })).toEqual(["system.clockSyncing", "info"]);
    expect(clockState({ ...base, source: "olt", state: "set" })).toEqual([
      "system.clockSet",
      "info",
    ]);
    expect(clockState({ ...base, source: "none", state: "holdover" })).toEqual([
      "system.clockHoldover",
      "warn",
    ]);
    expect(clockState({ ...base, source: "none", state: "unsynchronized" })).toEqual([
      "system.clockUnset",
      "bad",
    ]);
  });

  it("says that nothing sets the clock without timed's state", () => {
    expect(clockState({ ...base, source: null, state: null, updated_s: null })).toEqual([
      "system.clockNoDaemon",
      "bad",
    ]);
  });

  it("takes a state that timed stopped writing for a daemon that stopped", () => {
    expect(stale({ ...base, updated_s: base.now_s - 30 })).toBe(false);
    expect(stale({ ...base, updated_s: base.now_s - 31 })).toBe(true);
    expect(clockState({ ...base, updated_s: base.now_s - 60 })).toEqual([
      "system.clockNoDaemon",
      "bad",
    ]);
  });
});

describe("sourceKey", () => {
  it("names the source in use", () => {
    expect(sourceKey(base)).toBe("system.clockFromPtp");
    expect(sourceKey({ ...base, source: "ntp" })).toBe("system.clockFromNtp");
    expect(sourceKey({ ...base, source: "olt" })).toBe("system.clockFromOlt");
    expect(sourceKey({ ...base, source: "none" })).toBe("system.clockFromNone");
    expect(sourceKey({ ...base, source: null })).toBe("system.clockFromNone");
  });
});

describe("offsetText", () => {
  it("gives a signed offset in the unit that reads best", () => {
    expect(offsetText(-1200, "en")).toBe("−1.2\u00a0µs");
    expect(offsetText(48229, "en")).toBe("+48\u00a0µs");
    expect(offsetText(-30005728, "en")).toBe("−30\u00a0ms");
    expect(offsetText(-4931810024, "en")).toBe("−4.9\u00a0s");
    expect(offsetText(700, "en")).toBe("+700\u00a0ns");
    expect(offsetText(-1200, "pl")).toBe("−1,2\u00a0µs");
  });

  it("gives a dash for no offset", () => {
    expect(offsetText(null, "en")).toBe("–");
    expect(offsetText(undefined, "en")).toBe("–");
    expect(spanText(null, "en")).toBe("–");
  });

  it("gives a frequency correction in ppm", () => {
    expect(ppmText(5120, "en")).toBe("+5.12\u00a0ppm");
    expect(ppmText(-30005, "pl")).toBe("−30,01\u00a0ppm");
    expect(ppmText(null, "en")).toBe("–");
  });

  it("gives a span without a sign", () => {
    expect(spanText(48229, "en")).toBe("48\u00a0µs");
    expect(spanText(-180625, "pl")).toBe("181\u00a0µs");
  });
});
