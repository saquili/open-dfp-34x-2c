// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The ends of a firewall rule, as the stick's settings write them. */

/** Where traffic comes from or goes to. */
export type End =
  /* The internet, or a part of it: an address or a network, "" for all. */
  | { kind: "internet"; addr: string }
  /* Every local network. */
  | { kind: "lans" }
  /* One local network. */
  | { kind: "lan"; lan: number }
  /* A device of a local network, by its MAC. */
  | { kind: "dev"; lan: number; mac: string };

/** An end as written ("internet", "internet:ADDR/LEN", "lans", "lan:N", "dev:N:MAC"), or null. */
export function readEnd(s: string): End | null {
  if (s === "internet") return { kind: "internet", addr: "" };
  if (s.startsWith("internet:")) return { kind: "internet", addr: s.slice(9) };
  if (s === "lans") return { kind: "lans" };
  let m = /^lan:(\d+)$/.exec(s);
  if (m) return { kind: "lan", lan: Number(m[1]) };
  m = /^dev:(\d+):((?:[0-9a-f]{2}:){5}[0-9a-f]{2})$/i.exec(s);
  if (m) return { kind: "dev", lan: Number(m[1]), mac: (m[2] ?? "").toLowerCase() };
  return null;
}

/** An end as written. */
export function writeEnd(e: End): string {
  switch (e.kind) {
    case "internet":
      return e.addr ? `internet:${e.addr}` : "internet";
    case "lans":
      return "lans";
    case "lan":
      return `lan:${e.lan}`;
    case "dev":
      return `dev:${e.lan}:${e.mac}`;
  }
}

/** The local network an end lies in, or null for the internet and for every network. */
export function lanOf(e: End): number | null {
  return e.kind === "lan" || e.kind === "dev" ? e.lan : null;
}
