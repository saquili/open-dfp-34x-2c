// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The few pieces the pages of System share. */

import type { ReactNode } from "react";

/** A fingerprint or a token, read character by character, broken where it must. */
export const Mono = ({ children }: { children: ReactNode }) => (
  <span className="break-all font-mono text-xs">{children}</span>
);

/** A value too long for a row of facts: its label above it, it under the label. */
export function Pair({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="grid gap-1">
      <span className="text-xs text-base-content/70">{label}</span>
      <div className="grid gap-1 text-sm">{children}</div>
    </div>
  );
}

/** A hint under a card's values: 12 px, in the secondary colour. */
export const Hint = ({ children }: { children: ReactNode }) => (
  <p className="text-xs text-base-content/70">{children}</p>
);

/**
 * Two columns of cards on a wide screen, one on a phone; each column a stack
 * of its own, so that a long card does not stretch its neighbour.
 */
export function Columns({ children, wide }: { children: ReactNode; wide?: "left" | "right" }) {
  const cols =
    wide === "left"
      ? "lg:grid-cols-[minmax(0,3fr)_minmax(0,2fr)]"
      : wide === "right"
        ? "lg:grid-cols-[minmax(0,2fr)_minmax(0,3fr)]"
        : "lg:grid-cols-2";
  return <div className={`grid shrink-0 content-start gap-4 ${cols}`}>{children}</div>;
}

/** A column of Columns: its cards one under another, at the top. */
export const Stack = ({ children }: { children: ReactNode }) => (
  <div className="grid min-w-0 content-start gap-4">{children}</div>
);
