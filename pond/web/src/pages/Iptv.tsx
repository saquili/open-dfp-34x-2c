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
import type { Iptv, IptvTest } from "../api/types";
import { SaveState, useChangeState } from "../components/Field";
import { Page } from "../components/Page";
import { QueryState } from "../components/Read";
import { Badge, Card, Cell, Detail, Facts, Row, Table } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { bitrate, duration, integer } from "../lib/format";
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

const EVENT: Record<Iptv["log"][number]["event"], Key> = {
  join: "tv.evJoin",
  leave: "tv.evLeave",
  reject: "tv.evReject",
  query: "tv.evQuery",
  lost: "tv.evLost",
  back: "tv.evBack",
};

function Watched({ tv }: { tv: Iptv }) {
  const { t, lang } = useI18n();
  return (
    <Card title={t("tv.watched", { n: tv.groups.length })}>
      {tv.groups.length === 0 ? (
        <p className="text-sm">{t("tv.watchedNone")}</p>
      ) : (
        <Table head={[t("tv.group"), "VLAN", t("tv.client"), t("tv.since"), t("tv.rate")]}>
          {tv.groups.map((g) => (
            <Row key={`${g.group}/${g.source ?? ""}/${g.client ?? ""}`}>
              <Cell head={t("tv.group")}>
                <span className="font-mono">{g.group}</span>
                {g.source && <Detail>{t("tv.source", { source: g.source })}</Detail>}
              </Cell>
              <Cell head="VLAN">{g.vlan ?? "–"}</Cell>
              <Cell head={t("tv.client")}>
                <span className="font-mono">{g.client ?? "–"}</span>
              </Cell>
              <Cell head={t("tv.since")}>{duration(tv.now_s - g.since_s, lang)}</Cell>
              <Cell head={t("tv.rate")}>
                {g.rate_bps == null ? "–" : bitrate(g.rate_bps, lang)}
              </Cell>
            </Row>
          ))}
        </Table>
      )}
    </Card>
  );
}

function Refused({ tv }: { tv: Iptv }) {
  const { t, lang } = useI18n();
  const none = tv.rejected.length === 0 && tv.lost.length === 0;
  return (
    <Card title={t("tv.refused")}>
      {none ? (
        <p className="text-sm">{t("tv.refusedNone")}</p>
      ) : (
        <Table head={[t("tv.when"), t("tv.group"), t("tv.why")]}>
          {tv.lost.map((l) => (
            <Row key={`lost/${l.group}`}>
              <Cell head={t("tv.when")}>
                {t("tv.ago", { t: duration(tv.now_s - l.since_s, lang) })}
              </Cell>
              <Cell head={t("tv.group")}>
                <span className="font-mono">{l.group}</span>
              </Cell>
              <Cell head={t("tv.why")}>
                <Badge tone="bad">{t("tv.lost")}</Badge>
              </Cell>
            </Row>
          ))}
          {tv.rejected.map((r) => (
            <Row key={`${r.at_s}/${r.group}`}>
              <Cell head={t("tv.when")}>
                {t("tv.ago", { t: duration(tv.now_s - r.at_s, lang) })}
              </Cell>
              <Cell head={t("tv.group")}>
                <span className="font-mono">{r.group}</span>
                {r.client && <Detail>{r.client}</Detail>}
              </Cell>
              <Cell head={t("tv.why")}>
                <Badge tone="warn">{t(REASON[r.reason])}</Badge>
              </Cell>
            </Row>
          ))}
        </Table>
      )}
    </Card>
  );
}

function Events({ tv }: { tv: Iptv }) {
  const { t, lang } = useI18n();
  const c = tv.counters;
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
          {tv.log.map((e) => (
            <li
              key={`${e.at_s}/${e.event}/${e.group ?? ""}`}
              className="flex gap-x-2 whitespace-nowrap"
            >
              <span className="tabular w-32 shrink-0 whitespace-nowrap text-base-content/70">
                {t("tv.ago", { t: duration(tv.now_s - e.at_s, lang) })}
              </span>
              <span className="font-medium">{t(EVENT[e.event])}</span>
              {e.group && <span className="font-mono">{e.group}</span>}
              {e.vlan != null && <span className="text-base-content/70">VLAN {e.vlan}</span>}
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

export function IptvPage() {
  const { t } = useI18n();
  const tv = useIptv();
  return (
    <Page title={t("nav.tv")} hint={t("tv.hint")} actions={<FibreState />}>
      <QueryState reads={tv}>
        {() =>
          tv.data && (
            <Columns wide="left">
              <Stack>
                <Watched tv={tv.data} />
                <Refused tv={tv.data} />
                <Events tv={tv.data} />
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
