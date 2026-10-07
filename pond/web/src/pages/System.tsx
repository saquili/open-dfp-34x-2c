// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The stick itself: what it runs and from where, its memory and flash, the
 * settings store, security, a restart, and a report of every figure for
 * support.
 */

import { useQueryClient } from "@tanstack/react-query";
import { Link } from "@tanstack/react-router";
import { Download, KeyRound, LogOut, Play, RotateCw, Upload } from "lucide-react";
import { useRef, useState } from "react";
import { useSignOut } from "../api/auth";
import { ApiError, request } from "../api/client";
import { uploadFirmware, useBanks, useFirmware, useRestart, useTryBank } from "../api/config";
import { useLive } from "../api/live";
import { Badge, Card, Facts, Meter, PageTitle } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import {
  type BankTag,
  bankLetter,
  bankViews,
  otherBank,
  updateTarget,
  uploading,
} from "../lib/firmware";
import { bytes, day, duration, percent } from "../lib/format";
import { memory, type Tone } from "../lib/state";

/* What each partition of the flash is, in words. */
const PARTITION: Record<string, Key> = {
  boot: "system.partBoot",
  env: "system.partEnv",
  env2: "system.partEnv",
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

type Phase = "idle" | "sending" | "writing" | "done" | "failed";

/*
 * The two systems of the flash, and the update of the one that does not run:
 * an update file goes up, the stick writes and checks it, and a start of the
 * new system follows when the person asks; the stick keeps it only when it
 * comes up.
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
  const f = firmware.data;
  const target = f ? updateTarget(f) : null;
  const other = f ? otherBank(f) : null;
  const elsewhere = uploading(f) && phase !== "sending" && phase !== "writing";

  async function send(file: File) {
    setPhase("sending");
    setShare(0);
    try {
      const version = await uploadFirmware(file, (sent, total) => {
        setShare(total ? sent / total : 0);
        if (total && sent >= total) setPhase("writing");
      });
      setResult(version);
      setPhase("done");
    } catch (e) {
      /* A cut connection: the stick's own account of the upload decides. */
      const now = (await firmware.refetch()).data?.update;
      if (now?.state === "done") {
        setResult(now.version ?? "");
        setPhase("done");
      } else {
        setResult(now?.message ?? (e instanceof ApiError ? (e.detail ?? e.message) : String(e)));
        setPhase("failed");
      }
    } finally {
      qc.invalidateQueries({ queryKey: ["firmware"] });
      if (input.current) input.current.value = "";
    }
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

  return (
    <Card title={t("system.updates")} className="lg:col-span-2">
      <ul className="grid gap-2">
        {(f ? bankViews(f) : []).map((b) => (
          <li key={b.bank} className="flex flex-wrap items-center justify-between gap-2">
            <span className="font-medium">
              {t("system.systemLetter", { letter: b.letter })}
              {b.version && <span className="ml-2 tabular text-base-content/70">{b.version}</span>}
              {b.time != null && (
                <span className="ml-2 text-xs text-base-content/60">
                  {t("system.builtOn", { date: day(b.time, lang) })}
                </span>
              )}
            </span>
            <span className="flex flex-wrap gap-1">
              {b.tags.map((tag) => (
                <Badge key={tag} tone={TAG[tag][1]}>
                  {t(TAG[tag][0])}
                </Badge>
              ))}
            </span>
          </li>
        ))}
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

      <div className="flex flex-wrap gap-2">
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
        <button
          type="button"
          className="btn btn-sm btn-primary"
          disabled={target === null || phase === "sending" || phase === "writing" || elsewhere}
          onClick={() => input.current?.click()}
        >
          <Upload className="size-4" />
          {t("system.upload")}
        </button>
        {phase === "done" && target !== null && (
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

export function System() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const banks = useBanks();
  const restart = useRestart();
  const signOut = useSignOut();
  const [busy, setBusy] = useState(false);
  const mem = memory(live);
  const store = live?.settings.store ? STORE[live.settings.store] : undefined;
  const bank = banks.data?.rootfs.bank;

  return (
    <>
      <PageTitle>{t("system.title")}</PageTitle>
      <div className="grid gap-4 lg:grid-cols-2">
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
        </Card>

        <Card title={t("system.memory")}>
          {mem && (
            <Meter
              share={mem.share}
              tone={mem.share > 0.85 ? "bad" : mem.share > 0.7 ? "warn" : "info"}
              label={t("system.memoryUse", {
                used: bytes(mem.usedBytes, lang),
                total: bytes(mem.totalBytes, lang),
              })}
            />
          )}
          <h3 className="mt-2 text-sm font-semibold">{t("system.storage")}</h3>
          <ul className="grid gap-1 text-sm">
            {(banks.data?.partitions ?? [])
              .filter((p) => p.name !== "env2")
              .map((p) => (
                <li key={p.name} className="flex items-center justify-between gap-3">
                  <span>
                    {PARTITION[p.name] ? t(PARTITION[p.name] as Key) : p.name}
                    {!p.writable && (
                      <span className="ml-2">
                        <Badge tone="neutral">{t("system.readOnly")}</Badge>
                      </span>
                    )}
                  </span>
                  <span className="tabular text-base-content/70">
                    {p.name === "env" ? `2 × ${bytes(p.size_b, lang)}` : bytes(p.size_b, lang)}
                  </span>
                </li>
              ))}
          </ul>
        </Card>

        <Updates />

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
    </>
  );
}
