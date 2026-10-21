// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { afterEach, describe, expect, it } from "vitest";
import { handlers } from "./handlers";
import { getResponse } from "./http";

const ask = (path: string, method = "GET") =>
  getResponse(handlers, new Request(`http://stick.invalid${path}`, { method }));

afterEach(async () => {
  await ask("/api/__mock/fail", "POST");
  await ask("/api/__mock/slow?ms=0", "POST");
});

describe("the mock stick's switches", () => {
  it("refuses a path with the status asked for, and answers it again", async () => {
    expect((await ask("/api/auth/session"))?.status).toBe(401);
    await ask("/api/__mock/fail?path=/api/auth/session&status=503", "POST");
    expect((await ask("/api/auth/session"))?.status).toBe(503);
    await ask("/api/__mock/fail?path=/api/auth/session&status=0", "POST");
    expect((await ask("/api/auth/session"))?.status).toBe(401);
  });

  it("refuses with 500 and says why when no status is given", async () => {
    await ask("/api/__mock/fail?path=/api/auth/session", "POST");
    const r = await ask("/api/auth/session");
    expect(r?.status).toBe(500);
    const reply = (await r?.json()) as { detail?: string } | undefined;
    expect(reply?.detail).toContain("/api/auth/session");
  });

  it("holds every answer as long as asked", async () => {
    await ask("/api/__mock/slow?ms=120", "POST");
    const t = Date.now();
    await ask("/api/auth/session");
    expect(Date.now() - t).toBeGreaterThanOrEqual(110);
  });
});

describe("the mock stick's product name", () => {
  const send = (path: string, body: string) =>
    getResponse(handlers, new Request(`http://stick.invalid${path}`, { method: "POST", body }));
  const shown = async () => {
    const reply = (await (await ask("/api/getm?ids=product"))?.json()) as
      | { product: string }
      | undefined;
    return reply?.product;
  };
  const file = async () =>
    (await ask("/api/firmware/download"))?.headers.get("Content-Disposition") ?? "";

  it("is BuchOS, takes another, and names the files after it, as pond does", async () => {
    await send("/api/auth/login", JSON.stringify({ user: "root", secret: "admin" }));
    try {
      expect(await shown()).toBe("BuchOS");
      expect(await file()).toContain('filename="buchos-gpon-');
      expect((await send("/api/set/product", "PRODUCT_NAME=Fibre Box 2.0\n"))?.status).toBe(200);
      expect(await shown()).toBe("Fibre Box 2.0");
      expect(await file()).toContain('filename="fibre-box-2-0-gpon-');
      expect((await send("/api/set/product", "PRODUCT_NAME=<b>x</b>\n"))?.status).toBe(400);
      expect(await shown()).toBe("Fibre Box 2.0");
      expect((await send("/api/set/product", "PRODUCT_NAME=\n"))?.status).toBe(200);
      expect(await shown()).toBe("BuchOS");
    } finally {
      await send("/api/auth/logout", "");
    }
  });
});

describe("the mock stick's servers' keys", () => {
  /* The header of root's password, which each change of a key takes. */
  const CONFIRM = "X-Pond-Password";
  const ED25519 =
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJUZp+xf8C/t3NT+0VYL5nV7MNOQXr21ORnzDVmED9xE me@laptop";
  const FP = "SHA256:lxckqci4MwTwEjJFI7q4oX1lApk+VkklJ0/B5U0TTc0";
  const send = (path: string, body: string, secret = "admin") =>
    getResponse(
      handlers,
      new Request(`http://stick.invalid${path}`, {
        method: "POST",
        body,
        headers: { [CONFIRM]: encodeURIComponent(secret) },
      }),
    );
  const value = async <T>(id: string) =>
    ((await (await ask(`/api/get/${id}`))?.json()) as { value: T } | undefined)?.value as T;
  const detail = async (r: Response | undefined) =>
    ((await r?.json()) as { detail?: string } | undefined)?.detail;
  type Tls = { own: { chain: number } | null; certificates: { sha256: string }[] };
  type Ssh = {
    host_key: { fingerprint: string };
    keys: { fingerprint: string; source?: string }[];
  };

  it("takes a key and keys alone with root's password, and refuses as the stick", async () => {
    await send("/api/auth/login", JSON.stringify({ user: "root", secret: "admin" }));
    try {
      expect((await send("/api/ssh/keys", ED25519, "wrong"))?.status).toBe(403);
      expect((await send("/api/ssh/keys-only", "on"))?.status).toBe(400);
      const added = await send("/api/ssh/keys", `${ED25519}\n`);
      expect(added?.status).toBe(200);
      const key = (
        (await added?.json()) as { key?: { fingerprint: string; source: string } } | undefined
      )?.key;
      expect(key).toMatchObject({ fingerprint: FP, source: "panel" });
      /* The panel's keys first, then the image's, which it does not change. */
      const keys = (await value<Ssh>("ssh")).keys;
      expect(keys.map((k) => k.source)).toEqual(["panel", "image"]);
      const bench = keys[1]?.fingerprint ?? "";
      expect(await detail(await send("/api/ssh/keys/remove", bench))).toContain("no key");
      expect(await detail(await send("/api/ssh/keys", ED25519))).toContain("kept already");
      expect(await detail(await send("/api/ssh/keys", "ssh-dss AAAA"))).toContain("another type");
      expect((await send("/api/ssh/keys-only", "maybe"))?.status).toBe(400);
      expect((await send("/api/ssh/keys-only", "on"))?.status).toBe(200);
      expect(await detail(await send("/api/ssh/keys/remove", FP))).toContain("last key stays");
      expect((await send("/api/ssh/keys-only", "off"))?.status).toBe(200);
      expect((await send("/api/ssh/keys/remove", FP))?.status).toBe(200);
      expect(await detail(await send("/api/ssh/keys/remove", FP))).toContain("no key");
      const before = (await value<Ssh>("ssh")).host_key;
      expect((await send("/api/ssh/host-key", ""))?.status).toBe(200);
      expect((await value<Ssh>("ssh")).host_key).not.toEqual(before);
    } finally {
      await send("/api/auth/logout", "");
    }
    /* The mock stick takes as long over each change as the stick. */
  }, 15_000);

  it("makes a new key for the panel, takes an own certificate, and goes back", async () => {
    await send("/api/auth/login", JSON.stringify({ user: "root", secret: "admin" }));
    try {
      const first = (await value<Tls>("tls")).certificates[0]?.sha256;
      expect((await send("/api/tls/key", ""))?.status).toBe(200);
      expect((await value<Tls>("tls")).certificates[0]?.sha256).not.toBe(first);
      const cert = "-----BEGIN CERTIFICATE-----\nQUFB\n-----END CERTIFICATE-----\n";
      const pem = (key: string) => JSON.stringify({ certificate: cert + cert, key });
      const rsa = await send("/api/tls/certificate", pem("-----BEGIN RSA PRIVATE KEY-----"));
      expect(await detail(rsa)).toContain("RSA");
      const own = await send("/api/tls/certificate", pem("-----BEGIN PRIVATE KEY-----"));
      expect(own?.status).toBe(200);
      expect((await value<Tls>("tls")).own?.chain).toBe(2);
      expect((await send("/api/tls/self-signed", ""))?.status).toBe(200);
      expect((await value<Tls>("tls")).own).toBeNull();
    } finally {
      await send("/api/auth/logout", "");
    }
  });
});

