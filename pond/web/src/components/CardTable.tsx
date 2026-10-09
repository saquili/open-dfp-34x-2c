// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * A table whose rows are cards, as the internet page and the local network
 * page show their lists: a caret that opens a card, its name, three cells
 * and its buttons, the names of the columns once over the cards, and what a
 * card opens on under it. The columns line up from card to card (subgrid).
 * In a narrower list the name and the buttons stand in a row, and the three
 * cells under them, under the names of their columns; in a list as narrow as
 * a phone the third cell has a row of its own, and each cell stands under
 * its column's name. The list is a container (@container): these follow its
 * width, not the window's.
 */

import { ChevronRight } from "lucide-react";
import type { ReactNode } from "react";

/* The cells' places in a narrower list, and in one as narrow as a phone. */
export const AT = {
  caret: "@max-3xl:col-start-1 @max-3xl:row-start-1",
  name: "@max-3xl:col-span-3 @max-3xl:col-start-2 @max-3xl:row-start-1 @max-lg:col-span-2",
  acts: "@max-3xl:col-start-5 @max-3xl:row-start-1 @max-lg:col-start-4",
  first: "@max-3xl:col-start-2 @max-3xl:row-start-2",
  second: "@max-3xl:col-start-3 @max-3xl:row-start-2",
  third:
    "@max-3xl:col-start-4 @max-3xl:row-start-2 @max-lg:col-span-2 @max-lg:col-start-2 @max-lg:row-start-3",
  more: "@max-3xl:col-span-4 @max-3xl:col-start-2 @max-3xl:row-start-3 @max-lg:col-span-3 @max-lg:row-start-4",
};

/* The columns of a narrower list, the same in its head and in every card. */
const MEDIUM = "@max-3xl:grid-cols-[2rem_minmax(0,1fr)_minmax(0,1fr)_minmax(0,1.4fr)_3.75rem]";
const PHONE = "@max-lg:grid-cols-[2rem_minmax(0,1fr)_minmax(0,1fr)_auto]";

/** The cards and their head: on a wide list a grid of the columns @cols. */
export const ROWS = "gap-x-4 gap-y-2 pb-2 @max-3xl:flex @max-3xl:flex-col @max-3xl:gap-3";

/** The names of the columns, standing over the cards as they scroll; as high as a form's tabs. */
export const HEAD = `sticky top-0 z-10 col-span-full mb-2 grid h-11 grid-cols-subgrid items-center border-x border-b border-x-transparent border-b-base-300 bg-base-200 px-4 text-xs font-semibold text-base-content/70 @max-3xl:mb-1 ${MEDIUM} @max-3xl:gap-x-4 @max-lg:hidden`;

/** A card: a row of the table. */
export const CARD = `col-span-full grid grid-cols-subgrid items-start gap-y-4 rounded-box border bg-base-100 px-4 py-3 ${MEDIUM} ${PHONE} @max-3xl:gap-x-4 @max-3xl:gap-y-3 @max-lg:gap-x-3`;

/** A cell at @at; in a list as narrow as a phone, under its column's name @head. */
export function Cell({ head, at, children }: { head?: string; at: string; children: ReactNode }) {
  return (
    <div
      data-h={head}
      className={`grid min-w-0 content-start gap-1 ${at} ${
        head
          ? "@max-lg:before:text-xs @max-lg:before:text-base-content/70 @max-lg:before:content-[attr(data-h)]"
          : ""
      }`}
    >
      {children}
    </div>
  );
}

/** The button that opens a card on what is under it, @controls. */
export function Caret({
  open,
  controls,
  label,
  onToggle,
}: {
  open: boolean;
  controls: string;
  label: string;
  onToggle: () => void;
}) {
  return (
    <div className={`-my-1.5 -ml-2 ${AT.caret}`}>
      <button
        type="button"
        className="btn btn-ghost btn-sm btn-square text-base-content/70"
        aria-expanded={open}
        aria-controls={controls}
        aria-label={label}
        onClick={onToggle}
      >
        <ChevronRight className={`size-4 transition-transform ${open ? "rotate-90" : ""}`} />
      </button>
    </div>
  );
}

/** A card's buttons, at its end. */
export function Acts({ children }: { children: ReactNode }) {
  return (
    <div className={`-my-1.5 -mr-2 flex items-center justify-end gap-1 ${AT.acts}`}>{children}</div>
  );
}

/** A card opens when it is clicked, but not where a button, a field or what it opened is. */
export function opens(onToggle: () => void) {
  return (e: { target: EventTarget | null }) => {
    if ((e.target as Element).closest("button, a, input, [data-more]")) return;
    onToggle();
  };
}
