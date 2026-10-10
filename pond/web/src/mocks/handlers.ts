// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * A stick in the browser, for work on the panel without one: the sign-in
 * with the default password and its forced change, the live figures with
 * traffic that moves, and changes of settings that are kept until reload.
 */

import { delay, HttpResponse, http } from "msw";
import type {
  Identity,
  IpoeInfo,
  LaneMode,
  Mgmt,
  Session,
  SessionInfo,
  Settings,
  SfpLane,
} from "../api/types";
import { BANKS, BRIDGE, FIRMWARE, LIVE, NETWORK, TC_RULES } from "./fixture";
import { OMCI, withServices } from "./omci";

let rootSecret = "admin";
const firmware = structuredClone(FIRMWARE);
let signedIn: Session | null = null;
let offline = false;
let omci = OMCI;
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

/*
 * POND_MOCK_SCENARIO=sessions: the stick's own connections in the shapes
 * the internet page shows (PPPoE with IPv4, IPv6 and both, up and dialling,
 * a static address and one from DHCP, one place of the seven free), and a
 * person signed in already with a password of their own, for a look at the
 * internet page without a sign-in.
 */
const SCENARIO = typeof process === "undefined" ? undefined : process.env.POND_MOCK_SCENARIO;

/* The VID on the fibre of each service the stand-in OLT translates, as onu-mode names its interface. */
const FIBRE: Record<string, string> = { "410": "100", "420": "200", "421": "300" };
const wanOf = (vlan: string) => `wan${FIBRE[vlan] ?? vlan}`;

