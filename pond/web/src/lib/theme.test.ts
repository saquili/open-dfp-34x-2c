// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { applyTheme, saveTheme, storedTheme, THEME_KEY, THEMES, type Theme } from "./theme";

/* A browser's storage, in a map. */
function memory(start: Record<string, string> = {}) {
  const m = new Map(Object.entries(start));
  return {
    m,
    getItem: (k: string) => m.get(k) ?? null,
    setItem: (k: string, v: string) => void m.set(k, v),
    removeItem: (k: string) => void m.delete(k),
  };
}

/* Storage that refuses every use, as a browser that blocks it does. */
const refusing = {
  getItem: () => {
    throw new Error("SecurityError");
  },
  setItem: () => {
    throw new Error("QuotaExceededError");
  },
  removeItem: () => {
    throw new Error("SecurityError");
  },
};

describe("the theme the reader chose", () => {
  it("is the system's until one is chosen", () => {
    expect(storedTheme(memory())).toBe("system");
  });

  it("is read back as it was kept", () => {
    for (const theme of THEMES) {
      const store = memory();
      saveTheme(theme, store);
      expect(storedTheme(store)).toBe(theme);
    }
  });

  it("keeps the system's as no choice at all", () => {
    const store = memory({ [THEME_KEY]: "dark" });
    saveTheme("system", store);
    expect(store.m.has(THEME_KEY)).toBe(false);
  });

  it("takes a value it does not know for the system's", () => {
    expect(storedTheme(memory({ [THEME_KEY]: "sepia" }))).toBe("system");
  });

  it("works without storage, or with storage that refuses", () => {
    expect(storedTheme(null)).toBe("system");
    expect(storedTheme(refusing)).toBe("system");
    expect(() => saveTheme("dark", refusing)).not.toThrow();
    expect(() => saveTheme("dark", null)).not.toThrow();
  });
});

describe("the theme on the page", () => {
  it("forces light or dark, and leaves the system's to the colour scheme", () => {
    const root = { dataset: {} as DOMStringMap };
    applyTheme("dark", root);
    expect(root.dataset.theme).toBe("onu-dark");
    applyTheme("light", root);
    expect(root.dataset.theme).toBe("onu-light");
    applyTheme("system", root);
    expect(root.dataset.theme).toBeUndefined();
  });
});

/*
 * theme-boot.js runs before the bundle, as a script of its own: run with a
 * stored value, it must put on the page what this module puts there.
 */
describe("the script that puts the theme on before the first paint", () => {
  const code = readFileSync(new URL("../theme-boot.js", import.meta.url), "utf8");
  const boot = (store: unknown) => {
    const root = { dataset: {} as DOMStringMap };
    new Function("localStorage", "document", code)(store, { documentElement: root });
    return root.dataset.theme;
  };

  it("agrees with the panel on every choice", () => {
    for (const theme of THEMES) {
      const store = memory();
      saveTheme(theme, store);
      const root = { dataset: {} as DOMStringMap };
      applyTheme(storedTheme(store), root);
      expect(boot(store)).toBe(root.dataset.theme);
    }
  });

  it("leaves the page to the system without storage, or with a value it does not know", () => {
    expect(boot(refusing)).toBeUndefined();
    expect(boot(memory({ [THEME_KEY]: "sepia" }))).toBeUndefined();
    expect(boot(undefined)).toBeUndefined();
  });

  it("reads the key the panel keeps the choice under", () => {
    expect(code).toContain(`"${THEME_KEY}"`);
    expect(boot(memory({ [THEME_KEY]: "dark" satisfies Theme }))).toBe("onu-dark");
  });
});
