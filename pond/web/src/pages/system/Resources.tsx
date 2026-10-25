// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * System, Resources: the processor, the memory and the flash, as gauges, the
 * last ten minutes of the first two, and a map of the flash with what each
 * partition holds. It is the one place of the flash's partitions and of how
 * full each system is.
 */

import { useBanks, useFirmware } from "../../api/config";
import { useLive, useTraffic } from "../../api/live";
import type { Banks, Firmware } from "../../api/types";
import { Page } from "../../components/Page";
import { QueryNote, QueryState } from "../../components/Read";
import { HistoryNote, Sparkline } from "../../components/TrafficChart";
import { Card, Gauge, Stat } from "../../components/ui";
import { type Key, useI18n } from "../../i18n";
import { bankLetter } from "../../lib/firmware";
import { bytes, decimal, percent } from "../../lib/format";
import { memory, type Tone, thermal } from "../../lib/state";

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

const shareTone = (v: number | null): Tone =>
  v == null ? "neutral" : v > 0.85 ? "bad" : v > 0.65 ? "warn" : "info";

interface PartUse {
  name: string;
  size: number;
  /** What it holds, when known: the images of a system, everything of the rest. */
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

export function ResourcesPage() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const traffic = useTraffic();
  const now = traffic.at(-1);
  const banks = useBanks();
  const fw = useFirmware();
  const mem = memory(live);
  const parts = partitions(banks.data, fw.data);
  const total = parts.reduce((a, p) => a + p.size, 0);
  /* The running system's flash: its kernel and its files. */
  const running = fw.data?.running;
  const sys = parts.filter(
    (p) => running != null && (p.name === `k${running}` || p.name === `r${running}`),
  );
  const sysSize = sys.reduce((a, p) => a + p.size, 0);
  const sysUsed = sys.every((p) => p.used != null)
    ? sys.reduce((a, p) => a + (p.used ?? 0), 0)
    : null;
  const cpu = live?.cpu;
  const th = thermal(live);
  const m = live?.memory;
  const cache = m?.cached_kb != null ? (m.cached_kb + (m.buffers_kb ?? 0)) * 1024 : null;

  return (
    <Page title={t("nav.resources")} hint={t("system.resourcesHint")}>
      <div className="grid min-h-0 flex-1 gap-4 lg:grid-rows-[auto_minmax(0,1fr)]">
        <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-4">
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
                  sub={
                    mem ? t("system.ofTotal", { total: bytes(mem.totalBytes, lang) }) : undefined
                  }
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
          <Card title={t("system.temperature")} action={<HistoryNote />}>
            <div className="grid grid-cols-[auto_minmax(0,1fr)] items-center gap-4">
              <Gauge
                share={th?.share ?? null}
                value={th?.dieC != null ? `${decimal(th.dieC, 0, lang)} °C` : "–"}
                label={t("system.tempChip")}
                tone={th?.tone ?? "neutral"}
              />
              <div className="grid gap-2">
                <Stat
                  label={t("system.tempModule")}
                  value={th?.moduleC != null ? `${decimal(th.moduleC, 1, lang)} °C` : "–"}
                  sub={t("system.tempModuleHint")}
                />
                <Stat
                  label={t("system.tempLevels")}
                  value={
                    th?.warnC != null && th.critC != null
                      ? `${decimal(th.warnC, 0, lang)} / ${decimal(th.critC, 0, lang)} °C`
                      : "–"
                  }
                  sub={th?.warnC != null ? t("system.tempLevelsHint") : t("system.tempNoLevels")}
                />
              </div>
            </div>
            <div className="h-20">
              <Sparkline
                data={traffic}
                value={(r) => r.dieC}
                min={30}
                max={th?.critC ?? 110}
                format={(v) => `${decimal(v, 0, lang)} °C`}
                tone="var(--color-warning)"
                label={t("system.tempChip")}
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
    </Page>
  );
}
