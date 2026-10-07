// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The switch: how traffic crosses the stick, which interfaces sit on each
 * port and what each is for, the bridges, the MAC table, the routes, the
 * neighbours and the rules the hardware applies as frames come in.
 */

import { useState } from "react";
import { ApiError, OfflineError } from "../api/client";
import { useBridge, useNetwork, useTcRules } from "../api/config";
import { useLive } from "../api/live";
import type { Link, Neighbour, Route, TcAction, TcRule } from "../api/types";
import { useGemText } from "../components/Gem";
import { Badge, Card, Dot, PageTitle } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { gems, linkSpeed, port } from "../lib/state";
import {
  type IfaceView,
  isSystemEntry,
  type PortView,
  type Role,
  shownRoutes,
  switchView,
} from "../lib/switch";

const ROLE: Record<Role, Key> = {
  management: "switch.roleManagement",
  handover: "switch.roleHandover",
  wan: "switch.roleWan",
  session: "switch.roleSession",
  bridged: "switch.roleBridged",
  channel: "switch.roleChannel",
  unused: "switch.roleUnused",
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

function useVlanName() {
  const { t } = useI18n();
  return (vlan: number | null) =>
    vlan == null ? t("switch.untagged") : t("switch.vlanN", { id: vlan });
}

function failure(t: (k: Key, v?: Record<string, string>) => string, e: Error) {
  return e instanceof OfflineError
    ? t("connection.offline")
    : t("switch.unreadable", {
        reason: e instanceof ApiError ? (e.detail ?? e.message) : e.message,
      });
}

function Addresses({ link }: { link: Link }) {
  const shown = link.addresses.filter((a) => !a.link_local);
  if (!shown.length) return null;
  return (
    <span className="tabular text-xs text-base-content/70">
      {shown.map((a) => a.address).join(", ")}
    </span>
  );
}

function Iface({ v, depth }: { v: IfaceView; depth: number }) {
  const { t } = useI18n();
  const { data: live } = useLive();
  const vlanName = useVlanName();
  const gem = useGemText();
  const channel = v.link.kind === "gem" ? gems(live).find((g) => g.gem.name === v.link.name) : null;
  const down = v.link.operstate === "down" || !v.link.admin_up;

  return (
    <li className="grid gap-1">
      <div
        className="grid gap-0.5 rounded-box border border-base-300 px-3 py-2"
        style={{ marginLeft: `${depth * 0.9}rem` }}
      >
        <div className="flex flex-wrap items-baseline gap-x-2">
          <span className="font-mono text-sm font-medium">{v.link.name}</span>
          {channel ? (
            <span className="text-sm">GEM {channel.gem.port_id}</span>
          ) : (
            v.link.kind === "vlan" && <Badge tone="info">{vlanName(v.link.vlan)}</Badge>
          )}
          {down && <Badge tone="warn">{t("switch.down")}</Badge>}
        </div>
        <div className="text-sm">
          {channel
            ? `${gem.direction(channel.gem)} · ${gem.use(channel)}`
            : t(ROLE[v.role], { bridge: v.link.master ?? "" })}
        </div>
        <Addresses link={v.link} />
      </div>
      {v.children.length > 0 && (
        <ul className="grid gap-1">
          {v.children.map((c) => (
            <Iface key={c.link.name} v={c} depth={depth + 1} />
          ))}
        </ul>
      )}
    </li>
  );
}

function Port({ p }: { p: PortView }) {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const counts = port(live, p.link.name);
  const up = p.link.operstate === "up";
  const speed = linkSpeed(counts && counts.speed_mbit > 0 ? counts.speed_mbit : null, lang);
  /* The port's own address is management without a VLAN. */
  const untagged = p.link.addresses.some((a) => !a.link_local);

  return (
    <div className="grid content-start gap-2">
      <div className="text-xs font-semibold uppercase tracking-wide text-base-content/55">
        {p.side === "host" ? t("switch.hostSide") : t("switch.operatorSide")}
      </div>
      <div className="flex items-center gap-2">
        <Dot tone={up ? "good" : "bad"} />
        <span className="font-semibold">
          {p.side === "host" ? t("switch.portSfp") : t("switch.portPon")}
        </span>
        <span className="font-mono text-xs text-base-content/60">{p.link.name}</span>
      </div>
      <div className="text-xs text-base-content/70">
        {[speed, p.link.mtu != null && t("switch.mtu", { n: String(p.link.mtu) })]
          .filter(Boolean)
          .join(" · ")}
      </div>
      {untagged && (
        <div className="rounded-box border border-dashed border-base-300 px-3 py-2">
          <div className="text-sm">
            {t("switch.untagged")} · {t("switch.roleManagement")}
          </div>
          <Addresses link={p.link} />
        </div>
      )}
      {p.ifaces.length ? (
        <ul className="grid gap-1">
          {p.ifaces.map((v) => (
            <Iface key={v.link.name} v={v} depth={0} />
          ))}
        </ul>
      ) : (
        !untagged && <p className="text-sm text-base-content/60">{t("switch.nothingOver")}</p>
      )}
    </div>
  );
}

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
  if (a.kind === "gact") return t("switch.actDrop");
  if (a.kind === "mirred") return t("switch.actRedirect");
  if (a.kind === "police") return t("switch.actPolice");
  return a.kind;
}

