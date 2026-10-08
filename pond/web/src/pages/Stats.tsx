// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Every count the stick keeps, in three tabs: the switch's counters of the
 * SFP port and the fibre, the GPON MAC's (the line, PLOAM, bandwidth maps,
 * FEC) and each GEM channel's, and every network interface's. Counts run
 * from the stick's start; "count from now" keeps the counts of a moment in
 * the browser and shows what came since, the stick's own counters left as
 * they are.
 */

import { RotateCcw } from "lucide-react";
import { useState } from "react";
import { useLive, useTraffic } from "../api/live";
import type { Live } from "../api/types";
import { Page, Tabs, useTab } from "../components/Page";
import { Card, Stat } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { bitrate, bytes, clock, decimal, integer } from "../lib/format";
import { gems } from "../lib/state";

const TABS = ["ports", "gpon", "ifaces"] as const;

/** Every number of a value, by a path of keys, flattened. */
function flat(v: unknown, prefix = "", out: Record<string, number> = {}): Record<string, number> {
  if (typeof v === "number") out[prefix] = v;
  else if (v && typeof v === "object")
    for (const [k, x] of Object.entries(v)) flat(x, prefix ? `${prefix}.${k}` : k, out);
  return out;
}

/* The counts at the moment the reader asked to count from, kept for this visit. */
let baseline: { at: number; values: Record<string, number> } | null = null;

function useCounts() {
  const { data: live } = useLive();
  const [base, setBase] = useState(baseline);
  const all = flat({
    port_counters: live?.port_counters,
    gem_counters: live?.gem_counters,
    onu: live?.onu?.counters,
    ports: Object.fromEntries((live?.ports ?? []).map((p) => [p.name, p.cpu_path])),
  });
  const get = (key: string) => {
    const v = all[key];
    if (v == null) return null;
    const b = base?.values[key];
    return b != null && v >= b ? v - b : v;
  };
  const reset = () => {
    baseline = { at: Date.now(), values: all };
    setBase(baseline);
  };
  const clear = () => {
    baseline = null;
    setBase(null);
  };
  return { live, get, base, reset, clear };
}

type Get = (key: string) => number | null;

const PORT_ROWS: { label: Key; rx?: string; tx?: string; bytes?: boolean }[] = [
  { label: "switch.cOctets", rx: "rx_octets", tx: "tx_octets", bytes: true },
  { label: "switch.cUnicast", rx: "rx_unicast", tx: "tx_unicast" },
  { label: "switch.cMulticast", rx: "rx_multicast", tx: "tx_multicast" },
  { label: "switch.cBroadcast", rx: "rx_broadcast", tx: "tx_broadcast" },
  { label: "switch.cPause", rx: "rx_pause", tx: "tx_pause" },
  { label: "switch.cDrops", rx: "rx_drops", tx: "tx_discards" },
  { label: "stats.forwardDiscards", rx: "rx_forward_discards" },
  { label: "switch.cCrc", rx: "rx_crc_errors" },
  { label: "switch.cSymbol", rx: "rx_symbol_errors" },
  { label: "switch.cUndersize", rx: "rx_undersize", tx: "tx_undersize" },
  { label: "switch.cOversize", rx: "rx_oversize", tx: "tx_oversize" },
  { label: "switch.cFragments", rx: "rx_fragments" },
  { label: "switch.cJabbers", rx: "rx_jabbers" },
  { label: "stats.unknownOpcodes", rx: "rx_unknown_opcodes" },
  { label: "stats.collisions", tx: "tx_collisions" },
  { label: "stats.size64", rx: "rx_64_octets", tx: "tx_64_octets" },
  { label: "stats.size127", rx: "rx_65_127_octets", tx: "tx_65_127_octets" },
  { label: "stats.size255", rx: "rx_128_255_octets", tx: "tx_128_255_octets" },
  { label: "stats.size511", rx: "rx_256_511_octets", tx: "tx_256_511_octets" },
  { label: "stats.size1023", rx: "rx_512_1023_octets", tx: "tx_512_1023_octets" },
  { label: "stats.size1518", rx: "rx_1024_1518_octets", tx: "tx_1024_1518_octets" },
  { label: "stats.sizeMax", rx: "rx_1519_max_octets", tx: "tx_1519_max_octets" },
];

