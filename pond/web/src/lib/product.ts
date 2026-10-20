// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The product name: what the panel calls this system, in its title and the
 * browser's tab, and what the files the stick hands out are named after.
 * Without one it is BuchOS, the author's own name for the system; the stick
 * keeps another among its own settings (PRODUCT_NAME, under System,
 * Settings). pond and onu-config check it as productNameOk() does: 1 to 20
 * characters of ASCII, letters, digits, blanks, hyphens and dots, a letter
 * or a digit first and last. ASCII, so that the limit counts the same in
 * every program that checks it and the name means nothing to HTML, a header
 * or a file system; 20, about as many as the heading of the menu shows
 * whole, 171 px beside its icon, where " – GPON" after a long name is cut
 * and the heading's tip holds it all.
 *
 * The browser keeps the last name it read, as it keeps the theme, so that
 * the sign-in and the tab show it before the stick can be asked; a browser
 * that never signed in shows BuchOS there.
 */

import { useEffect, useState } from "react";
import { z } from "./schemas";

export const DEFAULT_PRODUCT = "BuchOS";
export const PRODUCT_MAX = 20;
const RULE = /^[A-Za-z0-9]([A-Za-z0-9 .-]*[A-Za-z0-9])?$/;

/** Whether @v is a product name the stick takes. */
export const productNameOk = (v: string) => v.length <= PRODUCT_MAX && RULE.test(v);

/**
 * The start of the names of the stick's files for the name @name, as pond
 * makes it: its letters in lower case and its digits, each run of the other
 * characters one hyphen; BuchOS's for one that is no name.
 */
export const productSlug = (name: string) =>
  (productNameOk(name) ? name : DEFAULT_PRODUCT).toLowerCase().replace(/[^a-z0-9]+/g, "-");

/** The field of the product name: trimmed; empty, for BuchOS, or a name. */
export const productField = (message: string) =>
  z.string().check(
    z.trim(),
    z.refine((v) => v === "" || productNameOk(v), message),
  );

/** Where the browser keeps the last name it read. */
export const PRODUCT_KEY = "pond-product";

type Store = Pick<Storage, "getItem" | "setItem" | "removeItem">;

function browserStore(): Store | null {
  try {
    return globalThis.localStorage ?? null;
  } catch {
    return null;
  }
}

/** The name the browser read last, or BuchOS. */
export function storedProduct(store: Store | null = browserStore()): string {
  try {
    const saved = store?.getItem(PRODUCT_KEY);
    return saved != null && productNameOk(saved) ? saved : DEFAULT_PRODUCT;
  } catch {
    return DEFAULT_PRODUCT;
  }
}

/** Keep @name in the browser; BuchOS is kept as none. */
export function saveProduct(name: string, store: Store | null = browserStore()) {
  try {
    if (name === DEFAULT_PRODUCT || !productNameOk(name)) store?.removeItem(PRODUCT_KEY);
    else store?.setItem(PRODUCT_KEY, name);
  } catch {
    /* the sign-in shows BuchOS */
  }
}

type Title = (key: "app.product", vars: { name: string }) => string;

/**
 * The product name to show: @read, the stick's, once it is read, else the
 * one the browser kept. It is kept for the next sign-in, and is the title of
 * the page in the words of @t.
 */
export function useProductName(t: Title, read?: string | null): string {
  const [kept] = useState(storedProduct);
  const fresh = read != null && productNameOk(read) ? read : null;
  const name = fresh ?? kept;
  useEffect(() => {
    if (fresh != null) saveProduct(fresh);
  }, [fresh]);
  useEffect(() => {
    document.title = t("app.product", { name });
  }, [t, name]);
  return name;
}
