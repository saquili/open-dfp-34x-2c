// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What an attribute's value means, as ITU-T G.988 defines it for the
 * attributes a reader of the MIB looks at most: a pointer, to the entity of
 * the class it points into (which may hang on another attribute of the same
 * entity, as a bridge port's on its TP type); a choice from a list, given
 * as the list's name and the number, for the panel to put in words; a TPID,
 * an address; text; a number. What none of these fits is left to its bytes.
 */

import { CLASSES } from "./omci-classes";

export type Shown =
  /** A password omcid does not give. */
  | { kind: "hidden" }
  /** A pointer to no entity (0xffff), or an Alloc-ID not given. */
  | { kind: "none" }
  | { kind: "num"; n: number }
  /** A number best read in hexadecimal, a TPID. */
  | { kind: "hex"; n: number }
  | { kind: "text"; text: string }
  /** A choice from list @list (the panel's words for it), number @n. */
  | { kind: "word"; list: string; n: number }
  /** A pointer to an entity of a known class. */
  | { kind: "ref"; cls: number; inst: number }
  /** A pointer whose class the attribute alone does not say. */
  | { kind: "id"; inst: number }
  | { kind: "mac"; text: string }
  | { kind: "ip"; text: string }
  /** A table's rows. */
  | { kind: "rows"; n: number }
  /** Only the bytes say it. */
  | { kind: "raw" };

/** Another attribute of the same entity, for a value that hangs on it. */
export type Other = (attr: number) => Uint8Array | null | undefined;

const number = (b: Uint8Array) => b.reduce((v, c) => v * 256 + c, 0);

/* The classes a TP type of a bridge port (47 attribute 3) points into. */
const BRIDGE_TP: Record<number, number> = { 1: 11, 3: 130, 4: 134, 5: 266, 6: 281, 11: 329 };
/* The classes an association type of the VLAN rules (171 attribute 1) points into. */
const ASSOCIATION: Record<number, number> = {
  0: 47,
  1: 130,
  2: 11,
  3: 134,
  5: 266,
  6: 281,
  10: 329,
};
/* The classes an interworking option (266, 281 attribute 2) has the service profile in. */
const SERVICE: Record<number, number> = { 1: 45, 6: 130 };

/* The class each pointer points into, when the attribute alone says it. */
const POINTS: Record<string, number> = {
  "47/1": 45,
  "268/2": 262,
  "268/4": 277,
  "268/5": 280,
  "268/7": 277,
  "268/9": 280,
  "266/1": 268,
  "266/7": 272,
  "281/1": 268,
  "281/7": 272,
  "277/7": 278,
  "278/1": 262,
  "278/2": 278,
  "310/2": 309,
  "47/11": 280,
  "47/12": 280,
};

/* The pointer's class from another attribute: [the one it hangs on, its classes]. */
const POINTS_BY: Record<string, [number, Record<number, number>]> = {
  "47/4": [3, BRIDGE_TP],
  "171/7": [1, ASSOCIATION],
  "266/3": [2, SERVICE],
  "281/3": [2, SERVICE],
};

/* The makers' codes of four letters: of the ONU-G and of the OLT-G. */
const FOUR_LETTERS = new Set(["256/1", "131/1"]);

/* The attributes that choose from a list, by the list's name. */
const LISTS: Record<string, string> = {
  "47/3": "tpType",
  "171/1": "association",
  "268/3": "direction",
  "266/2": "interworking",
  "281/2": "interworking",
  "256/4": "tmOption",
};

function pointsInto(cls: number, attr: number, other?: Other): number | null {
  const fixed = POINTS[`${cls}/${attr}`];
  if (fixed != null) return fixed;
  if (cls === 130 && attr >= 2 && attr <= 9) return 266;
  const by = POINTS_BY[`${cls}/${attr}`];
  if (!by) return null;
  const v = other?.(by[0]);
  return v?.length ? (by[1][number(v)] ?? null) : null;
}

/* Text: printable bytes, at least one a letter or digit, zeros only at the end. */
function textOf(b: Uint8Array): string | null {
  let end = b.length;
  while (end > 0 && b[end - 1] === 0) end--;
  const body = b.subarray(0, end);
  if (!body.length) return null;
  if (!body.every((c) => c >= 0x20 && c < 0x7f)) return null;
  if (!body.some((c) => (c >= 0x30 && c <= 0x39) || ((c | 0x20) >= 0x61 && (c | 0x20) <= 0x7a)))
    return null;
  return String.fromCharCode(...body).trim();
}

/** What the value @b of attribute @attr of class @cls means. */
export function shown(
  cls: number,
  attr: number,
  b: Uint8Array | null | undefined,
  opts: { other?: Other; table?: boolean; rows?: number } = {},
): Shown {
  if (b == null) return { kind: "hidden" };
  const name = CLASSES[cls]?.attrs[attr] ?? "";
  if (opts.table) return { kind: "rows", n: opts.rows ?? 0 };

  const list = LISTS[`${cls}/${attr}`];
  if (list && b.length) return { kind: "word", list, n: number(b) };
  if (/^administrative state$/i.test(name) && b.length === 1)
    return { kind: "word", list: "admin", n: number(b) };
  if (/^operational state$/i.test(name) && b.length === 1)
    return { kind: "word", list: "operational", n: number(b) };
  if (/^is (committed|active|valid)$/i.test(name) && b.length === 1)
    return { kind: "word", list: "yesNo", n: number(b) };

  if (/pointer/i.test(name) && b.length === 2) {
    const inst = number(b);
    if (inst === 0xffff) return { kind: "none" };
    const into = pointsInto(cls, attr, opts.other);
    return into != null ? { kind: "ref", cls: into, inst } : { kind: "id", inst };
  }
  if (cls === 262 && attr === 1) {
    const n = number(b);
    return n === 0xff || n === 0xffff ? { kind: "none" } : { kind: "num", n };
  }
  if (/TPID/.test(name) || (cls === 257 && attr === 2)) return { kind: "hex", n: number(b) };
  if (/MAC address/i.test(name) && b.length === 6)
    return { kind: "mac", text: [...b].map((c) => c.toString(16).padStart(2, "0")).join(":") };
  if (/(address|mask|gateway|DNS)/i.test(name) && !/MAC/.test(name) && b.length === 4)
    return { kind: "ip", text: [...b].join(".") };
  /* A serial number: the maker's four letters, then four bytes in hexadecimal. */
  if (cls === 256 && attr === 3 && b.length === 8) {
    const maker = textOf(b.subarray(0, 4));
    if (maker?.length === 4)
      return {
        kind: "text",
        text: maker + [...b.subarray(4)].map((c) => c.toString(16).padStart(2, "0")).join(""),
      };
  }
  if (FOUR_LETTERS.has(`${cls}/${attr}`)) {
    const text = textOf(b);
    if (text) return { kind: "text", text };
  }
  if (b.length <= 4) return { kind: "num", n: number(b) };
  const text = textOf(b);
  return text ? { kind: "text", text } : { kind: "raw" };
}
