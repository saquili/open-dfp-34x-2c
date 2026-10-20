// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What a page shows of a read of the stick's, the same on every page
 * (docs/handbook/panel-design.rst, Reads and changes): until the first
 * answer, that it is being read; then the answer; and when a read failed,
 * why, with the button that reads again, in place of the answer while there
 * is none, or over the last answer, which stays, with its time. A read
 * again in the background shows only in the bar over the page (Busy), so
 * that a poll never makes a page blink.
 */

import { useIsFetching } from "@tanstack/react-query";
import { CircleAlert, RotateCw } from "lucide-react";
import { type ReactNode, useEffect, useRef, useState } from "react";
import { useI18n } from "../i18n";
import { clock } from "../lib/format";
import { type Read, readView, reason } from "../lib/read";

const list = (reads: Read | Read[]) => (Array.isArray(reads) ? reads : [reads]);

/**
 * @reads with the error each last had kept while it is read again. TanStack
 * Query clears the error of a read that has no answer yet as soon as it
 * reads again, so that a failure would turn into a first load at every
 * retry and poll, and a page would blink, or mount and read again without
 * end. The error goes once a read answers.
 */
export function useKept(reads: Read | Read[]): Read[] {
  const last = useRef<unknown[]>([]);
  return list(reads).map((r, i) => {
    if (r.error != null) last.current[i] = r.error;
    else if (!r.isFetching || r.data !== undefined) last.current[i] = null;
    const error = r.error ?? last.current[i] ?? null;
    return error === r.error ? r : { ...r, error };
  });
}

/** That the stick is being read, where its answer will stand. */
export function Reading({ children }: { children?: ReactNode }) {
  const { t } = useI18n();
  return (
    <p role="status" className="flex items-center gap-2 text-sm text-base-content/70">
      <span className="loading loading-spinner loading-sm" />
      {children ?? t("read.reading")}
    </p>
  );
}

/** The button that reads @reads again, busy while they are read. */
export function Retry({ reads, small = false }: { reads: Read | Read[]; small?: boolean }) {
  const { t } = useI18n();
  const all = list(reads);
  const busy = all.some((r) => r.isFetching);
  return (
    <button
      type="button"
      className={`btn ${small ? "btn-xs" : "btn-sm"}`}
      disabled={busy}
      onClick={() => {
        for (const r of all) if (r.error != null || r.data === undefined) void r.refetch();
      }}
    >
      {busy ? (
        <span className="loading loading-spinner loading-xs" />
      ) : (
        <RotateCw className="size-3.5" aria-hidden />
      )}
      {t("common.retry")}
    </button>
  );
}

/** Nothing to show: why the read failed, and the button that reads again. */
function Failed({ error, reads }: { error: unknown; reads: Read[] }) {
  const { t } = useI18n();
  return (
    <div role="alert" className="flex flex-wrap items-center gap-x-3 gap-y-2 text-sm">
      <span className="text-error">{t("read.failed", { reason: reason(t, error) })}</span>
      <Retry reads={reads} />
    </div>
  );
}

/** The last answer stays: that the read again failed, why, since when, and the button. */
function Stale({ read, reads }: { read: Read; reads: Read[] }) {
  const { t, lang } = useI18n();
  return (
    <div role="status" className="flex flex-wrap items-center gap-x-2 gap-y-1 text-xs">
      <CircleAlert className="size-3.5 shrink-0 text-warning" aria-hidden />
      <span className="text-base-content/70">
        {t("read.stale", {
          reason: reason(t, read.error),
          time: clock(read.dataUpdatedAt, lang, true),
        })}
      </span>
      <Retry reads={reads} small />
    </div>
  );
}

/**
 * A line on @reads where the page shows what it has of them anyway: while
 * they are read, that they are; when one failed, why, and the button that
 * reads again; nothing once they answered. @missing is what to say when the
 * stick has no such value at all. Reads @aside, which only add to what the
 * page shows, say nothing while they are read but in the bar (Busy), so
 * that the page does not move under the reader for them.
 */
export function QueryNote({
  reads,
  missing,
  aside = false,
}: {
  reads: Read | Read[];
  missing?: ReactNode;
  aside?: boolean;
}) {
  const all = useKept(reads);
  const v = readView(all);
  if (v.state === "reading") return aside ? null : <Reading />;
  if (v.state === "missing")
    return missing ? <p className="text-sm text-base-content/70">{missing}</p> : null;
  if (v.state === "failed") return <Failed error={v.error} reads={all} />;
  return v.stale ? <Stale read={v.stale} reads={all} /> : null;
}

/**
 * @children, which make sense only with @reads answered (a function, when
 * they take the reads' data, so that they are made only then): in their
 * place, until then, that they are being read (@reading, or the panel's
 * line), or why they failed; over them, when a read again failed, that it did.
 */
export function QueryState({
  reads,
  reading,
  missing,
  children,
}: {
  reads: Read | Read[];
  reading?: ReactNode;
  missing?: ReactNode;
  children: ReactNode | (() => ReactNode);
}) {
  const all = useKept(reads);
  const v = readView(all);
  if (v.state === "reading") return reading ?? <Reading />;
  if (v.state === "missing" && missing) return <p className="text-sm">{missing}</p>;
  if (v.state === "failed" || v.state === "missing")
    return <Failed error={v.state === "failed" ? v.error : v.read.error} reads={all} />;
  return (
    <>
      {v.stale && <Stale read={v.stale} reads={all} />}
      {typeof children === "function" ? children() : children}
    </>
  );
}

/* How long a read goes on before the bar says so: a quick poll says nothing. */
const BUSY_AFTER_MS = 600;

/**
 * In the bar over every page: that the panel is reading the stick, while a
 * read has gone on for longer than a quick poll takes.
 */
export function Busy() {
  const { t } = useI18n();
  const fetching = useIsFetching() > 0;
  const [shown, setShown] = useState(false);
  useEffect(() => {
    if (!fetching) {
      setShown(false);
      return;
    }
    const id = setTimeout(() => setShown(true), BUSY_AFTER_MS);
    return () => clearTimeout(id);
  }, [fetching]);
  return (
    <span
      role="status"
      aria-label={shown ? t("read.busy") : undefined}
      title={shown ? t("read.busy") : undefined}
      className={`loading loading-spinner loading-xs text-base-content/70 ${shown ? "" : "invisible"}`}
    />
  );
}
