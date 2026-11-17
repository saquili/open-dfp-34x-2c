// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The kernel's last words before the last restart (pond's value crash):
 * which record says why it stopped, the line that says it, and the text the
 * panel hands out.
 */

import type { Crash, CrashRecord } from "../api/types";

/** The record of an oops or a panic: the last one by name, as pstore numbers them from 0. */
export function kernelRecord(c: Crash | undefined): CrashRecord | undefined {
  return c?.records.filter((r) => r.kind === "dmesg").at(-1);
}

/** The console's last lines of the start before. */
export function consoleRecord(c: Crash | undefined): CrashRecord | undefined {
  return c?.records.find((r) => r.kind === "console");
}

/* A line of the kernel's log without its level and time: "<0>[   23.10] text". */
const PREFIX = /^<\d+>\[\s*\d+\.\d+\]\s*/;

/* The line pstore begins a record with: "Panic#1 Part1". */
const PSTORE_HEADER = /^[A-Za-z]+#\d+ Part\d+$/;

/*
 * What the kernel prints where it finds itself broken, before the panic it
 * may lead to; on MIPS a bad address is told with the CPU in front.
 */
const TROUBLE =
  /^(CPU \d+ )?(BUG: |kernel BUG at |Oops|Unable to handle kernel|stack-protector: |detected buffer overflow|usercopy: |list_(add|del) corruption|Kernel panic - not syncing: )/;

/**
 * The line that says why the kernel stopped: the first trouble it reported,
 * which a panic that follows an oops only repeats as "Fatal exception", or,
 * failing one, the record's last line.
 */
export function crashLine(text: string): string {
  const lines = text
    .split("\n")
    .map((l) => l.replace(PREFIX, "").trim())
    .filter((l) => !PSTORE_HEADER.test(l));
  return lines.find((l) => TROUBLE.test(l)) ?? lines.filter(Boolean).at(-1) ?? "";
}

/**
 * The last @n lines of a record that hold text, without the level the
 * kernel gave each, "<6>", and without ramoops's note of its ECC.
 */
export function lastLines(text: string, n: number): string {
  const lines = text.split("\n").filter((l) => l.trim() && !l.startsWith("ECC: "));
  return lines
    .slice(-n)
    .map((l) => l.replace(/^<\d+>/, ""))
    .join("\n");
}

/* Seconds since 1970 before which the stick's clock had not been set: 2020-09-13. */
const CLOCK_SET = 1_600_000_000;

/** When a record was written, in @lang's words, or null when the clock had not been set then. */
export function crashWhen(timeS: number | null, lang: string): string | null {
  if (timeS == null || timeS < CLOCK_SET) return null;
  return new Intl.DateTimeFormat(lang, { dateStyle: "medium", timeStyle: "short" }).format(
    timeS * 1000,
  );
}

/** Every record in one text, each under its name, for a file to keep or send. */
export function crashText(c: Crash): string {
  return c.records.map((r) => `===== ${r.name}\n${r.text.replace(/\n?$/, "\n")}`).join("\n");
}
