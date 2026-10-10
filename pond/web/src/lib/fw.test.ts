// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { lanOf, readEnd, writeEnd } from "./fw";

describe("readEnd", () => {
  it("reads every kind of end, and writes it back the same", () => {
    for (const s of [
      "internet",
      "internet:203.0.113.0/24",
      "internet:2001:db8::/32",
      "lans",
      "lan:2",
      "dev:1:00:00:5e:00:53:16",
    ]) {
      const e = readEnd(s);
      expect(e).not.toBeNull();
      expect(writeEnd(e as NonNullable<typeof e>)).toBe(s);
    }
  });
  it("takes a MAC in either case, and keeps it in lower case", () => {
    expect(readEnd("dev:1:00:00:5E:00:53:16")).toEqual({
      kind: "dev",
      lan: 1,
      mac: "00:00:5e:00:53:16",
    });
  });
  it("refuses what is not an end", () => {
    expect(readEnd("lan:")).toBeNull();
    expect(readEnd("dev:1:00:00")).toBeNull();
    expect(readEnd("wan")).toBeNull();
  });
  it("names the network an end lies in", () => {
    expect(lanOf({ kind: "dev", lan: 2, mac: "00:00:5e:00:53:3d" })).toBe(2);
    expect(lanOf({ kind: "lans" })).toBeNull();
    expect(lanOf({ kind: "internet", addr: "" })).toBeNull();
  });
});
