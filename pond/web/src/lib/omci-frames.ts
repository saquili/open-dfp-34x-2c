// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The conversation with the OLT, from the frames omcid keeps (pond's value
 * "omci_frames"): each frame taken apart as ITU-T G.988 lays out a baseline
 * message (11.2 and Annex A), and the frames put together the way a person
 * reads them: an OLT's request and the stick's answer in one exchange, what
 * the stick says on its own (attribute changes, alarms) as an exchange of
 * its own, and a run of steps of one task (the MIB upload, a table read, a
 * software download) as one.
 */

import type { OmciFrame } from "../api/types";
import { CLASSES } from "./omci-classes";

/** What omcid did with a frame (its enum omci_ctl_tap_verdict). */
export type Fate =
  | "parse"
  | "mic"
  | "arak"
  | "replay"
  | "done"
  | "sent"
  | "action"
  | "unknownMe"
  | "unhandled";

const FATES: Fate[] = [
  "parse",
  "mic",
  "arak",
  "replay",
  "done",
  "sent",
  "action",
  "unknownMe",
  "unhandled",
];

/** The message types (G.988 Table 11.2.2-1). */
export const MT = {
  create: 4,
  delete: 6,
  set: 8,
  get: 9,
  getAllAlarms: 11,
  getAllAlarmsNext: 12,
  mibUpload: 13,
  mibUploadNext: 14,
  mibReset: 15,
  alarm: 16,
  avc: 17,
  test: 18,
  startDownload: 19,
  downloadSection: 20,
  endDownload: 21,
  activate: 22,
  commit: 23,
  syncTime: 24,
  reboot: 25,
  getNext: 26,
  testResult: 27,
  getCurrentData: 28,
} as const;

/** A frame taken apart. */
export interface Frame {
  seq: number;
  /** When it passed, in milliseconds since 1970. */
  at: number;
  fate: Fate;
  /** Received from the OLT; otherwise sent by the stick. */
  fromOlt: boolean;
  tci: number;
  mt: number;
  /** The sender wants an answer. */
  ar: boolean;
  /** This is an answer. */
  ak: boolean;
  cls: number;
  inst: number;
  contents: Uint8Array;
  /** omcid zeroed the logical password in it. */
  masked: boolean;
}

/* A byte of a frame; one past its end reads as zero. */
const byte = (b: Uint8Array, at: number) => b[at] ?? 0;
const be16 = (b: Uint8Array, at: number) => (byte(b, at) << 8) | byte(b, at + 1);

export function frameOf(f: OmciFrame): Frame {
  const b = new Uint8Array(48);
  for (let i = 0; i < 48 && 2 * i < f.frame.length; i++)
    b[i] = Number.parseInt(f.frame.slice(2 * i, 2 * i + 2), 16);
  const fate = FATES[f.verdict] ?? "unhandled";
  return {
    seq: f.seq,
    at: f.sec * 1000 + f.ms,
    fate,
    fromOlt: fate !== "sent",
    tci: be16(b, 0) & 0x7fff,
    mt: byte(b, 2) & 0x1f,
    ar: (byte(b, 2) & 0x40) !== 0,
    ak: (byte(b, 2) & 0x20) !== 0,
    cls: be16(b, 4),
    inst: be16(b, 6),
    contents: b.slice(8, 40),
    masked: f.masked,
  };
}

/** The attributes of a mask, attribute 1 the most significant bit. */
export function maskAttrs(mask: number): number[] {
  const out: number[] = [];
  for (let n = 1; n <= 16; n++) if (mask & (0x8000 >> (n - 1))) out.push(n);
  return out;
}

/** An attribute a frame carries: its value, or none (a Get asks for it). */
export interface AttrValue {
  attr: number;
  bytes: Uint8Array | null;
  /** The value is a table's size in bytes, not its rows (a Get of a table). */
  tableSize?: boolean;
}

/*
 * The values of the attributes of @mask, one after another from @at, as a
 * class lays them out; what runs past @end is cut off, and an attribute the
 * table does not know ends the reading.
 */
function valuesOf(
  cls: number,
  mask: number,
  b: Uint8Array,
  at: number,
  end: number,
  tableSizes = false,
): AttrValue[] {
  const c = CLASSES[cls];
  const out: AttrValue[] = [];
  for (const attr of maskAttrs(mask)) {
    const table = tableSizes && c?.tables.includes(attr);
    const size = table ? 4 : c?.sizes[attr - 1];
    if (size == null || at >= end) {
      out.push({ attr, bytes: null });
      continue;
    }
    out.push({ attr, bytes: b.slice(at, Math.min(at + size, end)), tableSize: table || undefined });
    at += size;
  }
  return out;
}

