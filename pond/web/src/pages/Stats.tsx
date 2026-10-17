// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Every count the stick keeps, in three tabs: the switch's counters of the
 * SFP port and the fibre, in groups (the traffic, the errors and drops, the
 * frame sizes); the GPON line's (the line, FEC, the registration, the
 * messages from the OLT) and each GEM channel's; and every network
 * interface's, with what it is for. Counts run from the stick's start;
 * "count from now" keeps the counts of a moment in this browser, across
 * reloads, and shows what came since, the stick's own counters left as
 * they are.
 */

import { Link } from "@tanstack/react-router";
import { RotateCcw } from "lucide-react";
import { Fragment, type ReactNode, useState } from "react";
import { useLive, useTraffic } from "../api/live";
import type { Live } from "../api/types";
import { Page, Tabs, useTab } from "../components/Page";
import { Card, Detail, Dot, GroupRow, TABLE } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { bitrate, bytes, clock, decimal, integer } from "../lib/format";
import { type GemUse, gems } from "../lib/state";
import { useIfaceText } from "./Switch";

const TABS = ["ports", "gpon", "ifaces"] as const;

/* What each tab shows, under the page's title. */
const HINT: Record<(typeof TABS)[number], Key> = {
  ports: "stats.portsHint",
  gpon: "stats.gponHint",
  ifaces: "stats.ifacesHint",
};

/** Every number of a value, by a path of keys, flattened. */
function flat(v: unknown, prefix = "", out: Record<string, number> = {}): Record<string, number> {
  if (typeof v === "number") out[prefix] = v;
  else if (v && typeof v === "object")
    for (const [k, x] of Object.entries(v)) flat(x, prefix ? `${prefix}.${k}` : k, out);
  return out;
}

type Baseline = { at: number; values: Record<string, number> };

/* The counts at the moment the reader asked to count from, kept in this browser. */
const BASELINE = "pond.stats.baseline";

function stored(): Baseline | null {
  try {
    const v = JSON.parse(localStorage.getItem(BASELINE) ?? "null");
    return v && typeof v.at === "number" && v.values && typeof v.values === "object" ? v : null;
  } catch {
    return null;
  }
}

function store(b: Baseline | null) {
  try {
    if (b) localStorage.setItem(BASELINE, JSON.stringify(b));
    else localStorage.removeItem(BASELINE);
  } catch {
    /* Without storage the counts from now last until the page is left. */
  }
}

let baseline = stored();

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
    store(baseline);
    setBase(baseline);
  };
  const clear = () => {
    baseline = null;
    store(null);
    setBase(null);
  };
  return { live, get, base, reset, clear };
}

type Get = (key: string) => number | null;

/* A count of errors, with a mark when there are any. */
function Errors({ n, children }: { n: number | null; children: ReactNode }) {
  if (!n) return children;
  return (
    <span className="inline-flex items-center gap-1.5">
      <Dot tone="warn" />
      {children}
    </span>
  );
}

/* A count of bytes, and of the frames that carried them under it. */
function Volume({ octets, frames }: { octets: number | null; frames: number | null }) {
  const { t, lang } = useI18n();
  return (
    <span className="grid gap-0.5 whitespace-nowrap">
      <span>{bytes(octets, lang)}</span>
      <Detail>{t("stats.framesN", { n: integer(frames, lang) })}</Detail>
    </span>
  );
}

/* ------------------------------------------------------------- the ports */

type PortRow = { label: Key; rx?: string; tx?: string; bytes?: boolean };

const TRAFFIC: PortRow[] = [
  { label: "switch.cOctets", rx: "rx_octets", tx: "tx_octets", bytes: true },
  { label: "switch.cUnicast", rx: "rx_unicast", tx: "tx_unicast" },
  { label: "switch.cMulticast", rx: "rx_multicast", tx: "tx_multicast" },
  { label: "switch.cBroadcast", rx: "rx_broadcast", tx: "tx_broadcast" },
  { label: "switch.cPause", rx: "rx_pause", tx: "tx_pause" },
];

