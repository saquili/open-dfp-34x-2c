// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What an OLT sets up on a line with internet, a phone and television, as
 * pond's value "omci" carries it, with made-up names and numbers: one
 * bridge; the host's port; an 802.1p mapper per service, internet on two
 * channels by priority; a downstream broadcast channel for all three; a
 * multicast channel for television; rules that translate the host's VLANs
 * 410, 420 and 421 to 100, 200 and 300 on the fibre and drop the rest.
 */

import type { OmciValue } from "../api/types";

const hexOf = (v: number, n: number) =>
  v
    .toString(16)
    .padStart(2 * n, "0")
    .slice(-2 * n);
const u8 = (v: number) => hexOf(v, 1);
const u16 = (v: number) => hexOf(v, 2);
const u32 = (v: number) => hexOf(v >>> 0, 4);
const str = (s: string, n: number) =>
  [...s.padEnd(n, "\0").slice(0, n)].map((c) => hexOf(c.charCodeAt(0), 1)).join("");
const ip = (s: string) =>
  s
    .split(".")
    .map((x) => u8(Number(x)))
    .join("");

type Attrs = Record<number, string | null>;
const mes: OmciValue["mes"] = [];
function me(cls: number, instance: number, attrs: Attrs, tables: number[] = []) {
  mes.push({
    class: cls,
    instance,
    attrs: Object.fromEntries(Object.entries(attrs).map(([k, v]) => [k, v])),
    tables,
  });
}

/* An extended VLAN rule: filter outer and inner tag, ethertype, tags removed, tags added. */
function rule(
  f: { op: number; ov: number; ip: number; iv: number },
  remove: number,
  t: { op: number; ov: number; ip: number; iv: number },
) {
  const w1 = ((f.op << 28) | (f.ov << 15)) >>> 0;
  const w2 = ((f.ip << 28) | (f.iv << 15)) >>> 0;
  const w3 = ((remove << 30) | (t.op << 16) | (t.ov << 3)) >>> 0;
  const w4 = ((t.ip << 16) | (t.iv << 3)) >>> 0;
  return u32(w1) + u32(w2) + u32(w3) + u32(w4);
}
const NO = 15; /* no such tag */
const VANY = 4096;
const DEF = 14;
/* A single-tagged frame of VLAN @from, priority @p, re-tagged @to keeping its priority. */
const translate = (from: number, p: number, to: number) =>
  rule({ op: NO, ov: VANY, ip: p, iv: from }, 1, { op: NO, ov: 0, ip: 8, iv: to });

me(256, 0, {
  1: str("ZZZZ", 4),
  2: str("V1.0", 14),
  3: `${str("ZZZZ", 4)}00000001`,
  4: u8(1),
  5: u8(0),
  6: u8(0),
  7: u8(0),
  8: u8(0),
  9: u8(0),
  10: str("", 24),
  11: null,
  12: u8(0),
  13: u16(0),
});
me(257, 0, {
  1: str("EXAMPLE-ONT", 20),
  2: u8(0xa3),
  3: u16(0),
  4: u8(1),
  5: u8(1),
  6: u16(72),
  7: u8(8),
  8: u8(1),
  9: u16(128),
  10: u32(0),
  11: u16(0),
  12: u8(0),
  13: u16(0),
  14: u16(1),
});
me(7, 0, {
  1: str("V1.0.0", 14),
  2: u8(1),
  3: u8(1),
  4: u8(1),
  5: str("", 25),
  6: "00".repeat(16),
});
me(7, 1, {
  1: str("V1.0.0", 14),
  2: u8(0),
  3: u8(0),
  4: u8(1),
  5: str("", 25),
  6: "00".repeat(16),
});
me(6, 0x0180, { 1: u8(248), 2: u8(1), 4: str("V1.0", 14), 5: str("ZZZZ", 4) });
me(131, 0, {
  1: str("XXXX", 4),
  2: str("EXAMPLE-OLT", 20),
  3: str("R1.0", 14),
  4: "00".repeat(14),
});
me(11, 0x0101, { 1: u8(0), 2: u8(0x2f), 5: u8(0), 6: u8(0), 8: u16(2000), 14: u8(0) });
me(45, 1, { 1: u8(0), 2: u8(0), 3: u8(0), 4: u16(0x8000), 8: u8(0), 9: u8(0), 10: u32(300) });

/* The bridge's ports: the host's, three mappers, multicast, broadcast. */
const port = (id: number, type: number, tp: number) =>
  me(47, id, { 1: u16(1), 2: u8(id & 0xff), 3: u8(type), 4: u16(tp), 13: u8(0) });
port(0x0101, 1, 0x0101);
port(0x1102, 3, 0x1102);
port(0x1103, 3, 0x1103);
port(0x1104, 3, 0x1104);
port(0x110a, 6, 0x07ff);
port(0x110b, 5, 0x07fe);

const mapper = (id: number, byPbit: Record<number, number>) => {
  const a: Attrs = { 1: u16(0xffff), 10: u8(0), 12: u8(0), 13: u8(0) };
  for (let p = 0; p < 8; p++) a[2 + p] = u16(byPbit[p] ?? 0xffff);
  me(130, id, a);
};
mapper(0x1102, { 0: 0x0116, 6: 0x0117, 7: 0x0117 });
mapper(0x1103, { 4: 0x0118 });
mapper(0x1104, { 4: 0x0119 });

