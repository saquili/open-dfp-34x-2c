// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* TCP and UDP ports: one, or a range of them. */

/** "443" or "8000-8100" as the first and last port, or null for what is not one. */
export function readPorts(text: string): { from: number; to: number } | null {
  const m = /^\s*(\d{1,5})\s*(?:[-–]\s*(\d{1,5}))?\s*$/.exec(text);
  if (!m) return null;
  const from = Number(m[1]);
  const to = m[2] ? Number(m[2]) : from;
  return from >= 1 && to <= 65535 && from <= to ? { from, to } : null;
}

/** Ports as they are read: "443", "8000–8100". */
export const ports = (from: number, to: number) => (from === to ? String(from) : `${from}–${to}`);
