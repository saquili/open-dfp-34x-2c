// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The first page: is it working, how the traffic flows, how much of it. */

import { Link } from "@tanstack/react-router";
import { Activity, ChevronRight, Cpu, EthernetPort, Globe } from "lucide-react";
import type { ReactNode } from "react";
import { useLive, useTraffic } from "../api/live";
import { Diagram } from "../components/Diagram";
import { Page } from "../components/Page";
import { HistoryNote, TrafficChart } from "../components/TrafficChart";
import { Badge, Card, Figure } from "../components/ui";
import { useI18n } from "../i18n";
import { bitrate, dbmText, duration, integer, percent } from "../lib/format";
import {
  fibre,
  hostLink,
  LIGHT_TONE,
  laneMode,
  linkSpeed,
  memory,
  session,
  type Tone,
} from "../lib/state";

function Tile({
  to,
  icon,
  title,
  badge,
  children,
}: {
  to: string;
  icon: ReactNode;
  title: string;
  badge?: ReactNode;
  children: ReactNode;
}) {
  return (
    <Link
      to={to}
      className="card group border border-base-300 bg-base-100 transition-colors hover:border-primary focus-visible:outline-2 focus-visible:outline-primary"
    >
      <div className="card-body gap-1.5 p-4">
        <div className="flex items-center gap-2 text-sm font-medium text-base-content/70">
          {icon}
          <span className="flex-1">{title}</span>
          {badge}
          <ChevronRight className="size-4 opacity-0 transition-opacity group-hover:opacity-60" />
        </div>
        {children}
      </div>
    </Link>
  );
}

/** A thin bar for a share, with its words on one line. */
function Bar({ label, share, tone }: { label: string; share: number | null; tone: Tone }) {
  const { lang } = useI18n();
  const cls = {
    good: "bg-success",
    warn: "bg-warning",
    bad: "bg-error",
    info: "bg-info",
    neutral: "bg-base-content/40",
  }[tone];
  return (
    <div className="grid grid-cols-[3.5rem_minmax(0,1fr)_2.75rem] items-center gap-2 text-xs">
      <span className="text-base-content/65">{label}</span>
      <span className="h-1.5 overflow-hidden rounded-full bg-base-300">
        <span
          className={`block h-full rounded-full transition-[width] duration-700 ${cls}`}
          style={{ width: `${Math.round((share ?? 0) * 100)}%` }}
        />
      </span>
      <span className="tabular text-right">{share == null ? "–" : percent(share, lang)}</span>
    </div>
  );
}

const shareTone = (v: number | null): Tone =>
  v == null ? "neutral" : v > 0.85 ? "bad" : v > 0.65 ? "warn" : "info";

export function Overview() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const traffic = useTraffic();
  const now = traffic.at(-1);

  const f = fibre(live);
  const s = session(live);
  const link = hostLink(live);
  const mem = memory(live);
  const lane = laneMode(live);

  const hardware = now && now.wanDown + now.wanUp > 20e6 && now.cpuFrames < 200;
  const netTone: Tone = s.configured ? (s.up ? "good" : "warn") : f.connected ? "info" : "bad";

  return (
    <Page title={t("overview.title")}>
      <div className="grid min-h-0 flex-1 gap-4 lg:grid-rows-[auto_auto_minmax(11rem,1fr)]">
        <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
          <Tile
            to="/fibre"
            icon={<Activity className="size-4" />}
            title={t("overview.fibre")}
            badge={<Badge tone={f.tone}>{t(`state.${f.stateKey}`)}</Badge>}
          >
            <Figure
              value={dbmText(f.rxDbm, lang).replace(" dBm", "")}
              unit="dBm"
              tone={LIGHT_TONE[f.quality]}
              caption={`${t("overview.received")} · ${t(`light.${f.quality}`)}`}
            />
          </Tile>

          <Tile
            to="/internet"
            icon={<Globe className="size-4" />}
            title={t("overview.internet")}
            badge={
              <Badge tone={netTone}>
                {s.configured ? (s.up ? t("internet.up") : t("internet.down")) : t("internet.off")}
              </Badge>
            }
          >
            {s.configured ? (
              <dl className="grid grid-cols-[2.75rem_minmax(0,1fr)] items-baseline gap-x-2 gap-y-0.5">
                <dt className="text-xs text-base-content/60">IPv4</dt>
                <dd className="tabular truncate text-lg font-semibold" title={s.address ?? ""}>
                  {s.address ?? "–"}
                </dd>
                <dt className="text-xs text-base-content/60">IPv6</dt>
                <dd className="tabular truncate text-sm font-semibold" title={s.prefix ?? ""}>
                  {s.prefix ?? "–"}
                </dd>
                <dd className="col-span-2 text-xs text-base-content/65">
                  {t("overview.handedTo")}
                </dd>
              </dl>
            ) : (
              <div>
                <div className="font-medium">{t("overview.passing")}</div>
                <div className="text-xs text-base-content/65">{t("overview.passingHint")}</div>
              </div>
            )}
          </Tile>

          <Tile
            to="/sfp"
            icon={<EthernetPort className="size-4" />}
            title={t("overview.sfp")}
            badge={
              <Badge tone={link.up ? "good" : "bad"}>{link.up ? t("sfp.up") : t("sfp.down")}</Badge>
            }
          >
            <Figure
              value={linkSpeed(link.speedMbit, lang) ?? "–"}
              caption={lane ? `${t("overview.linkSpeed")} · ${lane}` : t("overview.linkSpeed")}
            />
          </Tile>

          <Tile to="/system" icon={<Cpu className="size-4" />} title={t("overview.device")}>
            <Figure
              value={live ? duration(live.uptime.up_s, lang) : "–"}
              caption={t("overview.uptime")}
            />
            <div className="mt-1 grid gap-1">
              <Bar
                label={t("overview.cpu")}
                share={now?.cpuBusy ?? null}
                tone={shareTone(now?.cpuBusy ?? null)}
              />
              <Bar
                label={t("overview.ram")}
                share={mem?.share ?? null}
                tone={shareTone(mem?.share ?? null)}
              />
            </div>
          </Tile>
        </div>

        <Card
          title={t("diagram.title")}
          action={<span className="text-xs text-base-content/55">{t("diagram.hint")}</span>}
        >
          <Diagram live={live} rate={now} />
        </Card>

        <Card
          title={t("overview.traffic")}
          action={
            now &&
            (hardware ? (
              <span className="flex items-center gap-2">
                <span className="hidden text-xs text-base-content/60 sm:inline">
                  {t("overview.inHardwareHint", { frames: integer(now.cpuFrames, lang) })}
                </span>
                <Badge tone="good">{t("overview.inHardware")}</Badge>
              </span>
            ) : (
              <span className="text-xs text-base-content/60">
                {t("overview.viaCpu", { frames: integer(now.cpuFrames, lang) })}
              </span>
            ))
          }
        >
          <div className="grid min-h-0 flex-1 gap-4 sm:grid-cols-[11rem_minmax(0,1fr)]">
            <div className="grid content-start grid-cols-2 gap-3 sm:grid-cols-1">
              <Figure value={bitrate(now?.wanDown, lang)} caption={`↓ ${t("overview.download")}`} />
              <Figure value={bitrate(now?.wanUp, lang)} caption={`↑ ${t("overview.upload")}`} />
              <HistoryNote />
            </div>
            <TrafficChart data={traffic} down={(r) => r.wanDown} up={(r) => r.wanUp} fill />
          </div>
        </Card>
      </div>
    </Page>
  );
}
