// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The changes on trial (ADR 0014): the stick takes each back unless the
 * panel confirms it. The panel confirms them as soon as a fresh reading
 * shows them applied, which it could only get by reaching the stick again,
 * and once for each; a new management address only from a panel opened at
 * that address, which shows that it answers.
 */

import type { Settings, Trial } from "../api/types";

/** Whether this panel, opened at @here, may confirm @t. */
export function confirmable(t: Trial, here: string): boolean {
  return t.name !== "mgmt" || t.address?.split("/")[0] === here;
}

/**
 * The changes to confirm now, as a key that names them, or null: those a
 * fresh reading shows that this panel may confirm, while nothing is being
 * applied, unless the last confirmation (@sent) was of the same.
 */
export function trialsToConfirm(
  settings: Pick<Settings, "trials" | "applying"> | undefined,
  fresh: boolean,
  sent: string | null,
  here: string,
): string | null {
  if (!fresh || !settings || settings.applying) return null;
  const key = (settings.trials ?? [])
    .filter((t) => confirmable(t, here))
    .map((t) => `${t.name}:${t.until_s}`)
    .join(",");
  return key && key !== sent ? key : null;
}
