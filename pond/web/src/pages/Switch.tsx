// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Pages of the switch: which way each VLAN goes through the stick, from the
 * host to the operator, and what it is for; and the MAC table. And the routes and neighbours, which
 * the menu shows under the internet.
 */

import { Link } from "@tanstack/react-router";
import { Search } from "lucide-react";
import { useMemo, useState } from "react";
import { ApiError, OfflineError } from "../api/client";
import { useBridge, useNetwork, useOmci } from "../api/config";
import { useLive } from "../api/live";
import type { Neighbour, Route } from "../api/types";
import { Page } from "../components/Page";
import { Badge, Card, Detail, TABLE } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { Mib, services } from "../lib/omci";
import { fibreVlan, sessions } from "../lib/state";
import { isSystemEntry, shownRoutes } from "../lib/switch";
import { type Member, type VlanRole, vlanMatrix, vlanPaths } from "../lib/vlans";

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

/* How a VLAN crosses the SFP port, to the host. */
const HOST: Record<Member, Key> = {
  tagged: "switch.mTagged",
  untagged: "switch.mUntagged",
  pvid: "switch.mPvid",
};

/* The page that tells of a VLAN's use in full. */
const ROLE_PAGE: Partial<Record<VlanRole, string>> = {
  session: "/internet/wan",
  bridged: "/operator/services",
};

/*
 * The VLANs, a row for each way through the stick, read as a frame goes:
 * from the host on the SFP port, through the stick, to the operator on the
 * fibre. A VLAN the OLT translates is one row, under the host's VID.
 */
function Vlans() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const net = useNetwork();
  const bridge = useBridge();
  const omci = useOmci();
  const list = useMemo(() => services(new Mib(omci.data)), [omci.data]);
  const translations = useMemo(
    () => list.flatMap((s) => s.hostVlans.map((h) => ({ host: h, fibre: s.vlan }))),
    [list],
  );
  const paths = vlanPaths(vlanMatrix(net.data, bridge.data, live, translations), translations);
  /* What a way carries that its columns do not show: the stick's sessions, multicast. */
  const carries = (vid: number) => {
    const own = sessions(live).filter((x) => fibreVlan(x) === String(vid));
    return [
      own.length && t("fibre.tagSession", { list: own.map((x) => x.id).join(", ") }),
      list.find((x) => x.vlan === vid)?.multicast && t("fibre.tagMulticast"),
    ].filter(Boolean);
  };
  if (net.error) return <p className="text-sm text-error">{failure(t, net.error)}</p>;

  return (
    <Card scroll className="min-h-0">
      <table className={`${TABLE} table-pin-rows`}>
        <thead>
          <tr className="[&>th]:align-bottom">
            <th className="w-40">VLAN</th>
            <th>{t("switch.vlanUse")}</th>
            <th className="w-48">
              {t("switch.hostSide")}
              <span className="block font-normal">{t("switch.sfpPort")}</span>
            </th>
            <th className="w-40">{t("switch.stick")}</th>
            <th className="w-56">
              {t("switch.operatorSide")}
              <span className="block font-normal">{t("switch.fibreSide")}</span>
            </th>
          </tr>
        </thead>
        <tbody>
          {paths.map((p) => {
            const more = carries(p.fibre ?? p.vid);
            const to = ROLE_PAGE[p.role];
            return (
              <tr key={p.vid}>
                <td>
                  <span className="grid gap-0.5">
                    <span className="tabular font-semibold">{p.vid}</span>
                    {p.fibre != null && <Detail>{t("internet.onFibre", { vid: p.fibre })}</Detail>}
                  </span>
                </td>
                <td>
                  <span className="grid gap-0.5">
                    {to ? (
                      <Link to={to} className="link link-hover link-primary">
                        {t(VLAN_ROLE[p.role])}
                      </Link>
                    ) : (
                      <span>{t(VLAN_ROLE[p.role])}</span>
                    )}
                    {more.length > 0 && <Detail>{more.join(" · ")}</Detail>}
                  </span>
                </td>
                <td>{p.host ? t(HOST[p.host]) : "–"}</td>
                <td>
                  {p.ends
                    ? t("switch.endsHere")
                    : p.host && p.gems.length
                      ? t("switch.passes")
                      : "–"}
                </td>
                <td>
                  {p.gems.length ? (
                    <Link to="/operator/channels" className="link link-hover link-primary tabular">
                      GEM {p.gems.join(", ")}
                    </Link>
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
    <Page title={t("nav.vlans")} hint={t("switch.vlanHint")}>
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
