// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The conversation with the OLT of the mock stick, as pond's value
 * "omci_frames" carries it: the OLT resets the MIB and uploads it, creates
 * the services' entities of the mock MIB (mocks/omci.ts) and sets the
 * Alloc-IDs, sends one Create twice and one frame the stick cannot read,
 * then reads the unit every minute; the stick says once that its port came
 * up. The frames are laid out as ITU-T G.988 lays out a baseline message,
 * and each comes at its own time: a read gives the ones whose time has come.
 */

import type { OmciFrame, OmciFrames, OmciValue } from "../api/types";
import { CLASSES } from "../lib/omci-classes";

/* The fates omcid gives (its enum omci_ctl_tap_verdict). */
const PARSE = 0;
const DONE = 4;
const SENT = 5;

/* A frame: its TCI, type byte, entity and contents, in hexadecimal. */
function hexFrame(
  tci: number,
  type: number,
  cls: number,
  inst: number,
  contents: number[],
): string {
  const b = new Uint8Array(48);
  b[0] = tci >> 8;
  b[1] = tci & 0xff;
  b[2] = type;
  b[3] = 0x0a;
  b[4] = cls >> 8;
  b[5] = cls & 0xff;
  b[6] = inst >> 8;
  b[7] = inst & 0xff;
  b.set(contents.slice(0, 32), 8);
  b[43] = 0x28;
  return [...b].map((c) => c.toString(16).padStart(2, "0")).join("");
}

const be16 = (v: number) => [v >> 8, v & 0xff];
const bytes = (hex: string | null | undefined, n: number) => {
  const out: number[] = [];
  for (let i = 0; i < n; i++)
    out.push(hex ? Number.parseInt(hex.slice(2 * i, 2 * i + 2), 16) || 0 : 0);
  return out;
};
const maskOf = (attrs: number[]) => attrs.reduce((m, a) => m | (0x8000 >> (a - 1)), 0);

/* The classes the OLT creates, in the order it creates them. */
const CREATED = [45, 280, 272, 268, 266, 281, 130, 47, 84, 171, 309, 310];

type Me = OmciValue["mes"][number];

interface Step {
  /** Milliseconds after the start. */
  after: number;
  verdict: number;
  type: number;
  hex: string;
}

/* The frames of the conversation, in order, each with its time from the start. */
function steps(mib: OmciValue): Step[] {
  const out: Step[] = [];
  let tci = 0x100;
  let t = 0;
  const ask = (type: number, cls: number, inst: number, contents: number[], verdict = DONE) => {
    tci++;
    out.push({
      after: t,
      verdict,
      type: type & 0x1f,
      hex: hexFrame(tci, type, cls, inst, contents),
    });
    return tci;
  };
  const answer = (
    id: number,
    type: number,
    cls: number,
    inst: number,
    contents: number[],
    ms = 3,
  ) => {
    t += ms;
    out.push({
      after: t,
      verdict: SENT,
      type: type & 0x1f,
      hex: hexFrame(id, type, cls, inst, contents),
    });
  };
  const values = (me: Me, attrs: number[]) =>
    attrs.flatMap((a) => bytes(me.attrs?.[a], CLASSES[me.class]?.sizes[a - 1] ?? 0));

  /* The MIB reset and the upload of what the stick made itself. */
  answer(ask(0x4f, 2, 0, []), 0x2f, 2, 0, [0]);
  t += 400;
  const own = mib.mes.filter((m) => !CREATED.includes(m.class));
  answer(ask(0x4d, 2, 0, []), 0x2d, 2, 0, be16(own.length));
  own.forEach((me, i) => {
    t += 4;
    const attrs = Object.keys(me.attrs ?? {})
      .map(Number)
      .filter((a) => !CLASSES[me.class]?.tables.includes(a));
    const fit: number[] = [];
    let room = 26;
    for (const a of attrs) {
      const size = CLASSES[me.class]?.sizes[a - 1] ?? 99;
      if (size > room) break;
      fit.push(a);
      room -= size;
    }
    answer(ask(0x4e, 2, 0, be16(i)), 0x2e, 2, 0, [
      ...be16(me.class),
      ...be16(me.instance),
      ...be16(maskOf(fit)),
      ...values(me, fit),
    ]);
  });

  /* The services: each entity created, then the Alloc-IDs. */
  t += 900;
  let again: Me | undefined;
  for (const cls of CREATED) {
    for (const me of mib.mes.filter((m) => m.class === cls)) {
      t += 6;
      const create = CLASSES[cls]?.create ?? [];
      answer(
        ask(0x44, cls, me.instance, values(me, create)),
        0x24,
        cls,
        me.instance,
        [0, 0, 0, 0, 0],
      );
      again ??= cls === 171 ? me : undefined;
    }
  }
  if (again) {
    t += 6;
    const create = CLASSES[171]?.create ?? [];
    answer(
      ask(0x44, 171, again.instance, values(again, create)),
      0x24,
      171,
      again.instance,
      [7, 0, 0, 0, 0],
    );
  }
  for (const me of mib.mes.filter((m) => m.class === 262)) {
    t += 5;
    answer(
      ask(0x48, 262, me.instance, [...be16(maskOf([1])), ...values(me, [1])]),
      0x28,
      262,
      me.instance,
      [0, 0, 0, 0, 0],
    );
  }

  /* A frame the stick cannot read, and its port coming up. */
  t += 2000;
  ask(0x48, 256, 0, [], PARSE);
  t += 15_000;
  const uni = mib.mes.find((m) => m.class === 11);
  if (uni) {
    out.push({
      after: t,
      verdict: SENT,
      type: 17,
      hex: hexFrame(0, 0x11, 11, uni.instance, [...be16(maskOf([6])), 0]),
    });
  }

  /* Then the unit read every minute, for a day. */
  const unit = mib.mes.find((m) => m.class === 256);
  for (let i = 0; unit && i < 1440; i++) {
    t += 60_000;
    const attrs = [7, 8];
    answer(
      ask(0x49, 256, 0, be16(maskOf(attrs))),
      0x29,
      256,
      0,
      [0, ...be16(maskOf(attrs)), ...values(unit, attrs)],
      2,
    );
  }
  return out;
}

/*
 * The frames whose time has come, the conversation having started @ago
 * milliseconds before @now, from number @since on, at most 200 of them, with
 * the last 1024 kept, as omcid answers.
 */
export function conversation(mib: OmciValue, now: number, ago: number, since: number): OmciFrames {
  const start = now - ago;
  const all = steps(mib).filter((s) => s.after <= ago);
  const head = all.length;
  const first = Math.max(0, head - 1024);
  const from = since < first || since > head ? first : since;
  const frames: OmciFrame[] = all.slice(from, from + 200).map((s, i) => {
    const at = start + s.after;
    return {
      seq: from + i,
      sec: Math.floor(at / 1000),
      ms: at % 1000,
      verdict: s.verdict,
      type: s.type,
      masked: false,
      frame: s.hex,
    };
  });
  return { head, first, frames };
}