function PortsTab({ get }: { get: Get }) {
  const { t, lang } = useI18n();
  const v = (port: string, key: string | undefined, asBytes?: boolean) => {
    if (!key) return "";
    const n = get(`port_counters.${port}.${key}`);
    return asBytes ? bytes(n, lang) : integer(n, lang);
  };
  return (
    <Card title={t("stats.portsTitle")} scroll className="min-h-0 flex-1">
      <p className="text-xs text-base-content/65">{t("stats.portsHint")}</p>
      <table className="table table-sm table-pin-rows">
        <thead>
          <tr>
            <th />
            <th className="text-right">{t("stats.sfpRx")}</th>
            <th className="text-right">{t("stats.sfpTx")}</th>
            <th className="text-right">{t("stats.ponRx")}</th>
            <th className="text-right">{t("stats.ponTx")}</th>
          </tr>
        </thead>
        <tbody>
          {PORT_ROWS.map((r) => (
            <tr key={r.label}>
              <td>{t(r.label)}</td>
              <td className="tabular text-right">{v("sfp", r.rx, r.bytes)}</td>
              <td className="tabular text-right">{v("sfp", r.tx, r.bytes)}</td>
              <td className="tabular text-right">{v("pon", r.rx, r.bytes)}</td>
              <td className="tabular text-right">{v("pon", r.tx, r.bytes)}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </Card>
  );
}

function GponTab({ get, live }: { get: Get; live: Live | undefined }) {
  const { t, lang } = useI18n();
  const rates = useTraffic().at(-1)?.gems;
  const n = (k: string) => integer(get(`onu.${k}`), lang);
  const codewords = get("onu.fec_codewords");
  const corrected = get("onu.fec_corrected_codewords");
  return (
    <div className="grid min-h-0 flex-1 gap-4 lg:grid-cols-[22rem_minmax(0,1fr)]">
      <Card title={t("stats.gponMac")} scroll>
        <div className="grid grid-cols-2 gap-3">
          <Stat label={t("fibre.bipErrors")} value={n("bip_errors")} />
          <Stat label={t("fibre.gemFrames")} value={n("gem_frames")} />
          <Stat label={t("stats.fecCodewords")} value={n("fec_codewords")} />
          <Stat
            label={t("fibre.fecCorrected")}
            value={n("fec_corrected_codewords")}
            sub={
              codewords && corrected != null
                ? `${decimal((corrected / codewords) * 100, 6, lang)} %`
                : undefined
            }
          />
          <Stat label={t("stats.fecBytes")} value={n("fec_corrected_bytes")} />
          <Stat
            label={t("fibre.fecUncorrectable")}
            value={n("fec_uncorrectable_codewords")}
            tone={get("onu.fec_uncorrectable_codewords") ? "bad" : undefined}
          />
          <Stat label={t("fibre.ploamRx")} value={n("ploam_rx")} />
          <Stat label={t("stats.ploamCrc")} value={n("ploam_crc_errors")} />
          <Stat label={t("stats.ploamOverflows")} value={n("ploam_overflows")} />
          <Stat label={t("fibre.bwmap")} value={n("bwmap_rx")} />
          <Stat label={t("stats.bwmapCrc")} value={n("bwmap_crc_errors")} />
          <Stat label={t("stats.bwmapOverflows")} value={n("bwmap_overflows")} />
          <Stat label={t("stats.snRequests")} value={n("sn_requests")} />
          <Stat label={t("stats.rangingRequests")} value={n("ranging_requests")} />
        </div>
      </Card>
      <Card title={t("stats.gemTitle")} scroll>
        <table className="table table-sm table-pin-rows">
          <thead>
            <tr>
              <th>{t("fibre.channel")}</th>
              <th className="text-right">{t("stats.rxBytes")}</th>
              <th className="text-right">{t("stats.rxFrames")}</th>
              <th className="text-right">{t("stats.txBytes")}</th>
              <th className="text-right">{t("stats.txFrames")}</th>
              <th className="text-right">{t("fibre.now")}</th>
            </tr>
          </thead>
          <tbody>
            {gems(live).map((g) => {
              const k = `gem_counters.${g.gem.name}`;
              const r = rates?.[g.gem.name];
              return (
                <tr key={g.gem.name}>
                  <td className="tabular font-medium">GEM {g.gem.port_id}</td>
                  <td className="tabular text-right">{bytes(get(`${k}.pon_rx_octets`), lang)}</td>
                  <td className="tabular text-right">{integer(get(`${k}.pon_rx_frames`), lang)}</td>
                  <td className="tabular text-right">{bytes(get(`${k}.pon_tx_octets`), lang)}</td>
                  <td className="tabular text-right">{integer(get(`${k}.pon_tx_frames`), lang)}</td>
                  <td className="tabular text-right">
                    {r ? `↓ ${bitrate(r.down, lang)} ↑ ${bitrate(r.up, lang)}` : "–"}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </Card>
    </div>
  );
}

function IfacesTab({ get, live }: { get: Get; live: Live | undefined }) {
  const { t, lang } = useI18n();
  return (
    <Card title={t("stats.ifacesTitle")} scroll className="min-h-0 flex-1">
      <p className="text-xs text-base-content/65">{t("stats.ifacesHint")}</p>
      <table className="table table-sm table-pin-rows">
        <thead>
          <tr>
            <th>{t("switch.iface")}</th>
            <th>{t("switch.link")}</th>
            <th className="text-right">{t("stats.rxFrames")}</th>
            <th className="text-right">{t("stats.rxBytes")}</th>
            <th className="text-right">{t("stats.txFrames")}</th>
            <th className="text-right">{t("stats.txBytes")}</th>
            <th className="text-right">{t("switch.missed")}</th>
          </tr>
        </thead>
        <tbody>
          {(live?.ports ?? [])
            .slice()
            .sort((a, z) => a.name.localeCompare(z.name, undefined, { numeric: true }))
            .map((p) => {
              const k = `ports.${p.name}`;
              return (
                <tr key={p.name}>
                  <td className="font-mono text-xs">{p.name}</td>
                  <td className="text-xs">
                    {p.operstate === "down" ? t("sfp.down") : t("sfp.up")}
                    {p.speed_mbit > 0 && ` · ${p.speed_mbit} Mb/s`}
                  </td>
                  <td className="tabular text-right">{integer(get(`${k}.rx_packets`), lang)}</td>
                  <td className="tabular text-right">{bytes(get(`${k}.rx_bytes`), lang)}</td>
                  <td className="tabular text-right">{integer(get(`${k}.tx_packets`), lang)}</td>
                  <td className="tabular text-right">{bytes(get(`${k}.tx_bytes`), lang)}</td>
                  <td className="tabular text-right">{integer(get(`${k}.rx_missed`), lang)}</td>
                </tr>
              );
            })}
        </tbody>
      </table>
    </Card>
  );
}

export function Stats() {
  const { t, lang } = useI18n();
  const [tab, setTab] = useTab(TABS);
  const { live, get, base, reset, clear } = useCounts();
  return (
    <Page
      title={t("stats.title")}
      tabs={
        <Tabs
          items={[
            { id: "ports", label: t("stats.tabPorts") },
            { id: "gpon", label: t("stats.tabGpon") },
            { id: "ifaces", label: t("stats.tabIfaces") },
          ]}
          value={tab}
          onChange={setTab}
          label={t("stats.title")}
        />
      }
      actions={
        <span className="flex items-center gap-2">
          <span className="text-xs text-base-content/60">
            {base ? t("stats.since", { time: clock(base.at, lang, true) }) : t("stats.sinceStart")}
          </span>
          {base && (
            <button type="button" className="btn btn-ghost btn-sm" onClick={clear}>
              {t("stats.fromStart")}
            </button>
          )}
          <button type="button" className="btn btn-sm" onClick={reset}>
            <RotateCcw className="size-4" />
            {t("stats.reset")}
          </button>
        </span>
      }
    >
      {tab === "ports" && <PortsTab get={get} />}
      {tab === "gpon" && <GponTab get={get} live={live} />}
      {tab === "ifaces" && <IfacesTab get={get} live={live} />}
    </Page>
  );
}
