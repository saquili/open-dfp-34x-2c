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
  endOthersOptions,
  REFRESH,
  restartOptions,
  returnOptions,
  sshHostKeyOptions,
  sshKeyAddOptions,
  sshKeyRemoveOptions,
  sshKeysOnlyOptions,
  tlsCertificateOptions,
  tlsKeyOptions,
  tlsSelfSignedOptions,
  tokenNewOptions,
  tokenOffOptions,
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
  ["ssh"],
  ["sessions"],
  ["token"],
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
  tls: ["mgmt", "tls_key", "tls_certificate", "tls_self_signed"],
  ssh: ["ssh_host_key", "ssh_key", "ssh_keys_only"],
  sessions: ["password", "sessions_end"],
  token: ["token"],
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

  /* The changes of routes of their own, which their tests below run. */
  const OWN_ROUTE = ["upload", "try", "return", "restart", "password"];
  const KEYS = [
    ["ssh_host_key", sshHostKeyOptions, { secret: "s" }],
    ["ssh_key", sshKeyAddOptions, { key: "ssh-ed25519 AAAA", secret: "s" }],
    ["ssh_key", sshKeyRemoveOptions, { fingerprint: "SHA256:x", secret: "s" }],
    ["ssh_keys_only", sshKeysOnlyOptions, { on: true, secret: "s" }],
    ["tls_key", tlsKeyOptions, { secret: "s" }],
    ["tls_certificate", tlsCertificateOptions, { certificate: "c", key: "k", secret: "s" }],
    ["tls_self_signed", tlsSelfSignedOptions, { secret: "s" }],
    ["sessions_end", endOthersOptions, { secret: "s" }],
    ["token", tokenNewOptions, { secret: "s" }],
    ["token", tokenOffOptions, { secret: "s" }],
  ] as const;

  for (const change of Object.keys(REFRESH) as Change[]) {
    if (OWN_ROUTE.includes(change) || KEYS.some(([c]) => c === change)) continue;
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

  it("are, after a change of the servers' keys, the sessions or the token, those of the table", async () => {
    for (const [change, options, values] of KEYS) {
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

  it("keeps the new session after a change of the password, and every read but the sessions", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => Response.json(session)),
    );
    const qc = warm();
    await run(qc, passwordOptions(qc), { current: "a", next: "bbbbbbbb" });
    expect(qc.getQueryData(["session"])).toEqual(session);
    expect(stale(qc)).toEqual(expected("password"));
    expect(stale(qc)).toEqual(["sessions"]);
  });
});

describe("a change of the servers' keys", () => {
  type Sent = { url: string; body: string; headers: Record<string, string> };

  /* The requests sent, and pond's answer @reply to each. */
  function sending(reply: unknown) {
    const sent: Sent[] = [];
    vi.stubGlobal(
      "fetch",
      vi.fn(async (url: string, init: RequestInit) => {
        sent.push({
          url,
          body: String(init.body ?? ""),
          headers: init.headers as Record<string, string>,
        });
        return Response.json(reply);
      }),
    );
    return sent;
  }

  it("goes with root's password, percent-encoded, and its body as pond takes it", async () => {
    const sent = sending({ ok: true });
    const qc = warm();
    const secret = "zażółć 1";
    await run(qc, sshKeyAddOptions(qc), { key: "  ssh-ed25519 AAAA me \n", secret });
    await run(qc, sshKeyRemoveOptions(qc), { fingerprint: "SHA256:a/b+c", secret });
    await run(qc, sshKeysOnlyOptions(qc), { on: false, secret });
    await run(qc, sshHostKeyOptions(qc), { secret });
    await run(qc, tlsCertificateOptions(qc), { certificate: "C", key: "K", secret });
    await run(qc, endOthersOptions(qc), { secret });
    await run(qc, tokenNewOptions(qc), { secret });
    await run(qc, tokenOffOptions(qc), { secret });
    expect(sent.map((s) => [s.url, s.body])).toEqual([
      ["/api/ssh/keys", "ssh-ed25519 AAAA me\n"],
      ["/api/ssh/keys/remove", "SHA256:a/b+c"],
      ["/api/ssh/keys-only", "off"],
      ["/api/ssh/host-key", ""],
      ["/api/tls/certificate", JSON.stringify({ certificate: "C", key: "K" })],
      ["/api/auth/sessions/others", ""],
      ["/api/token/new", ""],
      ["/api/token/off", ""],
    ]);
    for (const s of sent) {
      expect(s.headers["X-Pond-Password"]).toBe(encodeURIComponent(secret));
      expect(s.headers["X-Pond"]).toBe("1");
    }
  });

  it("of the panel's certificate shows its answer at once, the new fingerprints", async () => {
    const tls = { https: true, own: null, certificates: [] };
    sending({ ok: true, tls });
    for (const options of [tlsKeyOptions, tlsCertificateOptions, tlsSelfSignedOptions]) {
      const qc = warm();
      await run(qc, options(qc), { certificate: "c", key: "k", secret: "s" });
      expect(qc.getQueryData(["tls"])).toEqual(tls);
    }
  });
});

describe("the sessions and the token", () => {
  it("are read where they are shown, the sessions again every half a minute", async () => {
    const { sessionsQuery, tokenQuery } = await import("./config");
    const asked: string[] = [];
    vi.stubGlobal(
      "fetch",
      vi.fn(async (url: string) => {
        asked.push(url);
        return Response.json(url.endsWith("token") ? { set: true } : { sessions: [] });
      }),
    );
    const qc = new QueryClient();
    expect(await qc.fetchQuery(tokenQuery)).toEqual({ set: true });
    expect(await qc.fetchQuery(sessionsQuery)).toEqual({ sessions: [] });
    expect(asked).toEqual(["/api/token", "/api/auth/sessions"]);
    expect(sessionsQuery.refetchInterval).toBe(30_000);
  });

  it("hands back a new token once, as its answer carries it", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => Response.json({ ok: true, token: "ab".repeat(32) })),
    );
    const qc = warm();
    const reply = await run(qc, tokenNewOptions(qc), { secret: "s" });
    expect(reply).toEqual({ ok: true, token: "ab".repeat(32) });
    /* The token itself is never read again: only whether there is one. */
    expect(stale(qc)).toEqual(["token"]);
  });
});
