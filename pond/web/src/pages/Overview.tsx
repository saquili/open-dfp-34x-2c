// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The first page: is it working, how the traffic flows, how much of it. */

import { Link } from "@tanstack/react-router";
import { Activity, ChevronRight, Cpu, Globe, Router } from "lucide-react";
import type { ReactNode } from "react";
import { useLive, useTraffic } from "../api/live";
import { Diagram } from "../components/Diagram";
import { TrafficChart } from "../components/TrafficChart";
import { Badge, Card, Figure, PageTitle } from "../components/ui";
import { useI18n } from "../i18n";
import { bitrate, dbmText, duration, integer } from "../lib/format";
import { fibre, LIGHT_TONE, linkSpeed, memory, routerLink, session, type Tone } from "../lib/state";

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
      <div className="card-body gap-2 p-4">
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

export function Overview() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const traffic = useTraffic();
  const now = traffic.at(-1);

  const f = fibre(live);
  const s = session(live);
  const link = routerLink(live);
  const mem = memory(live);

  const hardware = now && now.wanDown + now.wanUp > 20e6 && now.cpuFrames < 200;
  const netTone: Tone = s.configured ? (s.up ? "good" : "warn") : f.connected ? "info" : "bad";

  return (
    <>
      <PageTitle>{t("overview.title")}</PageTitle>

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
            <Figure
              value={<span className="text-2xl">{s.address ?? "–"}</span>}
              caption={`${t("overview.publicAddress")} · ${t("overview.handedTo")}`}
            />
          ) : (
            <div>
              <div className="font-medium">{t("overview.passing")}</div>
              <div className="text-xs text-base-content/65">{t("overview.passingHint")}</div>
            </div>
          )}
        </Tile>

        <Tile
          to="/router"
          icon={<Router className="size-4" />}
          title={t("overview.router")}
          badge={
            <Badge tone={link.up ? "good" : "bad"}>
              {link.up ? t("router.up") : t("router.down")}
            </Badge>
          }
        >
          <Figure
            value={linkSpeed(link.speedMbit, lang) ?? "–"}
            caption={t("overview.linkSpeed")}
          />
        </Tile>

        <Tile to="/system" icon={<Cpu className="size-4" />} title={t("overview.device")}>
          <Figure
            value={live ? duration(live.uptime.up_s, lang) : "–"}
            caption={
              mem ? `${t("system.memory")}: ${Math.round(mem.share * 100)} %` : t("system.unknown")
            }
          />
        </Tile>
      </div>

      <Card title={t("diagram.title")} className="mt-4">
        <Diagram live={live} rate={now} />
      </Card>

      <Card
        title={t("overview.traffic")}
        className="mt-4"
        action={
          now &&
          (hardware ? (
            <Badge tone="good">{t("overview.inHardware")}</Badge>
          ) : (
            <span className="text-xs text-base-content/60">
              {t("overview.viaCpu", { frames: integer(now.cpuFrames, lang) })}
            </span>
          ))
        }
      >
        <div className="grid gap-4 sm:grid-cols-[12rem_minmax(0,1fr)]">
          <div className="grid grid-cols-2 gap-3 sm:grid-cols-1">
            <Figure value={bitrate(now?.wanDown, lang)} caption={`↓ ${t("overview.download")}`} />
            <Figure value={bitrate(now?.wanUp, lang)} caption={`↑ ${t("overview.upload")}`} />
          </div>
          <TrafficChart data={traffic} down={(r) => r.wanDown} up={(r) => r.wanUp} />
        </div>
        {hardware && now && (
          <p className="text-xs text-base-content/60">
            {t("overview.inHardwareHint", { frames: integer(now.cpuFrames, lang) })}
          </p>
        )}
      </Card>
    </>
  );
}