const ERRORS: PortRow[] = [
  { label: "switch.cDrops", rx: "rx_drops", tx: "tx_discards" },
  { label: "stats.forwardDiscards", rx: "rx_forward_discards" },
  { label: "switch.cCrc", rx: "rx_crc_errors" },
  { label: "switch.cSymbol", rx: "rx_symbol_errors" },
  { label: "switch.cUndersize", rx: "rx_undersize", tx: "tx_undersize" },
  { label: "switch.cFragments", rx: "rx_fragments" },
  { label: "switch.cJabbers", rx: "rx_jabbers" },
  { label: "stats.unknownOpcodes", rx: "rx_unknown_opcodes" },
  { label: "stats.collisions", tx: "tx_collisions" },
];

const SIZES: PortRow[] = [
  { label: "stats.size64", rx: "rx_64_octets", tx: "tx_64_octets" },
  { label: "stats.size127", rx: "rx_65_127_octets", tx: "tx_65_127_octets" },
  { label: "stats.size255", rx: "rx_128_255_octets", tx: "tx_128_255_octets" },
  { label: "stats.size511", rx: "rx_256_511_octets", tx: "tx_256_511_octets" },
  { label: "stats.size1023", rx: "rx_512_1023_octets", tx: "tx_512_1023_octets" },
  { label: "stats.size1518", rx: "rx_1024_1518_octets", tx: "tx_1024_1518_octets" },
  { label: "stats.sizeMax", rx: "rx_1519_max_octets", tx: "tx_1519_max_octets" },
  { label: "switch.cOversize", rx: "rx_oversize", tx: "tx_oversize" },
];

const PORTS = ["sfp", "pon"] as const;

/*
 * The switch's counters of its two ports, the host's and the operator's, in
 * groups. Errors that are all zero fold into a line that names them, and
 * the frame sizes into their group's name, each opened on asking.
 */
function PortsTab({ get }: { get: Get }) {
  const { t, lang } = useI18n();
  const [allErrors, setAllErrors] = useState(false);
  const [sizes, setSizes] = useState(false);
  const n = (port: string, key: string | undefined) =>
    key ? get(`port_counters.${port}.${key}`) : null;
  const row = (r: PortRow, errors?: boolean) => (
    <tr key={r.label}>
      <td>{t(r.label)}</td>
      {PORTS.flatMap((p) =>
        (["rx", "tx"] as const).map((dir) => {
          const key = r[dir];
          const v = n(p, key);
          const text = !key ? "–" : r.bytes ? bytes(v, lang) : integer(v, lang);
          return (
            <td key={`${p}-${dir}`} className="tabular text-right">
              {errors ? <Errors n={v}>{text}</Errors> : text}
            </td>
          );
        }),
      )}
    </tr>
  );
  const some = (r: PortRow) => PORTS.some((p) => (n(p, r.rx) ?? 0) > 0 || (n(p, r.tx) ?? 0) > 0);
  const zero = ERRORS.filter((r) => !some(r));
  return (
    <Card scroll className="min-h-0">
      <table className={TABLE}>
        <thead>
          <tr>
            <th rowSpan={2} />
            <th colSpan={2} className="text-center">
              {t("switch.hostSide")} · {t("switch.sfpPort")}
            </th>
            <th colSpan={2} className="text-center">
              {t("switch.operatorSide")} · {t("switch.fibreSide")}
            </th>
          </tr>
          <tr>
            {PORTS.flatMap((p) => [
              <th key={`${p}rx`} className="text-right font-normal">
                {t("stats.received")}
              </th>,
              <th key={`${p}tx`} className="text-right font-normal">
                {t("stats.sent")}
              </th>,
            ])}
          </tr>
        </thead>
        <tbody>
          <GroupRow span={5} first>
            {t("stats.groupTraffic")}
          </GroupRow>
          {TRAFFIC.map((r) => row(r))}
          <GroupRow span={5}>{t("stats.groupErrors")}</GroupRow>
          {ERRORS.filter((r) => allErrors || some(r)).map((r) => row(r, true))}
          {zero.length > 0 && (
            <tr>
              <td colSpan={5} className="text-base-content/70">
                {!allErrors &&
                  `${t("stats.noErrors", { list: zero.map((r) => t(r.label)).join(" · ") })} `}
                <button
                  type="button"
                  className="link link-hover link-primary"
                  onClick={() => setAllErrors((v) => !v)}
                >
                  {allErrors ? t("stats.hideZeros") : t("stats.showAll")}
                </button>
              </td>
            </tr>
          )}
          <GroupRow span={5}>
            {t("stats.groupSizes")}
            <button
              type="button"
              className="link link-hover link-primary ml-3 font-normal"
              aria-expanded={sizes}
              onClick={() => setSizes((v) => !v)}
            >
              {sizes ? t("stats.collapse") : t("stats.showRows", { n: SIZES.length })}
            </button>
          </GroupRow>
          {sizes && SIZES.map((r) => row(r))}
        </tbody>
      </table>
    </Card>
  );
}

