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
  scroll = false,
}: {
  title?: ReactNode;
  action?: ReactNode;
  children: ReactNode;
  className?: string;
  /** The content scrolls inside the card when it is longer than the card. */
  scroll?: boolean;
}) {
  return (
    <section className={`card min-h-0 border border-base-300 bg-base-100 ${className}`}>
      <div className="card-body min-h-0 gap-3 p-4 sm:p-5">
        {(title || action) && (
          <div className="flex min-h-8 items-center justify-between gap-3">
            {title && <h2 className="card-title text-base">{title}</h2>}
            {action}
          </div>
        )}
        {scroll ? (
          <div className="-mx-2 grid min-h-0 flex-1 content-start gap-3 overflow-auto px-2">
            {children}
          </div>
        ) : (
          children
        )}
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

/**
 * A table of things of one kind, a row a thing, the names of its columns once
 * above them. On a narrow screen a row stands as a list of its cells, each
 * under its column's name.
 */
export function Table({ head, children }: { head: ReactNode[]; children: ReactNode }) {
  return (
    <div className="overflow-hidden rounded-box border border-base-300 bg-base-100">
      <table className="w-full border-collapse text-sm max-md:block">
        <thead className="max-md:hidden">
          <tr>
            {head.map((h, i) => (
              <th
                // biome-ignore lint/suspicious/noArrayIndexKey: the columns are static
                key={i}
                className="border-b border-base-300 px-4 py-3 text-left text-xs font-semibold text-base-content/70"
              >
                {h}
              </th>
            ))}
          </tr>
        </thead>
        <tbody className="max-md:block [&>tr:last-child>td]:border-b-0">{children}</tbody>
      </table>
    </div>
  );
}

export function Row({ children }: { children: ReactNode }) {
  return (
    <tr className="hover:bg-base-200/40 max-md:block max-md:border-b max-md:border-base-300 max-md:py-2 max-md:last:border-b-0">
      {children}
    </tr>
  );
}

/**
 * A cell: its value, then lines under it, one under another. @head names its
 * column on a narrow screen; @actions holds a row's buttons, at its end.
 */
export function Cell({
  head,
  span,
  actions = false,
  children,
}: {
  head?: string;
  span?: number;
  actions?: boolean;
  children: ReactNode;
}) {
  return (
    <td
      colSpan={span}
      data-h={head}
      className={`border-b border-base-300 px-4 align-top max-md:block max-md:border-0 max-md:py-1 ${
        actions ? "w-px whitespace-nowrap py-2 text-right max-md:w-auto max-md:text-left" : "py-3"
      } ${head ? "max-md:before:block max-md:before:text-xs max-md:before:text-base-content/70 max-md:before:content-[attr(data-h)]" : ""}`}
    >
      {actions ? children : <div className="grid gap-1">{children}</div>}
    </td>
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

const STROKE: Record<Tone, string> = {
  good: "var(--color-success)",
  warn: "var(--color-warning)",
  bad: "var(--color-error)",
  info: "var(--color-info)",
  neutral: "var(--color-base-content)",
};

/** A share of a whole as a ring, the figure in its middle and words below. */
export function Gauge({
  share,
  value,
  label,
  sub,
  tone = "info",
  size = 112,
}: {
  share: number | null;
  value: ReactNode;
  label: ReactNode;
  sub?: ReactNode;
  tone?: Tone;
  size?: number;
}) {
  const r = 42;
  const c = 2 * Math.PI * r;
  /* Three quarters of a circle, open at the bottom. */
  const arc = c * 0.75;
  const shown = share == null ? 0 : Math.max(0, Math.min(1, share));
  return (
    <figure className="grid justify-items-center gap-1 text-center">
      <svg viewBox="0 0 100 100" width={size} height={size} role="img" aria-label={String(label)}>
        <circle
          cx="50"
          cy="50"
          r={r}
          fill="none"
          stroke="var(--color-base-300)"
          strokeWidth="9"
          strokeLinecap="round"
          strokeDasharray={`${arc} ${c}`}
          transform="rotate(135 50 50)"
        />
        <circle
          cx="50"
          cy="50"
          r={r}
          fill="none"
          stroke={STROKE[tone]}
          strokeWidth="9"
          strokeLinecap="round"
          strokeDasharray={`${arc * shown} ${c}`}
          transform="rotate(135 50 50)"
          className="transition-[stroke-dasharray] duration-700"
        />
        <text
          x="50"
          y="54"
          textAnchor="middle"
          className="tabular fill-current text-[19px] font-semibold"
        >
          {value}
        </text>
      </svg>
      <figcaption className="-mt-3">
        <div className="text-sm font-medium">{label}</div>
        {sub && <div className="text-xs text-base-content/60">{sub}</div>}
      </figcaption>
    </figure>
  );
}

/** A small figure with its caption, for rows of figures. */
export function Stat({
  label,
  value,
  sub,
  tone,
}: {
  label: ReactNode;
  value: ReactNode;
  sub?: ReactNode;
  tone?: Tone;
}) {
  return (
    <div className="min-w-0">
      <div className="text-xs text-base-content/60">{label}</div>
      <div className={`tabular truncate text-lg font-semibold ${tone ? TEXT[tone] : ""}`}>
        {value}
      </div>
      {sub && <div className="truncate text-xs text-base-content/60">{sub}</div>}
    </div>
  );
}
