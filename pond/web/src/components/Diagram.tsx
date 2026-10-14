// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The path of the traffic through the stick, drawn: the host, the SFP
 * port, the switch with the processor beside it, the GPON MAC, the fibre
 * and the OLT, and below them every channel (GEM port) the OLT set up, with
 * its own traffic. A line grows with the traffic it carries and is labelled
 * with it; a part is coloured by its state, and a click on it opens its
 * page. Below a phone's width the drawing turns upright, the operator at the
 * top and the host at the bottom, the channels under it all.
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
import { bitrate, dbmText, decimal, integer, percent } from "../lib/format";
import {
  fibre as fibreView,
  type GemView,
  GPON_DOWN_BPS,
  GPON_UP_BPS,
  gems as gemViews,
  hostLink,
  LIGHT_TONE,
  laneMode,
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
  width: number;
  /** The drawing's height without the channels. */
  height: number;
  box: Record<Part, Box>;
  /** Where the channels start, how many sit in a row and how big each is. */
  gems: { x: number; y: number; cols: number; w: number; h: number; gap: number };
}

const WIDE: Layout = {
  width: 1100,
  height: 290,
  box: {
    host: { x: 8, y: 40, w: 150, h: 88 },
    sfp: { x: 278, y: 40, w: 150, h: 88 },
    switch: { x: 488, y: 40, w: 150, h: 88 },
    cpu: { x: 488, y: 196, w: 150, h: 88 },
    ponmac: { x: 698, y: 40, w: 180, h: 88 },
    olt: { x: 958, y: 40, w: 134, h: 88 },
  },
  gems: { x: 698, y: 172, cols: 3, w: 124, h: 54, gap: 8 },
};

const TALL: Layout = {
  width: 360,
  height: 760,
  box: {
    olt: { x: 20, y: 10, w: 200, h: 76 },
    ponmac: { x: 20, y: 172, w: 200, h: 76 },
    switch: { x: 20, y: 334, w: 200, h: 76 },
    cpu: { x: 236, y: 334, w: 114, h: 76 },
    sfp: { x: 20, y: 496, w: 200, h: 76 },
    host: { x: 20, y: 658, w: 200, h: 76 },
  },
  gems: { x: 20, y: 780, cols: 2, w: 160, h: 54, gap: 8 },
};

const TONE_COLOR: Record<Tone, string> = {
  good: "var(--color-success)",
  warn: "var(--color-warning)",
  bad: "var(--color-error)",
  info: "var(--color-info)",
  neutral: "var(--color-base-300)",
};

/* The colour of a channel's mark: what the stick does with it. */
const USE_COLOR: Record<GemView["use"] | "multicast", string> = {
  session: "var(--color-primary)",
  bridged: "var(--color-info)",
  ends: "var(--color-base-300)",
  multicast: "var(--color-secondary)",
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

function activate(open: () => void) {
  return (e: KeyboardEvent<SVGGElement>) => {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      open();
    }
  };
}

