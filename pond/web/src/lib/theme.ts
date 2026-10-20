// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The panel's theme: the system's colour scheme, as at the first visit, or
 * light or dark whatever the system says. The reader's choice is kept in the
 * browser, as the language is. theme-boot.js, the first script the page
 * loads, puts it on the page before the first paint; this module puts it
 * there when the reader changes it. Without storage the choice lasts the
 * visit, and the next one follows the system again.
 */

import { useCallback, useState } from "react";

export const THEMES = ["system", "light", "dark"] as const;
export type Theme = (typeof THEMES)[number];

/** Where the browser keeps the choice; theme-boot.js reads it there too. */
export const THEME_KEY = "pond-theme";

/* The theme of styles.css that a choice forces; the system's needs none. */
const NAMES: Record<Exclude<Theme, "system">, string> = {
  light: "onu-light",
  dark: "onu-dark",
};

type Store = Pick<Storage, "getItem" | "setItem" | "removeItem">;

/* The browser's storage, or null where it is not to be had. */
function browserStore(): Store | null {
  try {
    return globalThis.localStorage ?? null;
  } catch {
    return null;
  }
}

/** The theme the reader chose, or "system" when none was, or none can be read. */
export function storedTheme(store: Store | null = browserStore()): Theme {
  try {
    const saved = store?.getItem(THEME_KEY);
    return saved === "light" || saved === "dark" ? saved : "system";
  } catch {
    return "system";
  }
}

/** Keep @theme in the browser; the system's is kept as no choice at all. */
export function saveTheme(theme: Theme, store: Store | null = browserStore()) {
  try {
    if (theme === "system") store?.removeItem(THEME_KEY);
    else store?.setItem(THEME_KEY, theme);
  } catch {
    /* kept for this visit only */
  }
}

/** Put @theme on the page: the attribute daisyUI's themes go by, or none for the system's. */
export function applyTheme(
  theme: Theme,
  root: { dataset: DOMStringMap } = document.documentElement,
) {
  if (theme === "system") delete root.dataset.theme;
  else root.dataset.theme = NAMES[theme];
}

/** The theme in use, and the way to choose another, which is put on the page and kept. */
export function useTheme(): [Theme, (theme: Theme) => void] {
  const [theme, setState] = useState<Theme>(() => storedTheme());
  const choose = useCallback((next: Theme) => {
    applyTheme(next);
    saveTheme(next);
    setState(next);
  }, []);
  return [theme, choose];
}
