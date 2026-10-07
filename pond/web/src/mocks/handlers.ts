// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * A stick in the browser, for work on the panel without one: the sign-in
 * with the default password and its forced change, the live figures with
 * traffic that moves, and changes of settings that are kept until reload.
 */

import { delay, HttpResponse, http } from "msw";
import type { Identity, Session, Settings } from "../api/types";
import { BANKS, BRIDGE, LIVE, NETWORK, TC_RULES } from "./fixture";

let adminSecret = "admin";
let signedIn: Session | null = null;
let offline = false;
const started = Date.now();
const counters = structuredClone(LIVE.port_counters);
const cpu = structuredClone(
  LIVE.ports.find((p) => p.name === "cpu")?.cpu_path ?? {
    rx_packets: 0,
    tx_packets: 0,
    rx_bytes: 0,
    tx_bytes: 0,
    rx_missed: 0,
  },
);
const settings: Settings = structuredClone(LIVE.settings);
let lastTick = Date.now();
const identity: Identity = {
  serial: "XXXXXXXXXXXX",
  ploam_set: true,
  ploam_format: "hex",
  vendor_id: "XXXX",
  equipment_id: "EXAMPLE-ONT",
  version: "V1.0",
  equipment_serial: "EXAMPLE0001",
  model: "EXAMPLE",
  omcc_version: "0xa3",
  vendor_product_code: "0",
  tm_option: "1",
  source: "store",
};

function kv(text: string): Record<string, string> {
  return Object.fromEntries(
    text
      .split("\n")
      .filter(Boolean)
      .map((l) => [l.slice(0, l.indexOf("=")), l.slice(l.indexOf("=") + 1)]),
  );
}

/* Traffic that rises and falls: a download near 900 Mb/s for a while, then idle. */
function tick() {
  const now = Date.now();
  const dt = (now - lastTick) / 1000;
  lastTick = now;
  const phase = Math.sin((now - started) / 45_000);
  const down = phase > 0 ? 880e6 + Math.random() * 60e6 : 2e6 + Math.random() * 3e6;
  const up = phase > 0 ? 9e6 + Math.random() * 2e6 : 1e6 * Math.random();
  const add = (bits: number) => Math.round((bits * dt) / 8);
  counters.pon.rx_octets += add(down);
  counters.sfp.tx_octets += add(down);
  counters.pon.tx_octets += add(up);
  counters.sfp.rx_octets += add(up);
  cpu.rx_packets += Math.round(3 * dt);
  cpu.tx_packets += Math.round(2 * dt);
}

function live() {
  tick();
  const uptime = { ...LIVE.uptime, up_s: LIVE.uptime.up_s + (Date.now() - started) / 1000 };
  const ports = LIVE.ports.map((p) => (p.name === "cpu" ? { ...p, cpu_path: { ...cpu } } : p));
  return { ...LIVE, uptime, ports, port_counters: structuredClone(counters), settings };
}

const unauthorized = () => HttpResponse.json({ error: "no session" }, { status: 401 });

const away = () => (offline ? HttpResponse.error() : null);

