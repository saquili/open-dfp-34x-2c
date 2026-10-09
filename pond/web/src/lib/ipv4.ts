// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* IPv4 addresses as numbers, and the networks they are in. */

/** An address as a number, or null for what is not one. */
export function ip4(s: string): number | null {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(s.trim());
  if (!m) return null;
  const o = m.slice(1).map(Number);
  return o.every((x) => x <= 255) ? o.reduce((a, x) => a * 256 + x, 0) : null;
}

/** An address as it is written. */
export function ip4s(n: number): string {
  return [24, 16, 8, 0].map((b) => Math.floor(n / 2 ** b) % 256).join(".");
}

/** The network of @cidr (an address and its length): its first and last address, or null. */
export function net4(cidr: string): { first: number; last: number; text: string } | null {
  const m = /^([^/]+)\/(\d{1,2})$/.exec(cidr.trim());
  const a = m ? ip4(m[1] ?? "") : null;
  const len = m ? Number(m[2]) : -1;
  if (a == null || len < 0 || len > 32) return null;
  const size = 2 ** (32 - len);
  const first = Math.floor(a / size) * size;
  return { first, last: first + size - 1, text: `${ip4s(first)}/${len}` };
}
