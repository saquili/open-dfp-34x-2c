// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* IPv6 addresses as numbers, and the /64 networks a delegated prefix holds. */

/** An address as a number, or null for what is not one. */
export function ip6(s: string): bigint | null {
  const text = s.trim().toLowerCase();
  if (!/^[0-9a-f:]+$/.test(text) || text.split("::").length > 2) return null;
  const [head = "", tail] = text.split("::");
  const groups = (part: string) => (part ? part.split(":") : []);
  const a = groups(head);
  const z = tail === undefined ? [] : groups(tail);
  const missing = 8 - a.length - z.length;
  if (tail === undefined ? missing !== 0 : missing < 1) return null;
  const all = [...a, ...Array<string>(tail === undefined ? 0 : missing).fill("0"), ...z];
  if (!all.every((g) => /^[0-9a-f]{1,4}$/.test(g))) return null;
  return all.reduce((n, g) => (n << 16n) | BigInt(Number.parseInt(g, 16)), 0n);
}

/** An address as it is written: its longest run of zero groups as "::" (RFC 5952). */
export function ip6s(n: bigint): string {
  const g = Array.from({ length: 8 }, (_, i) => Number((n >> BigInt(112 - 16 * i)) & 0xffffn));
  let at = -1;
  let len = 0;
  for (let i = 0; i < 8; ) {
    if (g[i] !== 0) {
      i++;
      continue;
    }
    let j = i;
    while (j < 8 && g[j] === 0) j++;
    if (j - i > len && j - i > 1) {
      at = i;
      len = j - i;
    }
    i = j;
  }
  const hex = (xs: number[]) => xs.map((x) => x.toString(16)).join(":");
  return at < 0 ? hex(g) : `${hex(g.slice(0, at))}::${hex(g.slice(at + len))}`;
}

/** A prefix of @text ("2001:db8:5100::/56"): its first address and length, or null. */
export function prefix6(text: string): { addr: bigint; len: number } | null {
  const m = /^([^/]+)\/(\d{1,3})$/.exec(text.trim());
  const a = m ? ip6(m[1] ?? "") : null;
  const len = m ? Number(m[2]) : -1;
  if (a == null || len < 0 || len > 128) return null;
  const host = (1n << BigInt(128 - len)) - 1n;
  return { addr: a & ~host, len };
}

/** How many /64 networks @text holds: 256 for a /56, 1 for a /64, 0 for none or a longer one. */
export function subnets64(text: string): number {
  const p = prefix6(text);
  return p && p.len <= 64 ? 2 ** Math.min(64 - p.len, 16) : 0;
}

/** The /64 network @id of the prefix @text, as written, or null where it has none. */
export function subnet64(text: string, id: number): string | null {
  const p = prefix6(text);
  if (!p || p.len > 64 || id < 0 || id >= subnets64(text)) return null;
  return `${ip6s(p.addr | (BigInt(id) << 64n))}/64`;
}
