// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The two banks of the flash as a person sees them: system A and system B,
 * what each holds, which one runs, which the stick starts, and what can be
 * done with them.
 */

import type { Firmware } from "../api/types";

export type BankTag =
  | "running" /* the stick runs it now */
  | "default" /* the stick starts it */
  | "once" /* the next start only, on trial */
  | "trial" /* runs on trial, not kept yet */
  | "failed" /* its last trial did not come up */
  | "empty"
  | "incomplete";

export interface BankView {
  bank: number;
  letter: "A" | "B";
  version: string | null;
  time: number | null;
  tags: BankTag[];
  /** it holds a whole system, which can be started */
  usable: boolean;
}

export function bankLetter(bank: number): "A" | "B" {
  return bank === 0 ? "A" : "B";
}

export function bankViews(f: Firmware): BankView[] {
  return f.banks.map((b) => {
    const tags: BankTag[] = [];
    const usable = b.state === "valid";
    if (b.bank === f.running) tags.push("running");
    if (b.bank === f.committed) tags.push("default");
    if (b.bank === f.next && f.next !== f.committed) tags.push("once");
    if (b.bank === f.trial) tags.push("trial");
    if (b.bank === f.failed) tags.push("failed");
    if (b.state === "empty") tags.push("empty");
    if (b.state === "incomplete") tags.push("incomplete");
    return {
      bank: b.bank,
      letter: bankLetter(b.bank),
      version: usable ? b.version : null,
      time: usable ? b.time : null,
      tags,
      usable,
    };
  });
}

/** The bank an update goes into: the one that does not run; none after a start from memory. */
export function updateTarget(f: Firmware): number | null {
  return f.running === 0 || f.running === 1 ? 1 - f.running : null;
}

/** The other bank, when it holds a system that can be started. */
export function otherBank(f: Firmware): number | null {
  const target = updateTarget(f);
  if (target === null) return null;
  return f.banks.find((b) => b.bank === target)?.state === "valid" ? target : null;
}

/** Whether an upload is under way, on this page or another. */
export function uploading(f: Firmware | undefined): boolean {
  return f?.update.state === "receiving" || f?.update.state === "writing";
}
