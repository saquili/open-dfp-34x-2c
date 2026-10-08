// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The switch, in five tabs. Ports: the ports as on a front panel, the SFP
 * port to the host, the processor and the fibre with its GEM channels, each
 * with its link and traffic; a click shows a port's counters and the
 * interfaces on it. VLANs: which VLAN crosses which port, tagged or not, and
 * what each is for. The MAC table, the routes and neighbours, and the rules
 * the hardware applies as frames come in.
 */

import { Cpu, EthernetPort, Radio, Search } from "lucide-react";
import { type ReactNode, useMemo, useState } from "react";
import { ApiError, OfflineError } from "../api/client";
import { useBridge, useNetwork, useOmci, useTcRules } from "../api/config";
import { useLive, useTraffic } from "../api/live";
import type { Neighbour, PortCounters, Route, TcAction, TcRule } from "../api/types";
import { useGemText } from "../components/Gem";
import { Page, Tabs, useTab } from "../components/Page";
import { Badge, Card, Dot, Facts, Stat } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { bitrate, bytes, integer } from "../lib/format";
import { Mib, services } from "../lib/omci";
import { gems, laneMode, linkSpeed, port } from "../lib/state";
import { type IfaceView, isSystemEntry, type Role, shownRoutes, switchView } from "../lib/switch";
import { type Member, type VlanRole, vlanMatrix } from "../lib/vlans";

const TABS = ["ports", "vlans", "mac", "routing", "rules"] as const;

const ROLE: Record<Role, Key> = {
  management: "switch.roleManagement",
  handover: "switch.roleHandover",
  wan: "switch.roleWan",
  session: "switch.roleSession",
  bridged: "switch.roleBridged",
  channel: "switch.roleChannel",
  unused: "switch.roleUnused",
};

const VLAN_ROLE: Record<VlanRole, Key> = {
  management: "switch.roleManagement",
  handover: "switch.roleHandover",
  session: "switch.roleWan",
  bridged: "switch.vlanBridged",
  other: "switch.roleUnused",
};

const STATE: Record<Neighbour["state"], Key> = {
  reachable: "switch.stateReachable",
  stale: "switch.stateStale",
  permanent: "switch.statePermanent",
  static: "switch.stateStatic",
  incomplete: "switch.stateIncomplete",
  failed: "switch.stateFailed",
  none: "switch.stateNone",
};

const PROTOCOL: Record<Route["protocol"], Key> = {
  kernel: "switch.protoKernel",
  static: "switch.protoStatic",
  dhcp: "switch.protoDhcp",
  ra: "switch.protoRa",
  other: "switch.protoOther",
};

const IP_PROTO: Record<number, string> = { 1: "ICMP", 6: "TCP", 17: "UDP", 58: "ICMPv6" };

function failure(t: (k: Key, v?: Record<string, string>) => string, e: Error) {
  return e instanceof OfflineError
    ? t("connection.offline")
    : t("switch.unreadable", {
        reason: e instanceof ApiError ? (e.detail ?? e.message) : e.message,
      });
}

/* ------------------------------------------------------------------- ports */

type PortId = "sfp" | "cpu" | "pon" | string;

function PortTile({
  icon,
  title,
  sub,
  up,
  rate,
  selected,
  onSelect,
  small = false,
}: {
  icon?: ReactNode;
  title: string;
  sub: string;
  up: boolean;
  rate: string;
  selected: boolean;
  onSelect: () => void;
  small?: boolean;
}) {
  return (
    <button
      type="button"
      onClick={onSelect}
      className={`grid gap-1 rounded-box border text-left transition-colors ${small ? "px-3 py-2" : "p-4"} ${selected ? "border-primary bg-primary/5 ring-1 ring-primary" : "border-base-300 bg-base-100 hover:border-primary/60"}`}
    >
      <span className="flex items-center gap-2">
        <Dot tone={up ? "good" : "bad"} />
        {icon}
        <span className={`font-semibold ${small ? "text-sm" : ""}`}>{title}</span>
      </span>
      <span className="text-xs text-base-content/65">{sub}</span>
      <span className="tabular text-xs">{rate}</span>
    </button>
  );
}

