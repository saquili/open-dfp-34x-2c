// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * A page: its title, its tabs, and a body that fills the window below them.
 * On a wide screen the page never scrolls as a whole; a long list scrolls
 * inside its own card instead. On a phone the page scrolls as usual. The
 * open tab is in the address (?tab=...), so a link or a reload opens it
 * again.
 */

import { useNavigate, useSearch } from "@tanstack/react-router";
import type { ReactNode } from "react";

export interface TabItem<T extends string> {
  id: T;
  label: ReactNode;
  icon?: ReactNode;
  /** A small mark after the label: a count, a state. */
  badge?: ReactNode;
}

/** The tab the address names, or the first; and a way to open another. */
export function useTab<T extends string>(ids: readonly T[]): [T, (t: T) => void] {
  const search = useSearch({ strict: false }) as { tab?: unknown };
  const navigate = useNavigate();
  const named = typeof search.tab === "string" ? search.tab : "";
  const tab = (ids as readonly string[]).includes(named) ? (named as T) : (ids[0] as T);
  const open = (t: T) =>
    navigate({
      to: ".",
      search: ((s: Record<string, unknown>) => ({
        ...s,
        tab: t === ids[0] ? undefined : t,
      })) as never,
      replace: true,
    });
  return [tab, open];
}

export function Tabs<T extends string>({
  items,
  value,
  onChange,
  label,
}: {
  items: TabItem<T>[];
  value: T;
  onChange: (t: T) => void;
  label: string;
}) {
  return (
    <div
      role="tablist"
      aria-label={label}
      className="tabs tabs-box tabs-sm w-full flex-nowrap overflow-x-auto bg-base-200/70 [scrollbar-width:none] sm:w-auto"
    >
      {items.map((it) => (
        <button
          key={it.id}
          type="button"
          role="tab"
          aria-selected={it.id === value}
          className={`tab shrink-0 gap-1.5 whitespace-nowrap ${it.id === value ? "tab-active" : ""}`}
          onClick={() => onChange(it.id)}
        >
          {it.icon}
          {it.label}
          {it.badge}
        </button>
      ))}
    </div>
  );
}

/**
 * The frame of a page. @tabs sit beside the title on a wide screen, the
 * @hint under both, and on a narrow one below the hint; @actions sit at the
 * far right.
 */
export function Page({
  title,
  hint,
  tabs,
  actions,
  children,
}: {
  title: ReactNode;
  hint?: ReactNode;
  tabs?: ReactNode;
  actions?: ReactNode;
  children: ReactNode;
}) {
  return (
    <div className="flex min-h-0 flex-1 flex-col gap-4">
      <header className="flex flex-wrap items-center gap-x-6 gap-y-2">
        <h1 className="order-1 min-w-0 text-2xl font-semibold tracking-tight">{title}</h1>
        {hint && (
          <p className="order-2 -mt-1.5 basis-full text-sm text-base-content/70 sm:order-4">
            {hint}
          </p>
        )}
        {tabs && <div className="order-3 max-sm:w-full sm:order-2">{tabs}</div>}
        {actions && <div className="order-3 ml-auto flex items-center gap-2">{actions}</div>}
      </header>
      <div className="flex min-h-0 flex-1 flex-col">{children}</div>
    </div>
  );
}

/**
 * A body that fills the page: a grid whose rows share the height left. On a
 * phone it is a plain column.
 */
export function Fill({ className = "", children }: { className?: string; children: ReactNode }) {
  return (
    <div className={`grid min-h-0 flex-1 gap-4 lg:overflow-hidden ${className}`}>{children}</div>
  );
}
