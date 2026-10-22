// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * A stick in the browser, for work on the panel without one: the sign-in
 * with the default password and its forced change, the live figures with
 * traffic that moves, and changes of settings that are kept until reload.
 */

import type {
  Identity,
  IpoeInfo,
  Iptv,
  LaneMode,
  LeaseInfo,
  Mgmt,
  Phone,
  PotsPorts,
  ServiceRule,
  Session,
  SessionInfo,
  Sessions,
  Settings,
  SfpLane,
  UniPorts,
  UniReport,
} from "../api/types";
import { checkKey, checkPem, fingerprint, type KeyProblem, type PemProblem } from "../lib/keys";
import { MAX_MOVED, moved } from "../lib/ports";
import { DEFAULT_PRODUCT, productNameOk, productSlug } from "../lib/product";
import { MAX_FIXED, MAX_FORWARDS, MAX_LANS, MAX_RULES } from "../lib/state";
import { NotUpdate, readUpdate, type UpdateFile } from "../lib/updatefile";
import { conversation } from "./conversation";
import { BANKS, BRIDGE, clockNow, FIRMWARE, LIVE, NETWORK, SSH, TC_RULES, TLS } from "./fixture";
import { delay, type Handler, HttpResponse, http } from "./http";
import { OMCI, withServices, withVeip } from "./omci";

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
/* The servers' keys, which a change through the panel replaces, as on the stick. */
const ssh = structuredClone(SSH);
const tls = structuredClone(TLS);
const selfSigned = TLS.certificates.map((c) => c.sha256);
/* The devices with an address from the networks' DHCP servers: a value of their own, as on the stick. */
let leases: LeaseInfo[] = [];
/*
 * The sessions open besides this browser's, by the stick's clock in seconds,
 * and when this browser's began; and the token of scripts, or none.
 */
let others: Sessions["sessions"] = [];
let mine = Math.floor(Date.now() / 1000);
let token: string | null = null;

/*
 * POND_MOCK_SCENARIO=sessions: the stick's own connections in the shapes
 * the internet page shows (PPPoE with IPv4, IPv6 and both, up and dialling,
 * a static address and one from DHCP, one place of the seven free), and a
 * person signed in already with a password of their own, for a look at the
 * internet page without a sign-in. The demo build (vite --mode demo) shows
 * the same, with root's password the default one, admin, which its notice
 * gives for the actions that ask for it.
 */
const SCENARIO =
  import.meta.env?.MODE === "demo"
    ? "demo"
    : typeof process === "undefined"
      ? undefined
      : process.env.POND_MOCK_SCENARIO;

/*
 * POND_MOCK_SCENARIO=stick: the stick as its software serves it today, with
 * none of the routing (no static or DHCP connections, networks, forwards or
 * rules), the firmware it shipped with still in the other bank, as after an
 * install through that firmware's page, and a person signed in already, for a
 * look at what the panel says of what is not on the stick yet.
 */
/* A browser of another computer, signed in an hour ago, and a script's token. */
function elsewhere() {
  const now = Math.floor(Date.now() / 1000);
  mine = now - 600;
  others = [{ since_s: now - 3900, seen_s: now - 40, from: "192.0.2.10", this: false }];
  token = "5f".repeat(32);
}

if (SCENARIO === "stick") {
  rootSecret = "scenario-only";
  signedIn = { user: "root", must_change: false, idle_timeout_s: 900 };
  elsewhere();
  Object.assign(firmware.banks[1] ?? {}, {
    state: "shipped",
    version: "1.2.0",
    rootfs_used_b: null,
    sha256: null,
    signature: null,
    signer: null,
  });
}

/* The VID on the fibre of each service the stand-in OLT translates, as onu-apply names its interface. */
const FIBRE: Record<string, string> = { "410": "100", "420": "200", "421": "300" };
const wanOf = (vlan: string) => `wan${FIBRE[vlan] ?? vlan}`;

