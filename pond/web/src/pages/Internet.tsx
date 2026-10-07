// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The internet. Everything the operator set up passes to the router; on top
 * of that, with an account entered here, the stick holds a PPPoE session
 * itself and hands its public address to the router. Removing the account
 * leaves the router to dial through the stick.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { useEffect } from "react";
import { useForm } from "react-hook-form";
import { type ApiError, OfflineError } from "../api/client";
import { usePppoeChange } from "../api/config";
import { useLive } from "../api/live";
import { Field, SaveState } from "../components/Field";
import { useGemText } from "../components/Gem";
import { Badge, Card, Facts, Figure, PageTitle } from "../components/ui";
import { useI18n } from "../i18n";
import { mac, upTo, vlan, z } from "../lib/schemas";
import { gems, type SessionView, session } from "../lib/state";

function AccountForm({ s }: { s: SessionView }) {
  const { t } = useI18n();
  const change = usePppoeChange();
  const schema = z.object({
    user: upTo(63),
    secret: z.string().check(z.maxLength(255)),
    vlan: vlan(t),
    mac: mac(t),
  });
  type Values = z.infer<typeof schema>;
  const current = { user: s.user, secret: "", vlan: s.vlan, mac: s.mac };
  const form = useForm<Values>({ resolver: zodResolver(schema), defaultValues: current });
  const { errors, isDirty } = form.formState;

  // biome-ignore lint/correctness/useExhaustiveDependencies: refill only when the stick's values change
  useEffect(() => {
    if (!form.formState.isDirty) form.reset(current);
  }, [s.user, s.vlan, s.mac]);

  const send = (v: Values) =>
    change.mutate(
      { USER: v.user, SECRET: v.secret || undefined, VLAN: v.vlan, MAC: v.mac },
      { onSuccess: () => form.reset({ ...v, secret: "" }) },
    );

  const remove = () => {
    if (!window.confirm(t("internet.removeConfirm"))) return;
    change.mutate({ USER: "" }, { onSuccess: () => form.reset({ ...current, user: "" }) });
  };

  const state = change.isPending
    ? { state: "busy" as const, message: t("internet.saving") }
    : change.isSuccess
      ? { state: "ok" as const, message: t("internet.saved") }
      : change.error
        ? {
            state: "bad" as const,
            message:
              change.error instanceof OfflineError
                ? t("connection.offline")
                : t("internet.failed", {
                    reason: (change.error as ApiError).detail ?? change.error.message,
                  }),
          }
        : { state: "idle" as const };

  return (
    <form onSubmit={form.handleSubmit(send)} noValidate className="grid gap-4">
      <div className="grid gap-4 sm:grid-cols-2">
        <Field
          label={t("internet.user")}
          autoComplete="username"
          error={errors.user?.message}
          {...form.register("user")}
        />
        <Field
          label={t("internet.passwordField")}
          secret
          autoComplete="new-password"
          placeholder={s.secretSet ? t("internet.passwordKept") : t("internet.passwordNone")}
          error={errors.secret?.message}
          {...form.register("secret")}
        />
        <Field
          label={t("internet.vlan")}
          hint={t("internet.vlanHint")}
          inputMode="numeric"
          error={errors.vlan?.message}
          {...form.register("vlan")}
        />
        <Field
          label={t("internet.mac")}
          hint={t("internet.macHint")}
          error={errors.mac?.message}
          {...form.register("mac")}
        />
      </div>
      <div className="flex flex-wrap items-center gap-3">
        <button type="submit" className="btn btn-primary" disabled={!isDirty || change.isPending}>
          {t("internet.save")}
        </button>
        {isDirty && (
          <button type="button" className="btn btn-ghost" onClick={() => form.reset(current)}>
            {t("form.discard")}
          </button>
        )}
        {s.configured && (
          <button
            type="button"
            className="btn btn-ghost text-error"
            onClick={remove}
            disabled={change.isPending}
          >
            {t("internet.remove")}
          </button>
        )}
        <SaveState {...state} />
      </div>
    </form>
  );
}

export function Internet() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const s = session(live);
  const channels = gems(live);
  const gem = useGemText();

  const badge = s.configured ? (
    <Badge tone={s.up ? "good" : "warn"}>{s.up ? t("internet.up") : t("internet.down")}</Badge>
  ) : (
    <Badge tone="neutral">{t("internet.off")}</Badge>
  );

  return (
    <>
      <PageTitle hint={t("internet.sessionHint")}>{t("internet.title")}</PageTitle>
      <div className="grid gap-4 lg:grid-cols-[minmax(0,2fr)_minmax(0,3fr)]">
        <Card title={t("internet.sessionTitle")} action={badge}>
          {s.configured ? (
            <>
              <Figure value={s.address ?? "–"} caption={t("internet.address")} />
              <Facts
                rows={[
                  [t("internet.account"), s.user],
                  [t("internet.vlan"), s.vlan || t("common.none")],
                  [t("internet.dns"), s.dns.length ? s.dns.join(", ") : "–"],
                ]}
              />
            </>
          ) : (
            <p className="text-sm">{t("internet.noAccount")}</p>
          )}
        </Card>
        <Card title={t("internet.account")}>
          <AccountForm s={s} />
        </Card>
      </div>

      <Card title={t("internet.bridged")} className="mt-4">
        <p className="text-sm text-base-content/70">{t("internet.bridgedHint")}</p>
        <ul className="grid gap-2 sm:grid-cols-2">
          {channels.map((v) => (
            <li
              key={v.gem.name}
              className="grid gap-0.5 rounded-box border border-base-300 px-3 py-2"
            >
              <span className="flex items-baseline gap-3">
                <span className="tabular font-medium">GEM {v.gem.port_id}</span>
                <span className="text-sm text-base-content/70">{gem.direction(v.gem)}</span>
                {v.allocId != null && (
                  <span className="tabular ml-auto text-xs text-base-content/55">
                    Alloc-ID {v.allocId}
                  </span>
                )}
              </span>
              <span className="text-sm">{gem.use(v)}</span>
            </li>
          ))}
        </ul>
      </Card>
    </>
  );
}