const COUNTER_ROWS: { label: Key; rx?: string; tx?: string; bytes?: boolean }[] = [
  { label: "switch.cOctets", rx: "rx_octets", tx: "tx_octets", bytes: true },
  { label: "switch.cUnicast", rx: "rx_unicast", tx: "tx_unicast" },
  { label: "switch.cMulticast", rx: "rx_multicast", tx: "tx_multicast" },
  { label: "switch.cBroadcast", rx: "rx_broadcast", tx: "tx_broadcast" },
  { label: "switch.cPause", rx: "rx_pause", tx: "tx_pause" },
  { label: "switch.cDrops", rx: "rx_drops", tx: "tx_discards" },
  { label: "switch.cCrc", rx: "rx_crc_errors" },
  { label: "switch.cSymbol", rx: "rx_symbol_errors" },
  { label: "switch.cUndersize", rx: "rx_undersize", tx: "tx_undersize" },
  { label: "switch.cOversize", rx: "rx_oversize", tx: "tx_oversize" },
  { label: "switch.cFragments", rx: "rx_fragments" },
  { label: "switch.cJabbers", rx: "rx_jabbers" },
];

const SIZES = ["64", "65_127", "128_255", "256_511", "512_1023", "1024_1518", "1519_max"];

function Counters({ c }: { c: PortCounters | undefined }) {
  const { t, lang } = useI18n();
  const v = (k?: string, asBytes?: boolean) =>
    k == null ? "" : asBytes ? bytes(c?.[k], lang) : integer(c?.[k], lang);
  const sizes = SIZES.map((s) => ({
    s,
    rx: c?.[`rx_${s}_octets`] ?? 0,
    tx: c?.[`tx_${s}_octets`] ?? 0,
  }));
  const peak = Math.max(1, ...sizes.map((x) => x.rx + x.tx));
  return (
    <div className="grid gap-4 xl:grid-cols-[minmax(0,1fr)_14rem]">
      <table className="table table-xs">
        <thead>
          <tr>
            <th />
            <th className="text-right">{t("switch.rx")}</th>
            <th className="text-right">{t("switch.tx")}</th>
          </tr>
        </thead>
        <tbody>
          {COUNTER_ROWS.map((r) => (
            <tr key={r.label}>
              <td>{t(r.label)}</td>
              <td className="tabular text-right">{v(r.rx, r.bytes)}</td>
              <td className="tabular text-right">{v(r.tx, r.bytes)}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <div className="grid content-start gap-1">
        <div className="text-xs font-semibold text-base-content/70">{t("switch.sizes")}</div>
        {sizes.map((x) => (
          <div
            key={x.s}
            className="grid grid-cols-[4.5rem_minmax(0,1fr)] items-center gap-2 text-xs"
          >
            <span className="tabular text-base-content/65">
              {x.s.replace("_", "–").replace("max", "…")}
            </span>
            <span className="h-2 overflow-hidden rounded-full bg-base-300">
              <span
                className="block h-full bg-primary/70"
                style={{ width: `${Math.round(((x.rx + x.tx) / peak) * 100)}%` }}
              />
            </span>
          </div>
        ))}
      </div>
    </div>
  );
}

function Ifaces({ list, depth = 0 }: { list: IfaceView[]; depth?: number }) {
  const { t } = useI18n();
  if (!list.length) return null;
  return (
    <ul className="grid gap-1">
      {list.map((v) => (
        <li key={v.link.name} className="grid gap-1" style={{ marginLeft: `${depth * 0.9}rem` }}>
          <div className="flex flex-wrap items-baseline gap-x-2 rounded-box border border-base-300 px-3 py-1.5 text-sm">
            <span className="font-mono font-medium">{v.link.name}</span>
            {v.link.kind === "vlan" && <Badge tone="info">VLAN {v.link.vlan}</Badge>}
            <span className="text-base-content/70">
              {t(ROLE[v.role], { bridge: v.link.master ?? "" })}
            </span>
            <span className="tabular ml-auto text-xs text-base-content/60">
              {v.link.addresses
                .filter((a) => !a.link_local)
                .map((a) => a.address)
                .join(", ")}
            </span>
          </div>
          <Ifaces list={v.children} depth={depth + 1} />
        </li>
      ))}
    </ul>
  );
}

function PortsTab() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const net = useNetwork();
  const rate = useTraffic().at(-1);
  const gem = useGemText();
  const [sel, setSel] = useState<PortId>("sfp");
  const view = switchView(net.data, live);
  const channels = gems(live);
  const sfp = port(live, "sfp");
  const pon = port(live, "pon");
  const cpu = port(live, "cpu");
  const rates = (d?: number, u?: number) => `↓ ${bitrate(d ?? 0, lang)} ↑ ${bitrate(u ?? 0, lang)}`;
  const g = channels.find((x) => x.gem.name === sel);
  const portOf = (name: string) => view.ports.find((p) => p.link.name === name);

  let detail: ReactNode;
  if (sel === "sfp" || sel === "pon") {
    const p = sel === "sfp" ? sfp : pon;
    detail = (
      <>
        <div className="grid grid-cols-2 gap-3 sm:grid-cols-4">
          <Stat
            label={t("switch.link")}
            value={p?.carrier ? t("sfp.up") : t("sfp.down")}
            tone={p?.carrier ? "good" : "bad"}
          />
          <Stat
            label={t("sfp.speed")}
            value={
              sel === "sfp"
                ? (linkSpeed(p && p.speed_mbit > 0 ? p.speed_mbit : null, lang) ?? "–")
                : "GPON"
            }
            sub={sel === "sfp" ? (laneMode(live) ?? undefined) : t("switch.ponInternal")}
          />
          <Stat label="MTU" value={p?.mtu ?? "–"} />
          <Stat label="MAC" value={<span className="font-mono text-sm">{p?.mac ?? "–"}</span>} />
        </div>
        <Counters c={live?.port_counters?.[sel as "sfp" | "pon"]} />
        <div className="grid gap-2">
          <div className="text-sm font-semibold">{t("switch.ifacesOn")}</div>
          <Ifaces list={(portOf(sel)?.ifaces ?? []).filter((v) => v.link.kind !== "gem")} />
        </div>
      </>
    );
  } else if (sel === "cpu") {
    const c = cpu?.cpu_path;
    detail = (
      <>
        <p className="text-sm text-base-content/70">{t("switch.cpuHint")}</p>
        <div className="grid grid-cols-2 gap-3 sm:grid-cols-4">
          <Stat
            label={t("switch.cpuFrames")}
            value={integer(rate?.cpuFrames ?? 0, lang)}
            sub={t("switch.perSecond")}
          />
          <Stat label={t("switch.cpuLimit")} value="1 Gb/s" />
          <Stat
            label={t("switch.rxFrames")}
            value={integer(c?.rx_packets, lang)}
            sub={bytes(c?.rx_bytes, lang)}
          />
          <Stat
            label={t("switch.txFrames")}
            value={integer(c?.tx_packets, lang)}
            sub={bytes(c?.tx_bytes, lang)}
          />
          <Stat
            label={t("switch.missed")}
            value={integer(c?.rx_missed, lang)}
            tone={c?.rx_missed ? "warn" : undefined}
          />
        </div>
      </>
    );
  } else if (g) {
    const c = live?.gem_counters?.[g.gem.name];
    const r = rate?.gems[g.gem.name];
    detail = (
      <>
        <Facts
          rows={[
            [t("fibre.direction"), gem.direction(g.gem)],
            [t("fibre.allocId"), g.allocId ?? "–"],
            [t("fibre.use"), gem.use(g)],
            [t("fibre.now"), rates(r?.down, r?.up)],
            [
              t("fibre.received"),
              `${bytes(c?.pon_rx_octets, lang)} · ${integer(c?.pon_rx_frames, lang)} ${t("switch.frames")}`,
            ],
            [
              t("fibre.sent"),
              `${bytes(c?.pon_tx_octets, lang)} · ${integer(c?.pon_tx_frames, lang)} ${t("switch.frames")}`,
            ],
          ]}
        />
        <div className="grid gap-2">
          <div className="text-sm font-semibold">{t("switch.ifacesOn")}</div>
          <Ifaces
            list={
              portOf("pon")
                ?.ifaces.filter((v) => v.link.name === g.gem.name)
                .flatMap((v) => v.children) ?? []
            }
          />
        </div>
      </>
    );
  }

  const selTitle =
    sel === "sfp"
      ? t("switch.portSfp")
      : sel === "pon"
        ? t("switch.portPon")
        : sel === "cpu"
          ? t("switch.conduit")
          : g
            ? `GEM ${g.gem.port_id}`
            : sel;

  return (
    <div className="grid min-h-0 flex-1 gap-4 lg:grid-cols-[minmax(0,5fr)_minmax(0,6fr)]">
      <Card title={t("switch.panel")} scroll>
        <p className="text-xs text-base-content/65">{t("switch.panelHint")}</p>
        <div className="grid gap-3 sm:grid-cols-[minmax(0,1fr)_auto_minmax(0,1fr)] sm:items-center">
          <PortTile
            icon={<EthernetPort className="size-4 text-primary" />}
            title={t("switch.portSfp")}
            sub={`${t("switch.hostSide")} · ${linkSpeed(sfp && sfp.speed_mbit > 0 ? sfp.speed_mbit : null, lang) ?? "–"}`}
            up={sfp?.carrier === 1}
            rate={rates(rate?.lanDown, rate?.lanUp)}
            selected={sel === "sfp"}
            onSelect={() => setSel("sfp")}
          />
          <div className="hidden text-center text-xs text-base-content/50 sm:block">
            RTL9602C
            <div className="h-px w-10 bg-base-300" />
          </div>
          <PortTile
            icon={<Radio className="size-4 text-primary" />}
            title={t("switch.portPon")}
            sub={`${t("switch.operatorSide")} · GPON`}
            up={pon?.carrier === 1}
            rate={rates(rate?.wanDown, rate?.wanUp)}
            selected={sel === "pon"}
            onSelect={() => setSel("pon")}
          />
        </div>
        <div className="grid sm:grid-cols-[minmax(0,1fr)_auto_minmax(0,1fr)]">
          <div className="hidden sm:block" />
          <PortTile
            icon={<Cpu className="size-4 text-primary" />}
            title={t("switch.conduit")}
            sub={t("switch.cpuSub")}
            up
            rate={t("diagram.frames", { n: integer(rate?.cpuFrames ?? 0, lang) })}
            selected={sel === "cpu"}
            onSelect={() => setSel("cpu")}
          />
        </div>
        <div className="grid gap-2">
          <div className="text-xs font-semibold uppercase tracking-wide text-base-content/55">
            {t("switch.channels", { n: channels.length })}
          </div>
          <div className="grid grid-cols-2 gap-2 md:grid-cols-3">
            {channels.map((v) => {
              const r = rate?.gems[v.gem.name];
              return (
                <PortTile
                  key={v.gem.name}
                  small
                  title={`GEM ${v.gem.port_id}`}
                  sub={gem.short(v)}
                  up
                  rate={rates(r?.down, r?.up)}
                  selected={sel === v.gem.name}
                  onSelect={() => setSel(v.gem.name)}
                />
              );
            })}
          </div>
        </div>
      </Card>
      <Card title={selTitle} scroll>
        {detail}
      </Card>
    </div>
  );
}

/* ------------------------------------------------------------------- VLANs */

const MEMBER: Record<Member, { mark: string; cls: string; key: Key }> = {
  tagged: { mark: "T", cls: "bg-info/15 text-info", key: "switch.mTagged" },
  untagged: { mark: "U", cls: "bg-success/15 text-success", key: "switch.mUntagged" },
  pvid: { mark: "U*", cls: "bg-success/25 text-success", key: "switch.mPvid" },
};

function VlansTab() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const net = useNetwork();
  const bridge = useBridge();
  const omci = useOmci();
  const translations = useMemo(
    () =>
      services(new Mib(omci.data)).flatMap((s) =>
        s.hostVlans.map((h) => ({ host: h, fibre: s.vlan })),
      ),
    [omci.data],
  );
  const m = vlanMatrix(net.data, bridge.data, live, translations);
  if (net.error) return <p className="text-sm text-error">{failure(t, net.error)}</p>;

  return (
    <Card title={t("switch.vlanMatrix")} scroll className="min-h-0 flex-1">
      <p className="text-xs text-base-content/65">{t("switch.vlanMatrixHint")}</p>
      <div className="flex flex-wrap gap-3 text-xs">
        {(Object.keys(MEMBER) as Member[]).map((k) => (
          <span key={k} className="flex items-center gap-1">
            <span className={`rounded px-1.5 py-0.5 font-semibold ${MEMBER[k].cls}`}>
              {MEMBER[k].mark}
            </span>
            {t(MEMBER[k].key)}
          </span>
        ))}
        <span className="flex items-center gap-1">
          <span className="rounded px-1.5 py-0.5 text-base-content/40">·</span>
          {t("switch.mNone")}
        </span>
      </div>
      <table className="table table-sm table-pin-rows">
        <thead>
          <tr>
            <th>VLAN</th>
            <th>{t("switch.vlanUse")}</th>
            {m.columns.map((c) => (
              <th
                key={c.id}
                className={`text-center ${c.side === "host" ? "bg-primary/5" : c.side === "stick" ? "bg-base-200" : ""}`}
              >
                <div className="tabular whitespace-nowrap">{c.label}</div>
                <div className="text-[10px] font-normal text-base-content/50">
                  {c.side === "host"
                    ? t("switch.hostSide")
                    : c.side === "stick"
                      ? t("switch.stick")
                      : t("switch.operatorSide")}
                </div>
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {m.rows.map((r) => (
            <tr key={r.vid}>
              <td className="tabular font-semibold">
                {r.vid}
                {r.peer != null && (
                  <span className="ml-1 text-xs font-normal text-base-content/60">⇄ {r.peer}</span>
                )}
              </td>
              <td className="text-sm">{t(VLAN_ROLE[r.role])}</td>
              {m.columns.map((c) => {
                const mem = r.cells[c.id];
                return (
                  <td key={c.id} className="text-center">
                    {mem ? (
                      <span
                        className={`rounded px-1.5 py-0.5 text-xs font-semibold ${MEMBER[mem].cls}`}
                      >
                        {MEMBER[mem].mark}
                      </span>
                    ) : (
                      <span className="text-base-content/30">·</span>
                    )}
                  </td>
                );
              })}
            </tr>
          ))}
        </tbody>
      </table>
    </Card>
  );
}

/* --------------------------------------------------------------- MAC table */

function MacTab() {
  const { t } = useI18n();
  const bridge = useBridge();
  const [system, setSystem] = useState(false);
  const [q, setQ] = useState("");
  const fdb = (bridge.data?.fdb ?? [])
    .filter((e) => system || !isSystemEntry(e))
    .filter(
      (e) => !q || `${e.addr} ${e.dev} ${e.vlan ?? ""}`.toLowerCase().includes(q.toLowerCase()),
    );

  return (
    <Card
      title={t("switch.fdb")}
      scroll
      className="min-h-0 flex-1"
      action={
        <span className="flex items-center gap-3">
          <label className="input input-sm w-48">
            <Search className="size-4 opacity-50" />
            <input
              type="search"
              placeholder={t("switch.search")}
              value={q}
              onInput={(e) => setQ(e.currentTarget.value)}
            />
          </label>
          <label className="label cursor-pointer gap-2 text-sm">
            <input
              type="checkbox"
              className="toggle toggle-sm"
              checked={system}
              onChange={(e) => setSystem(e.currentTarget.checked)}
            />
            {t("switch.showSystem")}
          </label>
        </span>
      }
    >
      <p className="text-xs text-base-content/65">{t("switch.fdbHint")}</p>
      {bridge.error ? (
        <p className="text-sm text-error">{failure(t, bridge.error)}</p>
      ) : (
        <table className="table table-sm table-pin-rows">
          <thead>
            <tr>
              <th>{t("switch.mac")}</th>
              <th>{t("switch.iface")}</th>
              <th>{t("switch.vlan")}</th>
              <th>{t("switch.entry")}</th>
            </tr>
          </thead>
          <tbody>
            {fdb.map((e, i) => (
              // biome-ignore lint/suspicious/noArrayIndexKey: one MAC can sit in several rows
              <tr key={i}>
                <td className="font-mono text-xs">{e.addr ?? "–"}</td>
                <td className="font-mono text-xs">
                  {e.dev ?? "–"}
                  {e.master && <span className="text-base-content/50"> ({e.master})</span>}
                </td>
                <td className="tabular">{e.vlan ?? "–"}</td>
                <td className="flex flex-wrap gap-1">
                  {e.learned ? t("switch.learned") : t("switch.fixed")}
                  {e.offloaded && <Badge tone="good">{t("switch.inHw")}</Badge>}
                </td>
              </tr>
            ))}
            {!fdb.length && bridge.data && (
              <tr>
                <td colSpan={4} className="text-sm">
                  {t("switch.none")}
                </td>
              </tr>
            )}
          </tbody>
        </table>
      )}
    </Card>
  );
}

/* ----------------------------------------------------------------- routing */

function RoutingTab() {
  const { t } = useI18n();
  const net = useNetwork();
  const routes = shownRoutes(net.data?.routes ?? []);
  return (
    <div className="grid min-h-0 flex-1 gap-4 xl:grid-cols-2">
      <Card title={t("switch.routes")} scroll>
        <table className="table table-sm">
          <thead>
            <tr>
              <th>{t("switch.destination")}</th>
              <th>{t("switch.via")}</th>
              <th>{t("switch.iface")}</th>
              <th>{t("switch.origin")}</th>
            </tr>
          </thead>
          <tbody>
            {routes.map((r) => (
              <tr key={`${r.family}-${r.destination}-${r.dev}`}>
                <td className="tabular">
                  {r.destination === "default"
                    ? `${t("switch.everything")} (${r.family === "ipv4" ? "IPv4" : "IPv6"})`
                    : r.destination}
                  {r.offloaded && (
                    <span className="ml-1">
                      <Badge tone="good">{t("switch.inHw")}</Badge>
                    </span>
                  )}
                </td>
                <td className="tabular">{r.gateway ?? t("switch.direct")}</td>
                <td className="font-mono text-xs">{r.dev ?? "–"}</td>
                <td>{t(PROTOCOL[r.protocol])}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </Card>
      <Card title={t("switch.neighbours")} scroll>
        <p className="text-xs text-base-content/65">{t("switch.neighboursHint")}</p>
        <table className="table table-sm">
          <thead>
            <tr>
              <th>{t("switch.address")}</th>
              <th>{t("switch.mac")}</th>
              <th>{t("switch.iface")}</th>
              <th>{t("switch.state")}</th>
            </tr>
          </thead>
          <tbody>
            {(net.data?.neighbours ?? []).map((n) => (
              <tr key={`${n.dev}-${n.address}`}>
                <td className="tabular">{n.address}</td>
                <td className="font-mono text-xs">{n.mac ?? "–"}</td>
                <td className="font-mono text-xs">{n.dev ?? "–"}</td>
                <td>{t(STATE[n.state])}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </Card>
    </div>
  );
}

/* ------------------------------------------------------------------- rules */

function ruleWhen(r: TcRule, t: (k: Key, v?: Record<string, string>) => string): string {
  const m = r.match ?? {};
  const parts = [
    m.vlan_id != null && t("switch.vlanN", { id: String(m.vlan_id) }),
    m.eth_dst && `→ ${m.eth_dst}`,
    m.eth_src && `${m.eth_src} →`,
    m.ipv4_src && `${m.ipv4_src}${m.ipv4_src_mask ? `/${m.ipv4_src_mask}` : ""} →`,
    m.ipv4_dst && `→ ${m.ipv4_dst}${m.ipv4_dst_mask ? `/${m.ipv4_dst_mask}` : ""}`,
    m.ip_proto != null && (IP_PROTO[m.ip_proto] ?? `IP ${m.ip_proto}`),
  ].filter(Boolean);
  return parts.length ? parts.join(", ") : t("switch.anyFrame");
}

function actionText(a: TcAction, t: (k: Key, v?: Record<string, string>) => string): string {
  if (a.kind === "vlan") {
    const id = String(a.vlan_id ?? "");
    if (a.vlan_action === "pop") return t("switch.actPop");
    if (a.vlan_action === "push") return t("switch.actPush", { id });
    if (a.vlan_action === "modify") return t("switch.actModify", { id });
  }
  if (a.kind === "gact") return t("switch.actGact");
  if (a.kind === "mirred") return t("switch.actRedirect");
  if (a.kind === "police") return t("switch.actPolice");
  return a.kind;
}

function RulesTab() {
  const { t } = useI18n();
  const tc = useTcRules();
  if (tc.error) return <p className="text-sm text-error">{failure(t, tc.error)}</p>;
  return (
    <div className="grid min-h-0 flex-1 gap-4 lg:grid-cols-2">
      {Object.entries(tc.data ?? {}).map(([name, v]) => {
        const list = "error" in v ? [] : v.ingress.filter((r) => r.match || r.actions);
        return (
          <Card
            key={name}
            scroll
            title={
              name === "sfp" ? t("switch.rulesSfp") : name === "pon" ? t("switch.rulesPon") : name
            }
          >
            {"error" in v ? (
              <p className="text-sm text-error">{t("switch.unreadable", { reason: v.error })}</p>
            ) : list.length ? (
              <table className="table table-sm">
                <thead>
                  <tr>
                    <th>{t("switch.prio")}</th>
                    <th>{t("switch.ruleWhen")}</th>
                    <th>{t("switch.ruleThen")}</th>
                    <th />
                  </tr>
                </thead>
                <tbody>
                  {list.map((r) => (
                    <tr key={`${r.priority}-${r.handle}`}>
                      <td className="tabular">{r.priority}</td>
                      <td>{ruleWhen(r, t)}</td>
                      <td>
                        {(r.actions ?? []).map((a) => actionText(a, t)).join(", ") ||
                          t("switch.actPass")}
                      </td>
                      <td>
                        <Badge tone={r.offload?.in_hw ? "good" : "warn"}>
                          {r.offload?.in_hw ? t("switch.inHw") : t("switch.notInHw")}
                        </Badge>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            ) : (
              <p className="text-sm text-base-content/70">{t("switch.noRules")}</p>
            )}
          </Card>
        );
      })}
    </div>
  );
}

export function Switch() {
  const { t } = useI18n();
  const [tab, setTab] = useTab(TABS);
  const items: { id: (typeof TABS)[number]; label: string }[] = [
    { id: "ports", label: t("switch.tabPorts") },
    { id: "vlans", label: t("switch.tabVlans") },
    { id: "mac", label: t("switch.tabMac") },
    { id: "routing", label: t("switch.tabRouting") },
    { id: "rules", label: t("switch.tabRules") },
  ];
  return (
    <Page
      title={t("switch.title")}
      tabs={<Tabs items={items} value={tab} onChange={setTab} label={t("switch.title")} />}
    >
      {tab === "ports" && <PortsTab />}
      {tab === "vlans" && <VlansTab />}
      {tab === "mac" && <MacTab />}
      {tab === "routing" && <RoutingTab />}
      {tab === "rules" && <RulesTab />}
    </Page>
  );
}
