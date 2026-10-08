// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The stick itself, in three tabs. Resources: the processor, the memory and
 * the flash, as gauges, the last ten minutes of the first two, and a map of
 * the flash with what each partition holds. Software: the two systems of
 * the flash and the update of the one that does not run. Settings: the
 * stick's name, security, a restart and a report of every figure.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { useQueryClient } from "@tanstack/react-query";
import { Link } from "@tanstack/react-router";
import { Download, KeyRound, LogOut, Play, RotateCw, Upload } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { useForm } from "react-hook-form";
import { useSignOut } from "../api/auth";
import { ApiError, request } from "../api/client";
import {
  uploadFirmware,
  useBanks,
  useFirmware,
  useHostnameChange,
  useRestart,
  useTryBank,
} from "../api/config";
import { useLive, useTraffic } from "../api/live";
import type { Banks, Firmware } from "../api/types";
import { Field, SaveState, useChangeState } from "../components/Field";
import { Page, Tabs, useTab } from "../components/Page";
import { HistoryNote, Sparkline } from "../components/TrafficChart";
import { Badge, Card, Facts, Gauge, Meter, Stat } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import {
  type BankTag,
  bankLetter,
  bankViews,
  otherBank,
  updateTarget,
  uploading,
} from "../lib/firmware";
import { bytes, day, decimal, duration, percent } from "../lib/format";
import { z } from "../lib/schemas";
import { memory, type Tone } from "../lib/state";

const TABS = ["resources", "software", "settings"] as const;

/* What each partition of the flash is, in words. */
const PARTITION: Record<string, Key> = {
  boot: "system.partBoot",
  env: "system.partEnv",
  env2: "system.partEnv2",
  config: "system.partConfig",
  k0: "system.partKernelA",
  r0: "system.partSystemA",
  k1: "system.partKernelB",
  r1: "system.partSystemB",
};

const STORE: Record<string, [Key, Tone]> = {
  loaded: ["system.stored", "good"],
  saved: ["system.stored", "good"],
  defaults: ["system.defaults", "warn"],
  failed: ["system.storeFailed", "bad"],
  none: ["system.storeFailed", "bad"],
};

const TAG: Record<BankTag, [Key, Tone]> = {
  running: ["system.tagRunning", "good"],
  default: ["system.tagDefault", "info"],
  once: ["system.tagOnce", "warn"],
  trial: ["system.tagTrial", "warn"],
  failed: ["system.tagFailed", "bad"],
  empty: ["system.tagEmpty", "neutral"],
  incomplete: ["system.tagIncomplete", "bad"],
};

const shareTone = (v: number | null): Tone =>
  v == null ? "neutral" : v > 0.85 ? "bad" : v > 0.65 ? "warn" : "info";

/* ---------------------------------------------------------------- resources */

interface PartUse {
  name: string;
  size: number;
  /** What it holds, when known: the images of a bank, everything of the rest. */
  used: number | null;
}

/** Each partition, and what it holds as far as the stick says. */
function partitions(banks: Banks | undefined, fw: Firmware | undefined): PartUse[] {
  return (banks?.partitions ?? []).map((p) => {
    const bank = /^([kr])([01])$/.exec(p.name);
    let used: number | null = null;
    if (bank) {
      const b = fw?.banks.find((x) => x.bank === Number(bank[2]));
      used = (bank[1] === "k" ? b?.kernel_used_b : b?.rootfs_used_b) ?? null;
      if (b?.state === "empty") used = 0;
    }
    return { name: p.name, size: p.size_b, used };
  });
}

const PART_COLOR: Record<string, string> = {
  boot: "bg-neutral/70",
  env: "bg-neutral/50",
  env2: "bg-neutral/50",
  config: "bg-accent/70",
  k0: "bg-primary/80",
  r0: "bg-primary/60",
  k1: "bg-secondary/80",
  r1: "bg-secondary/60",
};

