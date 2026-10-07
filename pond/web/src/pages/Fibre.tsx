// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The fibre: the state of the connection to the operator, the light, the
 * errors on the line, what the OLT set up, and how the stick introduces
 * itself, which the owner may change to what the operator expects.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { useEffect } from "react";
import { useForm } from "react-hook-form";
import { type ApiError, OfflineError } from "../api/client";
import { useIdentity, useIdentityChange } from "../api/config";
import { useLive } from "../api/live";
import type { Identity } from "../api/types";
import { Field, SaveState } from "../components/Field";
import { Badge, Card, Facts, Figure, PageTitle } from "../components/ui";
import { useI18n } from "../i18n";
import { bytes, dbmText, decimal, integer } from "../lib/format";
import { upTo, z } from "../lib/schemas";
import { fibre, LIGHT_TONE, RX_MAX_DBM, RX_MIN_DBM, RX_WEAK_DBM } from "../lib/state";

/**
 * Where the received light sits: the receiver's working range, its weak end
 * marked, and the reading on it.
 */
function LightScale({ rx }: { rx: number | null }) {
  const { lang } = useI18n();
  const lo = RX_MIN_DBM - 4;
  const hi = RX_MAX_DBM + 4;
  const at = (v: number) => ((v - lo) / (hi - lo)) * 100;
  const band = (from: number, to: number, cls: string) => (
    <div
      className={`absolute inset-y-0 ${cls}`}
      style={{ left: `${at(from)}%`, width: `${at(to) - at(from)}%` }}
    />
  );
  return (
    <div className="mt-1">
      <div className="relative h-2 overflow-hidden rounded-full bg-base-300">
        {band(RX_MIN_DBM, RX_WEAK_DBM, "bg-warning/50")}
        {band(RX_WEAK_DBM, RX_MAX_DBM, "bg-success/50")}
      </div>
      <div className="relative h-0">
        {rx != null && (
          <div
            className="absolute -top-3 size-4 -translate-x-1/2 rounded-full border-2 border-base-100 bg-primary shadow"
            style={{ left: `${at(Math.min(hi, Math.max(lo, rx)))}%` }}
          />
        )}
      </div>
      <div className="relative mt-1 h-4 text-[11px] text-base-content/55">
        {[RX_MIN_DBM, RX_WEAK_DBM, RX_MAX_DBM].map((v) => (
          <span key={v} className="absolute -translate-x-1/2" style={{ left: `${at(v)}%` }}>
            {decimal(v, 0, lang)}
          </span>
        ))}
      </div>
    </div>
  );
}

const IDENTITY_FIELDS = [
  ["serial", "SERIAL", "fibre.serial"],
  ["vendor_id", "VENDOR_ID", "fibre.vendorId"],
  ["equipment_id", "EQUIPMENT_ID", "fibre.equipmentId"],
  ["version", "VERSION", "fibre.version"],
  ["equipment_serial", "EQUIPMENT_SERIAL", "fibre.equipmentSerial"],
  ["model", "MODEL", "fibre.model"],
  ["omcc_version", "OMCC_VERSION", "fibre.omccVersion"],
] as const;

function IdentityForm({ identity }: { identity: Identity }) {
  const { t } = useI18n();
  const change = useIdentityChange();
  const schema = z.object({
    serial: z
      .string()
      .check(z.trim(), z.regex(/^[A-Za-z0-9]{4}[0-9A-Fa-f]{8}$/, t("fibre.serialRule"))),
    vendor_id: upTo(4),
    equipment_id: upTo(20),
    version: upTo(14),
    equipment_serial: upTo(32),
    model: upTo(32),
    omcc_version: upTo(8),
    ploam: z.string().check(z.maxLength(36)),
  });
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: { ...identity, ploam: "" },
  });
  const { errors, isDirty } = form.formState;

  useEffect(() => {
    if (!form.formState.isDirty) form.reset({ ...identity, ploam: "" });
  }, [identity, form]);

  const submit = form.handleSubmit((v) => {
    if (!window.confirm(t("fibre.identityConfirm"))) return;
    const out: Record<string, string> = {};
    for (const [field, key] of IDENTITY_FIELDS) out[key] = v[field];
    if (v.ploam) out.PLOAM = v.ploam;
    change.mutate(out, { onSuccess: () => form.reset({ ...v, ploam: "" }) });
  });

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
    <form onSubmit={submit} noValidate className="grid gap-4">
      <div className="grid gap-4 sm:grid-cols-2">
        {IDENTITY_FIELDS.map(([field, , label]) => (
          <Field
            key={field}
            label={t(label)}
            error={errors[field]?.message}
            className={field === "serial" ? "sm:col-span-2" : ""}
            hint={field === "serial" ? t("fibre.serialHint") : undefined}
            {...form.register(field)}
          />
        ))}
        <Field
          label={t("fibre.ploam")}
          secret
          autoComplete="new-password"
          placeholder={identity.ploam_set ? t("internet.passwordKept") : t("internet.passwordNone")}
          hint={t("fibre.ploamHint")}
          className="sm:col-span-2"
          {...form.register("ploam")}
        />
      </div>
      <div role="alert" className="alert alert-warning alert-soft text-sm">
        {t("fibre.identityWarning")}
      </div>
      <div className="flex flex-wrap items-center gap-3">
        <button type="submit" className="btn btn-primary" disabled={!isDirty || change.isPending}>
          {t("internet.save")}
        </button>
        {isDirty && (
          <button type="button" className="btn btn-ghost" onClick={() => form.reset()}>
            {t("form.discard")}
          </button>
        )}
        <SaveState {...state} />
      </div>
    </form>
  );
}

