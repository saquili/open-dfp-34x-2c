// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The stick itself: what it runs and from where, its memory and flash, the
 * settings store, security, a restart, and a report of every figure for
 * support.
 */

import { Link } from "@tanstack/react-router";
import { Download, KeyRound, LogOut, RotateCw } from "lucide-react";
import { useState } from "react";
import { useSignOut } from "../api/auth";
import { request } from "../api/client";
import { useBanks, useRestart } from "../api/config";
import { useLive } from "../api/live";
import { Badge, Card, Facts, Meter, PageTitle } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { bytes, duration } from "../lib/format";
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
                    {p.writable && (
                      <span className="ml-2">
                        <Badge tone="info">{t("system.writable")}</Badge>
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
