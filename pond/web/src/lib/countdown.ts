// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* Time left until a moment the stick names in seconds of its uptime. */

import { useEffect, useState } from "react";

/**
 * Seconds left until a moment given in seconds of the stick's uptime, from
 * the uptime @upNow the stick told at @at (milliseconds of the clock here).
 */
export function useCountdown(
  until: number | null | undefined,
  upNow: number | undefined,
  at: number,
) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    if (until == null) return;
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [until]);
  if (until == null || upNow == null) return null;
  return Math.max(0, Math.round(until - upNow - (Math.max(now, at) - at) / 1000));
}
