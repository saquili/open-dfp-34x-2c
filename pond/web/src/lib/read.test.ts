// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { environmentManager, QueryClient, QueryObserver } from "@tanstack/react-query";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { ApiError, AuthError, OfflineError } from "../api/client";
import { clockQuery } from "../api/config";
import { liveQuery, POLL_MS } from "../api/live";
import { type Read, readView, reason } from "./read";

/* A read as TanStack Query gives it, at its first load by default. */
const read = (r: Partial<Read> = {}): Read => ({
  data: undefined,
  error: null,
  isFetching: true,
  dataUpdatedAt: 0,
  refetch: () => undefined,
  ...r,
});
const answered = (r: Partial<Read> = {}) =>
  read({ data: { ok: true }, isFetching: false, dataUpdatedAt: 1, ...r });

describe("where reads stand", () => {
  it("are being read until each has an answer", () => {
    expect(readView([read()]).state).toBe("reading");
    expect(readView([answered(), read()]).state).toBe("reading");
  });

  it("failed when one without an answer failed, whatever the others do", () => {
    const failed = read({ error: new OfflineError(), isFetching: false });
    expect(readView([read(), failed])).toEqual({
      state: "failed",
      read: failed,
      error: failed.error,
    });
  });

  it("is missing when the stick has no such value", () => {
    const gone = read({ error: new ApiError(404, "no such item"), isFetching: false });
    expect(readView([gone]).state).toBe("missing");
  });

  it("are shown once every one answered, a read again under way or not", () => {
    expect(readView([answered(), answered({ isFetching: true })])).toEqual({
      state: "shown",
      stale: null,
    });
  });

  it("are shown with the last answer when a read again failed", () => {
    const stale = answered({ error: new ApiError(500, "refused", "omcid does not answer") });
    expect(readView([answered(), stale])).toEqual({ state: "shown", stale });
  });
});

describe("why a read failed", () => {
  const t = (k: string) => `<${k}>`;

  it("is pond's reason, its detail first", () => {
    expect(reason(t, new ApiError(500, "refused", "omcid does not answer"))).toBe(
      "omcid does not answer",
    );
    expect(reason(t, new ApiError(503, "busy"))).toBe("busy");
  });

  it("is the panel's words for a stick away and a session ended", () => {
    expect(reason(t, new OfflineError())).toBe("<connection.offline>");
    expect(reason(t, new AuthError())).toBe("<read.signedOut>");
  });

  it("is the message of anything else", () => {
    expect(reason(t, new Error("boom"))).toBe("boom");
    expect(reason(t, "odd")).toBe("odd");
  });
});

/*
 * A polled read goes on after a failure: the next poll reads again, and the
 * answer before stays until one comes.
 */
describe("a polled read whose read fails", () => {
  /* The tests run without a window, which TanStack Query takes for a server, where it polls nothing. */
  beforeEach(() => environmentManager.setIsServer(() => false));
  afterEach(() => {
    environmentManager.setIsServer(() => typeof window === "undefined");
    vi.useRealTimers();
    vi.unstubAllGlobals();
  });

  async function poll(options: object, every: number, answer: (n: number) => Response | null) {
    vi.useFakeTimers();
    let n = 0;
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => {
        const r = answer(n++);
        if (!r) throw new TypeError("Failed to fetch");
        return r;
      }),
    );
    const qc = new QueryClient({ defaultOptions: { queries: { retry: false } } });
    const observer = new QueryObserver(qc, options as never);
    /* What each read left, as it ends. */
    const seen: { data: unknown; error: unknown }[] = [];
    let reading = false;
    const stop = observer.subscribe((r) => {
      if (reading && !r.isFetching) seen.push({ data: r.data, error: r.error });
      reading = r.isFetching;
    });
    await vi.advanceTimersByTimeAsync(every * 3 + every / 2);
    stop();
    return { calls: n, seen };
  }

  it("polls the live figures again after the stick did not answer, and keeps the last", async () => {
    const live = { uptime: { up_s: 1 } };
    const { calls, seen } = await poll(liveQuery, POLL_MS, (n) =>
      n === 1 || n === 2 ? null : Response.json(live),
    );
    expect(calls).toBe(4);
    expect(seen.map((s) => s.error instanceof OfflineError)).toEqual([false, true, true, false]);
    expect(seen.every((s) => s.data !== undefined)).toBe(true);
  });

  it("polls the clock again after pond refused it", async () => {
    const { calls, seen } = await poll(clockQuery, 10_000, (n) =>
      n === 1
        ? Response.json({ error: "refused" }, { status: 500 })
        : Response.json({ id: "clock", value: { now_s: n } }),
    );
    expect(calls).toBe(4);
    expect(seen.map((s) => s.error != null)).toEqual([false, true, false, false]);
    expect(seen.at(-1)?.data).toEqual({ now_s: 3 });
  });
});
