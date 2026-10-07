// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The SFP port, towards the host: whether it is up and how fast, the
 * traffic on it, and how the host gets the public address when the stick
 * holds the session.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { useEffect } from "react";
import { useForm } from "react-hook-form";
import { type ApiError, OfflineError } from "../api/client";
import { useLinkChange } from "../api/config";
import { useLive, useTraffic } from "../api/live";
import { Field, SaveState } from "../components/Field";
import { TrafficChart } from "../components/TrafficChart";
import { Badge, Card, Facts, Figure, PageTitle } from "../components/ui";
import { useI18n } from "../i18n";
import { bitrate } from "../lib/format";
import { ipv4, vlan, z } from "../lib/schemas";
import { hostLink, type LinkView, linkSpeed, port, session } from "../lib/state";

function HandoverForm({ link }: { link: LinkView }) {
  const { t } = useI18n();
  const change = useLinkChange();
  const schema = z.object({ vlan: vlan(t), gateway: ipv4(t) });
  type Values = z.infer<typeof schema>;
  const current = { vlan: link.vlan, gateway: link.gateway };
  const form = useForm<Values>({ resolver: zodResolver(schema), defaultValues: current });
  const { errors, isDirty } = form.formState;

  // biome-ignore lint/correctness/useExhaustiveDependencies: refill only when the stick's values change
  useEffect(() => {
    if (!form.formState.isDirty) form.reset(current);
  }, [link.vlan, link.gateway]);

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
    <form
      onSubmit={form.handleSubmit((v) =>
        change.mutate({ VLAN: v.vlan, GATEWAY: v.gateway }, { onSuccess: () => form.reset(v) }),
      )}
      noValidate
      className="grid gap-4"
    >
      <div className="grid gap-4 sm:grid-cols-2">
        <Field
          label={t("sfp.vlan")}
          hint={t("sfp.vlanHint")}
          inputMode="numeric"
          error={errors.vlan?.message}
          {...form.register("vlan")}
        />
        <Field
          label={t("sfp.gateway")}
          hint={t("sfp.gatewayHint")}
          error={errors.gateway?.message}
          {...form.register("gateway")}
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
        <SaveState {...state} />
      </div>
    </form>
  );
}

export function SfpPage() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const traffic = useTraffic();
  const now = traffic.at(-1);
  const link = hostLink(live);
  const s = session(live);
  const sfp = port(live, "sfp");

  return (
    <>
      <PageTitle>{t("sfp.title")}</PageTitle>
      <div className="grid gap-4 lg:grid-cols-2">
        <Card
          title={t("sfp.link")}
          action={
            <Badge tone={link.up ? "good" : "bad"}>{link.up ? t("sfp.up") : t("sfp.down")}</Badge>
          }
        >
          <Figure value={linkSpeed(link.speedMbit, lang) ?? "–"} caption={t("sfp.speed")} />
          <Facts
            rows={[
              [t("sfp.state"), link.up ? t("sfp.up") : t("sfp.down")],
              sfp && [
                t("sfp.stickMac"),
                <span key="m" className="font-mono text-xs">
                  {sfp.mac}
                </span>,
              ],
              sfp && [t("sfp.mtu"), sfp.mtu],
            ]}
          />
        </Card>
        <Card title={t("sfp.handover")}>
          <p className="text-sm text-base-content/70">{t("sfp.handoverHint")}</p>
          <Facts rows={[[t("sfp.lease"), s.up && link.lease ? link.lease : t("sfp.noLease")]]} />
          <HandoverForm link={link} />
        </Card>
      </div>
      <Card title={t("sfp.traffic")} className="mt-4">
        <div className="grid gap-4 sm:grid-cols-[12rem_minmax(0,1fr)]">
          <div className="grid grid-cols-2 gap-3 sm:grid-cols-1">
            <Figure value={bitrate(now?.lanDown, lang)} caption={`↓ ${t("sfp.toHost")}`} />
            <Figure value={bitrate(now?.lanUp, lang)} caption={`↑ ${t("sfp.fromHost")}`} />
          </div>
          <TrafficChart data={traffic} down={(r) => r.lanDown} up={(r) => r.lanUp} />
        </div>
      </Card>
    </>
  );
}
