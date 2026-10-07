// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The path of the traffic through the stick, drawn: the host, the SFP
 * port, the switch with the processor beside it, the GPON MAC with the
 * channels the OLT set up, the fibre and the OLT. A line grows with the
 * traffic it carries and is labelled with it; a part is coloured by its
 * state, and a click on it opens its page. Below a phone's width the drawing
 * turns upright, the operator at the top and the host at the bottom.
 */

import { useNavigate } from "@tanstack/react-router";
import {
  Cpu,
  Globe,
  Server as HostIcon,
  type LucideIcon,
  Network,
  Radio,
  ServerCog,
} from "lucide-react";
import { type KeyboardEvent, useEffect, useRef, useState } from "react";
import type { Rates } from "../api/live";
import type { Live } from "../api/types";
import { useI18n } from "../i18n";
import { bitrate, dbmText, integer } from "../lib/format";
import {
  fibre as fibreView,
  gems as gemViews,
  hostLink,
  LIGHT_TONE,
  linkSpeed,
  session as sessionView,
  type Tone,
} from "../lib/state";
import { useGemText } from "./Gem";

type Box = { x: number; y: number; w: number; h: number };
type Pt = { x: number; y: number };
type Seg = readonly [Pt, Pt];
type Part = "host" | "sfp" | "switch" | "cpu" | "ponmac" | "olt";

interface Layout {
  view: [number, number];
  box: Record<Part, Box>;
  gems: Box;
}

/* The gaps that carry labels (host-SFP, MAC-OLT) are wide enough for a rate. */
const WIDE: Layout = {
  view: [1100, 290],
  box: {
    host: { x: 8, y: 40, w: 150, h: 84 },
    sfp: { x: 278, y: 40, w: 140, h: 84 },
    switch: { x: 488, y: 40, w: 140, h: 84 },
    cpu: { x: 488, y: 196, w: 140, h: 84 },
    ponmac: { x: 698, y: 40, w: 140, h: 84 },
    olt: { x: 958, y: 40, w: 134, h: 84 },
  },
  gems: { x: 698, y: 150, w: 140, h: 26 },
};

const TALL: Layout = {
  view: [360, 760],
  box: {
    olt: { x: 20, y: 10, w: 190, h: 72 },
    ponmac: { x: 20, y: 172, w: 190, h: 72 },
    switch: { x: 20, y: 334, w: 190, h: 72 },
    cpu: { x: 228, y: 334, w: 122, h: 72 },
    sfp: { x: 20, y: 496, w: 190, h: 72 },
    host: { x: 20, y: 658, w: 190, h: 72 },
  },
  gems: { x: 228, y: 172, w: 122, h: 24 },
};

const TONE_COLOR: Record<Tone, string> = {
  good: "var(--color-success)",
  warn: "var(--color-warning)",
  bad: "var(--color-error)",
  info: "var(--color-info)",
  neutral: "var(--color-base-300)",
};

/** A line's width for a rate: a hair when idle, a thick line near a gigabit. */
function width(bps: number | undefined): number {
  if (!bps || bps < 0) return 1.5;
  return 1.5 + Math.min(7, Math.log10(1 + bps / 1e5) * 1.6);
}

function useNarrow(ref: React.RefObject<HTMLDivElement | null>): boolean {
  const [narrow, setNarrow] = useState(false);
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const ro = new ResizeObserver(([e]) => setNarrow((e?.contentRect.width ?? 1000) < 620));
    ro.observe(el);
    return () => ro.disconnect();
  }, [ref]);
  return narrow;
}

function Node({
  box,
  icon: Icon,
  title,
  sub,
  tone,
  onOpen,
  tip,
}: {
  box: Box;
  icon: LucideIcon;
  title: string;
  sub: string;
  tone: Tone;
  onOpen: () => void;
  tip: string;
}) {
  const key = (e: KeyboardEvent<SVGGElement>) => {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      onOpen();
    }
  };
  return (
    // biome-ignore lint/a11y/useSemanticElements: an SVG group cannot be a <button>
    <g
      role="button"
      tabIndex={0}
      aria-label={`${title}: ${sub}`}
      onClick={onOpen}
      onKeyDown={key}
      className="cursor-pointer outline-none [&:focus-visible>rect]:stroke-[3] [&:hover>rect]:fill-[var(--color-base-200)]"
    >
      <title>{tip}</title>
      <rect
        x={box.x}
        y={box.y}
        width={box.w}
        height={box.h}
        rx={12}
        fill="var(--color-base-100)"
        stroke={TONE_COLOR[tone]}
        strokeWidth={tone === "neutral" ? 1 : 2}
      />
      <Icon x={box.x + 12} y={box.y + 12} width={20} height={20} className="text-primary" />
      <text x={box.x + 40} y={box.y + 28} className="fill-current text-[15px] font-semibold">
        {title}
      </text>
      <text x={box.x + 12} y={box.y + box.h - 16} className="fill-current text-[13px] opacity-70">
        {sub}
      </text>
    </g>
  );
}

