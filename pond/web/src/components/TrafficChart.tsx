// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Ten minutes of traffic: the download as an area, the upload as a line,
 * on a scale that rounds up to the next step (10 Mb/s … 2.5 Gb/s). Pointing
 * at the chart shows the two rates at that moment.
 */

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
}: {
  data: Rates[];
  down: (r: Rates) => number;
  up: (r: Rates) => number;
  slots?: number;
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
    <div className="relative">
      <svg
        viewBox={`0 0 ${W} ${H}`}
        preserveAspectRatio="none"
        className="h-40 w-full touch-none text-base-content"
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