function Rules() {
  const { t } = useI18n();
  const tc = useTcRules();
  if (tc.error) return <p className="text-sm text-error">{failure(t, tc.error)}</p>;
  if (!tc.data) return <span className="loading loading-spinner loading-sm" />;

  return (
    <div className="grid gap-4 md:grid-cols-2">
      {Object.entries(tc.data).map(([name, v]) => (
        <div key={name} className="grid content-start gap-2">
          <div className="font-semibold">
            {name === "sfp" ? t("switch.portSfp") : name === "pon" ? t("switch.portPon") : name}
          </div>
          {"error" in v ? (
            <p className="text-sm text-error">{t("switch.unreadable", { reason: v.error })}</p>
          ) : v.ingress.length ? (
            <ul className="grid gap-1">
              {v.ingress.map((r) => (
                <li
                  key={`${r.priority}-${r.handle}`}
                  className="grid gap-0.5 rounded-box border border-base-300 px-3 py-2 text-sm"
                >
                  <span>
                    <span className="text-base-content/60">{t("switch.ruleWhen")}: </span>
                    {ruleWhen(r, t)}
                  </span>
                  <span>
                    <span className="text-base-content/60">{t("switch.ruleThen")}: </span>
                    {(r.actions ?? []).map((a) => actionText(a, t)).join(", ") ||
                      t("switch.actPass")}
                  </span>
                  <span>
                    <Badge tone={r.offload?.in_hw ? "good" : "warn"}>
                      {r.offload?.in_hw ? t("switch.inHw") : t("switch.notInHw")}
                    </Badge>
                  </span>
                </li>
              ))}
            </ul>
          ) : (
            <p className="text-sm text-base-content/70">{t("switch.noRules")}</p>
          )}
        </div>
      ))}
    </div>
  );
}

