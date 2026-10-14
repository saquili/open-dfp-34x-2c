// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The stick's clock, in words. timed sets it from the host, by PTP when the
 * host has a master and by NTP otherwise, and from the date the OLT gives when
 * the host gives none; until then it reads 1970.
 */

import type { Clock } from "../api/types";
import type { Key, Lang } from "../i18n";
import type { Tone } from "./state";

/* timed writes its state every 10 s: three times that without one is a daemon that stopped. */
const STALE_S = 30;

/** Whether the daemon's state is old: it stopped writing it. */
export function stale(c: Clock): boolean {
  return c.updated_s != null && c.now_s - c.updated_s > STALE_S;
}

/** What the clock's state means to a person, and its tone. */
export function clockState(c: Clock): [Key, Tone] {
  if (c.state == null || stale(c)) return ["system.clockNoDaemon", "bad"];
  switch (c.state) {
    case "locked":
      return ["system.clockLocked", "good"];
    case "syncing":
      return ["system.clockSyncing", "info"];
    case "set":
      return ["system.clockSet", "info"];
    case "holdover":
      return ["system.clockHoldover", "warn"];
    default:
      return ["system.clockUnset", "bad"];
  }
}

/** The source in use, in words. */
export function sourceKey(c: Clock): Key {
  switch (c.source) {
    case "ptp":
      return "system.clockFromPtp";
    case "ntp":
      return "system.clockFromNtp";
    case "olt":
      return "system.clockFromOlt";
    default:
      return "system.clockFromNone";
  }
}

/** A span of nanoseconds in the unit that reads best: "12 µs", "3,4 ms", "1,2 s". */
export function spanText(ns: number | null | undefined, lang: Lang): string {
  if (ns == null || !Number.isFinite(ns)) return "–";
  const a = Math.abs(ns);
  const [v, unit] =
    a < 1e3 ? [a, "ns"] : a < 1e6 ? [a / 1e3, "µs"] : a < 1e9 ? [a / 1e6, "ms"] : [a / 1e9, "s"];
  const digits = v < 10 && unit !== "ns" ? 1 : 0;
  const text = new Intl.NumberFormat(lang, {
    minimumFractionDigits: digits,
    maximumFractionDigits: digits,
  }).format(v);
  // A space that does not break: "48 µs" stays on one line.
  return `${text}\u00a0${unit}`;
}

/** A frequency correction in parts per million: "+5,12 ppm". */
export function ppmText(ppb: number | null | undefined, lang: Lang): string {
  if (ppb == null || !Number.isFinite(ppb)) return "–";
  const text = new Intl.NumberFormat(lang, {
    minimumFractionDigits: 2,
    maximumFractionDigits: 2,
  }).format(Math.abs(ppb) / 1000);
  return `${ppb < 0 ? "−" : "+"}${text}\u00a0ppm`;
}

/** A signed offset: "+12 µs", "−3,4 ms"; the local clock less the source's. */
export function offsetText(ns: number | null | undefined, lang: Lang): string {
  if (ns == null || !Number.isFinite(ns)) return "–";
  return `${ns < 0 ? "−" : "+"}${spanText(ns, lang)}`;
}
