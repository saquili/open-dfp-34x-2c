// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Operator, Television: television through the stick, as omcid's multicast
 * agent holds it (pond's value "iptv"). At the left what happens: the
 * channels watched now, the joins refused with why and the streams that
 * stopped, and the latest events with the counts. At the right what decides
 * it: the operator's policy, who gives upstream IGMP its priority, and a
 * test that joins a group from the stick itself and measures its stream for
 * a few seconds, with no set-top box.
 */

import { Play } from "lucide-react";
import { useState } from "react";
import { useIptv, useIptvChange, useIptvTest } from "../api/config";
import { useLive, useTraffic } from "../api/live";
import type { Iptv, IptvTest } from "../api/types";
import { SaveState, useChangeState } from "../components/Field";
import { Page } from "../components/Page";
import { QueryState } from "../components/Read";
import { Badge, Card, Cell, Detail, Facts, Row, Table } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { bitrate, duration, integer } from "../lib/format";
import {
  eventRows,
  knownRows,
  type LinkKind,
  linkLocal,
  refusedRows,
  shortAddr,
  withoutLinkLocal,
} from "../lib/iptv";
import { FibreState } from "./Fibre";
import { Columns, Hint, Stack } from "./system/shared";

const MODE: Record<NonNullable<Iptv["policy"]>["mode"], Key> = {
  snoop: "tv.modeSnoop",
  snoop_proxy: "tv.modeSnoopProxy",
  proxy: "tv.modeProxy",
};

const REASON: Record<Iptv["rejected"][number]["reason"], Key> = {
  not_allowed: "tv.reasonNotAllowed",
  max_groups: "tv.reasonMaxGroups",
  max_bandwidth: "tv.reasonMaxBandwidth",
  preview_over: "tv.reasonPreview",
};

const KIND: Record<LinkKind, Key> = {
  neighbours: "tv.linkNeighbours",
  all_nodes: "tv.linkAllNodes",
  all_routers: "tv.linkAllRouters",
  reports: "tv.linkReports",
  discovery: "tv.linkDiscovery",
  routing: "tv.linkRouting",
  dhcp: "tv.linkDhcp",
  other: "tv.linkOther",
};

const EVENT: Record<Iptv["log"][number]["event"], Key> = {
  join: "tv.evJoin",
  leave: "tv.evLeave",
  reject: "tv.evReject",
  query: "tv.evQuery",
  lost: "tv.evLost",
  back: "tv.evBack",
};

/* What the OLT sends on the multicast GEM ports now, bits a second, or null without them. */
function useMulticastNow(): number | null {
  const { data: live } = useLive();
  const now = useTraffic().at(-1);
  const names = (live?.gem_ports ?? []).filter((g) => g.multicast).map((g) => g.name);
  if (!now || names.length === 0) return null;
  return names.reduce((sum, n) => sum + (now.gems[n]?.down ?? 0), 0);
}

function Watched({ tv }: { tv: Iptv }) {
  const { t, lang } = useI18n();
  const total = useMulticastNow();
  const counted = tv.groups.reduce((sum, g) => sum + (g.rate_bps ?? 0), 0);
  const low = total != null && total > 100_000 && tv.groups.length > 0 && counted * 10 < total;
  return (
    <Card title={t("tv.watched", { n: tv.groups.length })}>
      {tv.groups.length === 0 ? (
        <p className="text-sm">{t("tv.watchedNone")}</p>
      ) : (
        <Table head={[t("tv.group"), "VLAN", t("tv.client"), t("tv.since"), t("tv.rate")]}>
          {tv.groups.map((g) => (
            <Row key={`${g.group}/${g.source ?? ""}/${g.client ?? ""}`}>
              <Cell head={t("tv.group")}>
                <span className="font-mono">{shortAddr(g.group)}</span>
                {g.source && <Detail>{t("tv.source", { source: shortAddr(g.source) })}</Detail>}
              </Cell>
              <Cell head="VLAN">{g.vlan ?? "–"}</Cell>
              <Cell head={t("tv.client")}>
                <span className="font-mono">{g.client ? shortAddr(g.client) : "–"}</span>
              </Cell>
              <Cell head={t("tv.since")}>{duration(tv.now_s - g.since_s, lang)}</Cell>
              <Cell head={t("tv.rate")}>
                {g.rate_bps == null ? "–" : bitrate(g.rate_bps, lang)}
              </Cell>
            </Row>
          ))}
        </Table>
      )}
      {total != null && (
        <p className="flex flex-wrap items-baseline gap-x-2 text-sm text-base-content/70">
          <span>{t("tv.multicastNow")}</span>
          <span className="tabular font-medium text-base-content">{bitrate(total, lang)}</span>
        </p>
      )}
      {low && <Hint>{t("tv.rateLow")}</Hint>}
    </Card>
  );
}

function Refused({ tv }: { tv: Iptv }) {
  const { t, lang } = useI18n();
  const asked = tv.rejected.filter((r) => !linkLocal(r.group));
  const known = knownRows(tv.rejected);
  const none = asked.length === 0 && tv.lost.length === 0;
  const rows = refusedRows(asked);
  return (
    <Card title={t("tv.refused")}>
      {none ? (
        <p className="text-sm">{t(known.length > 0 ? "tv.refusedOnlyKnown" : "tv.refusedNone")}</p>
      ) : (
        <Table head={[t("tv.when"), t("tv.group"), t("tv.why")]}>
          {tv.lost.map((l) => (
            <Row key={`lost/${l.group}`}>
              <Cell head={t("tv.when")}>
                {t("tv.ago", { t: duration(tv.now_s - l.since_s, lang) })}
              </Cell>
              <Cell head={t("tv.group")}>
                <span className="font-mono">{shortAddr(l.group)}</span>
              </Cell>
              <Cell head={t("tv.why")}>
                <Badge tone="bad">{t("tv.lost")}</Badge>
              </Cell>
            </Row>
          ))}
          {rows.map((r) => (
            <Row key={`${r.group}/${r.vlan ?? ""}/${r.reason}`}>
              <Cell head={t("tv.when")}>
                {r.count > 1
                  ? t("tv.repeat", { n: r.count, t: duration(tv.now_s - r.lastAt, lang) })
                  : t("tv.ago", { t: duration(tv.now_s - r.lastAt, lang) })}
              </Cell>
              <Cell head={t("tv.group")}>
                <span className="font-mono">{shortAddr(r.group)}</span>
                {(r.vlan != null || r.clients.length > 0) && (
                  <Detail>
                    {r.vlan != null && `VLAN ${r.vlan}`}
                    {r.vlan != null && r.clients.length > 0 && " · "}
                    <span className="font-mono">{r.clients.slice(0, 2).join(", ")}</span>
                    {r.clients.length > 2 && ` +${r.clients.length - 2}`}
                  </Detail>
                )}
              </Cell>
              <Cell head={t("tv.why")}>
                <Badge tone="warn">{t(REASON[r.reason])}</Badge>
              </Cell>
            </Row>
          ))}
        </Table>
      )}
      {known.length > 0 && (
        <>
          <div className="grid gap-0.5 pt-1">
            <h3 className="text-sm font-medium">{t("tv.known")}</h3>
            <Detail>{t("tv.knownHint")}</Detail>
          </div>
          <Table head={[t("tv.when"), t("tv.kind"), t("tv.why")]}>
            {known.map((k) => (
              <Row key={k.kind}>
                <Cell head={t("tv.when")}>
                  {k.count > 1
                    ? t("tv.repeat", { n: k.count, t: duration(tv.now_s - k.lastAt, lang) })
                    : t("tv.ago", { t: duration(tv.now_s - k.lastAt, lang) })}
                </Cell>
                <Cell head={t("tv.kind")}>
                  {t(KIND[k.kind])}
                  <Detail>
                    {t("tv.knownDetail", { groups: k.groups, clients: k.clients })}
                    {" · "}
                    <span className="font-mono">{k.example}</span>
                  </Detail>
                </Cell>
                <Cell head={t("tv.why")}>
                  <Badge tone="neutral">{t("tv.knownBadge")}</Badge>
                </Cell>
              </Row>
            ))}
          </Table>
        </>
      )}
    </Card>
  );
}

function Events({ tv }: { tv: Iptv }) {
  const { t, lang } = useI18n();
  const c = tv.counters;
  const rows = eventRows(tv.log);
  return (
    <Card title={t("tv.events")}>
      <p className="text-sm text-base-content/70">
        {t("tv.counts", {
          joins: integer(c.joins, lang),
          leaves: integer(c.leaves, lang),
          rejected: integer(c.rejected, lang),
          queries: integer(c.queries, lang),
          reports: integer(c.reports, lang),
        })}
      </p>
      {tv.log.length === 0 ? (
        <p className="text-sm">{t("tv.eventsNone")}</p>
      ) : (
        <ul className="-mx-2 grid max-h-44 gap-1 overflow-auto px-2 text-sm">
          {rows.map((e) => (
            <li
              key={`${e.event}/${e.group ?? ""}/${e.vlan ?? ""}`}
              className="flex gap-x-2 whitespace-nowrap"
            >
              <span className="tabular w-32 shrink-0 whitespace-nowrap text-base-content/70">
                {t("tv.ago", { t: duration(tv.now_s - e.lastAt, lang) })}
              </span>
              <span className="font-medium">{t(EVENT[e.event])}</span>
              {e.group && <span className="font-mono">{shortAddr(e.group)}</span>}
              {e.vlan != null && <span className="text-base-content/70">VLAN {e.vlan}</span>}
              {e.count > 1 && <span className="tabular text-base-content/70">×{e.count}</span>}
            </li>
          ))}
        </ul>
      )}
    </Card>
  );
}

function Policy({ tv }: { tv: Iptv }) {
  const { t, lang } = useI18n();
  const p = tv.policy;
  if (!p)
    return (
      <Card title={t("tv.policy")}>
        <p className="text-sm">{t("tv.policyNone")}</p>
      </Card>
    );
  return (
    <Card title={t("tv.policy")}>
      <Facts
        rows={[
          [t("tv.igmp"), t("tv.igmpValue", { v: p.igmp_version, mode: t(MODE[p.mode]) })],
          [
            t("tv.allowed"),
            <span key="allowed" className="grid gap-0.5">
              {p.allowed.length === 0
                ? t("tv.allowedNone")
                : p.allowed.map((a) => (
                    <span key={`${a.from}-${a.to}/${a.vlan ?? ""}`}>
                      <span className="font-mono">
                        {a.from === a.to ? a.from : `${a.from} – ${a.to}`}
                      </span>
                      {a.vlan != null && ` · VLAN ${a.vlan}`}
                      {a.preview && ` · ${t("tv.preview")}`}
                    </span>
                  ))}
            </span>,
          ],
          [
            t("tv.limits"),
            t("tv.limitsValue", {
              groups: p.max_groups ?? t("tv.noLimit"),
              bw:
                p.max_bandwidth_bps == null ? t("tv.noLimit") : bitrate(p.max_bandwidth_bps, lang),
            }),
          ],
          [t("tv.fastLeave"), t(p.fast_leave ? "tv.yes" : "tv.no")],
          [t("tv.hostVlan"), p.host_vlan ?? t("tv.hostVlanKept")],
          [
            t("tv.timers"),
            t("tv.timersValue", {
              q: p.query_interval_s,
              r: (p.query_response_ms / 1000).toLocaleString(lang),
              rob: p.robustness,
            }),
          ],
          [
            t("tv.querier"),
            tv.querier
              ? t("tv.querierValue", {
                  t: duration(tv.now_s - tv.querier.last_s, lang),
                  v: tv.querier.version,
                })
              : t("tv.querierNone"),
          ],
        ]}
      />
    </Card>
  );
}

function Priority({ tv }: { tv: Iptv }) {
  const { t } = useI18n();
  const change = useIptvChange();
  const state = useChangeState(change);
  const p = tv.priority;
  if (!p) return null;
  const choose = p.control === 0 || p.control === 3;
  return (
    <Card title={t("tv.priority")}>
      <p className="text-sm">
        {p.by === "olt"
          ? t("tv.byOlt")
          : p.by === "stick"
            ? t("tv.byStick", { pcp: p.pcp ?? "–" })
            : t("tv.byHost")}
      </p>
      {choose ? (
        <label className="flex cursor-pointer items-start gap-2.5">
          <input
            type="checkbox"
            className="toggle toggle-sm mt-0.5"
            checked={p.auto}
            disabled={change.isPending}
            onChange={(e) =>
              change.mutate({ IGMP_PRIORITY: e.currentTarget.checked ? "auto" : "host" })
            }
          />
          <span className="grid gap-0.5">
            <span className="text-sm font-medium">{t("tv.auto")}</span>
            <Detail>{t("tv.autoHint")}</Detail>
          </span>
        </label>
      ) : (
        <Hint>{t("tv.oltTags")}</Hint>
      )}
      {p.dropped > 0 && (
        <p className="flex items-start gap-2 text-sm">
          <Badge tone="warn">{t("tv.dropped", { n: p.dropped })}</Badge>
        </p>
      )}
      <SaveState {...state} />
    </Card>
  );
}

function Test({ tv }: { tv: Iptv }) {
  const { t, lang } = useI18n();
  const test = useIptvTest();
  const [group, setGroup] = useState(tv.groups[0]?.group ?? "");
  const [vlan, setVlan] = useState(String(tv.policy?.host_vlan ?? ""));
  const [result, setResult] = useState<IptvTest | null>(null);
  const SECONDS = 10;
  return (
    <Card title={t("tv.test")}>
      <form
        noValidate
        className="grid gap-3"
        onSubmit={(e) => {
          e.preventDefault();
          setResult(null);
          test.mutate({ group, vlan, seconds: SECONDS }, { onSuccess: setResult });
        }}
      >
        <div className="grid grid-cols-[minmax(0,1fr)_6rem] gap-2">
          <label className="grid gap-1">
            <span className="text-sm font-semibold">{t("tv.group")}</span>
            <input
              className="input w-full font-mono"
              value={group}
              placeholder="239.0.0.1"
              spellcheck={false}
              onInput={(e) => setGroup(e.currentTarget.value.trim())}
            />
          </label>
          <label className="grid gap-1">
            <span className="text-sm font-semibold">VLAN</span>
            <input
              className="input w-full"
              inputMode="numeric"
              value={vlan}
              onInput={(e) => setVlan(e.currentTarget.value.replace(/\D/g, "").slice(0, 4))}
            />
          </label>
        </div>
        <div className="flex flex-wrap items-center gap-3">
          <button
            type="submit"
            className="btn btn-primary btn-sm"
            disabled={test.isPending || !group}
          >
            <Play className="size-4" />
            {t(test.isPending ? "tv.testing" : "tv.testRun", { s: SECONDS })}
          </button>
          {test.isError && <span className="text-sm text-error">{test.error.message}</span>}
        </div>
        {result && (
          <p className="text-sm">
            {result.first_ms == null
              ? t("tv.testNothing", { s: result.seconds })
              : t("tv.testGot", {
                  ms: result.first_ms,
                  rate: bitrate(result.rate_bps, lang),
                })}
          </p>
        )}
        <Hint>{t("tv.testHint")}</Hint>
      </form>
    </Card>
  );
}

/* Whether the groups of the link are shown, as the reader last chose. */
function LinkLocal({
  hidden,
  show,
  onChange,
}: {
  hidden: number;
  show: boolean;
  onChange: (show: boolean) => void;
}) {
  const { t } = useI18n();
  if (hidden === 0 && !show) return null;
  return (
    <label className="flex cursor-pointer items-start gap-2.5">
      <input
        type="checkbox"
        className="toggle toggle-sm mt-0.5"
        checked={show}
        onChange={(e) => onChange(e.currentTarget.checked)}
      />
      <span className="grid gap-0.5">
        <span className="text-sm font-medium">{t("tv.localShow", { n: hidden })}</span>
        <Detail>{t("tv.localHint")}</Detail>
      </span>
    </label>
  );
}

export function IptvPage() {
  const { t } = useI18n();
  const tv = useIptv();
  const [showLocal, setShowLocal] = useState(false);
  const view = tv.data ? withoutLinkLocal(tv.data, showLocal) : null;
  return (
    <Page title={t("nav.tv")} hint={t("tv.hint")} actions={<FibreState />}>
      <QueryState reads={tv}>
        {() =>
          tv.data &&
          view && (
            <Columns wide="left">
              <Stack>
                <LinkLocal
                  hidden={view.hidden.events + view.hidden.watched}
                  show={showLocal}
                  onChange={setShowLocal}
                />
                <Watched tv={view.tv} />
                <Refused tv={view.tv} />
                <Events tv={view.tv} />
              </Stack>
              <Stack>
                <Policy tv={tv.data} />
                <Priority tv={tv.data} />
                <Test tv={tv.data} />
              </Stack>
            </Columns>
          )
        }
      </QueryState>
    </Page>
  );
}
