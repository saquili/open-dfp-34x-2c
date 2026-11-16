// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The switch's checks of malformed and hostile frames, as the firewall's page
 * offers them (onu-utils' settings.h): twelve that are on or off, three
 * floods that are off or have a limit, and whose frames they judge. The
 * switch drops a frame a check stops.
 */

import type { DosChecks } from "../api/types";

/** The checks that are on or off, in the order of the switch's bits. */
export const CHECKS = [
  "land",
  "blat",
  "syn_fin",
  "xmas",
  "null_scan",
  "syn_low_port",
  "tcp_short",
  "tcp_frag",
  "icmp_frag",
  "ping_of_death",
  "udp_bomb",
  "syn_data",
] as const;
export type Check = (typeof CHECKS)[number];

/** The checks of a flood: off, or the limit of frames the switch counts. */
export const FLOODS = ["syn_flood", "fin_flood", "icmp_flood"] as const;
export type Flood = (typeof FLOODS)[number];

/** What the stick says nothing of: every check off, the operator's side judged. */
export const DOS_OFF: DosChecks = {
  land: "off",
  blat: "off",
  syn_fin: "off",
  xmas: "off",
  null_scan: "off",
  syn_low_port: "off",
  tcp_short: "off",
  tcp_frag: "off",
  icmp_frag: "off",
  ping_of_death: "off",
  udp_bomb: "off",
  syn_data: "off",
  syn_flood: "off",
  fin_flood: "off",
  icmp_flood: "off",
  side: "internet",
};

/** The limit a flood check starts from when it is turned on: the highest, which cuts the least. */
export const FLOOD_START = "255";

/** Whether @v is a limit the switch takes: a number from 1 to 255, with no leading zero. */
export const isLimit = (v: string) => /^([1-9]\d?|1\d\d|2[0-4]\d|25[0-5])$/.test(v);

/** How many checks are on. */
export function dosOn(d: DosChecks) {
  return CHECKS.filter((k) => d[k] === "on").length + FLOODS.filter((k) => d[k] !== "off").length;
}
