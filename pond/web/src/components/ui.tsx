// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The few building blocks every page is made of. */

import type { ReactNode } from "react";
import type { Tone } from "../lib/state";

const BADGE: Record<Tone, string> = {
  good: "badge-success",
  warn: "badge-warning",
  bad: "badge-error",
  info: "badge-info",
  neutral: "badge-ghost",
};

const TEXT: Record<Tone, string> = {
  good: "text-success",
  warn: "text-warning",
  bad: "text-error",
  info: "text-info",
  neutral: "text-base-content/70",
};

export function Badge({ tone, children }: { tone: Tone; children: ReactNode }) {
  return <span className={`badge badge-soft badge-sm font-medium ${BADGE[tone]}`}>{children}</span>;
}

export function Dot({ tone }: { tone: Tone }) {
  return <span className={`inline-block size-2 rounded-full bg-current ${TEXT[tone]}`} />;
}

export function toneText(tone: Tone) {
  return TEXT[tone];
}

export function Card({
  title,
  action,
  children,
  className = "",
}: {
  title?: ReactNode;
  action?: ReactNode;
  children: ReactNode;
  className?: string;
}) {
  return (
    <section className={`card bg-base-100 border border-base-300 ${className}`}>
      <div className="card-body gap-3 p-4 sm:p-5">
        {(title || action) && (
          <div className="flex items-center justify-between gap-3">
            {title && <h2 className="card-title text-base">{title}</h2>}
            {action}
          </div>
        )}
        {children}
      </div>
    </section>
  );
}

export function PageTitle({ children, hint }: { children: ReactNode; hint?: ReactNode }) {
  return (
    <header className="mb-4">
      <h1 className="text-2xl font-semibold tracking-tight">{children}</h1>
      {hint && <p className="mt-1 text-sm text-base-content/70">{hint}</p>}
    </header>
  );
}

/** A label and its value, one per row. */
export function Facts({ rows }: { rows: ([ReactNode, ReactNode] | false | null | undefined)[] }) {
  return (
    <dl className="grid grid-cols-[minmax(0,auto)_minmax(0,1fr)] gap-x-4 gap-y-2 text-sm">
      {rows.filter(Boolean).map((row, i) => {
        const [k, v] = row as [ReactNode, ReactNode];
        return (
          // biome-ignore lint/suspicious/noArrayIndexKey: rows are static per render
          <div key={i} className="contents">
            <dt className="text-base-content/65">{k}</dt>
            <dd className="tabular min-w-0 break-words font-medium">{v}</dd>
          </div>
        );
      })}
    </dl>
  );
}

/** A big figure with its unit and a caption. */
export function Figure({
  value,
  unit,
  caption,
  tone,
}: {
  value: ReactNode;
  unit?: ReactNode;
  caption?: ReactNode;
  tone?: Tone;
}) {
  return (
    <div>
      <div className={`tabular text-3xl font-semibold leading-tight ${tone ? TEXT[tone] : ""}`}>
        {value}
        {unit && <span className="ml-1 text-base font-medium text-base-content/60">{unit}</span>}
      </div>
      {caption && <div className="mt-0.5 text-xs text-base-content/65">{caption}</div>}
    </div>
  );
}

/** A share of a whole, as a bar, with its words above. */
export function Meter({
  share,
  label,
  tone = "info",
}: {
  share: number;
  label: ReactNode;
  tone?: Tone;
}) {
  const cls = {
    good: "progress-success",
    warn: "progress-warning",
    bad: "progress-error",
    info: "progress-info",
    neutral: "",
  }[tone];
  return (
    <div className="grid gap-1">
      <div className="text-sm">{label}</div>
      <progress className={`progress h-2 ${cls}`} value={Math.round(share * 100)} max={100} />
    </div>
  );
}