function FlashMap({ parts }: { parts: PartUse[] }) {
  const { t, lang } = useI18n();
  const total = parts.reduce((a, p) => a + p.size, 0) || 1;
  return (
    <div className="grid gap-3">
      <div className="flex h-8 w-full overflow-hidden rounded-box border border-base-300">
        {parts.map((p) => (
          <div
            key={p.name}
            title={`${t(PARTITION[p.name] ?? "system.unknown")} · ${bytes(p.size, lang)}`}
            className="relative h-full border-r border-base-100 last:border-r-0 bg-base-200"
            style={{ width: `${(p.size / total) * 100}%` }}
          >
            <div
              className={`absolute inset-y-0 left-0 ${PART_COLOR[p.name] ?? "bg-base-300"}`}
              style={{ width: `${p.used == null ? 100 : Math.min(100, (p.used / p.size) * 100)}%` }}
            />
          </div>
        ))}
      </div>
      <table className="table table-xs">
        <thead>
          <tr>
            <th>{t("system.partition")}</th>
            <th className="text-right">{t("system.partSize")}</th>
            <th className="text-right">{t("system.partUsed")}</th>
            <th className="text-right">{t("system.partFree")}</th>
          </tr>
        </thead>
        <tbody>
          {parts.map((p) => (
            <tr key={p.name}>
              <td>
                <span
                  className={`mr-2 inline-block size-2.5 rounded-sm ${PART_COLOR[p.name] ?? "bg-base-300"}`}
                />
                {t(PARTITION[p.name] ?? "system.unknown")}
                <span className="ml-1 font-mono text-xs text-base-content/50">{p.name}</span>
              </td>
              <td className="tabular text-right">{bytes(p.size, lang)}</td>
              <td className="tabular text-right">
                {p.used == null
                  ? "–"
                  : `${bytes(p.used, lang)} (${percent(p.used / p.size, lang)})`}
              </td>
              <td className="tabular text-right">
                {p.used == null ? "–" : bytes(p.size - p.used, lang)}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function ResourcesTab() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const traffic = useTraffic();
  const now = traffic.at(-1);
  const banks = useBanks();
  const fw = useFirmware();
  const mem = memory(live);
  const parts = partitions(banks.data, fw.data);
  const total = parts.reduce((a, p) => a + p.size, 0);
  /* The running system's flash: the bank it started from. */
  const running = fw.data?.running;
  const sys = parts.filter(
    (p) => running != null && (p.name === `k${running}` || p.name === `r${running}`),
  );
  const sysSize = sys.reduce((a, p) => a + p.size, 0);
  const sysUsed = sys.every((p) => p.used != null)
    ? sys.reduce((a, p) => a + (p.used ?? 0), 0)
    : null;
  const cpu = live?.cpu;
  const m = live?.memory;
  const cache = m?.cached_kb != null ? (m.cached_kb + (m.buffers_kb ?? 0)) * 1024 : null;

  return (
    <div className="grid min-h-0 flex-1 gap-4 lg:grid-rows-[auto_minmax(0,1fr)]">
      <div className="grid gap-4 md:grid-cols-3">
        <Card title={t("system.cpu")} action={<HistoryNote />}>
          <div className="grid grid-cols-[auto_minmax(0,1fr)] items-center gap-4">
            <Gauge
              share={now?.cpuBusy ?? null}
              value={now?.cpuBusy != null ? percent(now.cpuBusy, lang) : "–"}
              label={t("system.cpuBusy")}
              tone={shareTone(now?.cpuBusy ?? null)}
            />
            <div className="grid gap-2">
              <Stat
                label={t("system.cpuModel")}
                value={<span className="text-sm">{cpu?.model || "–"}</span>}
              />
              <Stat
                label={t("system.load")}
                value={cpu ? cpu.load.map((l) => decimal(l, 2, lang)).join(" · ") : "–"}
                sub={t("system.loadHint")}
              />
            </div>
          </div>
          <div className="h-20">
            <Sparkline
              data={traffic}
              value={(r) => (r.cpuBusy == null ? null : r.cpuBusy * 100)}
              min={0}
              max={100}
              format={(v) => `${decimal(v, 0, lang)} %`}
              label={t("system.cpuBusy")}
            />
          </div>
        </Card>
        <Card title={t("system.memory")} action={<HistoryNote />}>
          <div className="grid grid-cols-[auto_minmax(0,1fr)] items-center gap-4">
            <Gauge
              share={mem?.share ?? null}
              value={mem ? percent(mem.share, lang) : "–"}
              label={t("system.memInUse")}
              tone={shareTone(mem?.share ?? null)}
            />
            <div className="grid gap-2">
              <Stat
                label={t("system.memUsed")}
                value={mem ? bytes(mem.usedBytes, lang) : "–"}
                sub={mem ? t("system.ofTotal", { total: bytes(mem.totalBytes, lang) }) : undefined}
              />
              <Stat
                label={t("system.memCache")}
                value={cache != null ? bytes(cache, lang) : "–"}
                sub={t("system.memCacheHint")}
              />
            </div>
          </div>
          <div className="h-20">
            <Sparkline
              data={traffic}
              value={(r) => (r.memUsed == null ? null : r.memUsed * 100)}
              min={0}
              max={100}
              format={(v) => `${decimal(v, 0, lang)} %`}
              tone="var(--color-secondary)"
              label={t("system.memInUse")}
            />
          </div>
        </Card>
        <Card title={t("system.flash")}>
          <div className="grid grid-cols-[auto_minmax(0,1fr)] items-center gap-4">
            <Gauge
              share={sysUsed != null && sysSize ? sysUsed / sysSize : null}
              value={sysUsed != null && sysSize ? percent(sysUsed / sysSize, lang) : "–"}
              label={t("system.flashSystem")}
              sub={
                running != null
                  ? t("system.systemLetter", { letter: bankLetter(running) })
                  : undefined
              }
              tone={shareTone(sysUsed != null && sysSize ? sysUsed / sysSize : null)}
            />
            <div className="grid gap-2">
              <Stat
                label={t("system.flashTotal")}
                value={total ? bytes(total, lang) : "–"}
                sub="SPI NOR"
              />
              <Stat
                label={t("system.flashSystemFree")}
                value={sysUsed != null ? bytes(sysSize - sysUsed, lang) : "–"}
                sub={t("system.flashSystemFreeHint")}
              />
            </div>
          </div>
        </Card>
      </div>
      <Card title={t("system.flashMap")} scroll>
        <FlashMap parts={parts} />
      </Card>
    </div>
  );
}

/* ----------------------------------------------------------------- software */

type Phase = "idle" | "sending" | "writing" | "done" | "failed";

/*
 * The two systems of the flash, and the update of the one that does not run:
 * an update file goes up, the stick writes and checks it, and a start of the
 * new system follows, at once when asked for beforehand or when the person
 * asks; the stick keeps it only when it comes up.
 */
function Updates() {
  const { t, lang } = useI18n();
  const qc = useQueryClient();
  const firmware = useFirmware();
  const tryBank = useTryBank();
  const input = useRef<HTMLInputElement>(null);
  const [phase, setPhase] = useState<Phase>("idle");
  const [share, setShare] = useState(0);
  const [result, setResult] = useState("");
  const [startAfter, setStartAfter] = useState(false);
  const f = firmware.data;
  const target = f ? updateTarget(f) : null;
  const other = f ? otherBank(f) : null;
  const elsewhere = uploading(f) && phase !== "sending" && phase !== "writing";

  async function send(file: File) {
    setPhase("sending");
    setShare(0);
    let ok = false;
    try {
      const version = await uploadFirmware(file, (sent, total) => {
        setShare(total ? sent / total : 0);
        if (total && sent >= total) setPhase("writing");
      });
      setResult(version);
      setPhase("done");
      ok = true;
    } catch (e) {
      /* A cut connection: the stick's own account of the upload decides. */
      const now = (await firmware.refetch()).data?.update;
      if (now?.state === "done") {
        setResult(now.version ?? "");
        setPhase("done");
        ok = true;
      } else {
        setResult(now?.message ?? (e instanceof ApiError ? (e.detail ?? e.message) : String(e)));
        setPhase("failed");
      }
    } finally {
      qc.invalidateQueries({ queryKey: ["firmware"] });
      if (input.current) input.current.value = "";
    }
    if (ok && startAfter && target !== null) tryBank.mutate(target);
  }

  function start(bank: number) {
    const back = f?.running ?? 0;
    if (
      !window.confirm(
        t("system.switchConfirm", { letter: bankLetter(bank), back: bankLetter(back) }),
      )
    )
      return;
    tryBank.mutate(bank);
  }

  function pickFile() {
    if (startAfter && target !== null) {
      const back = f?.running ?? 0;
      if (
        !window.confirm(
          t("system.uploadStartConfirm", { letter: bankLetter(target), back: bankLetter(back) }),
        )
      )
        return;
    }
    input.current?.click();
  }

  return (
    <div className="grid min-h-0 flex-1 content-start gap-4 lg:grid-cols-[minmax(0,3fr)_minmax(0,2fr)]">
      <Card title={t("system.updates")}>
        <ul className="grid gap-2">
          {(f ? bankViews(f) : []).map((b) => {
            const bank = f?.banks.find((x) => x.bank === b.bank);
            const used = (bank?.kernel_used_b ?? 0) + (bank?.rootfs_used_b ?? 0);
            const size = (bank?.kernel_size_b ?? 0) + (bank?.rootfs_size_b ?? 0);
            return (
              <li key={b.bank} className="grid gap-1 rounded-box border border-base-300 p-3">
                <div className="flex flex-wrap items-center justify-between gap-2">
                  <span className="font-medium">
                    {t("system.systemLetter", { letter: b.letter })}
                    {b.version && (
                      <span className="ml-2 tabular text-base-content/70">{b.version}</span>
                    )}
                  </span>
                  <span className="flex flex-wrap gap-1">
                    {b.tags.map((tag) => (
                      <Badge key={tag} tone={TAG[tag][1]}>
                        {t(TAG[tag][0])}
                      </Badge>
                    ))}
                  </span>
                </div>
                <div className="flex flex-wrap gap-x-4 text-xs text-base-content/60">
                  {b.time != null && (
                    <span>{t("system.builtOn", { date: day(b.time, lang) })}</span>
                  )}
                  {used > 0 && size > 0 && (
                    <span>
                      {t("system.bankUse", { used: bytes(used, lang), size: bytes(size, lang) })}
                    </span>
                  )}
                </div>
              </li>
            );
          })}
        </ul>

        {f?.trial != null && (
          <p className="text-sm text-warning">
            {t("system.trialNow", { letter: bankLetter(f.trial) })}
          </p>
        )}
        {f?.failed != null && (
          <p className="text-sm text-error">
            {t("system.failedNote", { letter: bankLetter(f.failed) })}
          </p>
        )}
        {f && target === null && (
          <p className="text-sm text-base-content/70">{t("system.fromMemory")}</p>
        )}
        {elsewhere && <p className="text-sm text-info">{t("system.busy")}</p>}

        {phase === "sending" && (
          <Meter share={share} label={t("system.sending", { percent: percent(share, lang) })} />
        )}
        {phase === "writing" && (
          <p className="flex items-center gap-2 text-sm">
            <span className="loading loading-spinner loading-xs" />
            {t("system.writing")}
          </p>
        )}
        {phase === "done" && target !== null && (
          <p className="text-sm text-success">
            {t("system.written", { letter: bankLetter(target), version: result })}
          </p>
        )}
        {phase === "failed" && (
          <p className="text-sm text-error">{t("system.notWritten", { reason: result })}</p>
        )}
        {tryBank.isSuccess && tryBank.variables !== undefined && (
          <p className="text-sm text-info">
            {t("system.switching", { letter: bankLetter(tryBank.variables) })}
          </p>
        )}
        {tryBank.isError && (
          <p className="text-sm text-error">
            {tryBank.error instanceof ApiError
              ? (tryBank.error.detail ?? tryBank.error.message)
              : String(tryBank.error)}
          </p>
        )}

        <input
          ref={input}
          type="file"
          accept=".bin,application/octet-stream"
          className="hidden"
          onChange={(e) => {
            const file = e.currentTarget.files?.[0];
            if (file) send(file);
          }}
        />
        <label className="label cursor-pointer justify-start gap-3 whitespace-normal">
          <input
            type="checkbox"
            className="toggle toggle-sm"
            checked={startAfter}
            disabled={target === null || phase === "sending" || phase === "writing"}
            onChange={(e) => setStartAfter(e.currentTarget.checked)}
          />
          <span>
            <span className="text-sm">{t("system.startAfter")}</span>
            <span className="block text-xs text-base-content/60">{t("system.startAfterHint")}</span>
          </span>
        </label>
        <div className="flex flex-wrap gap-2">
          <button
            type="button"
            className="btn btn-sm btn-primary"
            disabled={target === null || phase === "sending" || phase === "writing" || elsewhere}
            onClick={pickFile}
          >
            <Upload className="size-4" />
            {t("system.upload")}
          </button>
          {phase === "done" && target !== null && !startAfter && (
            <button
              type="button"
              className="btn btn-sm btn-success"
              disabled={tryBank.isPending || tryBank.isSuccess}
              onClick={() => start(target)}
            >
              <Play className="size-4" />
              {t("system.startNew")}
            </button>
          )}
          <a className="btn btn-sm" href="/api/firmware/download" download>
            <Download className="size-4" />
            {t("system.download")}
          </a>
          {other !== null && phase !== "done" && (
            <button
              type="button"
              className="btn btn-sm btn-outline"
              disabled={tryBank.isPending || tryBank.isSuccess || elsewhere}
              onClick={() => start(other)}
            >
              <RotateCw className="size-4" />
              {t("system.switchTo", { letter: bankLetter(other) })}
            </button>
          )}
        </div>
        {target !== null && (
          <p className="text-xs text-base-content/60">
            {t("system.uploadHint", { letter: bankLetter(target) })}
          </p>
        )}
      </Card>
      <Card title={t("system.howUpdate")}>
        <ol className="grid list-decimal gap-2 pl-5 text-sm">
          <li>{t("system.step1")}</li>
          <li>{t("system.step2")}</li>
          <li>{t("system.step3")}</li>
          <li>{t("system.step4")}</li>
        </ol>
      </Card>
    </div>
  );
}

/* ----------------------------------------------------------------- settings */

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

function SettingsTab() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const banks = useBanks();
  const restart = useRestart();
  const signOut = useSignOut();
  const [busy, setBusy] = useState(false);
  const store = live?.settings.store ? STORE[live.settings.store] : undefined;
  const bank = banks.data?.rootfs.bank;

  return (
    <div className="grid min-h-0 flex-1 content-start gap-4 lg:grid-cols-3">
      <Card title={t("system.device")}>
        <Facts
          rows={[
            [t("system.model"), live?.model ?? "–"],
            [t("system.software"), live?.image.version || live?.image.build || "–"],
            [
              t("system.runningFrom"),
              bank == null ? "–" : t("system.bank", { bank: bank === 0 ? "A" : "B" }),
            ],
            [t("system.uptime"), live ? duration(live.uptime.up_s, lang) : "–"],
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
          <p className="text-xs text-base-content/60">{t("system.hostnameUnavailable")}</p>
        )}
      </Card>

      <Card title={t("system.security")}>
        <div className="flex flex-wrap gap-2">
          <Link to="/password" className="btn btn-sm">
            <KeyRound className="size-4" />
            {t("nav.changePassword")}
          </Link>
          <button type="button" className="btn btn-sm" onClick={() => signOut.mutate()}>
            <LogOut className="size-4" />
            {t("nav.signOut")}
          </button>
        </div>
        <p className="text-xs text-base-content/60">{t("system.sessionsHint")}</p>
      </Card>

      <Card title={t("system.maintenance")}>
        <div className="flex flex-wrap gap-2">
          <button
            type="button"
            className="btn btn-sm"
            disabled={busy}
            onClick={async () => {
              setBusy(true);
              try {
                await report();
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
          <button
            type="button"
            className="btn btn-sm btn-error btn-outline"
            disabled={restart.isPending}
            onClick={() => window.confirm(t("system.restartConfirm")) && restart.mutate()}
          >
            <RotateCw className="size-4" />
            {t("system.restart")}
          </button>
        </div>
        <p className="text-xs text-base-content/60">{t("system.reportHint")}</p>
        {restart.isSuccess && <p className="text-sm text-info">{t("system.restarting")}</p>}
      </Card>
    </div>
  );
}

export function System() {
  const { t } = useI18n();
  const [tab, setTab] = useTab(TABS);
  return (
    <Page
      title={t("system.title")}
      tabs={
        <Tabs
          items={[
            { id: "resources", label: t("system.tabResources") },
            { id: "software", label: t("system.tabSoftware") },
            { id: "settings", label: t("system.tabSettings") },
          ]}
          value={tab}
          onChange={setTab}
          label={t("system.title")}
        />
      }
    >
      {tab === "resources" && <ResourcesTab />}
      {tab === "software" && <Updates />}
      {tab === "settings" && <SettingsTab />}
    </Page>
  );
}