const vlanFilter = (id: number, vlans: number[]) =>
  me(84, id, {
    1: vlans.map(u16).join("").padEnd(48, "0"),
    2: u8(0x10),
    3: u8(vlans.length),
  });
vlanFilter(0x1102, [100]);
vlanFilter(0x1103, [200]);
vlanFilter(0x1104, [300]);
vlanFilter(0x110b, [100, 200, 300]);

/* The channels, their interworking points and T-CONTs. */
const ctp = (id: number, portId: number, tcont: number, dir: number, queue: number) =>
  me(268, id, {
    1: u16(portId),
    2: u16(tcont),
    3: u8(dir),
    4: u16(queue),
    5: u16(0),
    6: u8(0),
    7: u16(0),
    8: u8(1),
    9: u16(0),
    10: u8(0),
  });
ctp(0x0116, 278, 0x8000, 3, 0x8000);
ctp(0x0117, 279, 0x8001, 3, 0x8008);
ctp(0x0118, 280, 0x8002, 3, 0x8010);
ctp(0x0119, 281, 0x8002, 3, 0x8011);
ctp(0x07fe, 2046, 0xffff, 2, 0xffff);
ctp(0x07ff, 2047, 0xffff, 2, 0xffff);
const iw = (id: number, option: number, profile: number) =>
  me(266, id, { 1: u16(id), 2: u8(option), 3: u16(profile), 4: u16(0xffff), 6: u8(0), 7: u16(1) });
iw(0x0116, 5, 0x1102);
iw(0x0117, 5, 0x1102);
iw(0x0118, 5, 0x1103);
iw(0x0119, 5, 0x1104);
iw(0x07fe, 6, 1);
me(281, 0x07ff, { 1: u16(0x07ff), 2: u8(0), 3: u16(0), 6: u8(0) });
for (const [i, alloc] of [271, 272, 282, 0xff, 0xff, 0xff, 0xff, 0xff].entries())
  me(262, 0x8000 + i, { 1: u16(alloc), 2: u8(0), 3: u8(1) });
for (let q = 0; q < 24; q++)
  me(277, 0x8000 + q, {
    1: u8(0),
    2: u16(64),
    3: u16(64),
    6: u16(0x8000 + Math.floor(q / 8)) + u16(q % 8),
    7: u16(0),
    8: u8(1),
  });

/* The rules for the host's frames: translate three VLANs, drop everything else. */
const translations = [
  translate(410, 0, 100),
  translate(410, 6, 100),
  translate(410, 7, 100),
  translate(420, 4, 200),
  translate(421, 4, 300),
];
const drops = [
  rule({ op: NO, ov: VANY, ip: NO, iv: VANY }, 3, { op: NO, ov: 0, ip: NO, iv: 0 }),
  rule({ op: NO, ov: VANY, ip: DEF, iv: VANY }, 3, { op: NO, ov: 0, ip: NO, iv: 0 }),
  rule({ op: DEF, ov: VANY, ip: DEF, iv: VANY }, 3, { op: NO, ov: 0, ip: NO, iv: 0 }),
];
const vlanRules = (more: string[]) =>
  me(
    171,
    0x0101,
    {
      1: u8(2),
      2: u16(32),
      3: u16(0x8100),
      4: u16(0x8100),
      5: u8(0),
      6: [...translations, ...more, ...drops].join(""),
      7: u16(0x0101),
    },
    [6],
  );
vlanRules([]);

/* Television: IGMPv2, snooping, its groups on the multicast channel in VLAN 300. */
me(
  309,
  1,
  {
    1: u8(2),
    2: u8(0),
    3: u8(1),
    4: u16(0),
    5: u8(0),
    6: u32(15),
    7:
      u16(0) +
      u16(2047) +
      u16(300) +
      ip("0.0.0.0") +
      ip("224.0.0.0") +
      ip("239.255.255.255") +
      u32(0) +
      u16(0),
    8: "",
    10: u8(2),
    11: ip("0.0.0.0"),
    12: u32(30),
    13: u32(100),
    14: u32(10),
    15: u8(0),
    16: u8(4) + u16(421),
  },
  [7, 8],
);
me(310, 0x0101, { 1: u8(0), 2: u16(1), 3: u16(0), 4: u32(0), 5: u8(0) });

export const OMCI: OmciValue = {
  mes: [...mes],
  truncated: false,
  logical_password_set: false,
  frames: { handled: 1912, rejected: 0, answered: 1912 },
};

/*
 * The same line with @n more services, for a look at a long list: host VLAN
 * 1000 + i to fibre VLAN 2000 + i, each on its own channel 300 + i.
 */
export function withServices(n: number): OmciValue {
  mes.length = 0;
  const more: string[] = [];
  for (let i = 0; i < n; i++) {
    const id = 0x2000 + i;
    port(id, 3, id);
    mapper(id, Object.fromEntries([0, 1, 2, 3, 4, 5, 6, 7].map((p) => [p, 0x3000 + i])));
    vlanFilter(id, [2000 + i]);
    ctp(0x3000 + i, 300 + i, 0x8002, 3, 0x8012);
    iw(0x3000 + i, 5, id);
    more.push(
      rule({ op: NO, ov: VANY, ip: 8, iv: 1000 + i }, 1, { op: NO, ov: 0, ip: 8, iv: 2000 + i }),
    );
  }
  vlanRules(more);
  const added = [...mes];
  const rules = added.pop();
  return {
    ...OMCI,
    mes: [...OMCI.mes.map((m) => (m.class === 171 && rules ? rules : m)), ...added],
  };
}
