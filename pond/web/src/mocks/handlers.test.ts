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
