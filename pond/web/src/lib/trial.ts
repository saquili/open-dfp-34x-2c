// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * A change of the settings on trial (ADR 0014): the stick goes back to the
 * settings of before unless the panel confirms the change. The panel
 * confirms it as soon as a fresh reading shows it applied, which it could
 * only get by reaching the stick again, and once for each trial.
 */

import type { Settings } from "../api/types";

/**
 * The trial to confirm now, by its end, or null: one that a fresh reading
 * shows, whose change is no longer being applied, and that was not
 * confirmed already (@sent, the end of the last one confirmed).
 */
export function trialToConfirm(
  settings: Pick<Settings, "trial" | "applying"> | undefined,
  fresh: boolean,
  sent: number | null,
): number | null {
  const trial = settings?.trial;
  if (!fresh || !trial || settings.applying) return null;
  return trial.until_s === sent ? null : trial.until_s;
}