/** The attributes a Create gives, from its contents. */
function createValues(cls: number, b: Uint8Array): AttrValue[] {
  const c = CLASSES[cls];
  const out: AttrValue[] = [];
  let at = 0;
  for (const attr of c?.create ?? []) {
    const size = c?.sizes[attr - 1] ?? b.length;
    if (at + size > b.length) break;
    out.push({ attr, bytes: b.slice(at, at + size) });
    at += size;
  }
  return out;
}

/*
 * The results of an answer (G.988 11.2.6): 0 done; 1 a processing error;
 * 2 not supported; 3 a parameter error; 4 an unknown class; 5 an unknown
 * instance; 6 busy; 7 the instance exists; 9 some attributes failed.
 */
export type Result = number;

/** What a frame says, its contents read by its message type. */
export interface Said {
  /** The entity it is about; for a MIB upload step, the one the step carries. */
  cls: number;
  inst: number;
  attrs: AttrValue[];
  /** The result of an answer. */
  result?: Result;
  /** The steps a MIB upload or a Get of all alarms announces. */
  steps?: number;
  /** The step of a MIB upload next, a Get next or a download section. */
  step?: number;
}

export function said(f: Frame): Said {
  const c = f.contents;
  const base: Said = { cls: f.cls, inst: f.inst, attrs: [] };
  switch (f.mt) {
    case MT.create:
      return f.ak ? { ...base, result: byte(c, 0) } : { ...base, attrs: createValues(f.cls, c) };
    case MT.set:
      return f.ak
        ? { ...base, result: byte(c, 0) }
        : { ...base, attrs: valuesOf(f.cls, be16(c, 0), c, 2, 32) };
    case MT.get:
    case MT.getCurrentData:
      return f.ak
        ? { ...base, result: byte(c, 0), attrs: valuesOf(f.cls, be16(c, 1), c, 3, 28, true) }
        : { ...base, attrs: maskAttrs(be16(c, 0)).map((attr) => ({ attr, bytes: null })) };
    case MT.getNext:
      return f.ak
        ? {
            ...base,
            result: byte(c, 0),
            attrs: maskAttrs(be16(c, 1)).map((attr) => ({ attr, bytes: null })),
          }
        : {
            ...base,
            attrs: maskAttrs(be16(c, 0)).map((attr) => ({ attr, bytes: null })),
            step: be16(c, 2),
          };
    case MT.mibUpload:
    case MT.getAllAlarms:
      return f.ak ? { ...base, steps: be16(c, 0) } : base;
    case MT.mibUploadNext:
      return f.ak
        ? {
            cls: be16(c, 0),
            inst: be16(c, 2),
            attrs: valuesOf(be16(c, 0), be16(c, 4), c, 6, 32),
          }
        : { ...base, step: be16(c, 0) };
    case MT.avc:
      return { ...base, attrs: valuesOf(f.cls, be16(c, 0), c, 2, 32) };
    case MT.downloadSection:
      return f.ak ? { ...base, result: byte(c, 0) } : { ...base, step: byte(c, 0) };
    default:
      return f.ak ? { ...base, result: byte(c, 0) } : base;
  }
}

/** The kinds of exchange the list shows, a run of steps as one. */
export type ExchangeKind = "request" | "onu" | "upload" | "tableRead" | "download" | "alarms";

/** An exchange: an OLT's request and the answer, or what the stick said on its own. */
export interface Exchange {
  /** The first frame's number: the exchange's key. */
  seq: number;
  at: number;
  kind: ExchangeKind;
  mt: number;
  cls: number;
  inst: number;
  /** For a request, what it gave or asked for, with the answer's values for a Get. */
  attrs: AttrValue[];
  result?: Result;
  /** Milliseconds from the request to the answer. */
  took?: number;
  /** The request is still unanswered. */
  waiting?: boolean;
  /** omcid did not carry it out, and why. */
  rejected?: Fate;
  /** For a run of steps: the first and the last, and the steps announced. */
  from?: number;
  to?: number;
  of?: number;
  masked?: boolean;
}

/*
 * The message types that change nothing and come again and again: the
 * reads, and the tests an OLT runs to read the optical levels, with their
 * results.
 */
const READS: number[] = [
  MT.get,
  MT.getCurrentData,
  MT.getNext,
  MT.getAllAlarms,
  MT.getAllAlarmsNext,
  MT.test,
  MT.testResult,
];

export function isRead(e: Exchange): boolean {
  return READS.includes(e.mt) || e.kind === "tableRead" || e.kind === "alarms";
}

/** The message types that change the MIB when they are carried out. */
export const CHANGES: number[] = [MT.create, MT.delete, MT.set, MT.mibReset];

