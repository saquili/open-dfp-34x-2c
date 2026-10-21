// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The addresses of System: its four pages, and the page of four tabs before
 * them, whose address, with the tab it named or none, opens the page that
 * holds what the tab held; and the password's page, which a session with a
 * password of its own finds under System, Access. A page is followed as the
 * router follows it before it shows anything: each route's check, from the
 * root's down, sends on to another address or lets it through.
 */

import { QueryClient } from "@tanstack/react-query";
import { type AnyRoute, createMemoryHistory, isRedirect } from "@tanstack/react-router";
import { describe, expect, it } from "vitest";
import { sessionQuery } from "./api/auth";
import type { Session } from "./api/types";
import { SYSTEM_PAGES, systemPath } from "./pages/System";
import { makeRouter } from "./router";

/* Where the panel ends up when @path?tab=@tab is opened by a browser signed in as @session. */
async function opened(path: string, session: Session | null, tab?: string): Promise<string> {
  const qc = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  qc.setQueryData(sessionQuery.queryKey, session);
  const router = makeRouter(qc, createMemoryHistory({ initialEntries: ["/"] }));
  const byPath = router.routesByPath as unknown as Record<string, AnyRoute>;
  let at = path;
  let search: Record<string, unknown> = tab === undefined ? {} : { tab };
  for (let hops = 0; hops < 5; hops++) {
    const chain: AnyRoute[] = [];
    for (let r: AnyRoute | undefined = byPath[at]; r; r = r.parentRoute) chain.unshift(r);
    if (!chain.length) return `no page ${at}`;
    let next: string | null = null;
    for (const r of chain) {
      try {
        await r.options.beforeLoad?.({ context: { queryClient: qc }, search } as never);
      } catch (e) {
        if (!isRedirect(e)) throw e;
        next = String(e.options.to);
        break;
      }
    }
    if (next === null) return at;
    at = next;
    search = {};
  }
  return `no end after ${path}`;
}

const root: Session = { user: "root", must_change: false, idle_timeout_s: 900 };

describe("the pages of System", () => {
  it("are four, each at an address of its own", async () => {
    expect(SYSTEM_PAGES).toEqual(["resources", "software", "device", "panel", "ssh", "https"]);
    for (const p of SYSTEM_PAGES) expect(await opened(`/system/${p}`, root)).toBe(`/system/${p}`);
  });

  it("hold what each tab of before held", () => {
    expect(systemPath("resources")).toBe("/system/resources");
    expect(systemPath("software")).toBe("/system/software");
    expect(systemPath("settings")).toBe("/system/device");
    expect(systemPath("access")).toBe("/system/panel");
    expect(systemPath("")).toBe("/system/resources");
    expect(systemPath("toString")).toBe("/system/resources");
  });

  it("are where an address of before leads, with its tab or none", async () => {
    expect(await opened("/system", root)).toBe("/system/resources");
    expect(await opened("/system", root, "software")).toBe("/system/software");
    expect(await opened("/system", root, "settings")).toBe("/system/device");
    expect(await opened("/system", root, "access")).toBe("/system/panel");
    expect(await opened("/system/access", root)).toBe("/system/panel");
    expect(await opened("/system", root, "nothing")).toBe("/system/resources");
  });

  it("want a sign-in first", async () => {
    expect(await opened("/system/access", null)).toBe("/login");
    expect(await opened("/system", null, "access")).toBe("/login");
  });
});

describe("the password's page", () => {
  it("is the change of the default password, which comes first", async () => {
    const fresh = { ...root, must_change: true };
    expect(await opened("/password", fresh)).toBe("/password");
    expect(await opened("/system/access", fresh)).toBe("/password");
  });

  it("leads to System, Access, once a password of root's own is set", async () => {
    expect(await opened("/password", root)).toBe("/system/panel");
    expect(await opened("/password", null)).toBe("/login");
  });
});
