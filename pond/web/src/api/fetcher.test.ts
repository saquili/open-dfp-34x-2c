// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The requests Orval makes from pond/openapi.json, as they leave the panel
 * through pondFetch: a read with its query, a change of settings as KEY=VALUE
 * lines, and a refusal as the panel's error.
 */

import { afterEach, describe, expect, it, vi } from "vitest";
import { ApiError } from "./client";
import { getm, getOmciFrames, setIdentity, setIptv } from "./gen/pond";

interface Sent {
  url: string;
  method: string;
  body: string;
  headers: Record<string, string>;
}

/* The requests sent, and pond's answer @reply, with @status, to each. */
function sending(reply: unknown, status = 200): Sent[] {
  const sent: Sent[] = [];
  vi.stubGlobal(
    "fetch",
    vi.fn(async (url: string, init: RequestInit) => {
      sent.push({
        url,
        method: String(init.method),
        body: String(init.body ?? ""),
        headers: init.headers as Record<string, string>,
      });
      return Response.json(reply, { status });
    }),
  );
  return sent;
}

afterEach(() => vi.unstubAllGlobals());

describe("a request Orval made", () => {
  it("reads with its query, and gives the answer's data", async () => {
    const sent = sending({ uptime: { up_s: 5, idle_s: 1 } });
    const live = await getm({ ids: "uptime,memory" });
    await getOmciFrames({ since: 12 });
    expect(live.uptime).toEqual({ up_s: 5, idle_s: 1 });
    expect(sent.map((s) => [s.method, decodeURIComponent(s.url)])).toEqual([
      ["GET", "/api/getm?ids=uptime,memory"],
      ["GET", "/api/get/omci_frames?since=12"],
    ]);
    expect(sent[0]?.headers["X-Pond"]).toBeUndefined();
  });

  it("sends a change of settings as KEY=VALUE lines", async () => {
    const sent = sending({ id: "iptv", ok: true });
    await setIptv({ IGMP_PRIORITY: "host" });
    await setIdentity({ SERIAL: "000000000000", LOID: undefined });
    expect(sent.map((s) => [s.method, s.url, s.body])).toEqual([
      ["POST", "/api/set/iptv", "IGMP_PRIORITY=host\n"],
      ["POST", "/api/set/identity", "SERIAL=000000000000\n"],
    ]);
    expect(sent[0]?.headers["Content-Type"]).toBe("text/plain");
    expect(sent[0]?.headers["X-Pond"]).toBe("1");
  });

  it("gives pond's refusal as the panel's error, with why", async () => {
    sending({ error: "iptv", detail: "IGMP_PRIORITY: auto or host" }, 400);
    await expect(setIptv({ IGMP_PRIORITY: "auto" })).rejects.toEqual(
      new ApiError(400, "iptv", "IGMP_PRIORITY: auto or host"),
    );
  });
});