export const handlers = [
  /* POST /api/__mock/offline?on=1 makes the stick vanish; on=0 brings it back. */
  http.post("*/api/__mock/offline", ({ request }) => {
    offline = new URL(request.url).searchParams.get("on") === "1";
    return HttpResponse.json({ offline });
  }),

  http.get("*/api/auth/session", async () => {
    await delay(80);
    return away() ?? (signedIn ? HttpResponse.json(signedIn) : unauthorized());
  }),

  http.post("*/api/auth/login", async ({ request }) => {
    await delay(300);
    const { user, secret } = (await request.json()) as { user: string; secret: string };
    if (user !== "admin" || secret !== adminSecret)
      return HttpResponse.json({ error: "wrong user or password" }, { status: 401 });
    signedIn = { user, must_change: adminSecret === "admin", idle_timeout_s: 900 };
    return HttpResponse.json(signedIn);
  }),

  http.post("*/api/auth/logout", () => {
    signedIn = null;
    return HttpResponse.json({});
  }),

  http.post("*/api/auth/password", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const { current, next } = (await request.json()) as { current: string; next: string };
    if (current !== adminSecret)
      return HttpResponse.json({ error: "wrong password" }, { status: 403 });
    if (next.length < 8) return HttpResponse.json({ error: "too short" }, { status: 400 });
    adminSecret = next;
    signedIn = { ...signedIn, must_change: false };
    return HttpResponse.json(signedIn);
  }),

  http.get("*/api/getm", async () => {
    await delay(120);
    return away() ?? (signedIn ? HttpResponse.json(live()) : unauthorized());
  }),

  ...(
    [
      ["network", NETWORK],
      ["bridge", BRIDGE],
      ["tc_rules", TC_RULES],
    ] as const
  ).map(([id, value]) =>
    http.get(`*/api/get/${id}`, async () => {
      await delay(300);
      return away() ?? (signedIn ? HttpResponse.json({ id, value }) : unauthorized());
    }),
  ),

  http.get("*/api/get/banks", async () => {
    await delay(400);
    return away() ?? (signedIn ? HttpResponse.json({ id: "banks", value: BANKS }) : unauthorized());
  }),

  http.post("*/api/set/pppoe", async ({ request }) => {
    await delay(500);
    if (!signedIn) return unauthorized();
    const body = Object.fromEntries(
      (await request.text())
        .split("\n")
        .filter(Boolean)
        .map((l) => [l.slice(0, l.indexOf("=")), l.slice(l.indexOf("=") + 1)]),
    );
    if (body.USER !== undefined) settings.pppoe.user = body.USER;
    if (body.SECRET) settings.pppoe.secret_set = true;
    if (body.USER === "") settings.pppoe.secret_set = false;
    if (body.VLAN !== undefined) settings.settings.WAN_VLAN = body.VLAN;
    if (body.MAC !== undefined) settings.settings.WAN_MAC = body.MAC;
    settings.store = "saved";
    settings.last_apply_status = 0;
    return HttpResponse.json({ id: "pppoe", ok: true });
  }),

  http.get("*/api/get/identity", async () => {
    await delay(150);
    return (
      away() ?? (signedIn ? HttpResponse.json({ id: "identity", value: identity }) : unauthorized())
    );
  }),

  http.post("*/api/set/identity", async ({ request }) => {
    await delay(700);
    if (!signedIn) return unauthorized();
    const b = kv(await request.text());
    const map: Record<string, keyof Identity> = {
      SERIAL: "serial",
      VENDOR_ID: "vendor_id",
      EQUIPMENT_ID: "equipment_id",
      VERSION: "version",
      EQUIPMENT_SERIAL: "equipment_serial",
      MODEL: "model",
      OMCC_VERSION: "omcc_version",
    };
    for (const [k, field] of Object.entries(map))
      if (b[k] !== undefined) (identity as unknown as Record<string, string>)[field] = b[k];
    if (b.PLOAM) identity.ploam_set = true;
    settings.store = "saved";
    return HttpResponse.json({ id: "identity", ok: true });
  }),

  http.post("*/api/system/restart", async () => {
    await delay(200);
    return signedIn ? HttpResponse.json({ ok: true }) : unauthorized();
  }),

  http.post("*/api/set/link", async ({ request }) => {
    await delay(500);
    if (!signedIn) return unauthorized();
    for (const l of (await request.text()).split("\n").filter(Boolean)) {
      const [k, v = ""] = [l.slice(0, l.indexOf("=")), l.slice(l.indexOf("=") + 1)];
      if (k === "VLAN") settings.settings.LAN_VLAN = v;
      if (k === "GATEWAY") settings.settings.PASS_GATEWAY = v;
    }
    settings.store = "saved";
    return HttpResponse.json({ id: "link", ok: true });
  }),
];
