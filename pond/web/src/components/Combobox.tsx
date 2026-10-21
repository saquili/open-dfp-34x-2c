// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * A field of text with a list under it to pick from, as the VLAN of a
 * connection has (pages/internet/VlanField.tsx): what is typed narrows the
 * list, by the start of an option's value or any word of its label; the
 * arrow keys walk it, Enter takes the option, Escape shuts it, and the
 * button at the field's end opens it whole. A value that is in no option is
 * kept as typed. The field keeps the focus and names the option it is on,
 * so that a screen reader follows.
 */

import { ChevronDown } from "lucide-react";
import { type KeyboardEvent, type ReactNode, useEffect, useId, useRef, useState } from "react";

export interface ComboOption {
  /** What the field takes. */
  value: string;
  /** The option's name in the list, beside its value. */
  label: string;
  /** A line under it. */
  hint?: string;
}

export function Combobox({
  label,
  value,
  onChange,
  onBlur,
  options,
  hint,
  error,
  mono,
  placeholder,
  listLabel,
  none,
}: {
  label: string;
  value: string;
  onChange: (v: string) => void;
  onBlur?: () => void;
  options: ComboOption[];
  hint?: ReactNode;
  error?: string;
  mono?: boolean;
  placeholder?: string;
  /** The words of the button that opens the list whole. */
  listLabel: string;
  /** What the list says when nothing matches. */
  none: string;
}) {
  const id = useId();
  const input = useRef<HTMLInputElement>(null);
  const [open, setOpen] = useState(false);
  /* What narrows the list: what was typed, or nothing when the list is opened whole. */
  const [filter, setFilter] = useState("");
  const [active, setActive] = useState(0);

  const needle = filter.trim().toLowerCase();
  const items = needle
    ? options.filter(
        (o) =>
          o.value.toLowerCase().startsWith(needle) ||
          o.label
            .toLowerCase()
            .split(/[\s()·,]+/)
            .some((w) => w.startsWith(needle)),
      )
    : options;
  const at = Math.min(active, Math.max(items.length - 1, 0));
  const option = (i: number) => `${id}-o${i}`;

  /* The option the keys are on stays in sight, the value's own as the list opens. */
  useEffect(() => {
    if (open) document.getElementById(`${id}-o${at}`)?.scrollIntoView({ block: "nearest" });
  }, [open, at, id]);

  const show = (whole: boolean) => {
    if (whole) setFilter("");
    const list = whole ? options : items;
    setActive(
      Math.max(
        0,
        list.findIndex((o) => o.value.toLowerCase() === value.toLowerCase()),
      ),
    );
    setOpen(true);
  };
  const pick = (v: string) => {
    onChange(v);
    setOpen(false);
  };
  const keys = (e: KeyboardEvent<HTMLInputElement>) => {
    if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      e.preventDefault();
      if (!open) return show(true);
      setActive(Math.max(0, Math.min(items.length - 1, at + (e.key === "ArrowDown" ? 1 : -1))));
    } else if (e.key === "Enter" && open && items[at]) {
      e.preventDefault();
      pick(items[at].value);
    } else if (e.key === "Escape" && open) {
      e.preventDefault();
      setOpen(false);
    }
  };

  return (
    <div className="grid content-start gap-1">
      <label htmlFor={id} className="text-sm font-semibold">
        {label}
      </label>
      <div className="relative">
        <input
          ref={input}
          id={id}
          type="text"
          autoComplete="off"
          spellcheck={false}
          role="combobox"
          aria-expanded={open}
          aria-controls={`${id}-list`}
          aria-autocomplete="list"
          aria-activedescendant={open && items.length ? option(at) : undefined}
          aria-invalid={!!error}
          placeholder={placeholder}
          className={`input w-full pr-10 ${mono ? "font-mono" : ""} ${error ? "input-error" : ""}`}
          value={value}
          onInput={(e) => {
            const v = e.currentTarget.value;
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
          aria-label={listLabel}
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
            aria-label={label}
            className="absolute inset-x-0 top-full z-30 mt-1 max-h-72 overflow-auto rounded-field border border-base-300 bg-base-100 p-1"
            onMouseDown={(e) => e.preventDefault()}
          >
            {items.map((o, i) => (
              /* The keys are the field's: it keeps the focus and names the option it is on. */
              // biome-ignore lint/a11y/useKeyWithClickEvents: the field's keys choose
              <div
                key={o.value}
                id={option(i)}
                role="option"
                tabIndex={-1}
                aria-selected={i === at}
                className={`grid cursor-pointer gap-0.5 rounded-field px-3 py-2 ${i === at ? "bg-base-200" : ""}`}
                onMouseEnter={() => setActive(i)}
                onClick={() => pick(o.value)}
              >
                <span>
                  <span className={mono ? "font-mono" : ""}>{o.value}</span>
                  <span className="text-base-content/70"> · {o.label}</span>
                </span>
                {o.hint && <span className="text-xs text-base-content/70">{o.hint}</span>}
              </div>
            ))}
            {!items.length && <p className="px-3 py-2 text-xs text-base-content/70">{none}</p>}
          </div>
        )}
      </div>
      {error ? (
        <p className="text-xs text-error">{error}</p>
      ) : (
        hint && <p className="text-xs text-base-content/70">{hint}</p>
      )}
    </div>
  );
}