describe("the mock stick's sessions and token", () => {
  const CONFIRM = "X-Pond-Password";
  const send = (path: string, body: string, secret = "admin") =>
    getResponse(
      handlers,
      new Request(`http://stick.invalid${path}`, {
        method: "POST",
        body,
        headers: { [CONFIRM]: encodeURIComponent(secret) },
      }),
    );
  const json = async <T>(r: Response | undefined) => (await r?.json()) as T;
  type Sessions = { max: number; sessions: { this: boolean; from: string }[] };

  it("lists this browser's session, and ends the others with root's password", async () => {
    expect((await ask("/api/auth/sessions"))?.status).toBe(401);
    await send("/api/auth/login", JSON.stringify({ user: "root", secret: "admin" }));
    try {
      const list = await json<Sessions>(await ask("/api/auth/sessions"));
      expect(list.max).toBe(3);
      expect(list.sessions.filter((x) => x.this)).toHaveLength(1);
      expect((await send("/api/auth/sessions/others", "", "wrong"))?.status).toBe(403);
      const ended = await send("/api/auth/sessions/others", "");
      expect(ended?.status).toBe(200);
      expect(await json<{ ended: number }>(ended)).toMatchObject({ ok: true });
      const after = await json<Sessions>(await ask("/api/auth/sessions"));
      expect(after.sessions.map((x) => x.this)).toEqual([true]);
    } finally {
      await send("/api/auth/logout", "");
    }
  });

  it("makes a token, shows it once, and takes it away, with root's password", async () => {
    await send("/api/auth/login", JSON.stringify({ user: "root", secret: "admin" }));
    try {
      expect(await json(await ask("/api/token"))).toEqual({ set: false });
      expect((await send("/api/token/new", "", "wrong"))?.status).toBe(403);
      const made = await json<{ token: string }>(await send("/api/token/new", ""));
      expect(made.token).toMatch(/^[0-9a-f]{64}$/);
      expect(await json(await ask("/api/token"))).toEqual({ set: true });
      expect(JSON.stringify(await json(await ask("/api/token")))).not.toContain(made.token);
      expect((await send("/api/token/off", ""))?.status).toBe(200);
      expect(await json(await ask("/api/token"))).toEqual({ set: false });
    } finally {
      await send("/api/auth/logout", "");
    }
  });

  it("stops every change that needs root's password after five wrong ones, the right one too", async () => {
    await send("/api/auth/login", JSON.stringify({ user: "root", secret: "admin" }));
    try {
      for (let i = 0; i < 5; i++)
        expect((await send("/api/token/off", "", "wrong"))?.status).toBe(403);
      const stopped = await send("/api/token/off", "");
      expect(stopped?.status).toBe(429);
      const reply = await json<{ retry_after: number }>(stopped);
      expect(reply.retry_after).toBeGreaterThan(0);
      expect(reply.retry_after).toBeLessThanOrEqual(30);
      expect(stopped?.headers.get("Retry-After")).toBe(String(reply.retry_after));
    } finally {
      await send("/api/auth/logout", "");
    }
  });
});