export function Switch() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const net = useNetwork();
  const bridge = useBridge();
  const vlanName = useVlanName();
  const [system, setSystem] = useState(false);
  const v = switchView(net.data, live);
  const gemId = (name: string | null) =>
    gems(live).find((g) => g.gem.name === name)?.gem.port_id ?? name ?? "–";

  const paths: string[] = [];
  for (const b of v.bridges)
    paths.push(
      t(b.spans ? "switch.bridge" : "switch.bridgeOneSide", {
        bridge: b.link.name,
        members: b.members.map((m) => m.link.name).join(", "),
      }),
    );
  if (net.data && !v.bridges.some((b) => b.spans)) paths.push(t("switch.noBridge"));
  if (v.routed)
    paths.push(
      t("switch.routed", {
        lan: vlanName(v.routed.lanVlan),
        gem: `GEM ${gemId(v.routed.gem)}`,
        wan: vlanName(v.routed.wanVlan),
      }),
    );
  if (v.management.length)
    paths.push(
      t("switch.management", {
        list: v.management
          .map((l) => {
            const ip = l.addresses.find((a) => a.family === "ipv4" && !a.link_local)?.address;
            return `${vlanName(l.kind === "vlan" ? l.vlan : null)} – ${ip?.split("/")[0]}`;
          })
          .join(", "),
      }),
    );

  const fdb = (bridge.data?.fdb ?? []).filter((e) => system || !isSystemEntry(e));
  const routes = shownRoutes(net.data?.routes ?? []);

  return (
    <>
      <PageTitle hint={t("switch.hint")}>{t("switch.title")}</PageTitle>

      <Card title={t("switch.paths")}>
        {net.error ? (
          <p className="text-sm text-error">{failure(t, net.error)}</p>
        ) : net.data ? (
          <ul className="grid list-disc gap-1 pl-5 text-sm">
            {paths.map((p) => (
              <li key={p}>{p}</li>
            ))}
          </ul>
        ) : (
          <span className="loading loading-spinner loading-sm" />
        )}
      </Card>

      <Card title={t("switch.ports")} className="mt-4">
        {net.data && (
          <div className="grid gap-5 lg:grid-cols-[minmax(0,1fr)_minmax(0,14rem)_minmax(0,1fr)]">
            {v.ports
              .filter((p) => p.side === "host")
              .map((p) => (
                <Port key={p.link.name} p={p} />
              ))}
            <div className="grid content-start gap-2 lg:border-x lg:border-base-300 lg:px-4">
              <div className="text-xs font-semibold uppercase tracking-wide text-base-content/55">
                CPU
              </div>
              <div className="flex items-center gap-2">
                <Dot tone="good" />
                <span className="font-semibold">{t("switch.conduit")}</span>
                {v.conduit && (
                  <span className="font-mono text-xs text-base-content/60">{v.conduit.name}</span>
                )}
              </div>
              <p className="text-xs text-base-content/70">{t("switch.conduitHint")}</p>
            </div>
            {v.ports
              .filter((p) => p.side === "operator")
              .map((p) => (
                <Port key={p.link.name} p={p} />
              ))}
          </div>
        )}
      </Card>

      <div className="mt-4 grid gap-4 lg:grid-cols-2">
        <Card title={t("switch.bridges")}>
          {v.bridges.length ? (
            v.bridges.map((b) => (
              <div key={b.link.name} className="grid gap-1 text-sm">
                <span className="font-mono font-medium">{b.link.name}</span>
                <span>
                  {t("switch.members")}:{" "}
                  {b.members.map((m) => m.link.name).join(", ") || t("switch.none")}
                </span>
              </div>
            ))
          ) : (
            <p className="text-sm text-base-content/70">{t("switch.noBridges")}</p>
          )}
        </Card>

        <Card title={t("switch.rules")}>
          <p className="text-sm text-base-content/70">{t("switch.rulesHint")}</p>
          <Rules />
        </Card>
      </div>

      <Card
        title={t("switch.fdb")}
        className="mt-4"
        action={
          <label className="label cursor-pointer gap-2 text-sm">
            <input
              type="checkbox"
              className="toggle toggle-sm"
              checked={system}
              onChange={(e) => setSystem(e.currentTarget.checked)}
            />
            {t("switch.showSystem")}
          </label>
        }
      >
        <p className="text-sm text-base-content/70">{t("switch.fdbHint")}</p>
        {bridge.error ? (
          <p className="text-sm text-error">{failure(t, bridge.error)}</p>
        ) : fdb.length ? (
          <div className="overflow-x-auto">
            <table className="table table-sm">
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
                    <td className="font-mono text-xs">{e.dev ?? "–"}</td>
                    <td>{e.vlan == null ? "–" : e.vlan}</td>
                    <td className="flex flex-wrap gap-1">
                      {e.learned ? t("switch.learned") : t("switch.fixed")}
                      {e.offloaded && <Badge tone="good">{t("switch.inHw")}</Badge>}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        ) : bridge.data ? (
          <p className="text-sm">{t("switch.none")}</p>
        ) : (
          <span className="loading loading-spinner loading-sm" />
        )}
      </Card>

      <div className="mt-4 grid gap-4 xl:grid-cols-2">
        <Card title={t("switch.routes")}>
          {routes.length ? (
            <div className="overflow-x-auto">
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
                        {r.destination === "default" ? t("switch.everything") : r.destination}
                      </td>
                      <td className="tabular">{r.gateway ?? t("switch.direct")}</td>
                      <td className="font-mono text-xs">{r.dev ?? "–"}</td>
                      <td>{t(PROTOCOL[r.protocol])}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          ) : (
            <p className="text-sm">{net.data ? t("switch.none") : ""}</p>
          )}
        </Card>

        <Card title={t("switch.neighbours")}>
          <p className="text-sm text-base-content/70">{t("switch.neighboursHint")}</p>
          {net.data?.neighbours.length ? (
            <div className="overflow-x-auto">
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
                  {net.data.neighbours.map((n) => (
                    <tr key={`${n.dev}-${n.address}`}>
                      <td className="tabular">{n.address}</td>
                      <td className="font-mono text-xs">{n.mac ?? "–"}</td>
                      <td className="font-mono text-xs">{n.dev ?? "–"}</td>
                      <td>{t(STATE[n.state])}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          ) : (
            <p className="text-sm">{net.data ? t("switch.none") : ""}</p>
          )}
        </Card>
      </div>
    </>
  );
}