/* The devices of a big house, as they name themselves, or not. */
const HOUSE = [
  "laptop",
  "telefon",
  null,
  "tv-salon",
  "iphone-ania",
  "galaxy-s24",
  "drukarka",
  "ps5",
  "chromecast-salon",
  "kamera-garaz",
  "kamera-ogrod",
  "kamera-wejscie",
  "termostat",
  "odkurzacz",
  null,
  "pralka",
  "zmywarka",
  "nas",
  "sonos-kuchnia",
  "sonos-salon",
  "echo-sypialnia",
  "tablet-kuba",
  "macbook-pawel",
  "rpi-homeassistant",
  "switch-kuba",
  null,
  "tv-sypialnia",
  "dekoder",
  "brama-garazowa",
  "alarm",
  "lodowka",
  "piekarnik",
  "rekuperator",
  "pompa-ciepla",
  "falownik",
  null,
  "ladowarka-auto",
  "zarowka-hol",
  "zarowka-salon",
  "gniazdko-biuro",
  "watch-ania",
  "kindle",
  "nvr",
  "kamera-ptz",
  null,
  "serwer-biuro",
  "drukarka-biuro",
  "voip",
] as const satisfies readonly (string | null)[];
/* A connection of @kind on VLAN @vlan, as it stands before it has an address. */
function ipoe(id: number, kind: IpoeInfo["kind"], vlan: string): IpoeInfo {
  return {
    id,
    kind,
    vlan,
    gem: "auto",
    mac: "",
    family: "ipv4",
    address: "",
    gateway: "",
    dns: "",
    address6: "",
    gateway6: "",
    prefix6: "",
    dev: wanOf(vlan),
    gem_dev: "gem279",
    up: false,
    ipv4: null,
    router: null,
    dns_now: null,
    ipv6: null,
    prefix: null,
    lease: null,
  };
}
if (SCENARIO === "sessions") {
  const one = (id: number, family: SessionInfo["family"], up: boolean): SessionInfo => ({
    id,
    user: family === "ipv6" ? `user${id}@example.net/ipv6` : `user${id}@example.net`,
    secret_set: true,
    gem: "auto",
    vlan: "410",
    mac: id === 1 ? "00:00:5e:00:53:a1" : "",
    family,
    pass: id <= 2 ? "on" : "off",
    host_vlan: "",
    hands_on: false,
    dev: "wan100",
    gem_dev: "gem278",
    ppp: up ? `ppp${id - 1}` : null,
    ipv4: up && family !== "ipv6" ? `203.0.113.${20 + id}` : null,
    ipv6: up && family !== "ipv4" ? `2001:db8:ff00::${id}` : null,
    prefix: null,
    prefix_host: null,
    lan: id <= 2 ? "lan410" : null,
    gateway: up && id === 1 ? "198.51.100.1" : null,
    dns: up && family !== "ipv6" ? "198.51.100.53 198.51.100.54" : null,
  });
  /* Slot 1 hands its IPv4 address down into VLAN 410, slot 2 its prefix. */
  const v4 = { ...one(1, "ipv4", true), hands_on: true };
  const v6 = {
    ...one(2, "ipv6", true),
    prefix: "2001:db8:1200::/56",
    prefix_host: "fe80::2",
  };
  /* Slot 3 hands both down into VLAN 36 of its own. */
  const both = {
    ...one(3, "both", true),
    pass: "on" as const,
    host_vlan: "36",
    hands_on: true,
    prefix: "2001:db8:1300::/56",
    prefix_host: "fe80::3",
    lan: "lan36",
    gateway: "198.51.100.9",
  };
  const list = [v4, v6, both, one(4, "both", false)];
  settings.sessions = list;
  /* Slot 3's concentrator did not take RFC 4638: its session came to 1492. */
  const ppp0 = NETWORK.links.find((l) => l.name === "ppp0");
  if (ppp0) NETWORK.links.push({ ...ppp0, name: "ppp2", ifindex: 18, mtu: 1492 });
  settings.pass = { address: "203.0.113.21", lan: "lan", peer_gateway: "198.51.100.1" };
  /* A fixed address on VLAN 420, and an address from DHCP on 421. */
  settings.ipoe = [
    {
      ...ipoe(1, "static", "420"),
      family: "both",
      address: "192.0.2.10/24",
      gateway: "192.0.2.1",
      dns: "198.51.100.53 198.51.100.54",
      address6: "2001:db8:5::2/64",
      gateway6: "2001:db8:5::1",
      prefix6: "2001:db8:5100::/56",
      up: true,
      ipv4: "192.0.2.10/24",
      router: "192.0.2.1",
      dns_now: "198.51.100.53 198.51.100.54",
      ipv6: "2001:db8:5::2/64",
      prefix: "2001:db8:5100::/56",
    },
    {
      ...ipoe(2, "dhcp", "421"),
      family: "both",
      up: true,
      ipv4: "198.51.100.77/24",
      router: "198.51.100.1",
      dns_now: "198.51.100.53",
      ipv6: "2001:db8:7::77/128",
      prefix: "2001:db8:7700::/56",
      lease: 82_800,
    },
  ];
  /*
   * A network behind the stick out through session 4, untagged, and one out
   * through DHCP on 421, in VLAN 20: a big house's fifty devices, a few
   * without a name; the static connection has none.
   */
  settings.lans = [
    {
      id: 1,
      name: "Dom",
      conn: "pppoe:4",
      vlan: "",
      address: "192.168.1.1/24",
      dhcp: "on",
      pool_from: "192.168.1.100",
      pool_to: "192.168.1.199",
      lease_s: 86_400,
      ipv6: "on",
      ipv6_subnet: "0",
    },
    {
      id: 2,
      name: "Biuro",
      conn: "ipoe:2",
      vlan: "20",
      address: "192.168.2.1/24",
      dhcp: "on",
      pool_from: "192.168.2.100",
      pool_to: "192.168.2.199",
      lease_s: 43_200,
      ipv6: "on",
      ipv6_subnet: "1",
    },
  ];
  /*
   * The devices of Biuro, whose connection has a prefix, have IPv6 in its
   * /64 network 1: one made of their MAC (EUI-64), and most a temporary one
   * besides; Dom's connection has none yet.
   */
  settings.leases = HOUSE.map((name, i) => {
    const lan = i < 42 ? 1 : 2;
    const host = 100 + (lan === 1 ? i : i - 42) * 2 + (i % 3 === 0 ? 1 : 0);
    const last = (0x10 + i).toString(16).padStart(2, "0");
    const temp = ((i * 2654435761) >>> 0).toString(16).padStart(8, "0");
    return {
      lan,
      mac: `00:00:5e:00:53:${last}`,
      ip: `192.168.${lan}.${host}`,
      name,
      left_s: [81_000, 43_200, 600, 3_600, 19_800, 72_000][i % 6] ?? 600,
      ipv6:
        lan === 2
          ? [
              `2001:db8:7700:1:200:5eff:fe00:53${last}`,
              ...(i % 3
                ? [`2001:db8:7700:1:${temp.slice(0, 4)}:${temp.slice(4)}:9e1:${last}0`]
                : []),
            ]
          : [],
    };
  });
  /*
   * Fixed addresses: the printer's where it is, with a name given by hand;
   * the NAS's elsewhere, which it takes at its next renewal; and a server
   * that is off now.
   */
  settings.fixed = [
    { lan: 1, mac: "00:00:5e:00:53:16", ip: "192.168.1.113", name: "Drukarka w biurze" },
    { lan: 1, mac: "00:00:5e:00:53:21", ip: "192.168.1.10", name: "" },
    { lan: 1, mac: "00:00:5e:00:53:80", ip: "192.168.1.20", name: "serwer-zapas" },
  ];
  rootSecret = "scenario-only";
  signedIn = { user: "root", must_change: false, idle_timeout_s: 900 };
}
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
let hostname = LIVE.hostname ?? "dfp-34x-2c3";
const upNow = () => LIVE.uptime.up_s + (Date.now() - started) / 1000;
let lane: SfpLane = { choice: "auto", active: "2500basex", trial: null };
let mgmt: Mgmt = { vlan: 4094, address: "192.0.2.250/30", data_address: null, pending: null };
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

