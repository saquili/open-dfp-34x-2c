// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Where a read of the stick's stands, for what a page shows of it
 * (components/Read.tsx): being read for the first time, failed with nothing
 * to show, or shown, the last answer kept when a read again failed. A page
 * that shows several reads together is as far as the least of them.
 */

import { ApiError, AuthError, OfflineError } from "../api/client";
import type { Key } from "../i18n";

/** What a page needs of a read: TanStack Query's result has all of it. */
export interface Read {
  data: unknown;
  error: unknown;
  isFetching: boolean;
  dataUpdatedAt: number;
  refetch: () => unknown;
}

export type ReadView =
  /* Nothing yet, and a read is under way or about to be. */
  | { state: "reading" }
  /* Nothing to show: the stick has no such value, an older software. */
  | { state: "missing"; read: Read }
  /* Nothing to show, and the last read failed. */
  | { state: "failed"; read: Read; error: unknown }
  /* Every read has an answer; @stale is one whose read again failed, kept as it was. */
  | { state: "shown"; stale: Read | null };

/** The stick has no such value: pond answers 404 for a name it does not know. */
export const missing = (e: unknown) => e instanceof ApiError && e.status === 404;

export function readView(reads: Read[]): ReadView {
  const empty = reads.filter((r) => r.data === undefined);
  const failed = empty.find((r) => r.error != null);
  if (failed)
    return missing(failed.error)
      ? { state: "missing", read: failed }
      : { state: "failed", read: failed, error: failed.error };
  if (empty.length) return { state: "reading" };
  return { state: "shown", stale: reads.find((r) => r.error != null) ?? null };
}

/** Why a read or a change failed, in the panel's words where it has some. */
export function reason(t: (k: Key) => string, e: unknown): string {
  if (e instanceof OfflineError) return t("connection.offline");
  if (e instanceof AuthError) return t("read.signedOut");
  if (e instanceof ApiError) return e.detail ?? e.message;
  return e instanceof Error ? e.message : String(e);
}
