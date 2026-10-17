// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { shown } from "./omci-values";

const b = (...v: number[]) => new Uint8Array(v);

describe("an attribute's value", () => {
  it("of a password omcid does not give is hidden", () => {
    expect(shown(256, 11, null)).toEqual({ kind: "hidden" });
  });

  it("of a pointer is the entity it points to", () => {
    expect(shown(268, 2, b(0x80, 0x01))).toEqual({ kind: "ref", cls: 262, inst: 0x8001 });
    expect(shown(130, 4, b(0x11, 0x02))).toEqual({ kind: "ref", cls: 266, inst: 0x1102 });
    expect(shown(268, 2, b(0xff, 0xff))).toEqual({ kind: "none" });
  });

  it("of a pointer that hangs on another attribute follows it", () => {
    /* A bridge port whose TP type is 3 points at an 802.1p mapper. */
    const other = (a: number) => (a === 3 ? b(3) : undefined);
    expect(shown(47, 4, b(1, 1), { other })).toEqual({ kind: "ref", cls: 130, inst: 0x0101 });
    expect(shown(47, 4, b(1, 1))).toEqual({ kind: "id", inst: 0x0101 });
  });

  it("of a choice is a word of its list", () => {
    expect(shown(268, 3, b(3))).toEqual({ kind: "word", list: "direction", n: 3 });
    expect(shown(11, 5, b(1))).toEqual({ kind: "word", list: "admin", n: 1 });
    expect(shown(7, 2, b(1))).toEqual({ kind: "word", list: "yesNo", n: 1 });
  });

  it("of a TPID is hexadecimal, and a table's size of rows a number, not text", () => {
    expect(shown(171, 3, b(0x81, 0))).toEqual({ kind: "hex", n: 0x8100 });
    /* 00 20 is 32 rows at most: a number, not a blank between quotes. */
    expect(shown(171, 2, b(0x00, 0x20))).toEqual({ kind: "num", n: 32 });
  });

  it("of an address is written as one", () => {
    expect(shown(134, 2, b(0, 0, 0x5e, 0, 0x53, 1))).toEqual({
      kind: "mac",
      text: "00:00:5e:00:53:01",
    });
    expect(shown(134, 4, b(192, 0, 2, 1))).toEqual({ kind: "ip", text: "192.0.2.1" });
  });

  it("of text is the text, and of other bytes only the bytes", () => {
    expect(shown(256, 1, b(0x5a, 0x5a, 0x5a, 0x5a))).toEqual({ kind: "text", text: "ZZZZ" });
    expect(shown(256, 2, b(0x56, 0x31, 0x2e, 0x30, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))).toEqual({
      kind: "text",
      text: "V1.0",
    });
    expect(shown(256, 3, b(0x41, 0x42, 0x43, 0x44, 0, 0, 0, 1))).toEqual({
      kind: "text",
      text: "ABCD00000001",
    });
    expect(shown(256, 10, new Uint8Array(24))).toEqual({ kind: "raw" });
    expect(shown(262, 1, b(0, 0xff))).toEqual({ kind: "none" });
  });

  it("of a table is its rows", () => {
    expect(shown(171, 6, b(1, 2), { table: true, rows: 9 })).toEqual({ kind: "rows", n: 9 });
  });
});