/* The fibre without light: the ONU waits in O1, and omcid has no services. */
let dark = false;

/* A connection gone, or handing its address down, takes its network behind the stick along. */
function dropLans(conn: string) {
  const gone = (settings.lans ?? []).filter((y) => y.conn === conn).map((y) => y.id);
  settings.lans = (settings.lans ?? []).filter((y) => y.conn !== conn);
  settings.leases = (settings.leases ?? []).filter((y) => !gone.includes(y.lan));
  settings.fixed = (settings.fixed ?? []).filter((y) => !gone.includes(y.lan));
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
    hostname,
    settings,
    ...(dark
      ? {
          onu: LIVE.onu ? { ...LIVE.onu, state: "o1", onu_id: null } : null,
          optics: LIVE.optics ? { ...LIVE.optics, rx_power_uw: null } : null,
        }
      : {}),
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

  /* POST /api/__mock/fibre?on=0 takes the light away, and with it the OLT's services; on=1 brings them back. */
  http.post("*/api/__mock/fibre", ({ request }) => {
    dark = new URL(request.url).searchParams.get("on") === "0";
    return HttpResponse.json({ dark });
  }),

  /* POST /api/__mock/vlans?n=300 gives the line 300 more services; n=0 takes them away. */
  http.post("*/api/__mock/vlans", ({ request }) => {
    const n = Math.min(1000, Number(new URL(request.url).searchParams.get("n")) || 0);
    omci = n ? withServices(n) : OMCI;
    return HttpResponse.json({ services: 3 + n });
  }),

  http.get("*/api/auth/session", async () => {
    await delay(80);
    return away() ?? (signedIn ? HttpResponse.json(signedIn) : unauthorized());
  }),

  http.post("*/api/auth/login", async ({ request }) => {
    await delay(300);
    const { user, secret } = (await request.json()) as { user: string; secret: string };
    if (user !== "root" || secret !== rootSecret)
      return HttpResponse.json({ error: "wrong user or password" }, { status: 401 });
    signedIn = { user, must_change: rootSecret === "admin", idle_timeout_s: 900 };
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
    if (current !== rootSecret)
      return HttpResponse.json({ error: "wrong password" }, { status: 403 });
    if (next.length < 8) return HttpResponse.json({ error: "too short" }, { status: 400 });
    rootSecret = next;
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

  http.get("*/api/get/sfp_lane", async () => {
    await delay(120);
    if (lane.trial && upNow() > lane.trial.until_s)
      lane = { ...lane, choice: lane.trial.previous, trial: null };
    return (
      away() ?? (signedIn ? HttpResponse.json({ id: "sfp_lane", value: lane }) : unauthorized())
    );
  }),

  http.post("*/api/set/sfp_lane", async ({ request }) => {
    await delay(400);
    if (!signedIn) return unauthorized();
    const b = kv(await request.text());
    if (b.MODE)
      lane = {
        ...lane,
        trial: { mode: b.MODE as LaneMode, previous: lane.choice, until_s: upNow() + 60 },
        choice: b.MODE as LaneMode,
      };
    else if (b.KEEP) lane = { ...lane, trial: null };
    else if (b.REVERT && lane.trial) lane = { ...lane, choice: lane.trial.previous, trial: null };
    return HttpResponse.json({ id: "sfp_lane", ok: true });
  }),

  http.get("*/api/get/mgmt", async () => {
    await delay(120);
    return away() ?? (signedIn ? HttpResponse.json({ id: "mgmt", value: mgmt }) : unauthorized());
  }),

  http.post("*/api/set/mgmt", async ({ request }) => {
    await delay(400);
    if (!signedIn) return unauthorized();
    const b = kv(await request.text());
    if (b.VLAN && b.ADDRESS)
      mgmt = {
        ...mgmt,
        pending: { vlan: Number(b.VLAN), address: b.ADDRESS, until_s: upNow() + 600 },
      };
    else if (b.COMMIT && mgmt.pending)
      mgmt = { ...mgmt, vlan: mgmt.pending.vlan, address: mgmt.pending.address, pending: null };
    else if (b.CANCEL) mgmt = { ...mgmt, pending: null };
    return HttpResponse.json({ id: "mgmt", ok: true });
  }),

  http.post("*/api/set/hostname", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const b = kv(await request.text());
    if (!/^[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$/.test(b.HOSTNAME ?? ""))
      return HttpResponse.json({ error: "refused", detail: "not a host name" }, { status: 400 });
    hostname = b.HOSTNAME ?? hostname;
    return HttpResponse.json({ id: "hostname", ok: true });
  }),

  http.get("*/api/get/omci", async () => {
    await delay(600);
    return (
      away() ??
      (signedIn
        ? HttpResponse.json({ id: "omci", value: dark ? { mes: [] } : omci })
        : unauthorized())
    );
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
      /* Seven connections at most, of every kind together. */
      if (list.length + (settings.ipoe?.length ?? 0) >= 7) id = 0;
      if (!id || !body.USER)
        return HttpResponse.json(
          {
            error: "refused",
            detail: id ? "a new session needs USER" : "all 7 connections are taken",
          },
          { status: 400 },
        );
    }
    let x = list.find((y) => y.id === id);
    if (body.USER === "") {
      settings.sessions = list.filter((y) => y.id !== id);
      dropLans(`pppoe:${id}`);
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
          pass: "on",
          host_vlan: "",
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
      if (body.PASS === "on" || body.PASS === "off") x.pass = body.PASS;
      if (x.pass === "on") dropLans(`pppoe:${id}`);
      if (body.HOST_VLAN !== undefined) x.host_vlan = body.HOST_VLAN;
      /* The stand-in stick brings a session up at once, in the VLAN given. */
      x.dev = x.vlan ? `wan${x.vlan}` : "gem257";
      x.gem_dev = "gem257";
      x.ppp = `ppp${x.id - 1}`;
      x.ipv4 = x.family === "ipv6" ? null : "203.0.113.24";
      x.ipv6 = x.family === "ipv4" ? null : "fe80::2";
      x.prefix = x.family === "ipv4" ? null : "2001:db8:1200::/56";
    }
    /*
     * In each VLAN towards the host, the first session told to hand its
     * address down hands its IPv4 address, and the first with IPv6 its prefix.
     */
    const down = (y: SessionInfo) => (y.host_vlan === "untagged" ? "" : y.host_vlan || y.vlan);
    const firstIn = (y: SessionInfo, v4: boolean) =>
      settings.sessions.find(
        (z) => z.pass !== "off" && down(z) === down(y) && z.family !== (v4 ? "ipv6" : "ipv4"),
      ) === y;
    for (const y of settings.sessions) {
      y.hands_on = y.pass !== "off" && y.family !== "ipv6" && firstIn(y, true);
      y.prefix_host =
        y.pass !== "off" && y.family !== "ipv4" && firstIn(y, false) ? "fe80::2" : null;
    }
    const first = settings.sessions.find((y) => y.hands_on);
    if (!first) settings.pass = null;
    else settings.pass = { address: "203.0.113.24", lan: "lan", peer_gateway: "198.51.100.1" };
    settings.mode = settings.sessions.length ? "passthrough" : "bridge";
    settings.store = "saved";
    settings.last_apply_status = 0;
    return HttpResponse.json({ id: "session", ok: true });
  }),

  /*
   * A network behind the stick: added without ID, changed by ID, removed by
   * CONN=; CONN, VLAN, ADDRESS, DHCP, POOL_FROM, POOL_TO, LEASE.
   */
  http.post("*/api/set/lan", async ({ request }) => {
    await delay(500);
    if (!signedIn) return unauthorized();
    const body = kv(await request.text());
    settings.lans ??= [];
    const list = settings.lans;
    const id = body.ID ? Number(body.ID) : Math.max(0, ...list.map((y) => y.id)) + 1;
    if (body.CONN === "") {
      settings.lans = list.filter((y) => y.id !== id);
      settings.leases = (settings.leases ?? []).filter((y) => y.lan !== id);
      settings.fixed = (settings.fixed ?? []).filter((y) => y.lan !== id);
      return HttpResponse.json({ id: "lan", ok: true });
    }
    let x = list.find((y) => y.id === id);
    if (!x) {
      if (!body.CONN)
        return HttpResponse.json(
          { error: "refused", detail: "a new network needs CONN" },
          { status: 400 },
        );
      x = {
        id,
        conn: body.CONN,
        vlan: "",
        address: "",
        dhcp: "on",
        pool_from: "",
        pool_to: "",
        lease_s: 86_400,
      };
      list.push(x);
    }
    if (body.CONN) x.conn = body.CONN;
    if (body.NAME !== undefined) x.name = body.NAME;
    if (body.VLAN !== undefined) x.vlan = body.VLAN;
    if (body.ADDRESS) x.address = body.ADDRESS;
    if (body.DHCP === "on" || body.DHCP === "off") x.dhcp = body.DHCP;
    if (body.POOL_FROM) x.pool_from = body.POOL_FROM;
    if (body.POOL_TO) x.pool_to = body.POOL_TO;
    if (body.LEASE) x.lease_s = Number(body.LEASE);
    if (body.IPV6 === "on" || body.IPV6 === "off") x.ipv6 = body.IPV6;
    if (body.IPV6_SUBNET !== undefined) x.ipv6_subnet = body.IPV6_SUBNET;
    return HttpResponse.json({ id: "lan", ok: true });
  }),

  /*
   * A fixed address: a device's MAC and the IP it always gets in network LAN,
   * with NAME; IP= takes it away. A network's address of another device, or
   * one already fixed, is refused, as onu-mode would.
   */
  http.post("*/api/set/fixed", async ({ request }) => {
    await delay(500);
    if (!signedIn) return unauthorized();
    const body = kv(await request.text());
    const lan = Number(body.LAN);
    const mac = (body.MAC ?? "").toLowerCase();
    const list = (settings.fixed ?? []).filter((y) => !(y.lan === lan && y.mac === mac));
    if (!body.IP) {
      settings.fixed = list;
      return HttpResponse.json({ id: "fixed", ok: true });
    }
    if (
      !(settings.lans ?? []).some((y) => y.id === lan) ||
      !/^([0-9a-f]{2}:){5}[0-9a-f]{2}$/.test(mac)
    )
      return HttpResponse.json(
        { error: "refused", detail: "a fixed address needs a network and a MAC" },
        { status: 400 },
      );
    if (list.some((y) => y.lan === lan && y.ip === body.IP))
      return HttpResponse.json(
        { error: "refused", detail: `${body.IP} is another device's fixed address` },
        { status: 409 },
      );
    settings.fixed = [...list, { lan, mac, ip: body.IP, name: body.NAME ?? "" }];
    return HttpResponse.json({ id: "fixed", ok: true });
  }),

  /* A connection: added to the first free number, changed by ID, removed by KIND=. */
  http.post("*/api/set/ipoe", async ({ request }) => {
    await delay(500);
    if (!signedIn) return unauthorized();
    const body = kv(await request.text());
    settings.ipoe ??= [];
    const list = settings.ipoe;
    const refuse = (detail: string) =>
      HttpResponse.json({ error: "refused", detail }, { status: 400 });
    let id = body.ID ? Number(body.ID) : 0;
    if (body.KIND === "") {
      settings.ipoe = list.filter((y) => y.id !== id);
      dropLans(`ipoe:${id}`);
      return HttpResponse.json({ id: "ipoe", ok: true });
    }
    if (body.KIND !== "static" && body.KIND !== "dhcp") return refuse("KIND is static or dhcp");
    if (!id) {
      for (let n = 1; n <= 7 && !id; n++) if (!list.some((x) => x.id === n)) id = n;
      /* Seven connections at most, of every kind together. */
      if (!id || list.length + settings.sessions.length >= 7)
        return refuse("all 7 connections are taken");
    }
    /* A static connection is given an address and a gateway of each family it has. */
    if (body.KIND === "static") {
      const family = body.FAMILY ?? "ipv4";
      if (family !== "ipv6" && (!body.ADDRESS || !body.GATEWAY))
        return refuse(`connection ${id}: static IPv4 needs ADDRESS and GATEWAY`);
      if (family !== "ipv4" && (!body.ADDRESS6 || !body.GATEWAY6))
        return refuse(`connection ${id}: static IPv6 needs ADDRESS6 and GATEWAY6`);
    }
    let x = list.find((y) => y.id === id);
    if (!x) {
      x = ipoe(id, body.KIND, body.VLAN ?? "");
      list.push(x);
      list.sort((a, z) => a.id - z.id);
    }
    x.kind = body.KIND;
    if (body.VLAN !== undefined) x.vlan = body.VLAN;
    if (body.MAC !== undefined) x.mac = body.MAC;
    if (body.FAMILY === "ipv4" || body.FAMILY === "ipv6" || body.FAMILY === "both")
      x.family = body.FAMILY;
    for (const [k, f] of [
      ["ADDRESS", "address"],
      ["GATEWAY", "gateway"],
      ["DNS", "dns"],
      ["ADDRESS6", "address6"],
      ["GATEWAY6", "gateway6"],
      ["PREFIX6", "prefix6"],
    ] as const)
      if (body[k] !== undefined) x[f] = body[k];
    /* The stand-in stick has its address at once: the one typed, or one from DHCP. */
    const v4 = x.family !== "ipv6";
    const v6 = x.family !== "ipv4";
    x.dev = wanOf(x.vlan);
    x.up = true;
    if (x.kind === "static") {
      x.ipv4 = x.address || null;
      x.router = x.gateway || null;
      x.dns_now = x.dns;
      x.ipv6 = x.address6 || null;
      x.prefix = x.prefix6 || null;
      x.lease = null;
    } else {
      x.ipv4 = v4 ? "198.51.100.80/24" : null;
      x.router = v4 ? "198.51.100.1" : null;
      x.dns_now = v4 ? "198.51.100.53" : null;
      x.ipv6 = v6 ? "2001:db8:7::80/128" : null;
      x.prefix = v6 ? "2001:db8:7800::/56" : null;
      x.lease = 86_400;
    }
    return HttpResponse.json({ id: "ipoe", ok: true });
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
];
