// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The stick itself, in four tabs. Resources: the processor, the memory and
 * the flash, as gauges, the last ten minutes of the first two, and a map of
 * the flash with what each partition holds. Software: the two systems of
 * the flash and the update of the one that does not run. Settings: the
 * stick's name and the name the panel shows for the system, its clock,
 * the connection to the panel, the password and the sign-out, a restart
 * and a report of every figure. Access: the keys of the ssh server and of
 * the panel's HTTPS (system/Access.tsx).
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { useQueryClient } from "@tanstack/react-query";
import { Link, useNavigate } from "@tanstack/react-router";
import {
  Check,
  CircleAlert,
  Download,
  HardDrive,
  KeyRound,
  LogOut,
  Play,
  RotateCw,
  Undo2,
  Upload,
} from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { useForm } from "react-hook-form";
import { useSignOut } from "../api/auth";
import { ApiError, request } from "../api/client";
import {
  downloadFlash,
  refresh,
  uploadFirmware,
  useBanks,
  useClock,
  useFirmware,
  useHostnameChange,
  useProductChange,
  useRestart,
  useReturnBank,
  useTryBank,
} from "../api/config";
import { useLive, useTraffic } from "../api/live";
import type { Banks, Firmware } from "../api/types";
import { Field, SaveState, useChangeState } from "../components/Field";
import { Page, Tabs, useTab } from "../components/Page";
import { QueryNote, QueryState } from "../components/Read";
import { HistoryNote, Sparkline } from "../components/TrafficChart";
import { Badge, Card, Facts, Gauge, Meter, Stat } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { clockState, offsetText, ppmText, sourceKey, spanText, stale } from "../lib/clock";
import {
  type BankTag,
  bankLetter,
  bankViews,
  otherBank,
  overShipped,
  shippedBank,
  updateTarget,
  uploading,
} from "../lib/firmware";
import { bytes, day, decimal, duration, percent } from "../lib/format";
import { DEFAULT_PRODUCT, PRODUCT_MAX, productField, productSlug } from "../lib/product";
import { reason } from "../lib/read";
import { z } from "../lib/schemas";
import { memory, type Tone } from "../lib/state";
import { AccessTab } from "./system/Access";

