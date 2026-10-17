// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import type { OmciFrame } from "../api/types";
import { conversation } from "../mocks/conversation";
import { OMCI } from "../mocks/omci";
import { exchanges, frameOf, isRead, lastChanges, MT, maskAttrs, said } from "./omci-frames";

/* A frame of the given TCI, type byte, entity and contents. */
function raw(
  seq: number,
  verdict: number,
  tci: number,
  type: number,
  cls: number,
  inst: number,
  contents: number[] = [],
  at = 1_000_000,
): OmciFrame {
  const b = new Uint8Array(48);
  b.set([tci >> 8, tci & 0xff, type, 0x0a, cls >> 8, cls & 0xff, inst >> 8, inst & 0xff]);
  b.set(contents, 8);
  return {
    seq,
    sec: Math.floor(at / 1000),
    ms: at % 1000,
    verdict,
    type: type & 0x1f,
    masked: false,
    frame: [...b].map((c) => c.toString(16).padStart(2, "0")).join(""),
  };
}

const DONE = 4;
const SENT = 5;

describe("a frame", () => {
  it("is taken apart as G.988 lays it out", () => {
    const f = frameOf(raw(7, DONE, 0x8123, 0x48, 47, 0x0101, [0x30, 0], 1_792_270_000_123));
    expect(f).toMatchObject({
      seq: 7,
      at: 1_792_270_000_123,
      fate: "done",
      fromOlt: true,
      tci: 0x0123,
      mt: MT.set,
      ar: true,
      ak: false,
      cls: 47,
      inst: 0x0101,
    });
  });

  it("names the attributes of a mask, attribute 1 first", () => {
    expect(maskAttrs(0x8020)).toEqual([1, 11]);
  });

  it("gives a Set's values by the sizes of its class", () => {
    /* A bridge port's port number (1 byte) and TP type (1 byte). */
    const s = said(frameOf(raw(0, DONE, 1, 0x48, 47, 1, [0x60, 0x00, 0x02, 0x03])));
    expect(s.attrs).toEqual([
      { attr: 2, bytes: new Uint8Array([2]) },
      { attr: 3, bytes: new Uint8Array([3]) },
    ]);
  });

  it("gives a Get's answer, a table by its size", () => {
    /* The VLAN rules' input TPID and their table, whose size is four bytes. */
    const s = said(frameOf(raw(0, SENT, 1, 0x29, 171, 1, [0, 0x24, 0, 0x81, 0, 0, 0, 0, 0x40])));
    expect(s.result).toBe(0);
    expect(s.attrs).toEqual([
      { attr: 3, bytes: new Uint8Array([0x81, 0]) },
      { attr: 6, bytes: new Uint8Array([0, 0, 0, 0x40]), tableSize: true },
    ]);
  });

  it("gives a Create's values in its class's order", () => {
    const s = said(frameOf(raw(0, DONE, 1, 0x44, 268, 1, [0x01, 0x02, 0x80, 0x00, 0x03])));
    expect(s.attrs.slice(0, 3)).toEqual([
      { attr: 1, bytes: new Uint8Array([1, 2]) },
      { attr: 2, bytes: new Uint8Array([0x80, 0]) },
      { attr: 3, bytes: new Uint8Array([3]) },
    ]);
  });

  it("gives the entity a MIB upload step carries", () => {
    const s = said(
      frameOf(raw(0, SENT, 1, 0x2e, 2, 0, [1, 0, 0, 0, 0x80, 0, 0x5a, 0x5a, 0x5a, 0x5a])),
    );
    expect(s.cls).toBe(256);
    expect(s.attrs[0]).toEqual({ attr: 1, bytes: new Uint8Array([0x5a, 0x5a, 0x5a, 0x5a]) });
  });
});

