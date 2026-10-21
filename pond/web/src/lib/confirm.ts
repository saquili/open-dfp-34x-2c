// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What the confirmation of an action says when the action does not go
 * through (components/Confirm.tsx): a wrong password, a stop after too many
 * of them with the seconds left, a stick that knows no password, or the
 * reason the stick gave. The dialog stays open with it; what concerns the
 * password stands under its field, which is emptied for the next try.
 */

import { ApiError, OfflineError } from "../api/client";
import type { Key } from "../i18n";
import { reason } from "./read";

type T = (key: Key, vars?: Record<string, string | number>) => string;

/** How long a stop after wrong passwords lasts when the stick does not say (pond: LOCK_S). */
export const LOCK_S = 30;

export interface Refusal {
  text: string;
  /** It concerns root's password, and stands under its field, which is emptied. */
  atField: boolean;
}

/** Why the action @e ended did not go through, in the panel's words. */
export function refusal(t: T, e: unknown): Refusal {
  if (e instanceof ApiError && e.status === 403 && e.message === "wrong password")
    return { text: t("confirm.wrong"), atField: true };
  if (e instanceof ApiError && e.status === 429)
    return { text: t("confirm.locked", { seconds: e.retryAfter ?? LOCK_S }), atField: true };
  if (e instanceof ApiError && e.status === 423)
    return { text: t("confirm.noStore"), atField: true };
  if (e instanceof OfflineError) return { text: t("connection.offline"), atField: false };
  return { text: t("confirm.failed", { reason: reason(t, e) }), atField: false };
}

export type Outcome = { done: true } | ({ done: false } & Refusal);

/**
 * The action @run, as the dialog runs it once its button or Enter is
 * pressed: with root's password @secret when it @needs one, which an empty
 * field is not, so that nothing is sent without it.
 */
export async function attempt(
  t: T,
  run: (secret: string) => Promise<unknown>,
  needs: boolean,
  secret: string,
): Promise<Outcome> {
  if (needs && !secret) return { done: false, text: t("confirm.empty"), atField: true };
  try {
    await run(secret);
    return { done: true };
  } catch (e) {
    return { done: false, ...refusal(t, e) };
  }
}
