// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * System, Device: the stick itself, its model, since when it runs and what
 * became of its settings, its name and the name the panel shows for the
 * system; its clock, the time it reads and where that comes from; and its
 * maintenance, a report of every figure and a restart. The software it runs
 * is under Software, and every way into it under Access.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Download, RotateCw } from "lucide-react";
import { useEffect, useState } from "react";
import { useForm } from "react-hook-form";
import { request } from "../../api/client";
import { useClock, useHostnameChange, useProductChange, useRestart } from "../../api/config";
import { useLive } from "../../api/live";
import { useConfirm } from "../../components/Confirm";
import { Field, SaveState, useChangeState } from "../../components/Field";
import { Page } from "../../components/Page";
import { QueryNote } from "../../components/Read";
import { Badge, Card, Facts } from "../../components/ui";
import { type Key, useI18n } from "../../i18n";
import { clockState, offsetText, ppmText, sourceKey, spanText, stale } from "../../lib/clock";
import { duration } from "../../lib/format";
import { DEFAULT_PRODUCT, PRODUCT_MAX, productField, productSlug } from "../../lib/product";
import { reason } from "../../lib/read";
import { z } from "../../lib/schemas";
import type { Tone } from "../../lib/state";
import { Columns, Hint, Stack } from "./shared";

const STORE: Record<string, [Key, Tone]> = {
  loaded: ["system.stored", "good"],
  saved: ["system.stored", "good"],
  defaults: ["system.defaults", "warn"],
  failed: ["system.storeFailed", "bad"],
  none: ["system.storeFailed", "bad"],
};

function HostnameForm({ name }: { name: string }) {
  const { t } = useI18n();
  const change = useHostnameChange();
  const state = useChangeState(change);
  const schema = z.object({
    name: z
      .string()
      .check(
        z.trim(),
        z.regex(/^(?=.{1,63}$)[a-zA-Z0-9]([a-zA-Z0-9-]*[a-zA-Z0-9])?$/, t("system.hostnameRule")),
      ),
  });
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({ resolver: zodResolver(schema), defaultValues: { name } });
  const { errors, isDirty } = form.formState;
  useEffect(() => {
    if (!form.formState.isDirty) form.reset({ name });
  }, [name, form]);
  return (
    <form
      onSubmit={form.handleSubmit((v) =>
        change.mutate({ HOSTNAME: v.name }, { onSuccess: () => form.reset(v) }),
      )}
      noValidate
      className="grid gap-3"
    >
      <Field
        label={t("system.hostname")}
        hint={t("system.hostnameHint")}
        error={errors.name?.message}
        {...form.register("name")}
      />
      <div className="flex flex-wrap items-center gap-3">
        <button
          type="submit"
          className="btn btn-primary btn-sm"
          disabled={!isDirty || change.isPending}
        >
          {t("internet.save")}
        </button>
        <SaveState {...state} />
      </div>
    </form>
  );
}

/*
 * The name the panel shows for the system, a setting of the stick's: empty
 * brings back BuchOS. The hint shows how the files the stick hands out are
 * named with what is typed. The field is "product", as the hostname's
 * beside it is "name": a field's name is its input's id.
 */
function ProductForm({ name }: { name: string }) {
  const { t } = useI18n();
  const change = useProductChange();
  const state = useChangeState(change);
  const schema = z.object({
    product: productField(t("system.productRule", { max: PRODUCT_MAX })),
  });
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: { product: name },
  });
  const { errors, isDirty } = form.formState;
  const typed = (form.watch("product") ?? "").trim();
  useEffect(() => {
    if (!form.formState.isDirty) form.reset({ product: name });
  }, [name, form]);
  return (
    <form
      onSubmit={form.handleSubmit((v) =>
        change.mutate(
          { PRODUCT_NAME: v.product },
          { onSuccess: () => form.reset({ product: v.product || DEFAULT_PRODUCT }) },
        ),
      )}
      noValidate
      className="grid gap-3"
    >
      <Field
        label={t("system.product")}
        hint={t("system.productHint", { slug: productSlug(typed || DEFAULT_PRODUCT) })}
        error={errors.product?.message}
        {...form.register("product")}
      />
      <div className="flex flex-wrap items-center gap-3">
        <button
          type="submit"
          className="btn btn-primary btn-sm"
          disabled={!isDirty || change.isPending}
        >
          {t("internet.save")}
        </button>
        <SaveState {...state} />
      </div>
    </form>
  );
}

/* The stick: its model, since when it runs, its settings, its names. */
function DeviceCard() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const store = live?.settings.store ? STORE[live.settings.store] : undefined;
  const up = live?.uptime.up_s;
  /* Since when, by this browser's clock, which is set where the stick's may not be. */
  const since =
    up != null
      ? new Intl.DateTimeFormat(lang, { dateStyle: "medium", timeStyle: "short" }).format(
          Date.now() - up * 1000,
        )
      : null;
  return (
    <Card title={t("system.device")}>
      <Facts
        rows={[
          [t("system.model"), live?.model ?? "–"],
          [
            t("system.uptime"),
            since ? (
              <span key="up" className="grid">
                <span>{since}</span>
                <span className="text-xs font-normal text-base-content/70">
                  {t("system.upFor", { span: duration(up, lang) })}
                </span>
              </span>
            ) : (
              "–"
            ),
          ],
          [
            t("system.settingsStore"),
            store ? (
              <Badge key="store" tone={store[1]}>
                {t(store[0])}
              </Badge>
            ) : (
              t("system.unknown")
            ),
          ],
        ]}
      />
      {live?.hostname != null ? (
        <HostnameForm name={live.hostname} />
      ) : (
        <Hint>{t("system.hostnameUnavailable")}</Hint>
      )}
      {live?.product != null ? (
        <ProductForm name={live.product} />
      ) : (
        <Hint>{t("system.productUnavailable")}</Hint>
      )}
    </Card>
  );
}

