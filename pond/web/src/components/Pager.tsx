// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * A long list a page at a time: the page shown, the size of a page, and the
 * buttons to move between pages. A list that fits one page shows no buttons.
 */

import { ChevronLeft, ChevronRight, Search } from "lucide-react";
import { useState } from "react";
import { useI18n } from "../i18n";

const SIZES = [10, 25, 50, 100];
const SMALLEST = 10;

export function usePages<T>(items: T[], initial = 25) {
  /* The page belongs to the list it was chosen in: a list of another length,
   * such as one a search narrowed, starts again on its first page. */
  const [chosen, setChosen] = useState({ page: 0, total: items.length });
  const [size, setSize] = useState(initial);
  const total = items.length;
  const pages = Math.max(1, Math.ceil(total / size));
  const at = chosen.total === total ? Math.min(chosen.page, pages - 1) : 0;
  return {
    page: at,
    pages,
    size,
    total,
    shown: items.slice(at * size, at * size + size),
    setPage: (page: number) => setChosen({ page, total }),
    setSize: (n: number) => {
      setSize(n);
      setChosen({ page: 0, total });
    },
  };
}

export function Pager({
  pager,
}: {
  pager: Pick<
    ReturnType<typeof usePages>,
    "page" | "pages" | "size" | "total" | "setPage" | "setSize"
  >;
}) {
  const { t } = useI18n();
  const { page, pages, size, total } = pager;
  if (total <= SMALLEST) return null;
  const from = page * size + 1;
  const to = Math.min(total, from + size - 1);
  return (
    <div className="flex flex-wrap items-center justify-between gap-2 border-t border-base-300 pt-2 text-sm">
      <span className="tabular text-base-content/70">{t("pager.range", { from, to, total })}</span>
      <span className="flex items-center gap-2">
        <select
          className="select select-xs w-auto"
          value={size}
          aria-label={t("pager.size")}
          onChange={(e) => pager.setSize(Number(e.currentTarget.value))}
        >
          {SIZES.map((n) => (
            <option key={n} value={n}>
              {t("pager.perPage", { n })}
            </option>
          ))}
        </select>
        <span className="join">
          <button
            type="button"
            className="join-item btn btn-xs"
            disabled={page === 0}
            onClick={() => pager.setPage(page - 1)}
            aria-label={t("pager.previous")}
          >
            <ChevronLeft className="size-3.5" />
          </button>
          <span className="join-item btn btn-xs tabular pointer-events-none">
            {page + 1} / {pages}
          </span>
          <button
            type="button"
            className="join-item btn btn-xs"
            disabled={page >= pages - 1}
            onClick={() => pager.setPage(page + 1)}
            aria-label={t("pager.next")}
          >
            <ChevronRight className="size-3.5" />
          </button>
        </span>
      </span>
    </div>
  );
}

/** A small search field for a list. */
export function Filter({
  value,
  onChange,
  placeholder,
  className = "sm:w-56",
}: {
  value: string;
  onChange: (v: string) => void;
  placeholder: string;
  className?: string;
}) {
  return (
    <label className={`input input-sm w-full ${className}`}>
      <Search className="size-4 opacity-50" />
      <input
        type="search"
        placeholder={placeholder}
        value={value}
        onInput={(e) => onChange(e.currentTarget.value)}
      />
    </label>
  );
}
