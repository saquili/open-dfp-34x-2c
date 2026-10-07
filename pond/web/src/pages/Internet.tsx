// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The internet. Everything the operator set up passes to the host; on top of
 * that, the stick holds the PPPoE sessions it has accounts for, up to seven,
 * and the first that carries IPv4 hands its public address to the host. With
 * no account, the host dials through the stick.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Plus } from "lucide-react";
import { useState } from "react";
import { useForm } from "react-hook-form";
import { type ApiError, OfflineError } from "../api/client";
import { useSessionChange } from "../api/config";
import { useLive } from "../api/live";
import type { Family } from "../api/types";
import { Field, SaveState } from "../components/Field";
import { useGemText } from "../components/Gem";
import { Badge, Card, Facts, Figure, PageTitle } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { mac, required, upTo, vlan, z } from "../lib/schemas";
import { gems, MAX_SESSIONS, type SessionView, session, sessions } from "../lib/state";

const FAMILY: Record<Family, Key> = {
  both: "internet.famBoth",
  ipv4: "internet.famV4",
  ipv6: "internet.famV6",
};

/** The state line of a change: saving, saved, or why it failed. */
function useChangeState(change: ReturnType<typeof useSessionChange>) {
  const { t } = useI18n();
  return change.isPending
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
}

/** A session's account and settings: a new one without @x, or @x changed. */
function SessionForm({ x, onDone }: { x?: SessionView; onDone: () => void }) {
  const { t } = useI18n();
  const change = useSessionChange();
  const state = useChangeState(change);
  const schema = z.object({
    user: x ? upTo(63) : required(t("form.required")),
    secret: x ? z.string().check(z.maxLength(255)) : required(t("internet.secretRequired")),
    vlan: vlan(t),
    mac: mac(t),
    family: z.enum(["both", "ipv4", "ipv6"]),
  });
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: {
      user: x?.user ?? "",
      secret: "",
      vlan: x?.vlan ?? "",
      mac: x?.mac ?? "",
      family: x?.family ?? "both",
    },
  });
  const { errors, isDirty } = form.formState;

  const send = (v: Values) =>
    change.mutate(
      {
        ID: x ? String(x.id) : undefined,
        USER: v.user,
        SECRET: v.secret || undefined,
        VLAN: v.vlan,
        MAC: v.mac,
        FAMILY: v.family,
      },
      { onSuccess: onDone },
    );

  return (
    <form
      onSubmit={form.handleSubmit(send)}
      noValidate
      className="grid gap-4 rounded-box border border-base-300 p-4"
    >
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
          placeholder={x?.secretSet ? t("internet.passwordKept") : t("internet.passwordNone")}
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
        <div className="grid gap-1 sm:col-span-2">
          <label htmlFor={`family-${x?.id ?? "new"}`} className="text-sm font-medium">
            {t("internet.family")}
          </label>
          <select
            id={`family-${x?.id ?? "new"}`}
            className="select w-full sm:w-auto"
            {...form.register("family")}
          >
            {(["both", "ipv4", "ipv6"] as const).map((f) => (
              <option key={f} value={f}>
                {t(FAMILY[f])}
              </option>
            ))}
          </select>
          <p className="text-xs text-base-content/60">{t("internet.familyHint")}</p>
        </div>
      </div>
      <div className="flex flex-wrap items-center gap-3">
        <button
          type="submit"
          className="btn btn-primary"
          disabled={(x && !isDirty) || change.isPending}
        >
          {x ? t("internet.save") : t("internet.add")}
        </button>
        <button type="button" className="btn btn-ghost" onClick={onDone}>
          {t("internet.cancel")}
        </button>
        <SaveState {...state} />
      </div>
    </form>
  );
}

