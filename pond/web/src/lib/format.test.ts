// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { bitrate, bytes, dbm, dbmText, duration } from "./format";

describe("figures for people", () => {
  it("scales a rate to the unit that reads best", () => {
    expect(bitrate(933e6, "en")).toBe("933 Mb/s");
    expect(bitrate(9.6e6, "en")).toBe("9.6 Mb/s");
    expect(bitrate(2.5e9, "pl")).toBe("2,5 Gb/s");
    expect(bitrate(0, "en")).toBe("0 b/s");
    expect(bitrate(null, "en")).toBe("–");
  });

  it("gives sizes in units of 1024", () => {
    expect(bytes(25308 * 1024, "en")).toBe("24.7 MB");
    expect(bytes(512, "en")).toBe("512 B");
  });

  it("turns microwatts into dBm, and no light into nothing", () => {
    expect(dbm(1000)).toBeCloseTo(0);
    expect(dbm(2)).toBeCloseTo(-26.99, 1);
    expect(dbm(0)).toBeNull();
    expect(dbmText(dbm(1682), "en")).toBe("+2.3 dBm");
    expect(dbmText(-27, "pl")).toBe("-27,0 dBm");
  });

  it("shows the two largest units of a span", () => {
    expect(duration(45, "en")).toBe("45 sec");
    expect(duration(2 * 3600 + 13 * 60 + 5, "en")).toBe("2 hr 13 min");
    expect(duration(3 * 86400 + 4 * 3600, "en")).toBe("3 days 4 hr");
  });
});
