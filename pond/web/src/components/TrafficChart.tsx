// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Ten minutes of traffic: the download as an area, the upload as a line,
 * on a scale that rounds up to the next step (10 Mb/s … 2.5 Gb/s). Pointing
 * at the chart shows the two rates at that moment.
 */

import { Info } from "lucide-react";
import { useState } from "react";
import type { Rates } from "../api/live";
import { useI18n } from "../i18n";
import { bitrate, clock } from "../lib/format";

const W = 600;
const H = 160;
const TOP = 16;
const BOTTOM = H - 18;
const STEPS = [10e6, 50e6, 100e6, 250e6, 500e6, 1e9, 2.5e9];

export function TrafficChart({
  data,
  down,
  up,
  slots = 120,
  fill = false,
}: {
  data: Rates[];
  down: (r: Rates) => number;
  up: (r: Rates) => number;
  slots?: number;
  /** Take the height the parent gives, instead of a fixed one. */
  fill?: boolean;
}) {
  const { t, lang } = useI18n();
  const [hover, setHover] = useState<number | null>(null);

  const peak = Math.max(10e6, ...data.map((r) => Math.max(down(r), up(r))));
  const scale = STEPS.find((s) => s >= peak) ?? peak;
  const x = (i: number) => W - (data.length - 1 - i) * (W / (slots - 1));
  const y = (v: number) => BOTTOM - ((BOTTOM - TOP) * Math.max(0, v)) / scale;
  const path = (f: (r: Rates) => number) =>
    data.map((r, i) => `${x(i).toFixed(1)},${y(f(r)).toFixed(1)}`).join(" ");

  const pick = (clientX: number, rect: DOMRect) => {
    const px = ((clientX - rect.left) / rect.width) * W;
    const i = Math.round(data.length - 1 - (W - px) / (W / (slots - 1)));
    setHover(i >= 0 && i < data.length ? i : null);
  };
  const point = hover != null ? data[hover] : undefined;

  return (
    <div className={`relative ${fill ? "flex h-full min-h-0 flex-col" : ""}`}>
      {/*
       * The drawing fills its box and asks no height of its own, so that a
       * card sized to its content is sized by the figures beside the chart.
       */}
      <div className={`relative ${fill ? "min-h-24 flex-1" : "h-40"}`}>
        <svg
          viewBox={`0 0 ${W} ${H}`}
          preserveAspectRatio="none"
          className="absolute inset-0 size-full touch-none text-base-content"
          role="img"
          aria-label={t("overview.lastMinutes")}
          onPointerMove={(e) => pick(e.clientX, e.currentTarget.getBoundingClientRect())}
          onPointerDown={(e) => pick(e.clientX, e.currentTarget.getBoundingClientRect())}
          onPointerLeave={() => setHover(null)}
        >
          {[0, 0.5, 1].map((f) => (
            <line
              key={f}
              x1={0}
              x2={W}
              y1={y(scale * f)}
              y2={y(scale * f)}
              stroke="currentColor"
              strokeOpacity={0.12}
              vectorEffect="non-scaling-stroke"
            />
          ))}
          {data.length > 1 && (
            <>
              <polygon
                points={`${x(0)},${BOTTOM} ${path(down)} ${x(data.length - 1)},${BOTTOM}`}
                fill="var(--color-down)"
                fillOpacity={0.15}
              />
              <polyline
                points={path(down)}
                fill="none"
                stroke="var(--color-down)"
                strokeWidth={2}
                vectorEffect="non-scaling-stroke"
              />
              <polyline
                points={path(up)}
                fill="none"
                stroke="var(--color-up)"
                strokeWidth={2}
                vectorEffect="non-scaling-stroke"
              />
            </>
          )}
          {point && hover != null && (
            <line
              x1={x(hover)}
              x2={x(hover)}
              y1={TOP}
              y2={BOTTOM}
              stroke="currentColor"
              strokeOpacity={0.4}
              vectorEffect="non-scaling-stroke"
            />
          )}
        </svg>
      </div>
      <div className="pointer-events-none absolute top-0 left-1 text-[11px] text-base-content/55">
        {bitrate(scale, lang)}
      </div>
      <div className="pointer-events-none flex justify-between text-[11px] text-base-content/55">
        <span>−10 min</span>
        <span>{t("overview.lastMinutes")}</span>
        <span>0</span>
      </div>
      {point && (
        <div className="pointer-events-none absolute top-1 right-1 rounded-box border border-base-300 bg-base-100 px-2 py-1 text-xs shadow-sm">
          <div className="tabular text-base-content/60">{clock(point.t, lang, true)}</div>
          <div className="tabular">
            <span className="text-primary">↓ {bitrate(down(point), lang)}</span>
            {"  "}
            <span className="text-secondary">↑ {bitrate(up(point), lang)}</span>
          </div>
        </div>
      )}
    </div>
  );
}

