// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Two languages, Polish and English. The browser's language picks one at the
 * first visit; the reader's own choice is kept in the browser after that.
 */

import { createContext, type ReactNode, useCallback, useContext, useMemo, useState } from "react";
import { type Dictionary, en } from "./en";
import { pl } from "./pl";

export type Lang = "pl" | "en";
const DICTIONARIES: Record<Lang, Dictionary> = { pl, en };
const KEY = "pond-lang";

function initial(): Lang {
  try {
    const saved = localStorage.getItem(KEY);
    if (saved === "pl" || saved === "en") return saved;
  } catch {
    /* no storage: follow the browser */
  }
  return navigator.language.toLowerCase().startsWith("pl") ? "pl" : "en";
}

/* A dotted path to a string of the dictionary, such as "nav.overview". */
type Paths<T, P extends string = ""> = {
  [K in keyof T & string]: T[K] extends string ? `${P}${K}` : Paths<T[K], `${P}${K}.`>;
}[keyof T & string];
export type Key = Paths<Dictionary>;

type Vars = Record<string, string | number>;

interface I18n {
  lang: Lang;
  setLang: (l: Lang) => void;
  t: (key: Key, vars?: Vars) => string;
}

const Context = createContext<I18n | null>(null);

export function I18nProvider({ children }: { children: ReactNode }) {
  const [lang, setLangState] = useState<Lang>(initial);

  const setLang = useCallback((l: Lang) => {
    setLangState(l);
    document.documentElement.lang = l;
    try {
      localStorage.setItem(KEY, l);
    } catch {
      /* kept for this visit only */
    }
  }, []);

  const t = useCallback(
    (key: Key, vars?: Vars) => {
      let node: unknown = DICTIONARIES[lang];
      for (const part of key.split(".")) node = (node as Record<string, unknown>)[part];
      let text = typeof node === "string" ? node : key;
      if (vars) for (const [k, v] of Object.entries(vars)) text = text.replace(`{${k}}`, String(v));
      return text;
    },
    [lang],
  );

  const value = useMemo(() => ({ lang, setLang, t }), [lang, setLang, t]);
  return <Context.Provider value={value}>{children}</Context.Provider>;
}

export function useI18n(): I18n {
  const ctx = useContext(Context);
  if (!ctx) throw new Error("useI18n outside I18nProvider");
  return ctx;
}
