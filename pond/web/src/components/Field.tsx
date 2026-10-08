// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* A labelled input of a form, with its hint and its error. */

import { Eye, EyeOff } from "lucide-react";
import type { JSX } from "preact";
import { forwardRef, type InputHTMLAttributes, type ReactNode, useState } from "react";
import { type ApiError, OfflineError } from "../api/client";
import { useI18n } from "../i18n";

/* The input type that hides what is typed. */
const MASKED = "password";

type SecretProps = Omit<InputHTMLAttributes<HTMLInputElement>, "type" | "size"> & {
  /** An icon at the start of the field. */
  icon?: ReactNode;
  small?: boolean;
  invalid?: boolean;
};

/**
 * The input of a secret: hidden as it is typed, with the eye that shows it
 * inside the field, at its end.
 */
export const SecretInput = forwardRef<HTMLInputElement, SecretProps>(function SecretInput(
  { icon, small, invalid, className = "", ...input },
  ref,
) {
  const { t } = useI18n();
  const [shown, setShown] = useState(false);
  return (
    <div
      className={`input w-full pr-1 ${small ? "input-sm" : ""} ${invalid ? "input-error" : ""} ${className}`}
    >
      {icon}
      <input
        ref={ref}
        {...({
          className: "min-w-0 grow",
          /* Preact types each input type apart; this one switches. */
          type: shown ? "text" : MASKED,
          spellcheck: false,
          autoComplete: "off",
          "aria-invalid": !!invalid,
          ...input,
        } as unknown as JSX.IntrinsicElements["input"])}
      />
      <button
        type="button"
        className={`btn btn-ghost btn-circle text-base-content/60 ${small ? "btn-xs" : "btn-sm"}`}
        onClick={() => setShown((v) => !v)}
        aria-label={shown ? t("internet.hide") : t("internet.show")}
        title={shown ? t("internet.hide") : t("internet.show")}
      >
        {shown ? (
          <EyeOff className={small ? "size-3.5" : "size-4"} />
        ) : (
          <Eye className={small ? "size-3.5" : "size-4"} />
        )}
      </button>
    </div>
  );
});

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
  const name = id ?? input.name;
  return (
    /*
     * Its rows stay at the top of the cell, so that in a grid of fields the
     * inputs of one row line up whether or not a neighbour has a hint under
     * it, which makes the row taller.
     */
    <div className={`grid content-start gap-1 ${className}`}>
      <label htmlFor={name} className="text-sm font-medium">
        {label}
      </label>
      {secret ? (
        <SecretInput ref={ref} id={name} invalid={!!error} {...input} />
      ) : (
        <input
          ref={ref}
          {...({
            id: name,
            className: `input w-full ${error ? "input-error" : ""}`,
            type: "text",
            spellcheck: false,
            autoComplete: "off",
            "aria-invalid": !!error,
            ...input,
          } as unknown as JSX.IntrinsicElements["input"])}
        />
      )}
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

/** The state line of a change in progress: saving, saved, or why it failed. */
export function useChangeState(change: {
  isPending: boolean;
  isSuccess: boolean;
  error: Error | null;
}): { state: "idle" | "busy" | "ok" | "bad"; message?: string } {
  const { t } = useI18n();
  if (change.isPending) return { state: "busy", message: t("internet.saving") };
  if (change.isSuccess) return { state: "ok", message: t("internet.saved") };
  if (change.error)
    return {
      state: "bad",
      message:
        change.error instanceof OfflineError
          ? t("connection.offline")
          : t("internet.failed", {
              reason: (change.error as ApiError).detail ?? change.error.message,
            }),
    };
  return { state: "idle" };
}