/**
 * A figure over the last ten minutes as a line, on a scale from @min to
 * @max, with the latest value and the range in words.
 */
export function Sparkline({
  data,
  value,
  min,
  max,
  format,
  slots = 120,
  tone = "var(--color-primary)",
  bars = false,
  label = "",
}: {
  data: Rates[];
  value: (r: Rates) => number | null;
  min: number;
  max: number;
  format: (v: number) => string;
  slots?: number;
  tone?: string;
  /** Bars from the bottom, for counts; a line otherwise. */
  bars?: boolean;
  /** What the chart shows, for those who cannot see it. */
  label?: string;
}) {
  const points = data.map((r, i) => ({ i, v: value(r) })).filter((p) => p.v != null) as {
    i: number;
    v: number;
  }[];
  const x = (i: number) => W - (data.length - 1 - i) * (W / (slots - 1));
  const y = (v: number) =>
    BOTTOM - ((BOTTOM - TOP) * (Math.min(max, Math.max(min, v)) - min)) / (max - min || 1);
  return (
    <div className="relative flex h-full min-h-20 flex-col">
      {/* Like the traffic's, the drawing asks no height of its own. */}
      <div className="relative min-h-24 flex-1 lg:min-h-16">
        <svg
          viewBox={`0 0 ${W} ${H}`}
          preserveAspectRatio="none"
          className="absolute inset-0 size-full text-base-content"
          role="img"
          aria-label={label}
        >
          {[0, 0.5, 1].map((f) => (
            <line
              key={f}
              x1={0}
              x2={W}
              y1={TOP + (BOTTOM - TOP) * f}
              y2={TOP + (BOTTOM - TOP) * f}
              stroke="currentColor"
              strokeOpacity={0.12}
              vectorEffect="non-scaling-stroke"
            />
          ))}
          {bars
            ? points.map((p) => (
                <line
                  key={p.i}
                  x1={x(p.i)}
                  x2={x(p.i)}
                  y1={BOTTOM}
                  y2={p.v > 0 ? Math.min(BOTTOM - 2, y(p.v)) : BOTTOM}
                  stroke={tone}
                  strokeWidth={3}
                  vectorEffect="non-scaling-stroke"
                />
              ))
            : points.length > 1 && (
                <polyline
                  points={points.map((p) => `${x(p.i).toFixed(1)},${y(p.v).toFixed(1)}`).join(" ")}
                  fill="none"
                  stroke={tone}
                  strokeWidth={2}
                  vectorEffect="non-scaling-stroke"
                />
              )}
        </svg>
      </div>
      <div className="pointer-events-none absolute top-0 left-1 text-[11px] text-base-content/55">
        {format(max)}
      </div>
      <div className="pointer-events-none flex justify-between text-[11px] text-base-content/55">
        <span>−10 min</span>
        <span>{format(min)}</span>
      </div>
    </div>
  );
}

/**
 * Where the figures of the charts come from: the browser keeps them, from
 * the moment the panel was opened, for ten minutes at most; a reload starts
 * them again. The stick keeps no history of its own.
 */
export function HistoryNote() {
  const { t } = useI18n();
  return (
    <span
      className="flex items-center gap-1 text-xs text-base-content/55"
      title={t("common.historyHint")}
    >
      <Info className="size-3.5" />
      {t("common.history")}
    </span>
  );
}