function Node({
  box,
  icon: Icon,
  title,
  lines,
  tone,
  onOpen,
  tip,
}: {
  box: Box;
  icon: LucideIcon;
  title: string;
  lines: string[];
  tone: Tone;
  onOpen: () => void;
  tip: string;
}) {
  return (
    // biome-ignore lint/a11y/useSemanticElements: an SVG group cannot be a <button>
    <g
      role="button"
      tabIndex={0}
      aria-label={`${title}: ${lines.join(", ")}`}
      onClick={onOpen}
      onKeyDown={activate(onOpen)}
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
      {lines.map((l, i) => (
        <text
          key={l}
          x={box.x + 12}
          y={box.y + box.h - 16 - (lines.length - 1 - i) * 18}
          className={`tabular fill-current text-[13px] ${i === 0 ? "opacity-80" : "opacity-60"}`}
        >
          {l}
        </text>
      ))}
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
  from: Pt;
  to: Pt;
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

/** One channel: its Port-ID and direction, its traffic, and what the stick does with it. */
function Channel({
  v,
  box,
  rate,
  onOpen,
}: {
  v: GemView;
  box: Box;
  rate: { down: number; up: number } | undefined;
  onOpen: () => void;
}) {
  const { t, lang } = useI18n();
  const gem = useGemText();
  const g = v.gem;
  const arrow = g.multicast
    ? "↓"
    : g.direction === "both"
      ? "↕"
      : g.direction === "downstream"
        ? "↓"
        : "↑";
  const downOnly = g.direction === "downstream" || g.multicast;
  const clip = `gem-clip-${g.name}`;
  return (
    // biome-ignore lint/a11y/useSemanticElements: an SVG group cannot be a <button>
    <g
      role="button"
      tabIndex={0}
      onClick={onOpen}
      onKeyDown={activate(onOpen)}
      className="cursor-pointer outline-none [&:focus-visible>rect]:stroke-[2] [&:hover>rect]:fill-[var(--color-base-200)]"
    >
      <title>{`${t("diagram.gem", { id: g.port_id })} · ${gem.direction(g)} · ${gem.use(v)}`}</title>
      <clipPath id={clip}>
        <rect x={box.x} y={box.y} width={box.w} height={box.h} rx={8} />
      </clipPath>
      <rect
        x={box.x}
        y={box.y}
        width={box.w}
        height={box.h}
        rx={8}
        fill="var(--color-base-100)"
        stroke="var(--color-base-300)"
      />
      {/* What is drawn inside never leaves the chip's outline. */}
      <g clipPath={`url(#${clip})`}>
        <rect
          x={box.x}
          y={box.y}
          width={5}
          height={box.h}
          fill={USE_COLOR[g.multicast ? "multicast" : v.use]}
        />
        <text
          x={box.x + 12}
          y={box.y + 16}
          className="tabular fill-current text-[12px] font-semibold"
        >
          {`${g.port_id} ${arrow}`}
          <tspan className="font-normal opacity-60">{` ${gem.short(v)}`}</tspan>
        </text>
        <text x={box.x + 12} y={box.y + 32} className="tabular fill-current text-[11px] opacity-75">
          {`↓ ${bitrate(rate?.down ?? 0, lang)}`}
        </text>
        {!downOnly && (
          <text
            x={box.x + 12}
            y={box.y + 46}
            className="tabular fill-current text-[11px] opacity-75"
          >
            {`↑ ${bitrate(rate?.up ?? 0, lang)}`}
          </text>
        )}
      </g>
    </g>
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
  /*
   * The channels hang from the PON MAC: a line from it down to a bracket,
   * their count under the bracket, so that the line never crosses it, and
   * their cards under the count.
   */
  const bracketY = L.gems.y;
  const countY = bracketY + 16;
  const cardsY = bracketY + 26;
  const rows = Math.ceil(channels.length / L.gems.cols);
  const gemsBottom = cardsY + 6 + rows * (L.gems.h + L.gems.gap);
  const height = Math.max(L.height, channels.length ? gemsBottom : L.height);
  const down = (v?: number) => `↓ ${bitrate(v, lang)}`;
  const up = (v?: number) => `↑ ${bitrate(v, lang)}`;
  const gbit = (bps: number) => decimal(bps / 1e9, 2, lang);

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
  const lane = laneMode(live);
  const cpuLines = [
    t("diagram.cpuLimit"),
    [
      t("diagram.frames", { n: integer(rate?.cpuFrames ?? 0, lang) }),
      rate?.cpuBusy != null ? percent(rate.cpuBusy, lang) : null,
    ]
      .filter(Boolean)
      .join(" · "),
  ];

  const gemsRight = L.gems.x + L.gems.cols * (L.gems.w + L.gems.gap) - L.gems.gap;

  return (
    /*
     * On a large screen the drawing takes the height the page leaves it and
     * fits in it at its own proportions, which grow taller with every row of
     * channels; on a smaller one it takes the width. The wrapper is always
     * as wide as the card, so its width alone chooses the layout.
     */
    <div ref={wrap} className="w-full lg:min-h-0 lg:flex-1">
      {/* biome-ignore lint/a11y/useSemanticElements: the drawing groups its clickable parts */}
      <svg
        viewBox={`0 0 ${L.width} ${height}`}
        className="w-full text-base-content lg:h-full"
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
              y={linkMid.y - 12}
              anchor="start"
              lines={[
                linkSpeed(link.speedMbit, lang) ?? t("overview.noLink"),
                down(rate?.lanDown),
                up(rate?.lanUp),
              ]}
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
          lines={[
            s.up && s.address
              ? s.address
              : link.up
                ? t("diagram.hostLinked")
                : t("overview.noLink"),
          ]}
          tone={link.up ? "good" : "bad"}
          onOpen={go("/switch/sfp")}
          tip={t("diagram.hostTip")}
        />
        <Node
          box={b.sfp}
          icon={Network}
          title={t("diagram.sfp")}
          lines={[lane ?? "–", link.up ? t("sfp.up") : t("overview.noLink")]}
          tone={link.up ? "good" : "bad"}
          onOpen={go("/switch/sfp")}
          tip={t("diagram.sfpTip")}
        />
        <Node
          box={b.switch}
          icon={ServerCog}
          title={t("diagram.switch")}
          lines={["RTL9602C"]}
          tone="neutral"
          onOpen={go("/switch/vlans")}
          tip={t("diagram.switchTip")}
        />
        <Node
          box={b.cpu}
          icon={Cpu}
          title={t("diagram.cpu")}
          lines={cpuLines}
          tone="neutral"
          onOpen={go("/system")}
          tip={t("diagram.cpuTip")}
        />
        <Node
          box={b.ponmac}
          icon={Radio}
          title={t("diagram.ponmac")}
          lines={[
            t("diagram.gponRate", { down: gbit(GPON_DOWN_BPS), up: gbit(GPON_UP_BPS) }),
            f.onuId != null
              ? `${t(`state.${f.stateKey}`)} · ONU ${f.onuId}`
              : t(`state.${f.stateKey}`),
          ]}
          tone={f.tone}
          onOpen={go("/switch/fibre")}
          tip={t("diagram.ponmacTip")}
        />
        <Node
          box={b.olt}
          icon={Globe}
          title={tall ? t("diagram.olt") : "OLT"}
          lines={[t(`light.${f.quality}`)]}
          tone={LIGHT_TONE[f.quality]}
          onOpen={go("/switch/fibre")}
          tip={t("diagram.olt")}
        />

        {channels.length > 0 && (
          <>
            <path
              d={
                tall
                  ? `M ${L.gems.x} ${bracketY} H ${gemsRight}`
                  : `M ${bottom(b.ponmac).x} ${b.ponmac.y + b.ponmac.h} V ${bracketY} M ${L.gems.x} ${bracketY} H ${gemsRight}`
              }
              fill="none"
              stroke="var(--color-base-content)"
              strokeOpacity={0.2}
            />
            <text
              x={L.gems.x}
              y={countY}
              className="fill-current text-[12px] font-medium opacity-70"
            >
              {t("diagram.gems", { n: channels.length })}
            </text>
            {channels.map((v, i) => {
              const col = i % L.gems.cols;
              const row = Math.floor(i / L.gems.cols);
              return (
                <Channel
                  key={v.gem.name}
                  v={v}
                  rate={rate?.gems[v.gem.name]}
                  onOpen={go("/operator/channels")}
                  box={{
                    x: L.gems.x + col * (L.gems.w + L.gems.gap),
                    y: cardsY + row * (L.gems.h + L.gems.gap),
                    w: L.gems.w,
                    h: L.gems.h,
                  }}
                />
              );
            })}
          </>
        )}
      </svg>
    </div>
  );
}