/* The devices of a big house, as they name themselves, or not. */
const HOUSE = [
  "laptop",
  "phone",
  null,
  "tv-living-room",
  "iphone-1",
  "galaxy-s24",
  "printer",
  "ps5",
  "chromecast-living-room",
  "garage-cam",
  "garden-cam",
  "entrance-cam",
  "thermostat",
  "vacuum",
  null,
  "washer",
  "dishwasher",
  "nas",
  "sonos-kitchen",
  "sonos-living-room",
  "echo-bedroom",
  "tablet-2",
  "macbook-3",
  "rpi-homeassistant",
  "switch-console",
  null,
  "tv-bedroom",
  "set-top-box",
  "garage-gate",
  "alarm",
  "fridge",
  "oven",
  "ventilation",
  "heat-pump",
  "inverter",
  null,
  "car-charger",
  "bulb-hall",
  "bulb-living-room",
  "plug-office",
  "watch-1",
  "kindle",
  "nvr",
  "ptz-cam",
  null,
  "office-server",
  "office-printer",
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
if (SCENARIO === "sessions" || SCENARIO === "demo") {
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
   * Two networks behind the stick out through session 4, the house's
   * untagged and the guests' in VLAN 30, and one out through DHCP on 421, in
   * VLAN 20: a big house's fifty devices, a few without a name; the static
   * connection has none.
   */
  settings.lans = [
    {
      id: 1,
      name: "Home",
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
      name: "Office",
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
    {
      id: 3,
      name: "Guests",
      conn: "pppoe:4",
      vlan: "30",
      address: "192.168.3.1/24",
      dhcp: "on",
      pool_from: "192.168.3.100",
      pool_to: "192.168.3.199",
      lease_s: 3_600,
      ipv6: "on",
      ipv6_subnet: "1",
    },
  ];
  /*
   * The devices of Office, whose connection has a prefix, have IPv6 in its
   * /64 network 1: one made of their MAC (EUI-64), and most a temporary one
   * besides; Home's connection has none yet.
   */
  leases = HOUSE.map((name, i) => {
    const lan = i < 38 ? 1 : i < 42 ? 3 : 2;
    const host = 100 + (lan === 1 ? i : lan === 3 ? i - 38 : i - 42) * 2 + (i % 3 === 0 ? 1 : 0);
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
    { lan: 1, mac: "00:00:5e:00:53:16", ip: "192.168.1.113", name: "Office printer" },
    { lan: 1, mac: "00:00:5e:00:53:21", ip: "192.168.1.10", name: "" },
    { lan: 1, mac: "00:00:5e:00:53:80", ip: "192.168.1.20", name: "backup-server" },
    { lan: 1, mac: "00:00:5e:00:53:17", ip: "192.168.1.114", name: "" },
    { lan: 1, mac: "00:00:5e:00:53:19", ip: "192.168.1.119", name: "" },
    { lan: 2, mac: "00:00:5e:00:53:3d", ip: "192.168.2.107", name: "" },
    { lan: 2, mac: "00:00:5e:00:53:3a", ip: "192.168.2.101", name: "" },
  ];
  /*
   * Ports forwarded: the office server's HTTPS through DHCP, the console's
   * game port and, turned off, the garage camera through session 4; Office's
   * NVR is DHCP's DMZ. Each device has its fixed address above.
   */
  settings.forwards = [
    {
      id: 1,
      name: "Web server",
      conn: "ipoe:2",
      proto: "tcp",
      ext_from: 443,
      ext_to: 443,
      lan: 2,
      mac: "00:00:5e:00:53:3d",
      int_port: 443,
      on: true,
    },
    {
      id: 2,
      name: "Game console",
      conn: "pppoe:4",
      proto: "udp",
      ext_from: 3074,
      ext_to: 3074,
      lan: 1,
      mac: "00:00:5e:00:53:17",
      int_port: 3074,
      on: true,
    },
    {
      id: 3,
      name: "Garage camera",
      conn: "pppoe:4",
      proto: "tcp",
      ext_from: 8080,
      ext_to: 8080,
      lan: 1,
      mac: "00:00:5e:00:53:19",
      int_port: 80,
      on: false,
    },
  ];
  settings.dmz = [{ conn: "ipoe:2", lan: 2, mac: "00:00:5e:00:53:3a" }];
  /*
   * The firewall: Office may print on the printer in Home; no network sends
   * mail out by SMTP; the NAS answers HTTPS from the internet by IPv6; and,
   * turned off, the garage camera kept from the internet.
   */
  settings.rules = [
    {
      id: 1,
      name: "Printer for Office",
      action: "allow",
      from: "lan:2",
      to: "dev:1:00:00:5e:00:53:16",
      proto: "tcp",
      ports: "9100",
      family: "any",
      on: true,
    },
    {
      id: 2,
      name: "No outgoing SMTP",
      action: "block",
      from: "lans",
      to: "internet",
      proto: "tcp",
      ports: "25",
      family: "any",
      on: true,
    },
    {
      id: 3,
      name: "NAS over IPv6",
      action: "allow",
      from: "internet",
      to: "dev:1:00:00:5e:00:53:21",
      proto: "tcp",
      ports: "443",
      family: "ipv6",
      on: true,
    },
    {
      id: 4,
      name: "Camera without cloud",
      action: "block",
      from: "dev:1:00:00:5e:00:53:19",
      to: "internet",
      proto: "any",
      ports: "",
      family: "any",
      on: false,
    },
  ];
  settings.fw = { out: "allow", between: "block" };
  rootSecret = SCENARIO === "demo" ? "admin" : "scenario-only";
  signedIn = { user: "root", must_change: false, idle_timeout_s: 900 };
  /* An OLT that set up a service on the VEIP too, which the stick moved to its own VLAN. */
  omci = withVeip();
  elsewhere();
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
/* The name the panel shows for the system, which the files handed out are named after. */
let product = LIVE.product ?? DEFAULT_PRODUCT;
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
  uni_report: "both",
  uni_ports: 1,
  pots_ports: 0,
  olt_login: null,
};
/*
 * The rules of the services: the stick's own, which settle a clash of the
 * OLT's services, and a person's, which take their place, by the VLAN on
 * the fibre.
 */
let autoRules: ServiceRule[] = [];
const ownRules = new Map<number | null, ServiceRule>();
const serviceRules = () => [
  ...autoRules.filter((r) => !ownRules.has(r.fibre)),
  ...ownRules.values(),
];
/*
 * Television, as omcid's multicast agent holds it: the OLT's policy of the
 * line (mocks/omci.ts: IGMPv2, snooping, groups 224.0.0.0 to
 * 239.255.255.255 in VLAN 300 on the multicast channel), two channels
 * watched, a join refused, and the latest events; times by the stick's clock.
 */
const iptvAt = () => Math.floor(upNow());
const iptvBase = iptvAt();
const iptv: Omit<Iptv, "now_s"> = {
  policy: {
    igmp_version: 2,
    mode: "snoop",
    fast_leave: true,
    max_groups: 4,
    max_bandwidth_bps: 40_000_000,
    robustness: 2,
    query_interval_s: 125,
    query_response_ms: 10000,
    last_member_ms: 1000,
    host_vlan: 421,
    allowed: [
      { from: "224.0.0.0", to: "239.255.255.255", vlan: 300, bandwidth_bps: null, preview: false },
    ],
    static: [],
  },
  groups: [
    {
      group: "239.10.0.1",
      source: null,
      vlan: 421,
      client: "192.168.1.60",
      since_s: iptvBase - 1830,
      rate_bps: 8_100_000,
    },
    {
      group: "239.10.0.17",
      source: null,
      vlan: 421,
      client: "192.168.1.61",
      since_s: iptvBase - 240,
      rate_bps: 9_700_000,
    },
  ],
  rejected: [
    {
      at_s: iptvBase - 610,
      group: "239.200.0.5",
      vlan: 421,
      client: "192.168.1.61",
      reason: "max_groups",
    },
  ],
  lost: [],
  counters: { joins: 214, leaves: 212, rejected: 1, queries: 96, reports: 188 },
  querier: { last_s: iptvBase - 37, version: 2 },
  priority: { control: 0, by: "stick", pcp: 4, auto: true, dropped: 0 },
  log: [
    { at_s: iptvBase - 37, event: "query", group: null, vlan: 300 },
    { at_s: iptvBase - 240, event: "join", group: "239.10.0.17", vlan: 421 },
    { at_s: iptvBase - 241, event: "leave", group: "239.10.0.9", vlan: 421 },
    { at_s: iptvBase - 610, event: "reject", group: "239.200.0.5", vlan: 421 },
    { at_s: iptvBase - 1200, event: "lost", group: "239.10.0.9", vlan: 421 },
    { at_s: iptvBase - 1188, event: "back", group: "239.10.0.9", vlan: 421 },
    { at_s: iptvBase - 1830, event: "join", group: "239.10.0.1", vlan: 421 },
  ],
};
/* The phone the OLT set up, and the SIP passwords, which root's password shows. */
const phone: Phone = {
  method: "none",
  lines: [],
  server: null,
  codecs: [],
  dtmf: null,
  vlan: null,
};
let phonePasswords: Record<number, string> = {};
/* The PPPoE password the OLT sent with olt_login, which root's password shows. */
let oltPassword = "";
/* The scenarios' OLT sent a PPPoE login. */
if (SCENARIO === "sessions" || SCENARIO === "demo") {
  identity.olt_login = { user: "abc12345@operator.example", password_set: true };
  oltPassword = "example-secret";
  /* The service the OLT set up on the VEIP clashes with internet: the stick moved it. */
  autoRules = [{ fibre: 500, action: "move", host: 500, auto: true }];
  /* An ONT of one phone socket stood in for, the phone set up through OMCI. */
  identity.pots_ports = 1;
  Object.assign(phone, {
    method: "omci",
    lines: [
      {
        port: 1,
        number: "+48221234567",
        display: "Example",
        user: "48221234567",
        password_set: true,
      },
    ],
    server: {
      proxy: "sip.operator.example",
      outbound_proxy: "192.0.2.10",
      registrar: "sip.operator.example",
      domain: "operator.example",
      port: 5060,
      transport: "udp",
    },
    codecs: ["G.711 A-law", "G.711 µ-law", "G.729"],
    dtmf: "rfc2833",
    vlan: { fibre: 600, host: 600 },
  } satisfies Phone);
  phonePasswords = { 1: "example-sip-secret" };
}

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
/* When the fibre went dark or came back, for the time in the ONU's state. */
let darkChanged = 0;

/* A connection gone, or handing its address down, takes its network behind the stick along. */
function dropLans(conn: string) {
  const gone = (settings.lans ?? []).filter((y) => y.conn === conn).map((y) => y.id);
  settings.lans = (settings.lans ?? []).filter((y) => y.conn !== conn);
  leases = leases.filter((y) => !gone.includes(y.lan));
  settings.fixed = (settings.fixed ?? []).filter((y) => !gone.includes(y.lan));
  settings.forwards = (settings.forwards ?? []).filter((y) => !gone.includes(y.lan));
  settings.dmz = (settings.dmz ?? []).filter((y) => !gone.includes(y.lan));
  for (const id of gone) dropRules(id);
}

/* The rules of network @id go with it, as their ends would no longer be. */
function dropRules(id: number) {
  const of = (end: string) => end === `lan:${id}` || end.startsWith(`dev:${id}:`);
  settings.rules = (settings.rules ?? []).filter((y) => !of(y.from) && !of(y.to));
}

/* A forward's or a DMZ's device keeps its address: the one it has now, fixed, if it has none fixed. */
function pin(lan: number, mac: string) {
  if ((settings.fixed ?? []).some((y) => y.lan === lan && y.mac === mac)) return;
  const now = leases.find((y) => y.lan === lan && y.mac === mac);
  if (now) settings.fixed = [...(settings.fixed ?? []), { lan, mac, ip: now.ip, name: "" }];
}

/* Whether the device's address could not be fixed, at the most fixed addresses. */
const noRoomToPin = (lan: number, mac: string) =>
  (settings.fixed ?? []).length >= MAX_FIXED &&
  !(settings.fixed ?? []).some((y) => y.lan === lan && y.mac === mac);

const fixedFull = () =>
  HttpResponse.json(
    { error: "refused", detail: `all ${MAX_FIXED} fixed addresses are taken` },
    { status: 400 },
  );

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
    product,
    settings,
    onu: LIVE.onu && {
      ...LIVE.onu,
      state_ms: darkChanged
        ? Date.now() - darkChanged
        : (LIVE.onu.state_ms ?? 0) + Date.now() - started,
    },
    ...(dark
      ? {
          onu: LIVE.onu
            ? {
                ...LIVE.onu,
                state: "o1",
                onu_id: null,
                eqd: null,
                state_ms: Date.now() - darkChanged,
              }
            : null,
          optics: LIVE.optics ? { ...LIVE.optics, rx_power_uw: null } : null,
        }
      : {}),
  };
}

const unauthorized = () => HttpResponse.json({ error: "no session" }, { status: 401 });

/* Root's password again, which an update and a start of the other bank need. */
const rootPassword = (request: Request) =>
  decodeURIComponent(request.headers.get("X-Pond-Password") ?? "") === rootSecret;
const wrongPassword = () =>
  HttpResponse.json(
    { error: "wrong password", detail: "root's password is needed for this" },
    { status: 403 },
  );

/*
 * The refusal of a change that needs root's password, or null for the right
 * one: five wrong ones in a row stop every one for half a minute, as pond
 * stops them, the right one too, and say how long.
 */
let wrongs = 0;
let stopped = 0;
function passwordRefused(request: Request): Response | null {
  const now = Date.now();
  if (now < stopped) {
    const left = Math.ceil((stopped - now) / 1000);
    return HttpResponse.json(
      { error: "too many attempts", detail: "wait half a minute and try again", retry_after: left },
      { status: 429, headers: { "Retry-After": String(left) } },
    );
  }
  if (rootPassword(request)) {
    wrongs = 0;
    return null;
  }
  if (++wrongs >= 5) {
    wrongs = 0;
    stopped = now + 30_000;
  }
  return wrongPassword();
}

const away = () => (offline ? HttpResponse.error() : null);

/* onu-config's refusal of a change of the servers' keys, as pond answers it. */
const refused = (detail: string) =>
  HttpResponse.json({ error: "refused", detail }, { status: 400 });

/* What onu-config and pond say of a key or a certificate they refuse, in their words. */
const KEY_SAYS: Record<KeyProblem, string> = {
  empty: "no key: a line TYPE BASE64 [COMMENT]",
  lines: "more than one line: one key at a time",
  options: "a key with options in front of it: give the key alone",
  type: "a key of another type: the stick takes ssh-ed25519, ecdsa-sha2-nistp256, -nistp384, -nistp521 and ssh-rsa",
  base64: "the key is not base64, or too long for the store",
  inside: "the key inside is not of its type",
  rsa: "an RSA key of another size: the stick takes 2048 to 4096 bits",
  comment: "the comment is longer than 100 bytes, or holds a control character",
};
const PEM_SAYS: Record<PemProblem, string> = {
  noCertificate: "no certificate: a PEM block BEGIN CERTIFICATE",
  noKey: "no key: a PEM block BEGIN PRIVATE KEY or BEGIN EC PRIVATE KEY",
  encrypted: "the key is encrypted: give it without its passphrase",
  rsa: "an RSA key: the panel's HTTPS takes an ECDSA key of P-256 alone",
};

const random = (n: number) => crypto.getRandomValues(new Uint8Array(n));
const hexFingerprint = () =>
  [...random(32)].map((b) => b.toString(16).toUpperCase().padStart(2, "0")).join(":");

/* The paths the stick refuses, with the status it refuses them with, and how long it takes over each answer. */
const failing = new Map<string, number>();
let slow = 0;

/*
 * A handler of the stick's, which the switches below make slow or refuse,
 * before it answers: so that a read's first load, its failure and a retry
 * are seen on any page. The switches' own paths answer as they are.
 */
function faulty(h: Handler): Handler {
  if (h.path.startsWith("/api/__mock/")) return h;
  return {
    ...h,
    resolve: async (info) => {
      if (slow) await delay(slow);
      const status = failing.get(h.path);
      if (status)
        return HttpResponse.json(
          { error: "refused", detail: `the mock stick refuses ${h.path} (/api/__mock/fail)` },
          { status },
        );
      return h.resolve(info);
    },
  };
}

const stick = [
  /* POST /api/__mock/offline?on=1 makes the stick vanish; on=0 brings it back. */
  http.post("*/api/__mock/offline", ({ request }) => {
    offline = new URL(request.url).searchParams.get("on") === "1";
    return HttpResponse.json({ offline });
  }),

  /*
   * POST /api/__mock/fail?path=/api/get/omci&status=500 makes the stick
   * refuse that path, a read or a change, with that status (500 without
   * one; 502 to 504 are taken for a stick that does not answer, 401 for a
   * session that ended); status=0 answers it again, and no path answers
   * every one again.
   */
  http.post("*/api/__mock/fail", ({ request }) => {
    const q = new URL(request.url).searchParams;
    const path = q.get("path");
    const status = Number(q.get("status") ?? 500) || 0;
    if (!path) failing.clear();
    else if (status) failing.set(path, status);
    else failing.delete(path);
    return HttpResponse.json({ failing: Object.fromEntries(failing) });
  }),

  /* POST /api/__mock/slow?ms=3000 holds every answer of the stick's that long; ms=0 lets them go. */
  http.post("*/api/__mock/slow", ({ request }) => {
    slow = Math.min(60_000, Number(new URL(request.url).searchParams.get("ms")) || 0);
    return HttpResponse.json({ slow });
  }),

  /* POST /api/__mock/fibre?on=0 takes the light away, and with it the OLT's services; on=1 brings them back. */
  http.post("*/api/__mock/fibre", ({ request }) => {
    const off = new URL(request.url).searchParams.get("on") === "0";
    if (off !== dark) darkChanged = Date.now();
    dark = off;
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
    mine = Math.floor(Date.now() / 1000);
    return HttpResponse.json(signedIn);
  }),

  http.post("*/api/auth/logout", () => {
    signedIn = null;
    return HttpResponse.json({});
  }),

  /* The sessions open, this browser's among them, by the stick's clock. */
  http.get("*/api/auth/sessions", async () => {
    await delay(150);
    if (!signedIn) return away() ?? unauthorized();
    const now = Math.floor(Date.now() / 1000);
    const value: Sessions = {
      now_s: now,
      max: 3,
      idle_timeout_s: 900,
      max_age_s: 43200,
      sessions: [...others, { since_s: mine, seen_s: now, from: "127.0.0.1", this: true }].sort(
        (a, b) => a.since_s - b.since_s,
      ),
    };
    return away() ?? HttpResponse.json(value);
  }),

  /* Every session but this browser's ended, with root's password. */
  http.post("*/api/auth/sessions/others", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    const ended = others.length;
    others = [];
    return HttpResponse.json({ ok: true, ended });
  }),

  /* Whether a token of scripts is in force; a new one, shown once; none. */
  http.get("*/api/token", async () => {
    await delay(100);
    if (!signedIn) return away() ?? unauthorized();
    return away() ?? HttpResponse.json({ set: token !== null });
  }),

  http.post("*/api/token/new", async ({ request }) => {
    await delay(400);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    token = [...random(32)].map((b) => b.toString(16).padStart(2, "0")).join("");
    return HttpResponse.json({ ok: true, token });
  }),

  http.post("*/api/token/off", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    token = null;
    return HttpResponse.json({ ok: true });
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

  /*
   * The devices with a lease, a value apart from the settings, as pond
   * serves it; a stick whose software has no networks has no such value.
   */
  http.get("*/api/get/leases", async () => {
    await delay(150);
    if (!signedIn) return away() ?? unauthorized();
    return (
      away() ??
      (settings.lans
        ? HttpResponse.json({ id: "leases", value: leases })
        : HttpResponse.json({ error: "no such value" }, { status: 404 }))
    );
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
        trial: { mode: b.MODE as LaneMode, previous: lane.choice, until_s: upNow() + 90 },
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

  /* An empty name takes the stick's away, for BuchOS, as onu-config does. */
  http.post("*/api/set/product", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const b = kv(await request.text());
    const name = b.PRODUCT_NAME;
    if (name === undefined || (name !== "" && !productNameOk(name)))
      return HttpResponse.json(
        { error: "refused", detail: "PRODUCT_NAME: not a product name" },
        { status: 400 },
      );
    product = name || DEFAULT_PRODUCT;
    return HttpResponse.json({ id: "product", ok: true });
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

  /* The conversation began 20 minutes before the mock stick did. */
  http.get("*/api/get/omci_frames", async ({ request }) => {
    await delay(150);
    const since = Number(new URL(request.url).searchParams.get("since") ?? 0) || 0;
    const value = dark
      ? { head: 0, first: 0, frames: [] }
      : conversation(omci, Date.now(), Date.now() - started + 20 * 60_000, since);
    return away() ?? (signedIn ? HttpResponse.json({ id: "omci_frames", value }) : unauthorized());
  }),

  http.get("*/api/get/tls", async () => {
    await delay(200);
    return away() ?? (signedIn ? HttpResponse.json({ id: "tls", value: tls }) : unauthorized());
  }),

  http.get("*/api/get/ssh", async () => {
    await delay(150);
    return away() ?? (signedIn ? HttpResponse.json({ id: "ssh", value: ssh }) : unauthorized());
  }),

  /*
   * The servers' keys, each change with root's password, which the stick
   * judges as onu-config does: a public key by the panel's own check, which
   * takes and refuses what the stick does, an own certificate by its blocks
   * alone, which the mock does not read further.
   */
  http.post("*/api/ssh/host-key", async ({ request }) => {
    await delay(500);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    ssh.host_key = { type: "ssh-ed25519", fingerprint: await fingerprint(random(51)) };
    return HttpResponse.json({ ok: true, host_key: ssh.host_key });
  }),

  http.post("*/api/ssh/keys", async ({ request }) => {
    await delay(400);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    const c = checkKey(await request.text());
    if (!c.ok) return refused(KEY_SAYS[c.problem]);
    const fp = await fingerprint(c.blob);
    const panel = ssh.keys.filter((k) => k.source === "panel");
    if (panel.some((k) => k.fingerprint === fp)) return refused(`the key ${fp} is kept already`);
    if (panel.length >= ssh.keys_max)
      return refused(`${ssh.keys_max} keys at most: remove one first`);
    const key = {
      type: c.type,
      bits: c.bits,
      fingerprint: fp,
      comment: c.comment,
      source: "panel" as const,
    };
    /* The store's keys first, then the image's, as onu-config says them. */
    ssh.keys.splice(panel.length, 0, key);
    return HttpResponse.json({ ok: true, key });
  }),

  http.post("*/api/ssh/keys/remove", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    const fp = (await request.text()).trim();
    const at = ssh.keys.findIndex((k) => k.source === "panel" && k.fingerprint === fp);
    if (at < 0) return refused(`no key ${fp} is kept`);
    if (ssh.keys_only && ssh.keys.filter((k) => k.source === "panel").length === 1)
      return refused("the last key stays while ssh takes keys alone: turn that off first");
    ssh.keys.splice(at, 1);
    return HttpResponse.json({ ok: true });
  }),

  http.post("*/api/ssh/keys-only", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    const state = (await request.text()).trim();
    if (state !== "on" && state !== "off")
      return HttpResponse.json(
        { error: "not on or off", detail: "the body is on or off" },
        { status: 400 },
      );
    if (state === "on" && !ssh.keys.some((k) => k.source === "panel"))
      return refused("no key is kept: add one first");
    ssh.keys_only = state === "on";
    return HttpResponse.json({ ok: true });
  }),

  http.post("*/api/tls/key", async ({ request }) => {
    await delay(500);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    for (let i = 0; i < selfSigned.length; i++) selfSigned[i] = hexFingerprint();
    if (!tls.own)
      for (const [i, c] of tls.certificates.entries()) c.sha256 = selfSigned[i] ?? c.sha256;
    return HttpResponse.json({ ok: true, tls });
  }),

  http.post("*/api/tls/certificate", async ({ request }) => {
    await delay(500);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    const body = (await request.json().catch(() => ({}))) as { certificate?: string; key?: string };
    const problem = checkPem(body.certificate ?? "", body.key ?? "");
    if (problem)
      return HttpResponse.json(
        { error: "not a certificate", detail: PEM_SAYS[problem] },
        { status: 400 },
      );
    const now = Date.now();
    tls.own = {
      subject: "O=Example, CN=stick.example",
      issuer: "O=Example CA, CN=Example Intermediate",
      sha256: hexFingerprint(),
      not_before: new Date(now).toISOString().replace(/\.\d+Z$/, "Z"),
      not_after: new Date(now + 397 * 86_400_000).toISOString().replace(/\.\d+Z$/, "Z"),
      chain: (body.certificate?.match(/-----BEGIN CERTIFICATE-----/g) ?? []).length,
      names: ["DNS:stick.example", "DNS:localhost", "IP:127.0.0.1"],
    };
    for (const c of tls.certificates)
      Object.assign(c, {
        sha256: tls.own.sha256,
        not_before: tls.own.not_before,
        not_after: tls.own.not_after,
        named: c.address === "localhost" || c.address === "127.0.0.1",
      });
    return HttpResponse.json({ ok: true, tls });
  }),

  http.post("*/api/tls/self-signed", async ({ request }) => {
    await delay(400);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    tls.own = null;
    for (const [i, c] of tls.certificates.entries())
      Object.assign(c, { ...TLS.certificates[i], sha256: selfSigned[i] ?? c.sha256 });
    return HttpResponse.json({ ok: true, tls });
  }),

  http.get("*/api/get/clock", async () => {
    await delay(150);
    return (
      away() ?? (signedIn ? HttpResponse.json({ id: "clock", value: clockNow() }) : unauthorized())
    );
  }),

  http.get("*/api/get/banks", async () => {
    await delay(400);
    return away() ?? (signedIn ? HttpResponse.json({ id: "banks", value: BANKS }) : unauthorized());
  }),

  /* The changes on trial kept (CONFIRM=1), but a new management address, or taken back (REVERT=1). */
  http.post("*/api/set/changes", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const body = kv(await request.text());
    if (body.CONFIRM !== "1" && body.REVERT !== "1")
      return HttpResponse.json(
        { error: "changes", detail: "CONFIRM=1 or REVERT=1" },
        { status: 400 },
      );
    settings.trials =
      body.CONFIRM === "1" ? (settings.trials ?? []).filter((x) => x.name === "mgmt") : [];
    return HttpResponse.json({ ok: true });
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
    /* On trial until the panel confirms it, as on the stick. */
    settings.trials = [
      ...(settings.trials ?? []).filter((x) => x.name !== "settings"),
      { name: "settings", until_s: Math.round(upNow()) + 90 },
    ];
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
      leases = leases.filter((y) => y.lan !== id);
      settings.fixed = (settings.fixed ?? []).filter((y) => y.lan !== id);
      settings.forwards = (settings.forwards ?? []).filter((y) => y.lan !== id);
      settings.dmz = (settings.dmz ?? []).filter((y) => y.lan !== id);
      dropRules(id);
      return HttpResponse.json({ id: "lan", ok: true });
    }
    let x = list.find((y) => y.id === id);
    if (!x) {
      if (!body.CONN)
        return HttpResponse.json(
          { error: "refused", detail: "a new network needs CONN" },
          { status: 400 },
        );
      if (list.length >= MAX_LANS)
        return HttpResponse.json(
          { error: "refused", detail: `all ${MAX_LANS} local networks are taken` },
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
   * with NAME; IP= takes it away, and the forwards to the device, the DMZ
   * that is it and the firewall's rules that go to it with it, as pond does:
   * a rule reaches a device by its fixed address. A network's address of
   * another device, or one already fixed, is refused, as onu-apply would.
   */
  http.post("*/api/set/fixed", async ({ request }) => {
    await delay(500);
    if (!signedIn) return unauthorized();
    const body = kv(await request.text());
    const lan = Number(body.LAN);
    const mac = (body.MAC ?? "").toLowerCase();
    const list = (settings.fixed ?? []).filter((y) => !(y.lan === lan && y.mac === mac));
    if (!body.IP) {
      const other = (y: { lan: number; mac: string }) => !(y.lan === lan && y.mac === mac);
      settings.fixed = list;
      settings.forwards = (settings.forwards ?? []).filter(other);
      settings.dmz = (settings.dmz ?? []).filter(other);
      settings.rules = (settings.rules ?? []).filter((y) => y.to !== `dev:${lan}:${mac}`);
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
    if (list.length >= MAX_FIXED)
      return HttpResponse.json(
        { error: "refused", detail: `all ${MAX_FIXED} fixed addresses are taken` },
        { status: 400 },
      );
    settings.fixed = [...list, { lan, mac, ip: body.IP, name: body.NAME ?? "" }];
    return HttpResponse.json({ id: "fixed", ok: true });
  }),

  /*
   * A forward: added with no ID, changed by ID, removed by CONN=. Its device
   * gets its address fixed; ports of another forward of the connection, of
   * a protocol of its, are refused.
   */
  http.post("*/api/set/forward", async ({ request }) => {
    await delay(500);
    if (!signedIn) return unauthorized();
    const b = kv(await request.text());
    const list = settings.forwards ?? [];
    const id = b.ID ? Number(b.ID) : Math.max(0, ...list.map((y) => y.id)) + 1;
    if (b.CONN === "") {
      settings.forwards = list.filter((y) => y.id !== id);
      return HttpResponse.json({ id: "forward", ok: true });
    }
    const x = {
      id,
      name: b.NAME ?? "",
      conn: b.CONN ?? "",
      proto: (b.PROTO ?? "tcp") as "tcp" | "udp" | "both",
      ext_from: Number(b.EXT_FROM),
      ext_to: Number(b.EXT_TO || b.EXT_FROM),
      lan: Number(b.LAN),
      mac: (b.MAC ?? "").toLowerCase(),
      int_port: Number(b.INT_PORT || b.EXT_FROM),
      on: b.ON !== "off",
    };
    if (!list.some((y) => y.id === id) && list.length >= MAX_FORWARDS)
      return HttpResponse.json(
        { error: "refused", detail: `all ${MAX_FORWARDS} forwards are taken` },
        { status: 400 },
      );
    if (noRoomToPin(x.lan, x.mac)) return fixedFull();
    const clash = list.find(
      (y) =>
        y.id !== id &&
        y.conn === x.conn &&
        (y.proto === "both" || x.proto === "both" || y.proto === x.proto) &&
        y.ext_from <= x.ext_to &&
        x.ext_from <= y.ext_to,
    );
    if (clash)
      return HttpResponse.json(
        {
          error: "refused",
          detail: `ports ${clash.ext_from}-${clash.ext_to} are forwarded already`,
        },
        { status: 409 },
      );
    /* The ranges moved to other ports move as many ports as the stick's table maps, at most. */
    const all = list.filter((y) => y.id !== id).reduce((n, y) => n + moved(y), 0) + moved(x);
    if (all > MAX_MOVED)
      return HttpResponse.json(
        {
          error: "refused",
          detail: `forward ${id}: with it, ranges moved to other ports move ${all} ports, more than ${MAX_MOVED} (TCP and UDP count apart)`,
        },
        { status: 400 },
      );
    pin(x.lan, x.mac);
    settings.forwards = [...list.filter((y) => y.id !== id), x];
    return HttpResponse.json({ id: "forward", ok: true });
  }),

  /*
   * A firewall rule: added at the end with no ID, changed by ID, removed by
   * ACTION=, or moved one place by MOVE=up or MOVE=down.
   */
  http.post("*/api/set/rule", async ({ request }) => {
    await delay(400);
    if (!signedIn) return unauthorized();
    const b = kv(await request.text());
    const list = settings.rules ?? [];
    const id = b.ID ? Number(b.ID) : Math.max(0, ...list.map((y) => y.id)) + 1;
    const at = list.findIndex((y) => y.id === id);
    if (b.MOVE === "up" || b.MOVE === "down") {
      const to = at + (b.MOVE === "up" ? -1 : 1);
      if (at < 0 || to < 0 || to >= list.length) return HttpResponse.json({ id: "rule", ok: true });
      const next = list.slice();
      const [x] = next.splice(at, 1);
      if (x) next.splice(to, 0, x);
      settings.rules = next;
      return HttpResponse.json({ id: "rule", ok: true });
    }
    if (b.ACTION === "") {
      settings.rules = list.filter((y) => y.id !== id);
      return HttpResponse.json({ id: "rule", ok: true });
    }
    if (at < 0 && list.length >= MAX_RULES)
      return HttpResponse.json(
        { error: "refused", detail: `all ${MAX_RULES} rules are taken` },
        { status: 400 },
      );
    const x = {
      id,
      name: b.NAME ?? "",
      action: (b.ACTION === "block" ? "block" : "allow") as "allow" | "block",
      from: b.FROM ?? "",
      to: b.TO ?? "",
      proto: (b.PROTO ?? "any") as "any" | "tcp" | "udp" | "both" | "icmp",
      ports: b.PORTS ?? "",
      family: (b.FAMILY ?? "any") as "any" | "ipv4" | "ipv6",
      on: b.ON !== "off",
    };
    settings.rules = at < 0 ? [...list, x] : list.map((y) => (y.id === id ? x : y));
    return HttpResponse.json({ id: "rule", ok: true });
  }),

  /*
   * What the firewall does where no rule decides: OUT, to the internet, and
   * BETWEEN the networks; and the pings the stick answers, PING_IN and PING_LANS.
   */
  http.post("*/api/set/fw", async ({ request }) => {
    await delay(400);
    if (!signedIn) return unauthorized();
    const b = kv(await request.text());
    const now = settings.fw ?? { out: "allow", between: "block" };
    const pick = (v: string | undefined, was: "allow" | "block" | undefined) =>
      v === "allow" || v === "block" ? v : (was ?? "block");
    settings.fw = {
      out: b.OUT === "block" ? "block" : b.OUT === "allow" ? "allow" : now.out,
      between: b.BETWEEN === "allow" ? "allow" : b.BETWEEN === "block" ? "block" : now.between,
      ping_in: pick(b.PING_IN, now.ping_in),
      ping_lans: pick(b.PING_LANS, now.ping_lans),
    };
    return HttpResponse.json({ id: "fw", ok: true });
  }),

  /* A connection's DMZ: its device by LAN and MAC, or none with MAC=. */
  http.post("*/api/set/dmz", async ({ request }) => {
    await delay(500);
    if (!signedIn) return unauthorized();
    const b = kv(await request.text());
    const list = (settings.dmz ?? []).filter((y) => y.conn !== b.CONN);
    if (b.MAC) {
      const x = { conn: b.CONN ?? "", lan: Number(b.LAN), mac: b.MAC.toLowerCase() };
      if (noRoomToPin(x.lan, x.mac)) return fixedFull();
      pin(x.lan, x.mac);
      list.push(x);
    }
    settings.dmz = list;
    return HttpResponse.json({ id: "dmz", ok: true });
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
    if (b.UNI_REPORT !== undefined) {
      if (!["both", "pptp", "veip"].includes(b.UNI_REPORT))
        return HttpResponse.json(
          { error: "identity", detail: "UNI_REPORT: both, pptp or veip" },
          { status: 400 },
        );
      identity.uni_report = b.UNI_REPORT as UniReport;
    }
    if (b.POTS_PORTS !== undefined) {
      if (!["0", "1", "2"].includes(b.POTS_PORTS))
        return HttpResponse.json(
          { error: "identity", detail: "POTS_PORTS: 0 to 2" },
          { status: 400 },
        );
      identity.pots_ports = Number(b.POTS_PORTS) as PotsPorts;
      /* The OLT sets up as many lines as there are sockets. */
      phone.lines = phone.lines.filter((l) => l.port <= (identity.pots_ports ?? 0));
    }
    if (b.UNI_PORTS !== undefined) {
      if (!/^[1-8]$/.test(b.UNI_PORTS))
        return HttpResponse.json(
          { error: "identity", detail: "UNI_PORTS: 1 to 8" },
          { status: 400 },
        );
      identity.uni_ports = Number(b.UNI_PORTS) as UniPorts;
    }
    settings.store = "saved";
    return HttpResponse.json({ id: "identity", ok: true });
  }),

  http.get("*/api/get/service_rules", async () => {
    await delay(150);
    return (
      away() ??
      (signedIn
        ? HttpResponse.json({ id: "service_rules", value: { rules: serviceRules() } })
        : unauthorized())
    );
  }),

  http.post("*/api/set/service_rule", async ({ request }) => {
    await delay(400);
    if (!signedIn) return unauthorized();
    const b = kv(await request.text());
    const fibre = b.FIBRE === "" || b.FIBRE === undefined ? null : Number(b.FIBRE);
    const refuse = (detail: string) =>
      HttpResponse.json({ error: "service_rule", detail }, { status: 400 });
    if (fibre !== null && !(fibre >= 0 && fibre <= 4095)) return refuse("FIBRE: a VID");
    if (b.ACTION === "auto") ownRules.delete(fibre);
    else if (b.ACTION === "move") {
      const host = Number(b.HOST);
      if (!(host >= 1 && host <= 4094)) return refuse("HOST: 1 to 4094");
      ownRules.set(fibre, { fibre, action: "move", host, auto: false });
    } else if (b.ACTION === "off" || b.ACTION === "only_uni" || b.ACTION === "only_veip")
      ownRules.set(fibre, { fibre, action: b.ACTION, auto: false });
    else return refuse("ACTION: move, off, only_uni, only_veip or auto");
    return HttpResponse.json({ id: "service_rule", ok: true });
  }),

  http.get("*/api/get/iptv", async () => {
    await delay(150);
    /* The streams' rates move a little from one read to the next. */
    for (const g of iptv.groups)
      if (g.rate_bps != null) g.rate_bps = Math.round(g.rate_bps * (0.97 + Math.random() * 0.06));
    return (
      away() ??
      (signedIn
        ? HttpResponse.json({ id: "iptv", value: { ...iptv, now_s: iptvAt() } })
        : unauthorized())
    );
  }),

  /* Who gives upstream IGMP its priority: the stick, or the host (whose 0 no rule takes here). */
  http.post("*/api/set/iptv", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const b = kv(await request.text());
    if (b.IGMP_PRIORITY !== "auto" && b.IGMP_PRIORITY !== "host")
      return HttpResponse.json(
        { error: "iptv", detail: "IGMP_PRIORITY: auto or host" },
        { status: 400 },
      );
    const auto = b.IGMP_PRIORITY === "auto";
    if (iptv.priority)
      iptv.priority = {
        ...iptv.priority,
        auto,
        by: auto ? "stick" : "host",
        pcp: auto ? 4 : null,
        dropped: auto ? 0 : 3,
      };
    return HttpResponse.json({ id: "iptv", ok: true });
  }),

  /* A test of a group: joined, counted for the seconds asked, left. */
  http.post("*/api/iptv/test", async ({ request }) => {
    if (!signedIn) return unauthorized();
    const b = kv(await request.text());
    const seconds = Math.min(30, Math.max(1, Number(b.SECONDS) || 10));
    const group = b.GROUP ?? "";
    if (!/^2(2[4-9]|3\d)\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(group))
      return HttpResponse.json(
        { error: "iptv", detail: "GROUP: a multicast address, 224.0.0.0 to 239.255.255.255" },
        { status: 400 },
      );
    await delay(seconds * 1000);
    const known = group.startsWith("239.10.");
    const rate = known ? 8_000_000 + Math.round(Math.random() * 2_000_000) : 0;
    return HttpResponse.json({
      group,
      vlan: b.VLAN ? Number(b.VLAN) : null,
      seconds,
      bytes: Math.round((rate * seconds) / 8),
      rate_bps: rate,
      first_ms: known ? 180 + Math.round(Math.random() * 120) : null,
    });
  }),

  http.get("*/api/get/phone", async () => {
    await delay(150);
    return away() ?? (signedIn ? HttpResponse.json({ id: "phone", value: phone }) : unauthorized());
  }),

  /* The SIP passwords of the phone's lines: root's password shows them. */
  http.post("*/api/phone/secrets", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    return HttpResponse.json({
      lines: phone.lines.map((l) => ({ port: l.port, secret: phonePasswords[l.port] ?? "" })),
    });
  }),

  /* The PPPoE login the OLT sent, its password with it: root's password shows it. */
  http.post("*/api/identity/olt-login", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    if (!identity.olt_login)
      return HttpResponse.json(
        { error: "no login", detail: "the OLT sent no PPPoE login" },
        { status: 404 },
      );
    return HttpResponse.json({ user: identity.olt_login.user, secret: oltPassword });
  }),

  http.get("*/api/get/firmware", async () => {
    await delay(200);
    return (
      away() ?? (signedIn ? HttpResponse.json({ id: "firmware", value: firmware }) : unauthorized())
    );
  }),

  /*
   * An upload: the file is read as onu-update reads it (lib/updatefile.ts)
   * and written into the other bank, unless it is no update file or its
   * signature does not match it.
   */
  http.post("*/api/firmware/upload", async ({ request }) => {
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    const body = new Uint8Array(await request.arrayBuffer());
    const target = 1 - (firmware.running ?? 0);
    firmware.update = {
      ...firmware.update,
      state: "writing",
      received_b: body.length,
      expected_b: body.length,
      sha256: null,
      signature: null,
      signer: null,
    };
    await delay(3000);
    const refuse = (message: string) => {
      firmware.update = { ...firmware.update, state: "failed", message };
      return HttpResponse.json({ error: "not written", detail: message }, { status: 400 });
    };
    let file: UpdateFile;
    try {
      file = await readUpdate(body, firmware.keys);
    } catch (e) {
      return refuse(e instanceof NotUpdate ? e.message : "not an update file");
    }
    if (file.signature === "bad")
      return refuse("the signature does not match the file: it is not written");
    const written = firmware.banks[target];
    const signature = file.signature ?? "valid";
    if (written)
      Object.assign(written, {
        state: "valid",
        version: file.version,
        time: file.time,
        sha256: file.sha256,
        signature,
        signer: file.signer,
      });
    firmware.update = {
      ...firmware.update,
      state: "done",
      bank: target,
      version: file.version,
      message: null,
      sha256: file.sha256,
      signature,
      signer: file.signer,
    };
    return HttpResponse.json({ ok: true, detail: file.version });
  }),

  http.post("*/api/firmware/try", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    const text = await request.text();
    firmware.next = text === "" ? 1 - (firmware.running ?? 0) : Number(text);
    return HttpResponse.json({ ok: true, detail: "the stick restarts into the bank once" });
  }),

  http.post("*/api/firmware/return", async ({ request }) => {
    await delay(300);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    const bank = Number(await request.text());
    if (firmware.banks.find((b) => b.bank === bank)?.state !== "shipped")
      return HttpResponse.json(
        { error: "not armed", detail: `bank ${bank} holds no shipped firmware` },
        { status: 409 },
      );
    firmware.committed = bank;
    firmware.next = bank;
    return HttpResponse.json({
      ok: true,
      detail: "the stick restarts into the shipped firmware, from now on",
    });
  }),

  http.get("*/api/firmware/backup", async ({ request }) => {
    await delay(600);
    if (!signedIn) return unauthorized();
    const no = passwordRefused(request);
    if (no) return no;
    return new HttpResponse(new Uint8Array(4096).fill(0xff), {
      headers: {
        "Content-Type": "application/octet-stream",
        "Content-Disposition": `attachment; filename="${productSlug(product)}-gpon-flash-325213d07776.bin"`,
      },
    });
  }),

  http.get("*/api/firmware/download", async () => {
    if (!signedIn) return unauthorized();
    return new HttpResponse(new Uint8Array([0x4f, 0x4e, 0x55, 0x46]), {
      headers: {
        "Content-Type": "application/octet-stream",
        "Content-Disposition": `attachment; filename="${productSlug(product)}-gpon-325213d07776.bin"`,
      },
    });
  }),

  http.post("*/api/system/restart", async () => {
    await delay(200);
    return signedIn ? HttpResponse.json({ ok: true }) : unauthorized();
  }),
];

export const handlers = stick.map(faulty);
