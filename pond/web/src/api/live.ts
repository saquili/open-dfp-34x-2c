// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The state of the stick, polled every few seconds in one request, and the
 * traffic worked out from it. The switch counts octets; two readings and the
 * time between them give a rate. The history of rates outlives the page that
 * shows it, so going from page to page does not empty the charts.
 */

import { queryOptions, useQuery } from "@tanstack/react-query";
import { useSyncExternalStore } from "react";
import { getm } from "./gen/pond";
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
  /** The share of time the processor was busy, 0 to 1; null without its figures. */
  cpuBusy: number | null;
  /** The share of memory in use, 0 to 1. */
  memUsed: number | null;
  /** Each GEM port's traffic, by interface name: down from the OLT, up to it. */
  gems: Record<string, { down: number; up: number }>;
  /** The light received, dBm; null without light. */
  rxDbm: number | null;
  /** Errors on the line a second: bit errors (BIP), codewords FEC corrected, and lost. */
  bip: number;
  fecCorrected: number;
  fecLost: number;
}

interface Reading {
  t: number;
  ponRx: number;
  ponTx: number;
  sfpRx: number;
  sfpTx: number;
  cpu: number;
  busy: number | null;
  idle: number | null;
  gems: Record<string, { rx: number; tx: number }>;
  rxUw: number | null;
  mem: number | null;
  bip: number;
  fecCorrected: number;
  fecLost: number;
}

let previous: Reading | null = null;
let history: Rates[] = [];
const listeners = new Set<() => void>();

function reading(live: Live, t: number): Reading | null {
  const pc = live.port_counters;
  if (!pc?.pon || !pc?.sfp) return null;
  const cpu = live.ports?.find((p) => p.name === "cpu")?.cpu_path;
  const k = live.cpu?.ticks;
  const gems: Reading["gems"] = {};
  for (const [name, c] of Object.entries(live.gem_counters ?? {}))
    gems[name] = { rx: c.pon_rx_octets, tx: c.pon_tx_octets };
  return {
    t,
    ponRx: pc.pon.rx_octets,
    ponTx: pc.pon.tx_octets,
    sfpRx: pc.sfp.rx_octets,
    sfpTx: pc.sfp.tx_octets,
    cpu: cpu ? cpu.rx_packets + cpu.tx_packets : 0,
    busy: k ? k.user + k.nice + k.system + k.irq + k.softirq + k.steal : null,
    idle: k ? k.idle + k.iowait : null,
    gems,
    rxUw: live.optics?.rx_power_uw ?? null,
    mem: live.memory?.total_kb ? 1 - live.memory.available_kb / live.memory.total_kb : null,
    bip: live.onu?.counters.bip_errors ?? 0,
    fecCorrected: live.onu?.counters.fec_corrected_codewords ?? 0,
    fecLost: live.onu?.counters.fec_uncorrectable_codewords ?? 0,
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
      cpuBusy: busyShare(prev, cur),
      memUsed: cur.mem,
      rxDbm: cur.rxUw != null && cur.rxUw > 0 ? 10 * Math.log10(cur.rxUw / 1000) : null,
      bip: Math.max(0, (cur.bip - prev.bip) / dt),
      fecCorrected: Math.max(0, (cur.fecCorrected - prev.fecCorrected) / dt),
      fecLost: Math.max(0, (cur.fecLost - prev.fecLost) / dt),
      gems: Object.fromEntries(
        Object.entries(cur.gems).map(([name, g]) => {
          const p = prev.gems[name];
          return [
            name,
            p && g.rx >= p.rx && g.tx >= p.tx
              ? { down: rate(p.rx, g.rx), up: rate(p.tx, g.tx) }
              : { down: 0, up: 0 },
          ];
        }),
      ),
    },
  ];
  for (const l of listeners) l();
}

function busyShare(prev: Reading, cur: Reading): number | null {
  if (prev.busy == null || prev.idle == null || cur.busy == null || cur.idle == null) return null;
  const busy = cur.busy - prev.busy;
  const all = busy + (cur.idle - prev.idle);
  return all > 0 && busy >= 0 ? busy / all : null;
}

export async function fetchLive(): Promise<Live> {
  /* pond gives a value it cannot read as an error in its place, which the panel takes as it comes. */
  const live = (await getm({ ids: LIVE_IDS.join(",") })) as Live;
  record(live, Date.now());
  return live;
}

/* Polled whether the last read answered or not: a failed read is tried again at the next poll. */
export const liveQuery = queryOptions({
  queryKey: ["live"],
  queryFn: fetchLive,
  refetchInterval: POLL_MS,
  refetchIntervalInBackground: false,
  retry: false,
});

export function useLive() {
  return useQuery(liveQuery);
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