/* --------------------------------------------------------------- the line */

const LINE: {
  group: Key;
  rows: { label: Key; key: string; error?: boolean; share?: boolean }[];
}[] = [
  {
    group: "stats.groupLine",
    rows: [
      { label: "fibre.bipErrors", key: "bip_errors", error: true },
      { label: "fibre.gemFrames", key: "gem_frames" },
    ],
  },
  {
    group: "stats.groupFec",
    rows: [
      { label: "stats.fecCodewords", key: "fec_codewords" },
      { label: "fibre.fecCorrected", key: "fec_corrected_codewords", share: true },
      { label: "stats.fecBytes", key: "fec_corrected_bytes" },
      { label: "fibre.fecUncorrectable", key: "fec_uncorrectable_codewords", error: true },
    ],
  },
  {
    group: "stats.groupRegistration",
    rows: [
      { label: "stats.snRequests", key: "sn_requests" },
      { label: "stats.rangingRequests", key: "ranging_requests" },
    ],
  },
];

/* What a channel carries, in a few words. */
const GEM_USE: Record<GemUse, Key> = {
  session: "fibre.chanSession",
  bridged: "fibre.svcBridged",
  ends: "fibre.svcEnds",
};

const MESSAGES: { label: Key; rx: string; crc: string; lost: string }[] = [
  { label: "fibre.ploamRx", rx: "ploam_rx", crc: "ploam_crc_errors", lost: "ploam_overflows" },
  { label: "fibre.bwmap", rx: "bwmap_rx", crc: "bwmap_crc_errors", lost: "bwmap_overflows" },
];

/*
 * The GPON line's counters, in groups; at their right each GEM channel's,
 * with what it carries, and under those the messages from the OLT, each
 * kind with how many came, with a CRC error, and lost.
 */
