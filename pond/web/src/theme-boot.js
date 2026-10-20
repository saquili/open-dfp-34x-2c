// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The theme the reader chose (lib/theme.ts), put on the page before its
 * first paint, so that a page in the dark theme never flashes light. The
 * page loads this file first, as a classic script of its own (vite.config.ts
 * names it by its content): pond's Content-Security-Policy runs no script
 * written into the page. Without storage the system's colour scheme decides.
 */
try {
  const theme = localStorage.getItem("pond-theme");
  if (theme === "light" || theme === "dark")
    document.documentElement.dataset.theme = `onu-${theme}`;
} catch {
  /* no storage: the system's colour scheme */
}
