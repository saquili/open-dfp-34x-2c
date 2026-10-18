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

/*
 * The most ports the forwards' ranges moved to other ports may move
 * together, as the stick takes them (onu-config's SETTINGS_MOVED_MAX): the
 * NAT of its kernel moves no range by an offset, so its table maps each
 * port of such a range to its own, and holds 1024 of them.
 */
export const MAX_MOVED = 1024;

/**
 * The ports a forward moves to other ports: those of a range in force that
 * goes to other numbers, each protocol's apart; none for a port alone, a
 * range that keeps its numbers, or a forward kept but not in force.
 */
export function moved(f: {
  proto: "tcp" | "udp" | "both";
  ext_from: number;
  ext_to: number;
  int_port: number;
  on: boolean;
}): number {
  if (!f.on || f.ext_to <= f.ext_from || f.int_port === f.ext_from) return 0;
  return (f.ext_to - f.ext_from + 1) * (f.proto === "both" ? 2 : 1);
}