export function Fibre() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const identity = useIdentity();
  const f = fibre(live);
  const onu = live?.onu;
  const optics = live?.optics;
  const gems = (live?.gem_ports ?? []).slice().sort((a, z) => a.port_id - z.port_id);
  const tconts = (live?.tconts ?? []).filter((x) => x.alloc_id != null);

  const direction = (d: string, multicast: boolean) =>
    multicast
      ? t("fibre.multicast")
      : d === "both"
        ? t("fibre.both")
        : d === "downstream"
          ? t("fibre.downstream")
          : t("fibre.upstream");

  return (
    <>
      <PageTitle>{t("fibre.title")}</PageTitle>
      <div className="grid gap-4 lg:grid-cols-2">
        <Card
          title={t("fibre.status")}
          action={<Badge tone={f.tone}>{t(`state.${f.stateKey}`)}</Badge>}
        >
          <Facts
            rows={[
              [t("fibre.state"), t(`state.${f.stateKey}`)],
              [t("fibre.onuId"), f.onuId ?? "–"],
              [
                t("fibre.alarms"),
                f.alarms.length ? (
                  <span key="alarms" className="text-error">
                    {f.alarms.join(", ")}
                  </span>
                ) : (
                  t("fibre.noAlarms")
                ),
              ],
              identity.data && [t("fibre.serial"), identity.data.serial],
            ]}
          />
          <p className="text-xs text-base-content/60">{t("fibre.onuIdHint")}</p>
        </Card>

        <Card
          title={t("fibre.optics")}
          action={<Badge tone={LIGHT_TONE[f.quality]}>{t(`light.${f.quality}`)}</Badge>}
        >
          <Figure
            value={dbmText(f.rxDbm, lang)}
            caption={t("fibre.rx")}
            tone={LIGHT_TONE[f.quality]}
          />
          <LightScale rx={f.rxDbm} />
          <p className="text-xs text-base-content/60">
            {t("fibre.rxRange", { min: dbmText(RX_MIN_DBM, lang), max: dbmText(RX_MAX_DBM, lang) })}
          </p>
          <Facts
            rows={[
              [t("fibre.tx"), dbmText(f.txDbm, lang)],
              optics && [t("fibre.temperature"), `${decimal(optics.temperature_c, 1, lang)} °C`],
              optics && [t("fibre.voltage"), `${decimal(optics.vcc_v, 2, lang)} V`],
              optics && [t("fibre.bias"), `${integer(optics.bias_ma, lang)} mA`],
            ]}
          />
        </Card>

        <Card title={t("fibre.quality")}>
          {onu ? (
            <Facts
              rows={[
                [t("fibre.bipErrors"), integer(onu.counters.bip_errors, lang)],
                [t("fibre.fecCorrected"), integer(onu.counters.fec_corrected_codewords, lang)],
                [
                  t("fibre.fecUncorrectable"),
                  <span
                    key="u"
                    className={onu.counters.fec_uncorrectable_codewords ? "text-error" : ""}
                  >
                    {integer(onu.counters.fec_uncorrectable_codewords, lang)}
                  </span>,
                ],
              ]}
            />
          ) : (
            <p className="text-sm">{t("state.noPon")}</p>
          )}
          <p className="text-xs text-base-content/60">{t("fibre.sinceStart")}</p>
        </Card>

        <Card title={t("fibre.provisioned")}>
          <p className="text-sm text-base-content/70">{t("fibre.provisionedHint")}</p>
          <div>
            <h3 className="mb-1 text-sm font-semibold">{t("fibre.gemPorts")}</h3>
            <div className="overflow-x-auto">
              <table className="table table-sm">
                <thead>
                  <tr>
                    <th>{t("fibre.channel")}</th>
                    <th>{t("fibre.direction")}</th>
                    <th className="text-right">{t("fibre.received")}</th>
                    <th className="text-right">{t("fibre.sent")}</th>
                  </tr>
                </thead>
                <tbody>
                  {gems.map((g) => {
                    const c = live?.gem_counters?.[g.name];
                    return (
                      <tr key={g.name}>
                        <td className="tabular">GEM {g.port_id}</td>
                        <td>{direction(g.direction, g.multicast)}</td>
                        <td className="tabular text-right">{bytes(c?.pon_rx_octets, lang)}</td>
                        <td className="tabular text-right">{bytes(c?.pon_tx_octets, lang)}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          </div>
          <div>
            <h3 className="mb-1 text-sm font-semibold">{t("fibre.tconts")}</h3>
            <p className="tabular text-sm">
              {tconts.length
                ? tconts.map((x) => `Alloc-ID ${x.alloc_id}`).join(", ")
                : t("common.none")}
            </p>
          </div>
        </Card>
      </div>

      <Card
        title={t("fibre.identity")}
        className="mt-4"
        action={
          identity.data?.source && (
            <Badge tone={identity.data.source === "store" ? "good" : "info"}>
              {t(identity.data.source === "store" ? "fibre.fromStore" : "fibre.fromFactory")}
            </Badge>
          )
        }
      >
        <p className="text-sm text-base-content/70">{t("fibre.identityHint")}</p>
        {identity.data ? (
          <IdentityForm identity={identity.data} />
        ) : identity.isError ? (
          <p className="text-sm text-error">{identity.error.message}</p>
        ) : (
          <span className="loading loading-spinner" />
        )}
      </Card>
    </>
  );
}
