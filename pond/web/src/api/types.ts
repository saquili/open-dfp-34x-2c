// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The values pond serves, as /api/get/ID and /api/getm?ids=... return them.
 * They are the stick's own figures in its own units (bytes, microwatts,
 * seconds); the panel turns them into words and units for people. Each is
 * described in pond/openapi.json, from which Orval makes its type
 * (src/api/gen/model, npm run gen:api); the values the panel reads together
 * are its own.
 */

import type {
  Cpu,
  GemCounters,
  GemPort,
  Image,
  Memory,
  Onu,
  Optics,
  Port,
  PortCounters,
  Settings,
  Tcont,
  Uptime,
} from "./gen/model";

export type {
  Banks,
  Bridge,
  BridgeVlan,
  Clock,
  Cpu,
  DmzInfo,
  Family,
  FdbEntry,
  Firmware,
  FirmwareBank,
  FirmwareBankSignature,
  FirmwareKeysItem,
  FixedInfo,
  ForwardInfo,
  FwDefaults,
  GemCounters,
  GemPort,
  Identity,
  Image,
  IpoeInfo,
  Iptv,
  IptvTest,
  LaneMode,
  LanInfo,
  LeaseInfo,
  Link,
  LinkAddress,
  Memory,
  Mgmt,
  Neighbour,
  Network,
  OltLogin,
  OmciFrame,
  OmciFrames,
  OmciValue,
  Onu,
  OnuCounters,
  OnuState,
  Optics,
  OwnCertificate,
  Phone,
  PhoneLine,
  PhoneSecrets,
  Port,
  PortCounters,
  PortCpuPath,
  PotsPorts,
  Route,
  RuleInfo,
  ServiceRule,
  ServiceRules,
  Session,
  SessionInfo,
  Sessions,
  Settings,
  SfpLane,
  Ssh,
  SshKey,
  TcAction,
  Tcont,
  TcRule,
  TcRules,
  Tls,
  TokenState,
  Trial,
  UniPorts,
  UniReport,
  Uptime,
} from "./gen/model";

/** The values the panel polls together, every few seconds. */
export interface Live {
  uptime: Uptime;
  memory: Memory;
  image: Image;
  model: string;
  kernel: string;
  ports: Port[];
  port_counters: { sfp: PortCounters; pon: PortCounters };
  gem_ports: GemPort[];
  gem_counters: Record<string, GemCounters>;
  onu: Onu | null;
  tconts: Tcont[];
  optics: Optics | null;
  settings: Settings;
  /** null from a stick whose pond does not serve them yet. */
  cpu: Cpu | null;
  hostname: string | null;
  /** The name the panel shows for the system (lib/product.ts); null from a pond without it. */
  product: string | null;
}

export const LIVE_IDS: (keyof Live)[] = [
  "uptime",
  "memory",
  "image",
  "model",
  "kernel",
  "ports",
  "port_counters",
  "gem_ports",
  "gem_counters",
  "onu",
  "tconts",
  "optics",
  "settings",
  "cpu",
  "hostname",
  "product",
];
