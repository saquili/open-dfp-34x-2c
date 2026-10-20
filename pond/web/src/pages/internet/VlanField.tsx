// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The VLAN a connection runs in: one of the operator's, picked from a list,
 * or a number typed in. A connection is named by the VID the operator gives
 * its service towards the host; the stick sends it on the fibre in the VID
 * the OLT translates that to. A VID the operator does not give is taken too:
 * the connection is kept and waits, as nothing passes the fibre in it until
 * the operator gives it.
 */

import { ChevronDown } from "lucide-react";
import { type KeyboardEvent, useId, useRef, useState } from "react";
import { Dot } from "../../components/ui";
import { useI18n } from "../../i18n";
import type { Service } from "../../lib/omci";
import { serviceVid } from "./shared";

/** A VLAN of the operator's: its VID towards the host, its VID on the fibre when other, its channel. */
export interface VlanOption {
  vid: string;
  fibre: number | null;
  gem: number | null;
}

export function vlanOptions(list: Service[]): VlanOption[] {
  const out = new Map<string, VlanOption>();
  for (const s of list) {
    const vid = serviceVid(s);
    if (!vid || out.has(vid)) continue;
    const gem = s.ports.flatMap((p) => p.channels).find((c) => !c.multicast)?.portId ?? null;
    out.set(vid, { vid, fibre: s.vlan != null && String(s.vlan) !== vid ? s.vlan : null, gem });
  }
  return [...out.values()].sort((a, z) => Number(a.vid) - Number(z.vid));
}

/** What a VLAN carries already, and whether it is closed to the connection in hand. */
export type VlanUse = { text: string; closed: boolean } | null;

/* The most VLANs the list shows at once; a number typed narrows it. */
const SHOWN = 100;

const valid = (v: string) => /^\d+$/.test(v) && +v >= 1 && +v <= 4094;