function GponTab({ get, live }: { get: Get; live: Live | undefined }) {
  const { t, lang } = useI18n();
  const rates = useTraffic().at(-1)?.gems;
  const n = (k: string) => get(`onu.${k}`);
  const codewords = n("fec_codewords");
  const corrected = n("fec_corrected_codewords");
  return (
    <div className="grid min-h-0 flex-1 items-start gap-4 lg:grid-cols-[24rem_minmax(0,1fr)] lg:grid-rows-[minmax(0,1fr)]">
      <Card title={t("stats.lineTitle")} scroll className="max-h-full">
        <table className={TABLE}>
          <tbody>
            {LINE.map((g, i) => (
              <Fragment key={g.group}>
                <GroupRow span={2} first={i === 0}>
                  {t(g.group)}
                </GroupRow>
                {g.rows.map((r) => {
                  const v = n(r.key);
                  return (
                    <tr key={r.key}>
                      <td>{t(r.label)}</td>
                      <td className="tabular text-right">
                        <span className="grid gap-0.5">
                          <span>
                            {r.error ? <Errors n={v}>{integer(v, lang)}</Errors> : integer(v, lang)}
                          </span>
                          {r.share && codewords != null && codewords > 0 && corrected != null && (
                            <Detail>{decimal((corrected / codewords) * 100, 6, lang)} %</Detail>
                          )}
                        </span>
                      </td>
                    </tr>
                  );
                })}
              </Fragment>
            ))}
          </tbody>
        </table>
      </Card>
      <div className="flex max-h-full min-h-0 flex-col gap-4">
        <Card title={t("stats.gemTitle")} scroll>
          <table className={TABLE}>
            <thead>
              <tr>
                <th>{t("fibre.channel")}</th>
                <th>{t("switch.vlanUse")}</th>
                <th className="text-right">{t("stats.received")}</th>
                <th className="text-right">{t("stats.sent")}</th>
                <th className="text-right">{t("fibre.now")}</th>
              </tr>
            </thead>
            <tbody>
              {gems(live).map((g) => {
                const k = `gem_counters.${g.gem.name}`;
                const r = rates?.[g.gem.name];
                const down = g.gem.multicast || g.gem.direction === "downstream";
                return (
                  <tr key={g.gem.name}>
                    <td>
                      <Link
                        to="/operator/channels"
                        className="link link-hover link-primary tabular whitespace-nowrap"
                      >
                        GEM {g.gem.port_id}
                      </Link>
                    </td>
                    <td>{t(g.gem.multicast ? "fibre.chanMulticast" : GEM_USE[g.use])}</td>
                    <td className="tabular text-right">
                      <Volume
                        octets={get(`${k}.pon_rx_octets`)}
                        frames={get(`${k}.pon_rx_frames`)}
                      />
                    </td>
                    <td className="tabular text-right">
                      {down ? (
                        "–"
                      ) : (
                        <Volume
                          octets={get(`${k}.pon_tx_octets`)}
                          frames={get(`${k}.pon_tx_frames`)}
                        />
                      )}
                    </td>
                    <td className="tabular text-right whitespace-nowrap">
                      {r ? (
                        <span className="grid gap-0.5">
                          <span>↓ {bitrate(r.down, lang)}</span>
                          {!down && <span>↑ {bitrate(r.up, lang)}</span>}
                        </span>
                      ) : (
                        "–"
                      )}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </Card>
        <Card title={t("stats.messagesTitle")} className="shrink-0">
          <table className={TABLE}>
            <thead>
              <tr>
                <th />
                <th className="text-right">{t("stats.received")}</th>
                <th className="text-right">{t("stats.crc")}</th>
                <th className="text-right">{t("stats.lost")}</th>
              </tr>
            </thead>
            <tbody>
              {MESSAGES.map((m) => (
                <tr key={m.label}>
                  <td>{t(m.label)}</td>
                  <td className="tabular text-right">{integer(n(m.rx), lang)}</td>
                  <td className="tabular text-right">
                    <Errors n={n(m.crc)}>{integer(n(m.crc), lang)}</Errors>
                  </td>
                  <td className="tabular text-right">
                    <Errors n={n(m.lost)}>{integer(n(m.lost), lang)}</Errors>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </Card>
      </div>
    </div>
  );
}

/* ---------------------------------------------------------- the interfaces */

/* Every network interface's counters, with what it is for and its link. */
function IfacesTab({ get, live }: { get: Get; live: Live | undefined }) {
  const { t, lang } = useI18n();
  const what = useIfaceText();
  const list = (live?.ports ?? [])
    .map((p) => ({ p, ...what(p.name) }))
    .sort(
      (a, z) => a.rank - z.rank || a.p.name.localeCompare(z.p.name, undefined, { numeric: true }),
    );
  return (
    <Card scroll className="min-h-0">
      <table className={`${TABLE} table-pin-rows`}>
        <thead>
          <tr>
            <th>{t("switch.iface")}</th>
            <th>{t("switch.vlanUse")}</th>
            <th>{t("switch.link")}</th>
            <th className="text-right">{t("stats.received")}</th>
            <th className="text-right">{t("stats.sent")}</th>
            <th className="text-right">{t("switch.missed")}</th>
          </tr>
        </thead>
        <tbody>
          {list.map(({ p, text }) => {
            const k = `ports.${p.name}`;
            const up = p.operstate !== "down";
            const missed = get(`${k}.rx_missed`);
            return (
              <tr key={p.name}>
                <td className="font-mono text-xs">{p.name}</td>
                <td>{text ?? "–"}</td>
                <td>
                  <span className="grid gap-0.5">
                    <span className="flex items-center gap-2">
                      <Dot tone={up ? "good" : "neutral"} />
                      {up ? t("sfp.up") : t("sfp.down")}
                    </span>
                    {p.speed_mbit > 0 && <Detail>{p.speed_mbit} Mb/s</Detail>}
                  </span>
                </td>
                <td className="tabular text-right">
                  <Volume octets={get(`${k}.rx_bytes`)} frames={get(`${k}.rx_packets`)} />
                </td>
                <td className="tabular text-right">
                  <Volume octets={get(`${k}.tx_bytes`)} frames={get(`${k}.tx_packets`)} />
                </td>
                <td className="tabular text-right">
                  <Errors n={missed}>{integer(missed, lang)}</Errors>
                </td>
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
      hint={t(HINT[tab])}
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
          <span className="text-xs text-base-content/70">
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
