// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * A stick in the browser, for work on the panel without one: the sign-in
 * with the default password and its forced change, the live figures with
 * traffic that moves, and changes of settings that are kept until reload.
 */

import { delay, HttpResponse, http } from "msw";
import type { Identity, Session, Settings } from "../api/types";
import { BANKS, BRIDGE, FIRMWARE, LIVE, NETWORK, TC_RULES } from "./fixture";
import { OMCI } from "./omci";

let adminSecret = "admin";
const firmware = structuredClone(FIRMWARE);
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
const ticks = structuredClone(
  LIVE.cpu?.ticks ?? {
    user: 0,
    nice: 0,
    system: 0,
    idle: 0,
    iowait: 0,
    irq: 0,
    softirq: 0,
    steal: 0,
  },
);
const gemCounters = structuredClone(LIVE.gem_counters);
/* The channel of the stick's own session, and the multicast one. */
const SESSION_GEM = LIVE.settings.sessions[0]?.gem_dev ?? "";
const MULTICAST_GEM = LIVE.gem_ports.find((g) => g.multicast)?.name ?? "";
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
  loid: "",
  loid_password_set: false,
  sw_version: "",
  sw_version0: "V1.0.0",
  sw_version1: "",
  pack_version: "",
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
  /* The processor: about 6 % busy idle, more while the traffic runs. */
  const busy = Math.round(dt * 100 * (phase > 0 ? 0.11 : 0.05) * (0.8 + Math.random() * 0.4));
  ticks.system += Math.round(busy * 0.8);
  ticks.user += busy - Math.round(busy * 0.8);
  ticks.idle += Math.max(0, Math.round(dt * 100) - busy);
  /* The channels: the session's carries the download, the multicast one a steady stream. */
  for (const g of Object.values(gemCounters)) {
    const share = g === gemCounters[SESSION_GEM] ? 1 : 0;
    g.pon_rx_octets += add(down * share);
    g.pon_tx_octets += add(up * share);
  }
  const mc = gemCounters[MULTICAST_GEM];
  if (mc) mc.pon_rx_octets += add(17.8e6);
}

