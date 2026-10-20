// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Which reads each change makes stale. The table below is the panel's, by
 * read: the changes that alter what it shows. Each change runs as the panel
 * runs it, against a fetch that answers, and must leave stale exactly the
 * reads the table gives it, and none when pond refuses it.
 */

import { MutationObserver, QueryClient } from "@tanstack/react-query";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { passwordOptions, signInOptions, signOutOptions } from "./auth";
import {
  type Change,
  changeOptions,
  REFRESH,
  restartOptions,
  returnOptions,
  tryOptions,
} from "./config";

/* Every read the panel keeps, by its key. */
const READS = [
  ["session"],
  ["live"],
  ["leases"],
  ["banks"],
  ["firmware"],
  ["tls"],
  ["clock"],
  ["identity"],
  ["omci"],
  ["sfp_lane"],
  ["mgmt"],
  ["switch", "network"],
  ["switch", "bridge"],
] as const;

type Read = (typeof READS)[number];
const name = (k: readonly string[]) => k.join("/");

/* The changes that alter what each read shows. */
const CHANGED_BY: Record<string, Change[]> = {
  session: [],
  live: [
    "session",
    "ipoe",
    "lan",
    "changes",
    "fixed",
    "forward",
    "dmz",
    "rule",
    "fw",
    "sfp_lane",
    "mgmt",
    "hostname",
    "product",
    "identity",
  ],
  leases: ["session", "ipoe", "lan", "changes"],
  banks: ["upload", "try", "return"],
  firmware: ["upload", "try", "return"],
  tls: ["mgmt"],
  clock: [],
  identity: ["identity"],
  omci: [],
  sfp_lane: ["changes", "sfp_lane"],
  mgmt: ["changes", "mgmt"],
  "switch/network": ["session", "ipoe", "lan", "changes", "mgmt"],
  "switch/bridge": ["session", "ipoe", "lan", "changes", "mgmt"],
};

/* The reads @change must leave stale, by the table. */
const expected = (change: Change) =>
  READS.map(name)
    .filter((r) => CHANGED_BY[r]?.includes(change))
    .sort();

/* A client that holds a fresh read of everything, as a panel that has been open a while. */
function warm(): QueryClient {
  const qc = new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity } } });
  for (const key of READS) qc.setQueryData([...key], { read: name(key) });
  return qc;
}

const stale = (qc: QueryClient) =>
  READS.filter((k: Read) => qc.getQueryState([...k])?.isInvalidated)
    .map(name)
    .sort();

/* pond's answer to every request: @status, with a body that says so. */
function answer(status: number) {
  return vi.fn(async () =>
    Response.json(status === 200 ? { ok: true } : { error: "refused" }, { status }),
  );
}

const run = <V>(qc: QueryClient, options: object, values: V) =>
  new MutationObserver(qc, options as never).mutate(values as never);

beforeEach(() => {
  vi.stubGlobal("fetch", answer(200));
});

afterEach(() => {
  vi.unstubAllGlobals();
});

describe("the reads a change makes stale", () => {
  it("are, for every change of the table, those the table gives it", () => {
    const changes = Object.keys(REFRESH) as Change[];
    for (const r of Object.keys(CHANGED_BY))
      for (const c of CHANGED_BY[r] ?? []) expect(changes).toContain(c);
    expect(Object.keys(CHANGED_BY).sort()).toEqual(READS.map(name).sort());
  });

  for (const change of Object.keys(REFRESH) as Change[]) {
    if (["upload", "try", "return", "restart"].includes(change)) continue;
    it(`are, after a change of ${change}, ${expected(change).join(", ") || "none"}`, async () => {
      const qc = warm();
      await run(qc, changeOptions(qc, change), { ID: "1" });
      expect(stale(qc)).toEqual(expected(change));
    });
  }

  it("are the banks and the firmware after a start of the other bank, or the way back", async () => {
    for (const [change, options, values] of [
      ["try", tryOptions, { bank: 1, secret: "s" }],
      ["return", returnOptions, { bank: 1, secret: "s" }],
    ] as const) {
      const qc = warm();
      await run(qc, options(qc), values);
      expect(stale(qc)).toEqual(expected(change));
    }
  });

  it("are none after a restart, which the stick's coming back shows", async () => {
    const qc = warm();
    await run(qc, restartOptions(qc), undefined);
    expect(stale(qc)).toEqual([]);
  });

  it("are none when pond refuses the change", async () => {
    vi.stubGlobal("fetch", answer(400));
    const qc = warm();
    await expect(run(qc, changeOptions(qc, "lan"), { ID: "1" })).rejects.toThrow("refused");
    expect(stale(qc)).toEqual([]);
  });
});

describe("signing in and out", () => {
  const session = { user: "root", must_change: false, idle_timeout_s: 900 };

  it("forgets every read of the session before when one signs in", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => Response.json(session)),
    );
    const qc = warm();
    await run(qc, signInOptions(qc), { user: "root", secret: "s" });
    expect(qc.getQueryData(["session"])).toEqual(session);
    expect(READS.filter((k) => qc.getQueryState([...k])).map(name)).toEqual(["session"]);
  });

  it("forgets every read when one signs out, even when pond does not answer", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => {
        throw new TypeError("Failed to fetch");
      }),
    );
    const qc = warm();
    await expect(run(qc, signOutOptions(qc), undefined)).rejects.toThrow("offline");
    expect(qc.getQueryData(["session"])).toBeNull();
    expect(READS.filter((k) => qc.getQueryState([...k])).map(name)).toEqual(["session"]);
  });

  it("keeps the new session after a change of the password, and every read", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => Response.json(session)),
    );
    const qc = warm();
    await run(qc, passwordOptions(qc), { current: "a", next: "bbbbbbbb" });
    expect(qc.getQueryData(["session"])).toEqual(session);
    expect(stale(qc)).toEqual([]);
  });
});
