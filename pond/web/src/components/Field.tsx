// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* A labelled input of a form, with its hint and its error. */

import { Eye, EyeOff } from "lucide-react";
import type { JSX } from "preact";
import { forwardRef, type InputHTMLAttributes, useState } from "react";
import { useI18n } from "../i18n";

/* The input type that hides what is typed. */
const MASKED = "password";

type Props = Omit<InputHTMLAttributes<HTMLInputElement>, "type"> & {
  label: string;
  hint?: string;
  error?: string;
  secret?: boolean;
};

export const Field = forwardRef<HTMLInputElement, Props>(function Field(
  { label, hint, error, secret, id, className = "", ...input },
  ref,
) {
  const { t } = useI18n();
  const [shown, setShown] = useState(false);
  const name = id ?? input.name;
  return (
    <div className={`grid gap-1 ${className}`}>
      <label htmlFor={name} className="text-sm font-medium">
        {label}
      </label>
      <div className="flex gap-2">
        <input
          ref={ref}
          {...({
            id: name,
            className: `input w-full ${error ? "input-error" : ""}`,
            /* Preact types each input type apart; this one switches. */
            type: secret && !shown ? MASKED : "text",
            spellcheck: false,
            autoComplete: "off",
            "aria-invalid": !!error,
            ...input,
          } as unknown as JSX.IntrinsicElements["input"])}
        />
        {secret && (
          <button
            type="button"
            className="btn btn-square btn-ghost"
            onClick={() => setShown((v) => !v)}
            aria-label={shown ? t("internet.hide") : t("internet.show")}
          >
            {shown ? <EyeOff className="size-4" /> : <Eye className="size-4" />}
          </button>
        )}
      </div>
      {error ? (
        <p className="text-xs text-error">{error}</p>
      ) : (
        hint && <p className="text-xs text-base-content/60">{hint}</p>
      )}
    </div>
  );
});

/** The state line under a form: saving, saved, failed. */
export function SaveState({
  state,
  message,
}: {
  state: "idle" | "busy" | "ok" | "warn" | "bad";
  message?: string;
}) {
  if (state === "idle" || !message) return null;
  const cls = {
    busy: "text-base-content/70",
    ok: "text-success",
    warn: "text-warning",
    bad: "text-error",
  }[state];
  return (
    <span className={`flex items-center gap-2 text-sm ${cls}`}>
      {state === "busy" && <span className="loading loading-spinner loading-xs" />}
      {message}
    </span>
  );
}