export function VlanField({
  value,
  onChange,
  onBlur,
  options,
  use,
  known,
  error,
}: {
  value: string;
  onChange: (v: string) => void;
  onBlur?: () => void;
  options: VlanOption[];
  use: (vid: string) => VlanUse;
  /** The operator's services are known: the OLT set some. */
  known: boolean;
  error?: string;
}) {
  const { t } = useI18n();
  const id = useId();
  const input = useRef<HTMLInputElement>(null);
  const [open, setOpen] = useState(false);
  /* What narrows the list: the number typed, or nothing when the list is opened whole. */
  const [filter, setFilter] = useState("");
  const [active, setActive] = useState(0);

  const matches = options.filter((o) => o.vid.startsWith(filter));
  const typed = valid(filter) && !options.some((o) => o.vid === filter);
  const items: { vid: string; o: VlanOption | null }[] = [
    ...matches.slice(0, SHOWN).map((o) => ({ vid: o.vid, o })),
    ...(typed ? [{ vid: filter, o: null }] : []),
  ];
  const at = Math.min(active, Math.max(items.length - 1, 0));
  const option = (i: number) => `${id}-o${i}`;

  const show = (whole: boolean) => {
    if (whole) setFilter("");
    setActive(Math.max(0, whole ? options.findIndex((o) => o.vid === value) : 0));
    setOpen(true);
  };
  const pick = (vid: string) => {
    if (use(vid)?.closed) return;
    onChange(vid);
    setOpen(false);
  };
  const keys = (e: KeyboardEvent<HTMLInputElement>) => {
    if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      e.preventDefault();
      if (!open) return show(true);
      setActive(Math.max(0, Math.min(items.length - 1, at + (e.key === "ArrowDown" ? 1 : -1))));
    } else if (e.key === "Enter" && open && items[at]) {
      e.preventDefault();
      pick(items[at].vid);
    } else if (e.key === "Escape" && open) {
      e.preventDefault();
      setOpen(false);
    }
  };

  const here = options.find((o) => o.vid === value);
  const note = error ? (
    <p className="text-xs text-error">{error}</p>
  ) : !value ? (
    <p className="text-xs text-base-content/70">{t("internet.vlanHint")}</p>
  ) : here ? (
    <p className="text-xs text-base-content/70">
      {here.fibre != null
        ? t("internet.vlanVia", { fibre: here.fibre, gem: here.gem ?? "–" })
        : t("internet.vlanSame", { gem: here.gem ?? "–" })}
    </p>
  ) : (
    <p className="flex gap-2 text-xs">
      <span className="mt-1">
        <Dot tone="warn" />
      </span>
      {known ? t("internet.vlanAbsent", { vid: value }) : t("internet.vlanLater", { vid: value })}
    </p>
  );

  return (
    <div className="grid content-start gap-1">
      <label htmlFor={id} className="text-sm font-semibold">
        {t("internet.vlan")}
      </label>
      <div className="relative">
        <input
          ref={input}
          id={id}
          type="text"
          inputMode="numeric"
          autoComplete="off"
          spellcheck={false}
          role="combobox"
          aria-expanded={open}
          aria-controls={`${id}-list`}
          aria-autocomplete="list"
          aria-activedescendant={open && items.length ? option(at) : undefined}
          aria-invalid={!!error}
          placeholder={t("internet.vlanPlaceholder")}
          className={`input w-full pr-10 ${error ? "input-error" : ""}`}
          value={value}
          onInput={(e) => {
            const v = e.currentTarget.value.replace(/\D/g, "").slice(0, 4);
            e.currentTarget.value = v;
            onChange(v);
            setFilter(v);
            setActive(0);
            setOpen(true);
          }}
          onClick={() => !open && show(true)}
          onKeyDown={keys}
          onBlur={() => {
            setOpen(false);
            onBlur?.();
          }}
        />
        <button
          type="button"
          tabIndex={-1}
          className="btn btn-ghost btn-sm btn-square absolute top-1 right-1 text-base-content/70"
          aria-label={t("internet.vlanList")}
          onMouseDown={(e) => e.preventDefault()}
          onClick={() => {
            input.current?.focus();
            open ? setOpen(false) : show(true);
          }}
        >
          <ChevronDown className="size-4" />
        </button>
        {open && (
          <div
            id={`${id}-list`}
            role="listbox"
            aria-label={t("internet.vlan")}
            className="absolute inset-x-0 top-full z-30 mt-1 max-h-72 overflow-auto rounded-field border border-base-300 bg-base-100 p-1"
            onMouseDown={(e) => e.preventDefault()}
          >
            {items.map((it, i) => {
              const u = use(it.vid);
              return (
                /* The keys are the field's: it keeps the focus and names the option it is on. */
                // biome-ignore lint/a11y/useKeyWithClickEvents: the field's keys choose
                <div
                  key={it.vid}
                  id={option(i)}
                  role="option"
                  tabIndex={-1}
                  aria-selected={i === at}
                  aria-disabled={u?.closed || undefined}
                  className={`grid gap-0.5 rounded-field px-3 py-2 ${i === at ? "bg-base-200" : ""} ${
                    u?.closed ? "cursor-default" : "cursor-pointer"
                  } ${it.o ? "" : "mt-1 border-t border-base-300"}`}
                  onMouseEnter={() => setActive(i)}
                  onClick={() => pick(it.vid)}
                >
                  <span className={u?.closed ? "text-base-content/45" : ""}>
                    {it.o ? `VLAN ${it.vid}` : t("internet.vlanTyped", { vid: it.vid })}
                  </span>
                  <span className="text-xs text-base-content/70">
                    {!it.o
                      ? t("internet.vlanTypedHint")
                      : it.o.fibre != null
                        ? t("internet.optVia", { fibre: it.o.fibre, gem: it.o.gem ?? "–" })
                        : t("internet.optSame", { gem: it.o.gem ?? "–" })}
                  </span>
                  {u && <span className="text-xs text-base-content/70">{u.text}</span>}
                </div>
              );
            })}
            {matches.length > SHOWN && (
              <p className="px-3 py-2 text-xs text-base-content/70">
                {t("internet.vlanMore", { n: matches.length - SHOWN })}
              </p>
            )}
            {!items.length && (
              <p className="px-3 py-2 text-xs text-base-content/70">
                {options.length ? t("internet.vlanNoMatch") : t("internet.vlanNone")}
              </p>
            )}
          </div>
        )}
      </div>
      {note}
    </div>
  );
}
