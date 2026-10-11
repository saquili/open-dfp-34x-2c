// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Signing in, and the password change a new stick asks for first. */

import { zodResolver } from "@hookform/resolvers/zod";
import { useNavigate } from "@tanstack/react-router";
import { Cable, KeyRound } from "lucide-react";
import { useEffect } from "react";
import { useForm } from "react-hook-form";
import { useChangePassword, useSignIn } from "../api/auth";
import { ApiError, OfflineError } from "../api/client";
import { useI18n } from "../i18n";
import { required, z } from "../lib/schemas";

function Frame({ children }: { children: React.ReactNode }) {
  const { t } = useI18n();
  return (
    <div className="grid min-h-dvh place-items-center bg-base-200 p-4">
      <div className="w-full max-w-sm">
        <div className="mb-6 flex items-center justify-center gap-2 text-lg font-semibold">
          <Cable className="size-6 text-primary" />
          {t("app.product")}
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

export function ChangePassword({ forced }: { forced: boolean }) {
  const { t } = useI18n();
  const navigate = useNavigate();
  const change = useChangePassword();
  const schema = z
    .object({
      current: z.string().check(z.minLength(1, t("form.required"))),
      next: z.string().check(z.minLength(8, t("auth.tooShort"))),
      repeat: z.string(),
    })
    .check(
      z.refine((v) => v.next === v.repeat, { path: ["repeat"], message: t("auth.mismatch") }),
      z.refine((v) => v.next !== v.current, { path: ["next"], message: t("auth.sameAsOld") }),
    );
  const form = useForm<z.infer<typeof schema>>({
    resolver: zodResolver(schema),
    defaultValues: { current: "", next: "", repeat: "" },
  });
  const errors = form.formState.errors;

  const submit = form.handleSubmit(({ current, next }) =>
    change.mutate({ current, next }, { onSuccess: () => navigate({ to: "/" }) }),
  );

  const failed =
    change.error instanceof OfflineError
      ? t("auth.offline")
      : change.error instanceof ApiError && change.error.status === 403
        ? t("auth.wrong")
        : change.error instanceof ApiError && change.error.status === 423
          ? t("auth.noStore")
          : change.error
            ? change.error.message
            : null;

  const body = (
    <>
      <div className="flex items-start gap-3">
        <KeyRound className="mt-1 size-5 text-primary" />
        <div>
          <h1 className="text-xl font-semibold">{t("auth.changeTitle")}</h1>
          {forced && <p className="text-sm text-base-content/70">{t("auth.changeForced")}</p>}
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
        <p className="text-xs text-base-content/60">{t("auth.rules")}</p>
        {failed && <p className="text-sm text-error">{failed}</p>}
        <button type="submit" className="btn btn-primary" disabled={change.isPending}>
          {change.isPending && <span className="loading loading-spinner loading-sm" />}
          {t("auth.save")}
        </button>
      </form>
    </>
  );

  return forced ? (
    <Frame>{body}</Frame>
  ) : (
    <div className="mx-auto max-w-md">
      <div className="card border border-base-300 bg-base-100">
        <div className="card-body gap-4">{body}</div>
      </div>
    </div>
  );
}
