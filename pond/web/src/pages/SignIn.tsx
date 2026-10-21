// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Signing in, and the password change a new stick asks for first. A change
 * of the password later is under System, Access, with the same rules.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { useNavigate } from "@tanstack/react-router";
import { Cable, KeyRound } from "lucide-react";
import { useEffect } from "react";
import { useForm } from "react-hook-form";
import { useChangePassword, useSignIn } from "../api/auth";
import { ApiError, OfflineError } from "../api/client";
import { useI18n } from "../i18n";
import { useProductName } from "../lib/product";
import { required, z } from "../lib/schemas";

/*
 * The product name is the one the browser read last: before a sign-in, and
 * while the default password must change, the stick tells nothing.
 */
function Frame({ children }: { children: React.ReactNode }) {
  const { t } = useI18n();
  const product = useProductName(t);
  return (
    <div className="grid min-h-dvh place-items-center bg-base-200 p-4">
      <div className="w-full max-w-sm">
        <div className="mb-6 flex items-center justify-center gap-2 text-lg font-semibold">
          <Cable className="size-6 text-primary" />
          {t("app.product", { name: product })}
        </div>
        <div className="card border border-base-300 bg-base-100">
          <div className="card-body gap-4">{children}</div>
        </div>
      </div>
    </div>
  );
}

export function SignIn() {
  const { t } = useI18n();
  const navigate = useNavigate();
  const signIn = useSignIn();
  const schema = z.object({
    user: required(t("form.required")),
    secret: z.string().check(z.minLength(1, t("form.required"))),
  });
  const form = useForm<z.infer<typeof schema>>({
    resolver: zodResolver(schema),
    defaultValues: { user: "root", secret: "" },
  });

  useEffect(() => form.setFocus("secret"), [form]);

  const submit = form.handleSubmit((v) =>
    signIn.mutate(v, {
      onSuccess: (s) => navigate({ to: s.must_change ? "/password" : "/" }),
    }),
  );

  const e = signIn.error;
  const message =
    e instanceof OfflineError
      ? t("auth.offline")
      : e instanceof ApiError && e.status === 429
        ? t("auth.tooMany", { seconds: e.retryAfter ?? 30 })
        : e instanceof ApiError && e.status === 423
          ? t("auth.noStore")
          : e
            ? t("auth.wrong")
            : null;

  return (
    <Frame>
      <div>
        <h1 className="text-xl font-semibold">{t("auth.title")}</h1>
        <p className="text-sm text-base-content/70">{t("auth.subtitle")}</p>
      </div>
      <form onSubmit={submit} className="grid gap-3" noValidate>
        <label className="floating-label">
          <span>{t("auth.user")}</span>
          <input className="input w-full" autoComplete="username" {...form.register("user")} />
        </label>
        <label className="floating-label">
          <span>{t("auth.passwordField")}</span>
          <input
            className="input w-full"
            type="password"
            autoComplete="current-password"
            {...form.register("secret")}
          />
        </label>
        {message && <p className="text-sm text-error">{message}</p>}
        <button type="submit" className="btn btn-primary" disabled={signIn.isPending}>
          {signIn.isPending && <span className="loading loading-spinner loading-sm" />}
          {t("auth.signIn")}
        </button>
      </form>
    </Frame>
  );
}

/* What the browser's password manager is told each field holds. */
const AUTOCOMPLETE = {
  current: "current-password",
  next: "new-password",
  repeat: "new-password",
} as const;

type T = ReturnType<typeof useI18n>["t"];

/** The rules of a new password: the current one, a new one of 8 or more, typed twice, another. */
export function passwordSchema(t: T) {
  return z
    .object({
      current: z.string().check(z.minLength(1, t("form.required"))),
      next: z.string().check(z.minLength(8, t("auth.tooShort"))),
      repeat: z.string(),
    })
    .check(
      z.refine((v) => v.next === v.repeat, { path: ["repeat"], message: t("auth.mismatch") }),
      z.refine((v) => v.next !== v.current, { path: ["next"], message: t("auth.sameAsOld") }),
    );
}

/** Why a change of the password was refused, in the panel's words; null for none. */
export function passwordFailure(t: T, e: unknown): string | null {
  if (e == null) return null;
  if (e instanceof OfflineError) return t("auth.offline");
  if (e instanceof ApiError && e.status === 403) return t("auth.wrong");
  if (e instanceof ApiError && e.status === 423) return t("auth.noStore");
  if (e instanceof ApiError && e.status === 429)
    return t("auth.tooMany", { seconds: e.retryAfter ?? 30 });
  if (e instanceof ApiError) return e.detail ?? e.message;
  return e instanceof Error ? e.message : String(e);
}

/** The change of the default password, before anything else. */
export function ChangePassword() {
  const { t } = useI18n();
  const navigate = useNavigate();
  const change = useChangePassword();
  const schema = passwordSchema(t);
  const form = useForm<z.infer<typeof schema>>({
    resolver: zodResolver(schema),
    defaultValues: { current: "", next: "", repeat: "" },
  });
  const errors = form.formState.errors;

  const submit = form.handleSubmit(({ current, next }) =>
    change.mutate({ current, next }, { onSuccess: () => navigate({ to: "/" }) }),
  );

  const failed = passwordFailure(t, change.error);

  return (
    <Frame>
      <div className="flex items-start gap-3">
        <KeyRound className="mt-1 size-5 text-primary" />
        <div>
          <h1 className="text-xl font-semibold">{t("auth.changeTitle")}</h1>
          <p className="text-sm text-base-content/70">{t("auth.changeForced")}</p>
        </div>
      </div>
      <form onSubmit={submit} className="grid gap-3" noValidate>
        {(["current", "next", "repeat"] as const).map((name) => (
          <div key={name}>
            <label className="floating-label">
              <span>
                {t(
                  name === "current"
                    ? "auth.current"
                    : name === "next"
                      ? "auth.next"
                      : "auth.repeat",
                )}
              </span>
              <input
                className={`input w-full ${errors[name] ? "input-error" : ""}`}
                type="password"
                autoComplete={AUTOCOMPLETE[name]}
                {...form.register(name)}
              />
            </label>
            {errors[name] && <p className="mt-1 text-xs text-error">{errors[name]?.message}</p>}
          </div>
        ))}
        <p className="text-xs text-base-content/70">{t("auth.rules")}</p>
        {failed && <p className="text-sm text-error">{failed}</p>}
        <button type="submit" className="btn btn-primary" disabled={change.isPending}>
          {change.isPending && <span className="loading loading-spinner loading-sm" />}
          {t("auth.save")}
        </button>
      </form>
    </Frame>
  );
}