describe("the exchanges", () => {
  it("pair a request with the answer of its TCI", () => {
    const list = exchanges(
      [
        raw(0, DONE, 5, 0x48, 47, 1, [0x20, 0, 3], 1000),
        raw(1, SENT, 5, 0x28, 47, 1, [0], 1004),
      ].map(frameOf),
    );
    expect(list).toHaveLength(1);
    expect(list[0]).toMatchObject({ kind: "request", mt: MT.set, result: 0, took: 4 });
    expect(list[0]?.waiting).toBeUndefined();
  });

  it("keep a request without an answer waiting, and one omcid refused as rejected", () => {
    const list = exchanges(
      [raw(0, DONE, 5, 0x49, 256, 0, [0x02, 0]), raw(1, 1, 6, 0x49, 256, 0, [0x02, 0])].map(
        frameOf,
      ),
    );
    expect(list[0]?.waiting).toBe(true);
    expect(list[1]).toMatchObject({ rejected: "mic" });
    expect(list[1]?.waiting).toBeUndefined();
  });

  it("put a MIB upload and its steps in one", () => {
    const list = exchanges(
      [
        raw(0, DONE, 1, 0x4d, 2, 0),
        raw(1, SENT, 1, 0x2d, 2, 0, [0, 2]),
        raw(2, DONE, 2, 0x4e, 2, 0, [0, 0]),
        raw(3, SENT, 2, 0x2e, 2, 0, [1, 0, 0, 0, 0x80, 0, 1, 2, 3, 4]),
        raw(4, DONE, 3, 0x4e, 2, 0, [0, 1]),
        raw(5, SENT, 3, 0x2e, 2, 0, [1, 1, 0, 0, 0x80, 0]),
      ].map(frameOf),
    );
    expect(list).toHaveLength(1);
    expect(list[0]).toMatchObject({ kind: "upload", from: 0, to: 1, of: 2 });
    expect(list[0]?.waiting).toBeUndefined();
  });

  it("give what the stick says on its own as its own", () => {
    const list = exchanges([raw(0, SENT, 0, 0x11, 11, 1, [0x04, 0, 0])].map(frameOf));
    expect(list[0]).toMatchObject({ kind: "onu", mt: MT.avc, cls: 11 });
    expect(list[0]?.attrs[0]?.attr).toBe(6);
  });

  it("tell reads from changes", () => {
    const list = exchanges(
      [raw(0, DONE, 1, 0x49, 256, 0, [0x02, 0]), raw(1, DONE, 2, 0x48, 256, 0, [0x02, 0, 1])].map(
        frameOf,
      ),
    );
    expect(list.map(isRead)).toEqual([true, false]);
    /* A test of the optical levels and its result come again and again too. */
    const tests = exchanges(
      [raw(0, DONE, 3, 0x52, 263, 0x8001), raw(1, SENT, 3, 0x1b, 263, 0x8001)].map(frameOf),
    );
    expect(tests.map(isRead)).toEqual([true, true]);
  });

  it("of the mock stick: the services set up, one Create refused, and the reads after", () => {
    const v = conversation(OMCI, 10_000_000, 30 * 60_000, 0);
    const list = exchanges(v.frames.map(frameOf));
    expect(list[0]).toMatchObject({ kind: "request", mt: MT.mibReset, result: 0 });
    expect(list[1]).toMatchObject({ kind: "upload" });
    expect(list.some((e) => e.mt === MT.create && e.result === 7)).toBe(true);
    expect(list.some((e) => e.rejected === "parse")).toBe(true);
  });
});

describe("the last changes", () => {
  it("are the Creates and Sets carried out, and the stick's own", () => {
    const list = exchanges(
      [
        raw(0, DONE, 1, 0x44, 262, 1, [], 1000),
        raw(1, SENT, 1, 0x24, 262, 1, [0], 1003),
        raw(2, DONE, 2, 0x48, 262, 1, [0x80, 0, 1, 2], 2000),
        raw(3, SENT, 2, 0x28, 262, 1, [0], 2003),
        raw(4, DONE, 3, 0x48, 262, 1, [0x20, 0, 1], 3000),
        raw(5, SENT, 3, 0x28, 262, 1, [3], 3003),
        raw(6, SENT, 0, 0x11, 11, 1, [0x04, 0, 0], 4000),
      ].map(frameOf),
    );
    const c = lastChanges(list);
    expect(c.get("262/1")?.get(0)).toMatchObject({ at: 1000, mt: MT.create });
    expect(c.get("262/1")?.get(1)).toMatchObject({ at: 2000, byOlt: true });
    expect(c.get("262/1")?.has(3)).toBe(false);
    expect(c.get("11/1")?.get(6)).toMatchObject({ at: 4000, byOlt: false });
  });

  it("are forgotten for a deleted entity, and all of them at a MIB reset", () => {
    const list = exchanges(
      [
        raw(0, DONE, 1, 0x48, 262, 1, [0x80, 0, 1, 2]),
        raw(1, SENT, 1, 0x28, 262, 1, [0]),
        raw(2, DONE, 2, 0x46, 262, 1),
        raw(3, SENT, 2, 0x26, 262, 1, [0]),
      ].map(frameOf),
    );
    expect(lastChanges(list).has("262/1")).toBe(false);
  });
});