/*
 * The stick's clock: the time it reads, the source that sets it and how near
 * it is, and what each source the daemon asks last gave.
 */
function ClockCard() {
  const { t, lang } = useI18n();
  const clock = useClock();
  const c = clock.data;
  const when = (s: number) =>
    new Intl.DateTimeFormat(lang, { dateStyle: "medium", timeStyle: "short" }).format(s * 1000);
  if (!c)
    return (
      <Card title={t("system.clock")}>
        <QueryNote reads={clock} />
      </Card>
    );
  const [state, tone] = clockState(c);
  const inUse = c.source != null && c.source !== "none" && !stale(c);
  return (
    <Card title={t("system.clock")}>
      <QueryNote reads={clock} />
      <Facts
        rows={[
          [t("system.clockTime"), when(c.now_s)],
          [
            t("system.clockState"),
            <Badge key="state" tone={tone}>
              {t(state)}
            </Badge>,
          ],
          [t("system.clockSource"), t(sourceKey(c))],
          inUse && [t("system.clockOffset"), offsetText(c.offset_ns, lang)],
          c.freq_ppb != null && [t("system.clockFreq"), ppmText(c.freq_ppb, lang)],
          c.ptp && [
            t("system.clockPtp"),
            c.ptp.master
              ? t("system.clockPtpMaster", {
                  master: c.ptp.master,
                  via: c.ptp.via === "udp" ? "UDP" : "L2",
                  delay: spanText(c.ptp.delay_ns, lang),
                })
              : t("system.clockPtpNone", { iface: c.ptp.interface ?? "–" }),
          ],
          c.ptp?.stamps && [
            t("system.clockStamps"),
            c.ptp.stamps === "hardware"
              ? t("system.clockStampsHw", {
                  freq: ppmText(c.ptp.phc_freq_ppb, lang),
                  offset: offsetText(c.ptp.sys_phc_offset_ns, lang),
                })
              : t("system.clockStampsSw"),
          ],
          c.ntp && [
            t("system.clockNtp"),
            c.ntp.good
              ? t("system.clockNtpAnswers", {
                  server: c.ntp.server ?? "–",
                  stratum: c.ntp.stratum ?? "–",
                  delay: spanText(c.ntp.delay_ns, lang),
                })
              : t("system.clockNtpSilent", { server: c.ntp.server ?? "–" }),
          ],
          c.olt && [
            t("system.clockOlt"),
            c.olt.utc_s != null
              ? t("system.clockOltGave", { date: when(c.olt.utc_s) })
              : t("system.clockOltNone"),
          ],
        ]}
      />
      <Hint>{t("system.clockHint")}</Hint>
    </Card>
  );
}

async function report() {
  const ids = (await request<{ items: { id: string }[] }>("/api/schema")).items.map((i) => i.id);
  const values: Record<string, unknown> = {};
  for (const id of ids) {
    try {
      values[id] = (await request<{ value: unknown }>(`/api/get/${id}`)).value;
    } catch (e) {
      values[id] = { error: String(e) };
    }
  }
  const blob = new Blob([JSON.stringify({ taken: new Date().toISOString(), values }, null, 2)], {
    type: "application/json",
  });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = `stick-report-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, "-")}.json`;
  a.click();
  URL.revokeObjectURL(a.href);
}

/* A report of every figure, for support, and a restart, which asks first. */
function Maintenance() {
  const { t } = useI18n();
  const restart = useRestart();
  const confirm = useConfirm();
  const [busy, setBusy] = useState(false);
  /* Why the last report could not be made, or null. */
  const [failed, setFailed] = useState<unknown>(null);
  return (
    <Card title={t("system.maintenance")}>
      <div className="grid gap-1">
        <button
          type="button"
          className="btn btn-sm w-fit"
          disabled={busy}
          onClick={async () => {
            setBusy(true);
            setFailed(null);
            try {
              await report();
            } catch (e) {
              setFailed(e);
            } finally {
              setBusy(false);
            }
          }}
        >
          {busy ? (
            <span className="loading loading-spinner loading-xs" />
          ) : (
            <Download className="size-4" />
          )}
          {t("system.report")}
        </button>
        <Hint>{t("system.reportHint")}</Hint>
        {failed != null && (
          <p role="alert" className="text-sm text-error">
            {t("system.notReported", { reason: reason(t, failed) })}
          </p>
        )}
      </div>
      <div className="grid gap-1">
        <button
          type="button"
          className="btn btn-sm btn-error btn-outline w-fit"
          disabled={restart.isPending || restart.isSuccess}
          onClick={() =>
            confirm.open({
              title: t("system.restart"),
              body: t("system.restartConfirm"),
              action: t("system.restartAction"),
              danger: true,
              password: false,
              run: () => restart.mutateAsync(),
            })
          }
        >
          <RotateCw className="size-4" />
          {t("system.restart")}
        </button>
        {restart.isSuccess && <SaveState state="ok" message={t("system.restarting")} />}
      </div>
      {confirm.dialog}
    </Card>
  );
}

export function DevicePage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.device")} hint={t("system.deviceHint")}>
      <Columns>
        <Stack>
          <DeviceCard />
        </Stack>
        <Stack>
          <ClockCard />
          <Maintenance />
        </Stack>
      </Columns>
    </Page>
  );
}
