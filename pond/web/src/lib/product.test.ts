// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { en } from "../i18n/en";
import { pl } from "../i18n/pl";
import {
  DEFAULT_PRODUCT,
  PRODUCT_KEY,
  PRODUCT_MAX,
  productField,
  productNameOk,
  productSlug,
  saveProduct,
  storedProduct,
} from "./product";

/* The names pond's and onu-config's tests take and refuse too. */
const GOOD = ["BuchOS", "x", "7", "a-1.b", "Fibre Box 2.0", "Abcdefghij Klmnopqr9"];
const BAD = [
  "",
  " x",
  "x ",
  "-x",
  "x.",
  ".x",
  "<b>x</b>",
  "a&b",
  'a"b',
  "a'b",
  "a/b",
  "a\\b",
  "a_b",
  "a;b",
  "a=b",
  "a\tb",
  "Abcdefghij Klmnopqrs9",
  "Światłowód",
];

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

describe("a product name", () => {
  it("is 1 to 20 letters, digits, blanks, hyphens and dots of ASCII, a letter or a digit first and last", () => {
    for (const name of GOOD) expect(productNameOk(name), name).toBe(true);
    for (const name of BAD) expect(productNameOk(name), name).toBe(false);
    expect(PRODUCT_MAX).toBe(20);
  });

  it("names the files by its letters in lower case and its digits, a hyphen for each run of the rest", () => {
    expect(productSlug("BuchOS")).toBe("buchos");
    expect(productSlug("Fibre Box 2.0")).toBe("fibre-box-2-0");
    expect(productSlug("A - . B")).toBe("a-b");
    expect(productSlug("Abcdefghij Klmnopqr9")).toBe("abcdefghij-klmnopqr9");
    expect(productSlug("<b>x</b>")).toBe("buchos");
  });

  it("is checked in the field trimmed, and may be empty there, for BuchOS", () => {
    const field = productField("the rule");
    expect(field.safeParse("  Fibre Box 2.0 ").data).toBe("Fibre Box 2.0");
    expect(field.safeParse("").success).toBe(true);
    for (const name of BAD.filter((n) => n.trim() !== "" && n.trim() === n)) {
      const r = field.safeParse(name);
      expect(r.success, name).toBe(false);
      expect(r.error?.issues[0]?.message).toBe("the rule");
    }
  });
});

describe("the name the browser keeps", () => {
  it("is BuchOS until one is read, and the one read after", () => {
    const store = memory();
    expect(storedProduct(store)).toBe(DEFAULT_PRODUCT);
    saveProduct("Fibre Box 2.0", store);
    expect(store.m.get(PRODUCT_KEY)).toBe("Fibre Box 2.0");
    expect(storedProduct(store)).toBe("Fibre Box 2.0");
  });

  it("keeps BuchOS, and a name that is none, as nothing", () => {
    const store = memory({ [PRODUCT_KEY]: "Fibre Box" });
    saveProduct(DEFAULT_PRODUCT, store);
    expect(store.m.has(PRODUCT_KEY)).toBe(false);
    saveProduct("<b>x</b>", store);
    expect(store.m.has(PRODUCT_KEY)).toBe(false);
    expect(storedProduct(memory({ [PRODUCT_KEY]: "<b>x</b>" }))).toBe(DEFAULT_PRODUCT);
  });

  it("is BuchOS without storage, or with storage that refuses", () => {
    expect(storedProduct(null)).toBe(DEFAULT_PRODUCT);
    expect(storedProduct(refusing)).toBe(DEFAULT_PRODUCT);
    expect(() => saveProduct("Fibre Box", refusing)).not.toThrow();
    expect(() => saveProduct("Fibre Box", null)).not.toThrow();
  });
});

describe("the page's title", () => {
  it("is the name and GPON in both languages, BuchOS's the title the page loads with", () => {
    const html = readFileSync(new URL("../../index.html", import.meta.url), "utf8");
    const title = /<title>([^<]*)<\/title>/.exec(html)?.[1];
    for (const words of [en, pl]) {
      expect(words.app.product.replace("{name}", DEFAULT_PRODUCT)).toBe(title);
      expect(words.app.product.replace("{name}", "Fibre Box")).toBe("Fibre Box – GPON");
    }
  });

  it("has the default and the limit that pond has", () => {
    const h = readFileSync(new URL("../../../src/providers.h", import.meta.url), "utf8");
    expect(h).toContain(`#define PRODUCT_NAME_DEFAULT "${DEFAULT_PRODUCT}"`);
    expect(h).toContain(`#define PRODUCT_NAME_MAX ${PRODUCT_MAX}`);
  });
});
