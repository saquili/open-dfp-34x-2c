// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Pages of the switch: which VLAN crosses which port, tagged or not, and
 * what each is for; and the MAC table. And the routes and neighbours, which
 * the menu shows under the internet.
 */

import { Search } from "lucide-react";
import { useMemo, useState } from "react";
import { ApiError, OfflineError } from "../api/client";
import { useBridge, useNetwork, useOmci } from "../api/config";
import { useLive } from "../api/live";
import type { Neighbour, Route } from "../api/types";
import { Page } from "../components/Page";
import { Badge, Card } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { Mib, services } from "../lib/omci";
import { isSystemEntry, shownRoutes } from "../lib/switch";
import { type Member, type VlanRole, vlanMatrix } from "../lib/vlans";

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

function failure(t: (k: Key, v?: Record<string, string>) => string, e: Error) {
  return e instanceof OfflineError
    ? t("connection.offline")
    : t("switch.unreadable", {
        reason: e instanceof ApiError ? (e.detail ?? e.message) : e.message,
      });
}

/* ------------------------------------------------------------------- VLANs */

const MEMBER: Record<Member, { mark: string; cls: string; key: Key }> = {
  tagged: { mark: "T", cls: "bg-info/15 text-info", key: "switch.mTagged" },
  untagged: { mark: "U", cls: "bg-success/15 text-success", key: "switch.mUntagged" },
  pvid: { mark: "U*", cls: "bg-success/25 text-success", key: "switch.mPvid" },
};

function Vlans() {
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

function MacTable() {
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

function Routing() {
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

export function VlansPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.vlans")}>
      <Vlans />
    </Page>
  );
}

export function MacPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.mac")}>
      <MacTable />
    </Page>
  );
}

export function RoutesPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.routes")}>
      <Routing />
    </Page>
  );
}
