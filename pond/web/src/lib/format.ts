// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Figures for people: rates in b/s to Gb/s, sizes in KB and MB, light in dBm,
 * spans of time in days, hours and minutes, all in the reader's language.
 */

import type { Lang } from "../i18n";

const number = (lang: Lang, digits: number) =>
  new Intl.NumberFormat(lang, { minimumFractionDigits: digits, maximumFractionDigits: digits });

/** A rate in bits per second, scaled to the unit that reads best. */
export function bitrate(bps: number | null | undefined, lang: Lang): string {
  if (bps == null || !Number.isFinite(bps)) return "–";
  const units: [number, string][] = [
    [1e9, "Gb/s"],
    [1e6, "Mb/s"],
    [1e3, "kb/s"],
  ];
  for (const [scale, unit] of units) {
    if (bps >= scale) {
      const v = bps / scale;
      return `${number(lang, v >= 100 ? 0 : 1).format(v)} ${unit}`;
    }
  }
  return `${number(lang, 0).format(bps)} b/s`;
}

/** A size in bytes, as KB, MB or GB of 1024. */
export function bytes(b: number | null | undefined, lang: Lang): string {
  if (b == null || !Number.isFinite(b)) return "–";
  const units: [number, string][] = [
    [1024 ** 3, "GB"],
    [1024 ** 2, "MB"],
    [1024, "KB"],
  ];
  for (const [scale, unit] of units) {
    if (b >= scale) {
      const v = b / scale;
      return `${number(lang, v >= 100 ? 0 : 1).format(v)} ${unit}`;
    }
  }
  return `${number(lang, 0).format(b)} B`;
}

/** Optical power: microwatts to dBm, or null where there is no light. */
export function dbm(microwatts: number | null | undefined): number | null {
  if (microwatts == null || microwatts <= 0) return null;
  return 10 * Math.log10(microwatts / 1000);
}

export function dbmText(v: number | null, lang: Lang): string {
  if (v == null) return "–";
  return `${v > 0 ? "+" : ""}${number(lang, 1).format(v)} dBm`;
}

export function decimal(v: number | null | undefined, digits: number, lang: Lang): string {
  if (v == null || !Number.isFinite(v)) return "–";
  return number(lang, digits).format(v);
}

export function integer(v: number | null | undefined, lang: Lang): string {
  if (v == null || !Number.isFinite(v)) return "–";
  return number(lang, 0).format(v);
}

/** A span of time, the two largest units: "3 d 4 h", "2 h 13 min", "45 s". */
export function duration(seconds: number | null | undefined, lang: Lang): string {
  if (seconds == null || !Number.isFinite(seconds)) return "–";
  const s = Math.floor(seconds);
  const parts: [number, Intl.NumberFormatOptions["unit"]][] = [
    [Math.floor(s / 86400), "day"],
    [Math.floor((s % 86400) / 3600), "hour"],
    [Math.floor((s % 3600) / 60), "minute"],
    [s % 60, "second"],
  ];
  const first = parts.findIndex(([v]) => v > 0);
  if (first < 0) return new Intl.NumberFormat(lang, { style: "unit", unit: "second" }).format(0);
  return parts
    .slice(first, first + 2)
    .filter(([v]) => v > 0)
    .map(([v, unit]) =>
      new Intl.NumberFormat(lang, { style: "unit", unit, unitDisplay: "short" }).format(v),
    )
    .join(" ");
}

/** The time of day of a moment, hours and minutes, with seconds when asked. */
export function clock(t: number, lang: Lang, seconds = false): string {
  return new Intl.DateTimeFormat(lang, {
    hour: "2-digit",
    minute: "2-digit",
    second: seconds ? "2-digit" : undefined,
  }).format(t);
}

/** The day of a moment given in seconds since 1970. */
export function day(seconds: number, lang: Lang): string {
  return new Intl.DateTimeFormat(lang, { dateStyle: "medium" }).format(seconds * 1000);
}

/** A share as a whole percent. */
export function percent(share: number, lang: Lang): string {
  return new Intl.NumberFormat(lang, { style: "percent", maximumFractionDigits: 0 }).format(share);
}
