// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The state of the stick, polled every few seconds in one request, and the
 * traffic worked out from it. The switch counts octets; two readings and the
 * time between them give a rate. The history of rates outlives the page that
 * shows it, so going from page to page does not empty the charts.
 */

import { useQuery } from "@tanstack/react-query";
import { useSyncExternalStore } from "react";
import { request } from "./client";
import { LIVE_IDS, type Live } from "./types";

export const POLL_MS = 5_000;
const HISTORY = 120; /* ten minutes at one sample every five seconds */

export interface Rates {
  t: number;
  /** From the operator to the stick, bits per second. */
  wanDown: number;
  /** From the stick to the operator. */
  wanUp: number;
  /** From the stick to the router. */
  lanDown: number;
  /** From the router to the stick. */
  lanUp: number;
  /** Frames per second the processor handled, both ways. */
  cpuFrames: number;
}

interface Reading {
  t: number;
  ponRx: number;
  ponTx: number;
  sfpRx: number;
  sfpTx: number;
  cpu: number;
}

let previous: Reading | null = null;
let history: Rates[] = [];
const listeners = new Set<() => void>();

function reading(live: Live, t: number): Reading | null {
  const pc = live.port_counters;
  if (!pc?.pon || !pc?.sfp) return null;
  const cpu = live.ports?.find((p) => p.name === "cpu")?.cpu_path;
  return {
    t,
    ponRx: pc.pon.rx_octets,
    ponTx: pc.pon.tx_octets,
    sfpRx: pc.sfp.rx_octets,
    sfpTx: pc.sfp.tx_octets,
    cpu: cpu ? cpu.rx_packets + cpu.tx_packets : 0,
  };
}

function record(live: Live, t: number) {
  const cur = reading(live, t);
  if (!cur) return;
  const prev = previous;
  previous = cur;
  if (!prev || cur.t <= prev.t) return;
  const dt = (cur.t - prev.t) / 1000;
  const rate = (a: number, b: number) => ((b - a) * 8) / dt;
  /* A counter that went back means the stick restarted: start again. */
  if (cur.ponRx < prev.ponRx || cur.ponTx < prev.ponTx || cur.sfpRx < prev.sfpRx) {
    history = [];
    return;
  }
  history = [
    ...history.slice(-(HISTORY - 1)),
    {
      t: cur.t,
      wanDown: rate(prev.ponRx, cur.ponRx),
      wanUp: rate(prev.ponTx, cur.ponTx),
      lanDown: rate(prev.sfpTx, cur.sfpTx),
      lanUp: rate(prev.sfpRx, cur.sfpRx),
      cpuFrames: Math.max(0, (cur.cpu - prev.cpu) / dt),
    },
  ];
  for (const l of listeners) l();
}

export async function fetchLive(): Promise<Live> {
  const live = await request<Live>(`/api/getm?ids=${LIVE_IDS.join(",")}`);
  record(live, Date.now());
  return live;
}

export function useLive() {
  return useQuery({
    queryKey: ["live"],
    queryFn: fetchLive,
    refetchInterval: POLL_MS,
    refetchIntervalInBackground: false,
    retry: false,
  });
}

/** The rates of the last ten minutes, oldest first. */
export function useTraffic(): Rates[] {
  return useSyncExternalStore(
    (l) => {
      listeners.add(l);
      return () => listeners.delete(l);
    },
    () => history,
  );
}

/** For the tests: forget every reading. */
export function resetTraffic() {
  previous = null;
  history = [];
}
