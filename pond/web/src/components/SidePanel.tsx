// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * A list and the form that adds to it or changes it: the list across the
 * page, and the form at its right only while it is open, which it comes into
 * from the right; the list narrows at once, since a table that widens frame
 * by frame tears. On a narrow screen the form stands above the list, and a
 * scroll to it stops below the page's bar. Escape,
 * within the form, shuts it, unless a field took the key first (a list of
 * choices that shuts); one who asked for less motion sees it at once.
 */

import { forwardRef, type ReactNode } from "react";

/** The grid of a list and, while @open, the form at its right. */
export function withPanel(open: boolean) {
  return `grid min-h-0 flex-1 content-start gap-6 xl:content-stretch ${open ? "xl:grid-cols-[minmax(0,1fr)_25rem]" : ""}`;
}

/** The form's column, coming in as it opens; @onClose shuts it, or nothing for one that stays. */
export const SidePanel = forwardRef<HTMLDivElement, { onClose?: () => void; children: ReactNode }>(
  function SidePanel({ onClose, children }, ref) {
    return (
      // biome-ignore lint/a11y/noStaticElementInteractions: Escape from within the form's own fields
      <div
        ref={ref}
        className="min-h-0 scroll-mt-20 animate-panel-in motion-reduce:animate-none max-xl:order-first xl:flex xl:flex-col"
        onKeyDown={(e) => {
          if (e.key !== "Escape" || e.defaultPrevented || !onClose) return;
          e.preventDefault();
          onClose();
        }}
      >
        <div className="xl:min-h-0 xl:flex-1">{children}</div>
      </div>
    );
  },
);