/*
 * The exchanges of a list of frames in their order. A request waits for the
 * answer with its TCI; a step of a run joins the run before it when that is
 * of the same task and entity. A frame omcid did not carry out is an
 * exchange of its own, rejected.
 */
export function exchanges(frames: Frame[]): Exchange[] {
  const out: Exchange[] = [];
  const open = new Map<number, Exchange>();
  const last = () => out[out.length - 1];

  for (const f of frames) {
    const s = said(f);

    /* An answer: to the request with its TCI that waits. */
    if (!f.fromOlt && f.ak) {
      const e = open.get(f.tci);
      if (!e) continue;
      open.delete(f.tci);
      e.waiting = undefined;
      e.took = Math.max(0, f.at - e.at);
      if (s.result != null && e.kind === "request") e.result = s.result;
      if (f.masked) e.masked = true;
      if (e.kind === "request" && (f.mt === MT.get || f.mt === MT.getCurrentData))
        e.attrs = s.attrs;
      if (e.kind === "upload" && f.mt === MT.mibUpload) e.of = s.steps;
      if (e.kind === "alarms" && f.mt === MT.getAllAlarms) e.of = s.steps;
      if (f.mt === MT.downloadSection && s.result) e.result = s.result;
      continue;
    }

    /* What the stick says on its own. */
    if (!f.fromOlt) {
      out.push({
        seq: f.seq,
        at: f.at,
        kind: "onu",
        mt: f.mt,
        cls: s.cls,
        inst: s.inst,
        attrs: s.attrs,
        masked: f.masked || undefined,
      });
      continue;
    }

    const rejected = f.fate === "done" || f.fate === "replay" ? undefined : f.fate;

    /* A step of a run joins the run before it. */
    const prev = last();
    const run =
      f.mt === MT.mibUploadNext
        ? "upload"
        : f.mt === MT.getNext
          ? "tableRead"
          : f.mt === MT.downloadSection
            ? "download"
            : f.mt === MT.getAllAlarmsNext
              ? "alarms"
              : null;
    if (
      run &&
      !rejected &&
      prev &&
      prev.kind === run &&
      !prev.rejected &&
      (run === "upload" || run === "alarms" || (prev.cls === f.cls && prev.inst === f.inst))
    ) {
      if (prev.from == null) prev.from = s.step;
      prev.to = s.step ?? prev.to;
      prev.waiting = true;
      if (f.ar) open.set(f.tci, prev);
      continue;
    }

    const e: Exchange = {
      seq: f.seq,
      at: f.at,
      kind:
        f.mt === MT.mibUpload ? "upload" : f.mt === MT.getAllAlarms ? "alarms" : (run ?? "request"),
      mt: f.mt,
      cls: f.cls,
      inst: f.inst,
      attrs: s.attrs,
      rejected,
      masked: f.masked || undefined,
    };
    if (run) {
      e.from = s.step;
      e.to = s.step;
    }
    if (f.mt === MT.mibUpload) e.from = e.to = undefined;
    out.push(e);
    if (f.ar && !rejected) {
      e.waiting = true;
      open.set(f.tci, e);
    }
  }
  return out;
}

/*
 * When each attribute of each entity last changed, from the exchanges:
 * a Create, a Set the stick carried out, or a change it said itself; a
 * Delete forgets the entity, and a MIB reset all of them. The key is
 * "class/instance"; the attribute 0 is the entity's creation.
 */
export interface Change {
  at: number;
  /** The OLT set it; otherwise the stick changed it. */
  byOlt: boolean;
  mt: number;
  seq: number;
}

export function lastChanges(list: Exchange[]): Map<string, Map<number, Change>> {
  const out = new Map<string, Map<number, Change>>();
  for (const e of list) {
    if (e.rejected || e.waiting) continue;
    const done = e.kind === "onu" || e.result === 0;
    if (!done) continue;
    if (e.kind === "request" && e.mt === MT.mibReset) {
      out.clear();
      continue;
    }
    if (e.kind === "request" && e.mt === MT.delete) {
      out.delete(`${e.cls}/${e.inst}`);
      continue;
    }
    if (!(e.kind === "onu" ? e.mt === MT.avc : e.mt === MT.create || e.mt === MT.set)) continue;
    const key = `${e.cls}/${e.inst}`;
    let m = out.get(key);
    if (!m) {
      m = new Map();
      out.set(key, m);
    }
    const change: Change = { at: e.at, byOlt: e.kind !== "onu", mt: e.mt, seq: e.seq };
    if (e.mt === MT.create) m.set(0, change);
    for (const a of e.attrs) m.set(a.attr, change);
  }
  return out;
}