/** One session: its state, what it carries, and its account. */
function SessionCard({ x }: { x: SessionView }) {
  const { t } = useI18n();
  const { data: live } = useLive();
  const [editing, setEditing] = useState(false);
  const change = useSessionChange();
  const state = useChangeState(change);
  const portId = gems(live).find((g) => g.gem.name === x.gemDev)?.gem.port_id;
  /* Sessions in one VLAN send from the MAC address of the first of them. */
  const first = sessions(live).find((y) => y.vlan === x.vlan && y.gem === x.gem);
  const macText =
    first && first.id !== x.id
      ? `${first.mac || t("internet.macStick")} (${t("internet.macShared", { id: first.id })})`
      : x.mac || t("internet.macStick");

  const remove = () => {
    if (!window.confirm(t("internet.removeSessionConfirm", { id: x.id }))) return;
    change.mutate({ ID: String(x.id), USER: "" });
  };

  return (
    <li className="grid gap-3 rounded-box border border-base-300 p-4">
      <div className="flex flex-wrap items-center gap-2">
        <h3 className="font-semibold">{t("internet.sessionN", { id: x.id })}</h3>
        <Badge tone={x.up ? "good" : "warn"}>
          {x.up ? t("internet.up") : t("internet.connecting")}
        </Badge>
        <Badge tone="neutral">{t(FAMILY[x.family])}</Badge>
        {x.handsOn && <Badge tone="info">{t("internet.handsOn")}</Badge>}
      </div>
      <Facts
        rows={[
          [t("internet.account"), x.user],
          [t("internet.vlan"), x.vlan || t("switch.untagged")],
          [
            t("internet.channel"),
            portId != null
              ? `GEM ${portId}`
              : x.gem === "auto"
                ? t("internet.autoGem")
                : `GEM ${x.gem}`,
          ],
          [t("internet.mac"), macText],
          x.family !== "ipv6" && [
            "IPv4",
            x.ipv4
              ? t(x.handsOn ? "internet.ipv4Host" : "internet.ipv4Stick", { addr: x.ipv4 })
              : "–",
          ],
          x.family !== "ipv4" && ["IPv6", x.ipv6 ? t("internet.ipv6Up") : "–"],
          x.family !== "ipv4" && [
            t("internet.prefix"),
            x.prefix
              ? t(x.prefixHost ? "internet.prefixHost" : "internet.prefixWaiting", {
                  prefix: x.prefix,
                })
              : x.ipv6
                ? t("internet.prefixNone")
                : "–",
          ],
          x.dev ? [t("internet.iface"), [x.dev, x.ppp].filter(Boolean).join(" → ")] : null,
        ]}
      />
      {x.family !== "ipv4" && x.prefix && (
        <p className="text-xs text-base-content/60">{t("internet.ipv6Software")}</p>
      )}
      {editing ? (
        <SessionForm x={x} onDone={() => setEditing(false)} />
      ) : (
        <div className="flex flex-wrap items-center gap-3">
          <button type="button" className="btn btn-sm" onClick={() => setEditing(true)}>
            {t("internet.edit")}
          </button>
          <button
            type="button"
            className="btn btn-sm btn-ghost text-error"
            onClick={remove}
            disabled={change.isPending}
          >
            {t("internet.removeSession")}
          </button>
          <SaveState {...state} />
        </div>
      )}
    </li>
  );
}

export function Internet() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const s = session(live);
  const list = sessions(live);
  const [adding, setAdding] = useState(false);
  const channels = gems(live);
  const gem = useGemText();

  const badge = s.configured ? (
    <Badge tone={s.up ? "good" : "warn"}>
      {s.up ? t("internet.up") : t("internet.connecting")}
    </Badge>
  ) : (
    <Badge tone="neutral">{t("internet.off")}</Badge>
  );

  return (
    <>
      <PageTitle hint={t("internet.sessionHint")}>{t("internet.title")}</PageTitle>
      <div className="grid gap-4 lg:grid-cols-2">
        <Card title={t("internet.summary")} action={badge}>
          {s.configured ? (
            <>
              <Figure value={s.address ?? "–"} caption={t("internet.hostAddress")} />
              <Facts
                rows={[
                  [
                    t("internet.sessionsLabel"),
                    t("internet.sessionsCount", { n: s.count, max: MAX_SESSIONS }),
                  ],
                  s.prefix ? [t("internet.hostPrefix"), s.prefix] : null,
                  [t("internet.dns"), s.dns.length ? s.dns.join(", ") : "–"],
                ]}
              />
            </>
          ) : (
            <p className="text-sm">{t("internet.noAccount")}</p>
          )}
        </Card>
        <Card title={t("internet.rules")}>
          <ul className="grid list-disc gap-1 pl-5 text-sm">
            <li>{t("internet.rule1")}</li>
            <li>{t("internet.rule2")}</li>
            <li>{t("internet.rule3")}</li>
            <li>{t("internet.rule4")}</li>
          </ul>
        </Card>
      </div>

      <Card
        title={t("internet.sessionsTitle")}
        className="mt-4"
        action={
          <button
            type="button"
            className="btn btn-sm btn-primary"
            onClick={() => setAdding(true)}
            disabled={adding || list.length >= MAX_SESSIONS}
          >
            <Plus className="size-4" />
            {t("internet.add")}
          </button>
        }
      >
        {list.length >= MAX_SESSIONS && (
          <p className="text-sm text-base-content/70">
            {t("internet.full", { max: MAX_SESSIONS })}
          </p>
        )}
        {adding && <SessionForm onDone={() => setAdding(false)} />}
        {list.length ? (
          <ul className="grid gap-3">
            {list.map((x) => (
              <SessionCard key={x.id} x={x} />
            ))}
          </ul>
        ) : (
          !adding && <p className="text-sm">{t("internet.noAccount")}</p>
        )}
      </Card>

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