function Edge({
  from,
  to,
  bps,
  dashed,
  color = "var(--color-base-content)",
  opacity = 0.35,
}: {
  from: { x: number; y: number };
  to: { x: number; y: number };
  bps?: number;
  dashed?: boolean;
  color?: string;
  opacity?: number;
}) {
  return (
    <line
      x1={from.x}
      y1={from.y}
      x2={to.x}
      y2={to.y}
      stroke={color}
      strokeOpacity={opacity}
      strokeWidth={width(bps)}
      strokeLinecap="round"
      strokeDasharray={dashed ? "6 6" : undefined}
    />
  );
}

function Label({
  x,
  y,
  lines,
  anchor = "middle",
}: {
  x: number;
  y: number;
  lines: string[];
  anchor?: "start" | "middle" | "end";
}) {
  return (
    <text x={x} y={y} textAnchor={anchor} className="tabular fill-current text-[12px] opacity-80">
      {lines.map((l, i) => (
        <tspan key={l} x={x} dy={i === 0 ? 0 : 15}>
          {l}
        </tspan>
      ))}
    </text>
  );
}

export function Diagram({ live, rate }: { live: Live | undefined; rate: Rates | undefined }) {
  const { t, lang } = useI18n();
  const navigate = useNavigate();
  const wrap = useRef<HTMLDivElement>(null);
  const tall = useNarrow(wrap);
  const L = tall ? TALL : WIDE;
  const b = L.box;

  const f = fibreView(live);
  const s = sessionView(live);
  const link = hostLink(live);
  const go = (to: string) => () => navigate({ to });

  const channels = gemViews(live);
  const shown = channels.slice(0, 4);
  const gem = useGemText();
  const down = (v?: number) => `↓ ${bitrate(v, lang)}`;
  const up = (v?: number) => `↑ ${bitrate(v, lang)}`;

  /* Edges run between the facing sides of two boxes. */
  const right = (x: Box) => ({ x: x.x + x.w, y: x.y + x.h / 2 });
  const left = (x: Box) => ({ x: x.x, y: x.y + x.h / 2 });
  const top = (x: Box) => ({ x: x.x + x.w / 2, y: x.y });
  const bottom = (x: Box) => ({ x: x.x + x.w / 2, y: x.y + x.h });

  const hostSfp: Seg = tall ? [top(b.host), bottom(b.sfp)] : [right(b.host), left(b.sfp)];
  const sfpSwitch: Seg = tall ? [top(b.sfp), bottom(b.switch)] : [right(b.sfp), left(b.switch)];
  const switchPon: Seg = tall
    ? [top(b.switch), bottom(b.ponmac)]
    : [right(b.switch), left(b.ponmac)];
  const ponOlt: Seg = tall ? [top(b.ponmac), bottom(b.olt)] : [right(b.ponmac), left(b.olt)];
  const switchCpu: Seg = tall ? [right(b.switch), left(b.cpu)] : [bottom(b.switch), top(b.cpu)];

  const mid = ([a, z]: Seg) => ({ x: (a.x + z.x) / 2, y: (a.y + z.y) / 2 });
  const fibreMid = mid(ponOlt);
  const linkMid = mid(hostSfp);

  return (
    <div ref={wrap} className="w-full">
      {/* biome-ignore lint/a11y/useSemanticElements: the drawing groups its clickable parts */}
      <svg
        viewBox={`0 0 ${L.view[0]} ${L.view[1]}`}
        className="w-full text-base-content"
        role="group"
        aria-label={t("diagram.title")}
      >
        <Edge from={hostSfp[0]} to={hostSfp[1]} bps={(rate?.lanDown ?? 0) + (rate?.lanUp ?? 0)} />
        <Edge
          from={sfpSwitch[0]}
          to={sfpSwitch[1]}
          bps={(rate?.lanDown ?? 0) + (rate?.lanUp ?? 0)}
        />
        <Edge
          from={switchPon[0]}
          to={switchPon[1]}
          bps={(rate?.wanDown ?? 0) + (rate?.wanUp ?? 0)}
        />
        <Edge
          from={ponOlt[0]}
          to={ponOlt[1]}
          bps={(rate?.wanDown ?? 0) + (rate?.wanUp ?? 0)}
          dashed
          color={TONE_COLOR[LIGHT_TONE[f.quality]]}
          opacity={0.9}
        />
        <Edge from={switchCpu[0]} to={switchCpu[1]} bps={(rate?.cpuFrames ?? 0) * 12_000} />

        {tall ? (
          <>
            <Label
              x={linkMid.x + 14}
              y={linkMid.y - 4}
              anchor="start"
              lines={[down(rate?.lanDown), up(rate?.lanUp)]}
            />
            <Label
              x={fibreMid.x + 14}
              y={fibreMid.y - 12}
              anchor="start"
              lines={[dbmText(f.rxDbm, lang), down(rate?.wanDown), up(rate?.wanUp)]}
            />
          </>
        ) : (
          <>
            <Label
              x={linkMid.x}
              y={linkMid.y - 14}
              lines={[linkSpeed(link.speedMbit, lang) ?? t("overview.noLink")]}
            />
            <Label
              x={linkMid.x}
              y={linkMid.y + 24}
              lines={[down(rate?.lanDown), up(rate?.lanUp)]}
            />
            <Label x={fibreMid.x} y={fibreMid.y - 14} lines={[dbmText(f.rxDbm, lang)]} />
            <Label
              x={fibreMid.x}
              y={fibreMid.y + 24}
              lines={[down(rate?.wanDown), up(rate?.wanUp)]}
            />
          </>
        )}

        <Node
          box={b.host}
          icon={HostIcon}
          title={t("diagram.host")}
          sub={
            s.up && s.address
              ? s.address
              : link.up
                ? (linkSpeed(link.speedMbit, lang) ?? "")
                : t("overview.noLink")
          }
          tone={link.up ? "good" : "bad"}
          onOpen={go("/sfp")}
          tip={t("diagram.host")}
        />
        <Node
          box={b.sfp}
          icon={Network}
          title={t("diagram.sfp")}
          sub={link.up ? (linkSpeed(link.speedMbit, lang) ?? "") : t("overview.noLink")}
          tone={link.up ? "good" : "bad"}
          onOpen={go("/sfp")}
          tip={t("diagram.sfp")}
        />
        <Node
          box={b.switch}
          icon={ServerCog}
          title={t("diagram.switch")}
          sub="RTL9602C"
          tone="neutral"
          onOpen={go("/system")}
          tip={t("diagram.switch")}
        />
        <Node
          box={b.cpu}
          icon={Cpu}
          title={t("diagram.cpu")}
          sub={t("diagram.frames", { n: integer(rate?.cpuFrames ?? 0, lang) })}
          tone="neutral"
          onOpen={go("/internet")}
          tip={t("diagram.cpuSub")}
        />
        <Node
          box={b.ponmac}
          icon={Radio}
          title={t("diagram.ponmac")}
          sub={t(`state.${f.stateKey}`)}
          tone={f.tone}
          onOpen={go("/fibre")}
          tip={t("diagram.ponmac")}
        />
        <Node
          box={b.olt}
          icon={Globe}
          title={tall ? t("diagram.olt") : "OLT"}
          sub={t(`light.${f.quality}`)}
          tone={LIGHT_TONE[f.quality]}
          onOpen={go("/fibre")}
          tip={t("diagram.olt")}
        />

        {shown.map((v, i) => {
          const g = v.gem;
          const y = L.gems.y + i * (L.gems.h + 6);
          const kind = g.multicast
            ? t("diagram.multicast")
            : g.direction === "both"
              ? "↕"
              : g.direction === "downstream"
                ? "↓"
                : "↑";
          return (
            // biome-ignore lint/a11y/useSemanticElements: an SVG group cannot be a <button>
            <g
              key={g.name}
              role="button"
              tabIndex={0}
              onClick={go("/fibre")}
              onKeyDown={(e) => e.key === "Enter" && navigate({ to: "/fibre" })}
              className="cursor-pointer outline-none"
            >
              <title>{`${t("diagram.gem", { id: g.port_id })} · ${gem.direction(g)} · ${gem.use(v)}`}</title>
              <rect
                x={L.gems.x}
                y={y}
                width={L.gems.w}
                height={L.gems.h}
                rx={L.gems.h / 2}
                fill="var(--color-base-200)"
                stroke="var(--color-base-300)"
              />
              <text
                x={L.gems.x + 12}
                y={y + L.gems.h / 2 + 4}
                className="tabular fill-current text-[12px]"
              >
                {`${t("diagram.gem", { id: g.port_id })} · ${kind}`}
              </text>
            </g>
          );
        })}
        {channels.length > shown.length && (
          <text
            x={L.gems.x + 12}
            y={L.gems.y + shown.length * (L.gems.h + 6) + 14}
            className="fill-current text-[12px] opacity-70"
          >
            +{channels.length - shown.length}
          </text>
        )}
      </svg>
      <p className="mt-2 text-xs text-base-content/60">{t("diagram.hint")}</p>
    </div>
  );
}