function live() {
  tick();
  const uptime = { ...LIVE.uptime, up_s: LIVE.uptime.up_s + (Date.now() - started) / 1000 };
  const ports = LIVE.ports.map((p) => (p.name === "cpu" ? { ...p, cpu_path: { ...cpu } } : p));
  return {
    ...LIVE,
    uptime,
    ports,
    port_counters: structuredClone(counters),
    gem_counters: structuredClone(gemCounters),
    cpu: LIVE.cpu ? { ...LIVE.cpu, ticks: { ...ticks } } : null,
    settings,
  };
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

  http.get("*/api/get/omci", async () => {
    await delay(600);
    return away() ?? (signedIn ? HttpResponse.json({ id: "omci", value: OMCI }) : unauthorized());
  }),

  http.get("*/api/get/banks", async () => {
    await delay(400);
    return away() ?? (signedIn ? HttpResponse.json({ id: "banks", value: BANKS }) : unauthorized());
  }),

  /* A session: added to the first free number, changed by ID, removed by USER=. */
  http.post("*/api/set/session", async ({ request }) => {
    await delay(500);
    if (!signedIn) return unauthorized();
    const body = kv(await request.text());
    const list = settings.sessions;
    let id = body.ID ? Number(body.ID) : 0;
    if (!id) {
      for (let n = 1; n <= 7 && !id; n++) if (!list.some((x) => x.id === n)) id = n;
      if (!id || !body.USER)
        return HttpResponse.json(
          {
            error: "refused",
            detail: id ? "a new session needs USER" : "all 7 sessions are taken",
          },
          { status: 400 },
        );
    }
    let x = list.find((y) => y.id === id);
    if (body.USER === "") {
      settings.sessions = list.filter((y) => y.id !== id);
    } else {
      if (!x) {
        if (!body.SECRET)
          return HttpResponse.json(
            { error: "refused", detail: `session ${id}: an account needs a secret` },
            { status: 400 },
          );
        x = {
          id,
          user: "",
          secret_set: false,
          gem: "auto",
          vlan: "",
          mac: "",
          family: "both",
          hands_on: false,
          dev: null,
          gem_dev: null,
          ppp: null,
          ipv4: null,
          ipv6: null,
          prefix: null,
          prefix_host: null,
        };
        list.push(x);
        list.sort((a, z) => a.id - z.id);
      }
      if (body.USER) x.user = body.USER;
      if (body.SECRET) x.secret_set = true;
      if (body.VLAN !== undefined) x.vlan = body.VLAN;
      if (body.MAC !== undefined) x.mac = body.MAC;
      if (body.FAMILY === "ipv4" || body.FAMILY === "ipv6" || body.FAMILY === "both")
        x.family = body.FAMILY;
      /* The stand-in stick brings a session up at once, in the VLAN given. */
      x.dev = x.vlan ? `wan${x.vlan}` : "gem257";
      x.gem_dev = "gem257";
      x.ppp = `ppp${x.id - 1}`;
      x.ipv4 = x.family === "ipv6" ? null : "203.0.113.24";
      x.ipv6 = x.family === "ipv4" ? null : "fe80::2";
      /* The first IPv6 session takes 2001:db8:1200::/56 and hands it on. */
      const pd = list.find((y) => y.family !== "ipv4");
      x.prefix = x === pd ? "2001:db8:1200::/56" : null;
      x.prefix_host = x === pd ? "fe80::2" : null;
    }
    const first = settings.sessions.find((y) => y.family !== "ipv6");
    for (const y of settings.sessions) y.hands_on = y === first;
    if (!first) settings.pass = null;
    else settings.pass = { address: "203.0.113.24", lan: "lan" };
    settings.mode = settings.sessions.length ? "passthrough" : "bridge";
    settings.store = "saved";
    settings.last_apply_status = 0;
    return HttpResponse.json({ id: "session", ok: true });
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
      VENDOR_PRODUCT_CODE: "vendor_product_code",
      TM_OPTION: "tm_option",
      LOID: "loid",
      SW_VERSION0: "sw_version0",
      SW_VERSION1: "sw_version1",
      PACK_VERSION: "pack_version",
    };
    for (const [k, field] of Object.entries(map))
      if (b[k] !== undefined) (identity as unknown as Record<string, string>)[field] = b[k];
    if (b.PLOAM) identity.ploam_set = true;
    if (b.LOID_PASSWORD) identity.loid_password_set = true;
    settings.store = "saved";
    return HttpResponse.json({ id: "identity", ok: true });
  }),

  http.get("*/api/get/firmware", async () => {
    await delay(200);
    return (
      away() ?? (signedIn ? HttpResponse.json({ id: "firmware", value: firmware }) : unauthorized())
    );
  }),

  /* An upload: a file of the update's magic is written into the other bank. */
  http.post("*/api/firmware/upload", async ({ request }) => {
    if (!signedIn) return unauthorized();
    const body = new Uint8Array(await request.arrayBuffer());
    const target = 1 - (firmware.running ?? 0);
    firmware.update = {
      ...firmware.update,
      state: "writing",
      received_b: body.length,
      expected_b: body.length,
    };
    await delay(3000);
    if (String.fromCharCode(...body.slice(0, 4)) !== "ONUF") {
      firmware.update = {
        ...firmware.update,
        state: "failed",
        message: "not an update file, or a damaged one",
      };
      return HttpResponse.json(
        { error: "not written", detail: firmware.update.message },
        { status: 400 },
      );
    }
    const version = new TextDecoder().decode(body.slice(96, 160)).replace(/\0+$/, "") || "unknown";
    const written = firmware.banks[target];
    if (written) Object.assign(written, { state: "valid", version, time: Date.now() / 1000 });
    firmware.update = { ...firmware.update, state: "done", bank: target, version, message: null };
    return HttpResponse.json({ ok: true, detail: version });
  }),

  http.post("*/api/firmware/try", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const text = await request.text();
    firmware.next = text === "" ? 1 - (firmware.running ?? 0) : Number(text);
    return HttpResponse.json({ ok: true, detail: "the stick restarts into the bank once" });
  }),

  http.get("*/api/firmware/download", async () => {
    if (!signedIn) return unauthorized();
    return new HttpResponse(new Uint8Array([0x4f, 0x4e, 0x55, 0x46]), {
      headers: {
        "Content-Type": "application/octet-stream",
        "Content-Disposition": 'attachment; filename="buchos-gpon-325213d07776.bin"',
      },
    });
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
      if (k === "PEER") settings.settings.PASS_PEER = v;
    }
    settings.store = "saved";
    return HttpResponse.json({ id: "link", ok: true });
  }),
];
