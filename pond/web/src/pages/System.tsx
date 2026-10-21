// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The stick itself, in four pages under one entry of the menu, each fact on
 * one of them alone: Resources, the processor, the memory and the flash
 * with its partitions (system/Resources.tsx); Software, the two systems of
 * the flash, their versions, which runs and which starts next, and their
 * update (system/Software.tsx); Device, the model, since when it runs, its
 * names, its clock and its maintenance (system/Device.tsx); and Access,
 * every way in: root's password, the sessions, the token of scripts, ssh
 * and its keys, the panel's HTTPS (system/Access.tsx). They were the tabs of
 * one page: an address of before opens the page its tab named.
 */

export { AccessPage } from "./system/Access";
export { DevicePage } from "./system/Device";
export { ResourcesPage } from "./system/Resources";
export { SoftwarePage } from "./system/Software";

/** The pages of System, by the name each has in the menu and in its address. */
export const SYSTEM_PAGES = ["resources", "software", "device", "access"] as const;
export type SystemPage = (typeof SYSTEM_PAGES)[number];

/* The tabs of the page of before, by the page that holds what each held. */
const TABS: Record<string, SystemPage> = {
  resources: "resources",
  software: "software",
  settings: "device",
  access: "access",
};

/** The address of the page that holds what the tab @tab of before held; Resources for none. */
export function systemPath(tab: string): `/system/${SystemPage}` {
  return `/system/${Object.hasOwn(TABS, tab) ? (TABS[tab] as SystemPage) : "resources"}`;
}