const TABS = ["resources", "software", "settings", "access"] as const;

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
  shipped: ["system.tagShipped", "warn"],
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
                <span className="ml-1 font-mono text-xs text-base-content/70">{p.name}</span>
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
        <QueryState reads={banks}>
          <QueryNote reads={fw} aside />
          <FlashMap parts={parts} />
        </QueryState>
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
  const returnBank = useReturnBank();
  const input = useRef<HTMLInputElement>(null);
  /* The copy of the whole flash: under way, or what came of it. */
  const [copy, setCopy] = useState<"idle" | "busy" | "done" | string>("idle");
  const [phase, setPhase] = useState<Phase>("idle");
  const [share, setShare] = useState(0);
  const [result, setResult] = useState("");
  const [startAfter, setStartAfter] = useState(false);
  /* Root's password, which writing and starting a system need again. */
  const [password, setPassword] = useState("");
  const f = firmware.data;
  const target = f ? updateTarget(f) : null;
  const other = f ? otherBank(f) : null;
  const shipped = f ? shippedBank(f) : null;
  const elsewhere = uploading(f) && phase !== "sending" && phase !== "writing";

  async function send(file: File) {
    setPhase("sending");
    setShare(0);
    let ok = false;
    try {
      const version = await uploadFirmware(file, password, (sent, total) => {
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
      } else if (e instanceof ApiError && e.status === 403) {
        setResult(t("system.wrongPassword"));
        setPhase("failed");
      } else {
        setResult(now?.message ?? (e instanceof ApiError ? (e.detail ?? e.message) : String(e)));
        setPhase("failed");
      }
    } finally {
      void refresh(qc, "upload");
      if (input.current) input.current.value = "";
    }
    if (ok && startAfter && target !== null) tryBank.mutate({ bank: target, secret: password });
  }

  function start(bank: number) {
    const back = f?.running ?? 0;
    if (
      !window.confirm(
        t("system.switchConfirm", { letter: bankLetter(bank), back: bankLetter(back) }),
      )
    )
      return;
    tryBank.mutate({ bank, secret: password });
  }

  function goBack(bank: number) {
    if (!window.confirm(t("system.returnConfirm", { letter: bankLetter(bank) }))) return;
    returnBank.mutate({ bank, secret: password });
  }

  async function copyFlash() {
    setCopy("busy");
    try {
      await downloadFlash(password);
      setCopy("done");
    } catch (e) {
      setCopy(
        e instanceof ApiError && e.status === 403
          ? t("system.wrongPassword")
          : e instanceof ApiError
            ? (e.detail ?? e.message)
            : String(e),
      );
    }
  }

  function pickFile() {
    if (f && overShipped(f, target) && target !== null) {
      if (!window.confirm(t("system.overShippedConfirm", { letter: bankLetter(target) }))) return;
    }
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
        <QueryNote reads={firmware} />
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
                <div className="flex flex-wrap gap-x-4 text-xs text-base-content/70">
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
          <p className="flex items-start gap-2 text-sm">
            <CircleAlert className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
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
        {elsewhere && <p className="text-sm">{t("system.busy")}</p>}

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
          <p className="flex items-start gap-2 text-sm">
            <Check className="mt-0.5 size-4 shrink-0 text-success" aria-hidden />
            {t("system.written", { letter: bankLetter(target), version: result })}
          </p>
        )}
        {phase === "failed" && (
          <p className="text-sm text-error">{t("system.notWritten", { reason: result })}</p>
        )}
        {tryBank.isSuccess && tryBank.variables?.bank !== undefined && (
          <p className="text-sm">
            {t("system.switching", { letter: bankLetter(tryBank.variables.bank) })}
          </p>
        )}
        {returnBank.isSuccess && <p className="text-sm">{t("system.returning")}</p>}
        {returnBank.isError && (
          <p className="text-sm text-error">
            {returnBank.error instanceof ApiError && returnBank.error.status === 403
              ? t("system.wrongPassword")
              : returnBank.error instanceof ApiError
                ? (returnBank.error.detail ?? returnBank.error.message)
                : String(returnBank.error)}
          </p>
        )}
        {copy === "busy" && (
          <p className="flex items-center gap-2 text-sm">
            <span className="loading loading-spinner loading-xs" />
            {t("system.copying")}
          </p>
        )}
        {copy === "done" && (
          <p className="flex items-start gap-2 text-sm">
            <Check className="mt-0.5 size-4 shrink-0 text-success" aria-hidden />
            {t("system.copied")}
          </p>
        )}
        {copy !== "idle" && copy !== "busy" && copy !== "done" && (
          <p className="text-sm text-error">{t("system.notCopied", { reason: copy })}</p>
        )}
        {tryBank.isError && (
          <p className="text-sm text-error">
            {tryBank.error instanceof ApiError && tryBank.error.status === 403
              ? t("system.wrongPassword")
              : tryBank.error instanceof ApiError
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
            <span className="block text-xs text-base-content/70">{t("system.startAfterHint")}</span>
          </span>
        </label>
        <label className="grid gap-1">
          <span className="text-sm">{t("system.password")}</span>
          <input
            type="password"
            className="input input-sm w-full max-w-xs"
            autoComplete="current-password"
            value={password}
            onChange={(e) => setPassword(e.currentTarget.value)}
          />
          <span className="text-xs text-base-content/70">{t("system.passwordHint")}</span>
        </label>
        <div className="flex flex-wrap gap-2">
          <button
            type="button"
            className="btn btn-sm btn-primary"
            disabled={
              target === null ||
              !password ||
              phase === "sending" ||
              phase === "writing" ||
              elsewhere
            }
            onClick={pickFile}
          >
            <Upload className="size-4" />
            {t("system.upload")}
          </button>
          {phase === "done" && target !== null && !startAfter && (
            <button
              type="button"
              className="btn btn-sm btn-primary"
              disabled={!password || tryBank.isPending || tryBank.isSuccess}
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
              disabled={!password || tryBank.isPending || tryBank.isSuccess || elsewhere}
              onClick={() => start(other)}
            >
              <RotateCw className="size-4" />
              {t("system.switchTo", { letter: bankLetter(other) })}
            </button>
          )}
          <button
            type="button"
            className="btn btn-sm"
            disabled={!password || copy === "busy" || elsewhere}
            onClick={copyFlash}
          >
            <HardDrive className="size-4" />
            {t("system.copyFlash")}
          </button>
          {shipped !== null && (
            <button
              type="button"
              className="btn btn-sm btn-outline"
              disabled={!password || returnBank.isPending || returnBank.isSuccess || elsewhere}
              onClick={() => goBack(shipped)}
            >
              <Undo2 className="size-4 text-warning" />
              {t("system.returnTo", { letter: bankLetter(shipped) })}
            </button>
          )}
        </div>
        <p className="text-xs text-base-content/70">{t("system.copyFlashHint")}</p>
        {target !== null && f && overShipped(f, target) ? (
          <p className="flex items-start gap-1.5 text-xs">
            <CircleAlert className="mt-px size-3.5 shrink-0 text-warning" aria-hidden />
            {t("system.uploadHintShipped", { letter: bankLetter(target) })}
          </p>
        ) : (
          target !== null && (
            <p className="text-xs text-base-content/70">
              {t("system.uploadHint", { letter: bankLetter(target) })}
            </p>
          )
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

/*
 * The connection to the panel: the certificate, its fingerprint and its key,
 * and the ssh server's keys, are under Access.
 */
function Certificate() {
  const { t } = useI18n();
  const secure = window.location.protocol === "https:";
  return (
    <>
      <Facts
        rows={[
          [
            t("system.connection"),
            <Badge key="tls" tone={secure ? "good" : "warn"}>
              {t(secure ? "system.connectionHttps" : "system.connectionHttp")}
            </Badge>,
          ],
        ]}
      />
      <p className="text-xs text-base-content/70">{t("system.accessHint")}</p>
    </>
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
      <p className="text-xs text-base-content/70">{t("system.clockHint")}</p>
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

function SettingsTab() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const banks = useBanks();
  const restart = useRestart();
  const signOut = useSignOut();
  const navigate = useNavigate();
  const [busy, setBusy] = useState(false);
  /* Why the last report could not be made, or null. */
  const [reportFailed, setReportFailed] = useState<unknown>(null);
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
        <QueryNote reads={banks} aside />
        {live?.hostname != null ? (
          <HostnameForm name={live.hostname} />
        ) : (
          <p className="text-xs text-base-content/70">{t("system.hostnameUnavailable")}</p>
        )}
        {live?.product != null ? (
          <ProductForm name={live.product} />
        ) : (
          <p className="text-xs text-base-content/70">{t("system.productUnavailable")}</p>
        )}
      </Card>

      <ClockCard />

      <Card title={t("system.security")}>
        <Certificate />
        <div className="flex flex-wrap gap-2">
          <Link to="/password" className="btn btn-sm">
            <KeyRound className="size-4" />
            {t("nav.changePassword")}
          </Link>
          <button
            type="button"
            className="btn btn-sm"
            disabled={signOut.isPending}
            onClick={() =>
              signOut.mutate(undefined, { onSettled: () => navigate({ to: "/login" }) })
            }
          >
            <LogOut className="size-4" />
            {t("nav.signOut")}
          </button>
        </div>
        <p className="text-xs text-base-content/70">{t("system.sessionsHint")}</p>
      </Card>

      <Card title={t("system.maintenance")}>
        <div className="flex flex-wrap gap-2">
          <button
            type="button"
            className="btn btn-sm"
            disabled={busy}
            onClick={async () => {
              setBusy(true);
              setReportFailed(null);
              try {
                await report();
              } catch (e) {
                setReportFailed(e);
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
        <p className="text-xs text-base-content/70">{t("system.reportHint")}</p>
        {reportFailed != null && (
          <p role="alert" className="text-sm text-error">
            {t("system.notReported", { reason: reason(t, reportFailed) })}
          </p>
        )}
        {restart.isSuccess && <p className="text-sm">{t("system.restarting")}</p>}
        {restart.isError && (
          <p role="alert" className="text-sm text-error">
            {t("internet.failed", { reason: reason(t, restart.error) })}
          </p>
        )}
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
            { id: "access", label: t("system.tabAccess") },
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
      {tab === "access" && <AccessTab />}
    </Page>
  );
}
