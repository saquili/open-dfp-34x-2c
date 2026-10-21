// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The fibre and what the operator set up on it, in five pages. Under the
 * switch, the fibre, in two tabs: the state of the connection, the light,
 * the registration with the OLT and the errors on the line; and what the
 * line registers with (the serial number or the LOID, and the passwords). Under
 * the operator, what OMCI carries: the services the OLT set up, each a VLAN
 * with its channels, the host's VLAN for it and multicast; the channels one
 * by one; what the stick tells the OLT of itself, which the owner may change
 * to what the operator expects; and the managed entities as omcid holds
 * them, for the curious.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Link, useNavigate } from "@tanstack/react-router";
import { Check, CircleAlert, Eye, Minus, Network, X } from "lucide-react";
import { Fragment, type ReactNode, useEffect, useMemo, useRef, useState } from "react";
import { Controller, type Resolver, useForm } from "react-hook-form";
import {
  useIdentity,
  useIdentityChange,
  useOltLogin,
  useOmci,
  useServiceRuleChange,
} from "../api/config";
import { useLive, useTraffic } from "../api/live";
import type { Identity, OltLogin, PotsPorts, ServiceRule, UniPorts, UniReport } from "../api/types";
import { Caret, opens } from "../components/CardTable";
import { Combobox } from "../components/Combobox";
import { useConfirm } from "../components/Confirm";
import { Field, SaveState, useChangeState } from "../components/Field";
import { useGemText } from "../components/Gem";
import { Page, Tabs, useTab } from "../components/Page";
import { Filter, Pager, usePages } from "../components/Pager";
import { QueryNote, QueryState, Reading } from "../components/Read";
import { PanelCard, SidePanel, withPanel } from "../components/SidePanel";
import { HistoryNote, Sparkline } from "../components/TrafficChart";
import { Badge, Card, Detail, Facts, Figure, GroupRow, Stat, TABLE } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { bitrate, dbmText, decimal, duration, integer } from "../lib/format";
import { handOltLogin } from "../lib/handoff";
import {
  type Channel,
  type Clash,
  clashes as clashesOf,
  isDefault,
  isDefaultEntry,
  Mib,
  type Multicast,
  num,
  type Service,
  services as servicesOf,
  tagsMatched,
  translation,
  type Unit,
  unit as unitOf,
  type VlanRule,
  vlanRules,
} from "../lib/omci";
import { upTo, z } from "../lib/schemas";
import {
  fibre,
  fibreVlan,
  type GemView,
  GPON_DOWN_BPS,
  GPON_UP_BPS,
  gems,
  LIGHT_TONE,
  REG_STEPS,
  type RegState,
  type RegStep,
  RX_MAX_DBM,
  RX_MIN_DBM,
  RX_WEAK_DBM,
  registration,
  sessions,
} from "../lib/state";
import { useStickServices } from "./internet/shared";
import { Columns, Stack } from "./system/shared";

/**
 * Where the received light sits: the receiver's working range, its weak end
 * marked, and the reading on it.
 */
function LightScale({ rx }: { rx: number | null }) {
  const { lang } = useI18n();
  const lo = RX_MIN_DBM - 4;
  const hi = RX_MAX_DBM + 4;
  const at = (v: number) => ((v - lo) / (hi - lo)) * 100;
  const band = (from: number, to: number, cls: string) => (
    <div
      className={`absolute inset-y-0 ${cls}`}
      style={{ left: `${at(from)}%`, width: `${at(to) - at(from)}%` }}
    />
  );
  return (
    <div className="mt-1">
      <div className="relative h-2 overflow-hidden rounded-full bg-base-300">
        {band(RX_MIN_DBM, RX_WEAK_DBM, "bg-warning/50")}
        {band(RX_WEAK_DBM, RX_MAX_DBM, "bg-success/50")}
      </div>
      <div className="relative h-0">
        {rx != null && (
          <div
            className="absolute -top-3 size-4 -translate-x-1/2 rounded-full border-2 border-base-100 bg-primary"
            style={{ left: `${at(Math.min(hi, Math.max(lo, rx)))}%` }}
          />
        )}
      </div>
      <div className="relative mt-1 h-4 text-xs text-base-content/70">
        {[RX_MIN_DBM, RX_WEAK_DBM, RX_MAX_DBM].map((v) => (
          <span key={v} className="absolute -translate-x-1/2" style={{ left: `${at(v)}%` }}>
            {decimal(v, 0, lang)}
          </span>
        ))}
      </div>
    </div>
  );
}

export function useMib() {
  const omci = useOmci();
  const mib = useMemo(() => new Mib(omci.data), [omci.data]);
  return { omci, mib };
}

/* ------------------------------------------------------------------ state */

/* A step's mark: its colour by how far it got, and a tick, a cross, a dash or its number. */
const MARK: Record<RegState, string> = {
  done: "bg-success text-success-content",
  now: "bg-warning text-warning-content animate-pulse",
  fail: "bg-error text-error-content",
  wait: "bg-base-200 text-base-content/70 ring-1 ring-base-300",
  off: "bg-base-200 text-base-content/70 ring-1 ring-base-300",
};

/*
 * A step, and where to look at it further: the line's configuration for what
 * the stick gives the OLT, the operator's page for what the OLT did over OMCI.
 */
const STEP: Record<RegStep, { title: Key; hint: Key; to?: string; tab?: FibreTab }> = {
  sync: { title: "fibre.stepSync", hint: "fibre.stepSyncHint" },
  serial: {
    title: "fibre.stepSerial",
    hint: "fibre.stepSerialHint",
    to: "/switch/fibre",
    tab: "config",
  },
  ranging: { title: "fibre.stepRanging", hint: "fibre.stepRangingHint" },
  ploam: {
    title: "fibre.stepPassword",
    hint: "fibre.stepPasswordHint",
    to: "/switch/fibre",
    tab: "config",
  },
  omcc: { title: "fibre.stepOmcc", hint: "fibre.stepOmccHint" },
  mib: { title: "fibre.stepMib", hint: "fibre.stepMibHint", to: "/operator/omci" },
  services: {
    title: "fibre.stepServices",
    hint: "fibre.stepServicesHint",
    to: "/operator/services",
  },
};

/**
 * The stick's registration on the OLT, step by step: synchronisation, the
 * serial number, the ONU number and the ranging, the PLOAM password, the OMCI
 * channel, the OLT's reading of the database and the services it set up;
 * each with how far it got and what the stick knows of it. Across the card on
 * a wide screen, one under the other with what each step is on a narrow one.
 */
function Registration() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const { omci, mib } = useMib();
  const identity = useIdentity();
  const f = fibre(live);
  const onu = live?.onu;
  const frames = omci.data?.frames?.handled ?? 0;
  const svc = mib.empty ? 0 : servicesOf(mib).length;
  const steps = registration({
    state: f.state,
    alarms: f.alarms,
    omcc: !!onu?.omcc_port_id,
    ploamSet: !!identity.data?.ploam_set,
    passwordAsked: onu?.counters?.password_requests,
    passwordAnswered: onu?.counters?.password_replies,
    omciFrames: frames,
    services: svc,
  });
  const n = (v: number | undefined) => integer(v ?? 0, lang);
  /* What the stick knows of a step, by how far it got. */
  const detail = (id: RegStep, at: RegState): string[] => {
    if (at === "wait") return [];
    switch (id) {
      case "sync":
        return [
          t(
            at === "done"
              ? "fibre.regSignal"
              : at === "now"
                ? "fibre.regSearching"
                : "fibre.regLost",
          ),
        ];
      case "serial":
        return at === "fail"
          ? [t("fibre.regStopped")]
          : [t("fibre.regAsked", { n: n(onu?.counters.sn_requests) })];
      case "ranging":
        return [
          t("fibre.regOnu", { id: onu?.onu_id ?? "–", n: n(onu?.counters.ranging_requests) }),
          ...(onu?.eqd != null
            ? /* The delay in bit periods of the upstream, as microseconds. */
              [t("fibre.regEqd", { us: decimal(onu.eqd / (GPON_UP_BPS / 1e6), 1, lang) })]
            : []),
        ];
      case "ploam": {
        const asked = onu?.counters?.password_requests;
        if (asked == null)
          return [t(at === "done" ? "fibre.regPasswordSet" : "fibre.regPasswordNone")];
        if (asked === 0) return [t("fibre.regNotAsked")];
        return [
          t(at === "fail" ? "fibre.regPasswordMissing" : "fibre.regPasswordGiven", {
            n: n(asked),
          }),
        ];
      }
      case "omcc":
        return [
          at === "done"
            ? t("fibre.regOmcc", { port: onu?.omcc_port_id ?? "" })
            : t("fibre.regWaitOlt"),
        ];
      case "mib":
        return [at === "done" ? t("fibre.regMib", { n: n(frames) }) : t("fibre.regWaitOlt")];
      case "services":
        return [
          at === "done"
            ? t("fibre.regServices", { s: svc, g: gems(live).length })
            : t("fibre.regNoServices"),
        ];
    }
  };
  return (
    <Card
      title={t("fibre.regTitle")}
      action={
        <span className="hidden text-xs text-base-content/70 sm:inline">{t("fibre.regHint")}</span>
      }
    >
      <QueryNote reads={[omci, identity]} />
      <ol className="grid gap-4 lg:grid-cols-7 lg:gap-2">
        {REG_STEPS.map((id, i) => {
          const at = steps[id];
          return (
            <li
              key={id}
              title={t(STEP[id].hint)}
              className="relative flex gap-3 lg:flex-col lg:items-center lg:text-center lg:not-last:after:absolute lg:not-last:after:top-3.5 lg:not-last:after:left-[calc(50%+1.25rem)] lg:not-last:after:right-[calc(-50%+1.25rem)] lg:not-last:after:h-px lg:not-last:after:bg-base-300"
            >
              <span
                className={`grid size-7 shrink-0 place-items-center rounded-full text-xs font-semibold ${MARK[at]}`}
              >
                {at === "done" ? (
                  <Check className="size-4" />
                ) : at === "fail" ? (
                  <X className="size-4" />
                ) : at === "off" ? (
                  <Minus className="size-4" />
                ) : (
                  i + 1
                )}
              </span>
              <span className="grid min-w-0 gap-0.5">
                {STEP[id].to ? (
                  <Link
                    to={STEP[id].to}
                    search={STEP[id].tab ? ({ tab: STEP[id].tab } as never) : undefined}
                    className="link link-hover link-primary text-sm font-medium"
                  >
                    {t(STEP[id].title)}
                  </Link>
                ) : (
                  <span className="text-sm font-medium">{t(STEP[id].title)}</span>
                )}
                {detail(id, at).map((line) => (
                  <span key={line} className="tabular truncate text-xs text-base-content/70">
                    {line}
                  </span>
                ))}
                <span className="text-xs text-base-content/70 lg:hidden">{t(STEP[id].hint)}</span>
              </span>
            </li>
          );
        })}
      </ol>
    </Card>
  );
}

function LineState() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const { omci, mib } = useMib();
  const f = fibre(live);
  const optics = live?.optics;
  const onu = live?.onu;
  const u = unitOf(mib);
  const encrypted = mib.all(268).filter((c) => (num(c, 8) ?? 0) !== 0).length;
  const c = onu?.counters;
  const fecShare =
    c && c.fec_codewords > 0 ? (c.fec_corrected_codewords / c.fec_codewords) * 100 : null;

  return (
    <div className="grid min-h-0 flex-1 gap-4 lg:grid-rows-[auto_min-content_minmax(min-content,1fr)]">
      <div className="grid gap-4 lg:grid-cols-3">
        <Card
          title={t("fibre.status")}
          action={<Badge tone={f.tone}>{t(`state.${f.stateKey}`)}</Badge>}
        >
          <Facts
            rows={[
              [t("fibre.onuId"), f.onuId ?? "–"],
              onu?.state_ms != null && [t("fibre.inState"), duration(onu.state_ms / 1000, lang)],
              [
                t("fibre.lineRate"),
                t("fibre.lineRateValue", {
                  down: decimal(GPON_DOWN_BPS / 1e9, 2, lang),
                  up: decimal(GPON_UP_BPS / 1e9, 2, lang),
                }),
              ],
              !mib.empty && [
                t("fibre.encryption"),
                encrypted ? t("fibre.encryptedN", { n: encrypted }) : t("fibre.encryptedNone"),
              ],
              u.olt && [
                t("fibre.olt"),
                [u.olt.vendorId, u.olt.equipment, u.olt.version].filter(Boolean).join(" · ") || "–",
              ],
              [
                t("fibre.alarms"),
                f.alarms.length ? (
                  <span key="alarms" className="text-error">
                    {f.alarms.join(", ")}
                  </span>
                ) : (
                  t("fibre.noAlarms")
                ),
              ],
              omci.data?.frames && [
                t("fibre.omciFrames"),
                t("fibre.omciFramesValue", {
                  handled: integer(omci.data.frames.handled, lang),
                  rejected: integer(omci.data.frames.rejected, lang),
                }),
              ],
            ]}
          />
          <p className="text-xs text-base-content/70">{t("fibre.onuIdHint")}</p>
        </Card>

        <Card
          title={t("fibre.optics")}
          action={<Badge tone={LIGHT_TONE[f.quality]}>{t(`light.${f.quality}`)}</Badge>}
        >
          <Figure
            value={dbmText(f.rxDbm, lang)}
            caption={t("fibre.rx")}
            tone={LIGHT_TONE[f.quality]}
          />
          <LightScale rx={f.rxDbm} />
          <p className="text-xs text-base-content/70">
            {t("fibre.rxRange", { min: dbmText(RX_MIN_DBM, lang), max: dbmText(RX_MAX_DBM, lang) })}
          </p>
          <div className="grid grid-cols-2 gap-3">
            <Stat label={t("fibre.tx")} value={dbmText(f.txDbm, lang)} />
            <Stat
              label={t("fibre.temperature")}
              value={optics ? `${decimal(optics.temperature_c, 1, lang)} °C` : "–"}
            />
            <Stat
              label={t("fibre.voltage")}
              value={optics ? `${decimal(optics.vcc_v, 2, lang)} V` : "–"}
            />
            <Stat
              label={t("fibre.bias")}
              value={optics ? `${integer(optics.bias_ma, lang)} mA` : "–"}
            />
          </div>
        </Card>

        <Card title={t("fibre.quality")}>
          {c ? (
            <>
              <div className="grid grid-cols-2 gap-3">
                <Stat
                  label={t("fibre.bipErrors")}
                  value={integer(c.bip_errors, lang)}
                  tone={c.bip_errors > 1000 ? "warn" : undefined}
                />
                <Stat
                  label={t("fibre.fecCorrected")}
                  value={integer(c.fec_corrected_codewords, lang)}
                  sub={fecShare != null ? `${decimal(fecShare, 6, lang)} %` : undefined}
                />
                <Stat
                  label={t("fibre.fecUncorrectable")}
                  value={integer(c.fec_uncorrectable_codewords, lang)}
                  tone={c.fec_uncorrectable_codewords ? "bad" : undefined}
                />
                <Stat label={t("fibre.gemFrames")} value={integer(c.gem_frames, lang)} />
                <Stat
                  label={t("fibre.ploamRx")}
                  value={integer(c.ploam_rx, lang)}
                  sub={t("fibre.crcErrors", { n: integer(c.ploam_crc_errors, lang) })}
                />
                <Stat
                  label={t("fibre.bwmap")}
                  value={integer(c.bwmap_rx, lang)}
                  sub={t("fibre.crcErrors", { n: integer(c.bwmap_crc_errors, lang) })}
                />
              </div>
              <p className="text-xs text-base-content/70">{t("fibre.sinceStart")}</p>
            </>
          ) : (
            <p className="text-sm">{t("state.noPon")}</p>
          )}
        </Card>
      </div>
      <Registration />
      <LineHistory />
    </div>
  );
}

/* The light and the errors of the last ten minutes. */
function LineHistory() {
  const { t, lang } = useI18n();
  const data = useTraffic();
  const rx = fibre(useLive().data).rxDbm;
  const now = data.at(-1);
  const peak = Math.max(1, ...data.map((r) => Math.max(r.bip, r.fecCorrected, r.fecLost)));
  return (
    <Card title={t("fibre.history")} action={<HistoryNote />}>
      <div className="grid min-h-0 flex-1 gap-6 md:grid-cols-2">
        <div className="flex min-h-0 flex-col gap-1">
          <div className="flex items-baseline justify-between text-sm">
            <span className="text-base-content/70">{t("fibre.rx")}</span>
            <span className="tabular font-medium">{dbmText(rx, lang)}</span>
          </div>
          <Sparkline
            data={data}
            value={(r) => r.rxDbm}
            min={RX_MIN_DBM - 4}
            max={RX_MAX_DBM + 4}
            format={(v) => dbmText(v, lang)}
          />
        </div>
        <div className="flex min-h-0 flex-col gap-1">
          <div className="flex items-baseline justify-between gap-3 text-sm">
            <span className="text-base-content/70">{t("fibre.errorsPerSecond")}</span>
            <span className="tabular text-xs">
              BIP {decimal(now?.bip ?? 0, 1, lang)} · FEC {decimal(now?.fecCorrected ?? 0, 1, lang)}{" "}
              ·{" "}
              <span className={now?.fecLost ? "text-error" : ""}>
                {t("fibre.lost")} {decimal(now?.fecLost ?? 0, 1, lang)}
              </span>
            </span>
          </div>
          <Sparkline
            data={data}
            value={(r) => r.bip + r.fecCorrected + r.fecLost}
            min={0}
            max={peak}
            format={(v) => `${decimal(v, 0, lang)}/s`}
            tone="var(--color-warning)"
            bars
          />
        </div>
      </div>
    </Card>
  );
}

/* --------------------------------------------------------------- services */

/*
 * A service's row in a card too narrow for the table: the caret, and beside
 * it the cells one under another, each under its column's name.
 */
const NARROW_ROW =
  "@max-4xl:grid @max-4xl:grid-cols-[2.5rem_minmax(0,1fr)] @max-4xl:border-t @max-4xl:border-base-300 @max-4xl:py-2 @max-4xl:first:border-t-0";
const NARROW_CELL =
  "@max-4xl:col-start-2 @max-4xl:border-0 @max-4xl:px-0 @max-4xl:py-1 @max-4xl:before:block @max-4xl:before:text-xs @max-4xl:before:font-normal @max-4xl:before:text-base-content/70 @max-4xl:before:content-[attr(data-h)]";

/* The same, from a narrower table on: the services' beside the form of a rule. */
const NARROW_ROW_SM =
  "@max-xl:grid @max-xl:grid-cols-[2.5rem_minmax(0,1fr)] @max-xl:border-t @max-xl:border-base-300 @max-xl:py-2 @max-xl:first:border-t-0";
const NARROW_CELL_SM =
  "@max-xl:col-start-2 @max-xl:border-0 @max-xl:px-0 @max-xl:py-1 @max-xl:before:block @max-xl:before:text-xs @max-xl:before:font-normal @max-xl:before:text-base-content/70 @max-xl:before:content-[attr(data-h)]";
/* The rest of a table's narrow layout, at each width. */
const NARROW_4XL = {
  block: "@max-4xl:block",
  hidden: "@max-4xl:hidden",
  caret: "@max-4xl:col-start-1 @max-4xl:row-start-1 @max-4xl:p-0",
  left: "@max-4xl:text-left",
  pb: "@max-4xl:pb-2",
  more: "@max-4xl:block @max-4xl:p-0 @max-4xl:pl-10",
};
const NARROW_SM = {
  block: "@max-xl:block",
  hidden: "@max-xl:hidden",
  caret: "@max-xl:col-start-1 @max-xl:row-start-1 @max-xl:p-0",
  left: "@max-xl:text-left",
  pb: "@max-xl:pb-2",
  more: "@max-xl:block @max-xl:p-0 @max-xl:pl-10",
};

function useChannelText() {
  const { t, lang } = useI18n();
  const dir = (c: Channel) =>
    c.multicast
      ? t("fibre.multicast")
      : c.direction === "both"
        ? t("fibre.both")
        : c.direction === "down"
          ? t("fibre.downstream")
          : c.direction === "up"
            ? t("fibre.upstream")
            : "–";
  const pbits = (c: Channel) =>
    c.pbits.length === 0
      ? null
      : c.pbits.length === 8
        ? t("fibre.pbitsAll")
        : t("fibre.pbits", { list: c.pbits.join(", ") });
  const rate = (r: { cir: number; pir: number } | null) =>
    r ? t("fibre.rate", { cir: bitrate(r.cir * 8, lang), pir: bitrate(r.pir * 8, lang) }) : null;
  return { dir, pbits, rate };
}

function MulticastFacts({ m }: { m: Multicast }) {
  const { t, lang } = useI18n();
  const version =
    m.version === 16 ? "MLDv1" : m.version === 17 ? "MLDv2" : `IGMPv${m.version ?? "?"}`;
  const fn = [t("fibre.mcSnoop"), t("fibre.mcSnoopReport"), t("fibre.mcProxy")][m.fn ?? 0];
  return (
    <li className={DETAIL_ROW}>
      <span className="grid content-start gap-1">
        <span className="font-semibold">{t("fibre.multicastTitle")}</span>
        <Detail>
          {version}, {fn}
        </Detail>
      </span>
      <span className="grid content-start gap-1 @4xl:col-span-3">
        {m.immediateLeave && <Detail>{t("fibre.immediateLeave")}</Detail>}
        {m.rate ? <Detail>{t("fibre.igmpRate", { n: integer(m.rate, lang) })}</Detail> : null}
        {m.downstreamVid != null && (
          <Detail>{t("fibre.mcastVlan", { vid: m.downstreamVid })}</Detail>
        )}
        {m.acl.map((a) => (
          <Detail key={`${a.portId}-${a.from}`}>
            {t("fibre.mcastGroups", { from: a.from, to: a.to, gem: a.portId, vlan: a.vlan })}
          </Detail>
        ))}
      </span>
    </li>
  );
}

/** A rule for the host's frames, in words. */
function useRuleText() {
  const { t } = useI18n();
  const prio = (p: number) => (p <= 7 ? t("fibre.prioN", { p }) : "");
  /* What a rule matches by its VLAN alone: the VLAN, any, or none. */
  const vlan = (r: VlanRule) => {
    const tags = tagsMatched(r);
    if (isDefault(r)) return tags === 1 ? t("fibre.matchOtherSingle") : t("fibre.matchOtherDouble");
    if (tags === 0) return t("fibre.matchUntagged");
    const tag = tags === 1 ? r.inner : r.outer;
    return tag.vid === 4096 ? t("fibre.anyVlan") : t("switch.vlanN", { id: tag.vid });
  };
  const match = (r: VlanRule) => {
    const tags = tagsMatched(r);
    if (isDefault(r) || tags === 0) return vlan(r);
    return [vlan(r), prio((tags === 1 ? r.inner : r.outer).prio)].filter(Boolean).join(", ");
  };
  const action = (r: VlanRule) => {
    if (r.remove === 3) return t("fibre.actDrop");
    const tr = translation(r);
    const added = r.addInner.prio !== 15 ? r.addInner : r.addOuter.prio !== 15 ? r.addOuter : null;
    const keep =
      added && (added.prio === 8 || added.prio === 9)
        ? t("fibre.prioKept")
        : added
          ? prio(added.prio)
          : "";
    if (!added) return r.remove ? t("fibre.actStrip") : t("fibre.actPass");
    const to = tr?.to == null ? "–" : t("switch.vlanN", { id: tr.to });
    return [r.remove ? t("fibre.actTranslate", { to }) : t("fibre.actAdd", { to }), keep]
      .filter(Boolean)
      .join(", ");
  };
  return { vlan, match, action };
}

/** What the stick does with a service: the channels' uses, as words, each a neutral pill. */
/*
 * What the stick does with a service's VLAN, as the VLAN page says it: its
 * sessions in it, or a bridge to the SFP port, or it ends on the stick; and
 * whether it carries multicast. The shared broadcast channel, which every
 * service has, says nothing of any one of them.
 */
function useServiceUse() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const views = gems(live);
  return (s: Service): { title: string; to?: string; more: string[] } => {
    const own = sessions(live).filter((x) => s.vlan != null && fibreVlan(x) === String(s.vlan));
    const ports = new Set(
      s.ports
        .filter((p) => (p.vlans?.length ?? 0) <= 1)
        .flatMap((p) => p.channels.map((c) => c.portId)),
    );
    const bridged = views.some((v) => ports.has(v.gem.port_id) && v.use === "bridged");
    const more = s.multicast ? [t("fibre.tagMulticast")] : [];
    if (own.length)
      return {
        title: t("fibre.svcSessions"),
        to: "/internet/wan",
        more: [`PPPoE ${own.map((x) => x.id).join(", ")}`, ...more],
      };
    return { title: t(bridged ? "fibre.svcBridged" : "fibre.svcEnds"), more };
  };
}

/* A service's VLAN as the host knows it, and the fibre's when the OLT translates it. */
function vlanName(
  t: (k: Key, v?: Record<string, string | number>) => string,
  s: Service,
): { main: string; fibre: number | null } {
  if (!s.hostVlans.length)
    return { main: s.vlan == null ? t("fibre.hostAll") : String(s.vlan), fibre: null };
  const main = s.hostVlans.map((v) => (v == null ? t("switch.untagged") : String(v))).join(", ");
  return { main, fibre: s.vlan != null && main !== String(s.vlan) ? s.vlan : null };
}

/*
 * A channel of a service's details: the channel and what the stick does with
 * it, its priorities, its upstream allocation, its traffic; one under
 * another in a narrow card. A line parts one from the next.
 */
const DETAIL_ROW =
  "grid gap-x-6 gap-y-2 border-t border-base-300 py-3 first:border-t-0 first:pt-0 @xl:grid-cols-2 @4xl:grid-cols-[minmax(10rem,14rem)_minmax(0,1fr)_minmax(0,1fr)_11rem]";

/* The details of a service: its channels one by one, and multicast. */
function ServiceDetails({ s }: { s: Service }) {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const rates = useTraffic().at(-1)?.gems;
  const ct = useChannelText();
  const gem = useGemText();
  const views = gems(live);
  const seen = new Set<number>();
  return (
    <ul className="grid">
      {s.ports.flatMap((port) =>
        port.channels.map((c) => {
          if (seen.has(c.portId)) return null;
          seen.add(c.portId);
          const v = views.find((x) => x.gem.port_id === c.portId);
          const r = v ? rates?.[v.gem.name] : undefined;
          return (
            <li key={c.portId} className={DETAIL_ROW}>
              <span className="grid content-start gap-1">
                <span className="tabular font-semibold">GEM {c.portId}</span>
                <Detail>{ct.dir(c)}</Detail>
                {v && <Detail>{gem.use(v)}</Detail>}
              </span>
              <span className="grid content-start gap-1">
                <span>{ct.pbits(c) ?? "–"}</span>
                {c.encrypted && <Detail>{t("fibre.aes")}</Detail>}
                {(port.vlans?.length ?? 0) > 1 && <Detail>{t("fibre.sharedBroadcast")}</Detail>}
              </span>
              <span className="grid content-start gap-1">
                <span className="tabular whitespace-nowrap">
                  {v?.gem.tcont != null
                    ? t("fibre.tcontGroup", { n: v.gem.tcont, alloc: c.allocId ?? "–" })
                    : c.allocId != null
                      ? `Alloc-ID ${c.allocId}`
                      : t("fibre.noUpstream")}
                </span>
                {c.queue && (
                  <Detail>
                    {t("fibre.queue", { p: c.queue.priority, w: c.queue.weight ?? "–" })}
                  </Detail>
                )}
                {ct.rate(c.up) && <Detail>↑ {ct.rate(c.up)}</Detail>}
                {ct.rate(c.down) && <Detail>↓ {ct.rate(c.down)}</Detail>}
              </span>
              <span className="tabular whitespace-nowrap @4xl:text-right">
                {r ? `↓ ${bitrate(r.down, lang)} ↑ ${bitrate(r.up, lang)}` : "–"}
              </span>
            </li>
          );
        }),
      )}
      {s.multicast && <MulticastFacts m={s.multicast} />}
    </ul>
  );
}

/*
 * Where a service leaves the stick, as the OLT set it up: Ethernet 1 to 4,
 * the PPTP, or the VEIP; and, when it does not stand apart on the one SFP
 * lane, why, with a warning.
 */
function Outputs({ s, clashes, onFix }: { s: Service; clashes: Clash[]; onFix: () => void }) {
  const { t } = useI18n();
  const said = (c: Clash) =>
    c.kind === "both"
      ? t("fibre.clashBoth")
      : t("fibre.clashVlan", { vid: c.vid ?? t("fibre.untagged"), with: c.with ?? "–" });
  const r = s.rule;
  const olt = s.oltHostVlans?.map((v) => v ?? t("fibre.untagged")).join(", ");
  return (
    <span className="grid justify-items-start gap-1">
      <span className="flex flex-wrap gap-1">
        {s.outputs.length === 0 && "–"}
        {s.outputs.map((o) => (
          <Badge key={o.kind === "uni" ? o.port : "veip"} tone="neutral">
            {o.kind === "uni" ? t("fibre.outEthernet", { n: o.port }) : "VEIP"}
          </Badge>
        ))}
      </span>
      {clashes.length > 0 ? (
        <>
          <Badge tone="warn">{t("fibre.clash")}</Badge>
          {[...new Set(clashes.map(said))].map((line) => (
            <Detail key={line}>{line}</Detail>
          ))}
        </>
      ) : (
        r && (
          <>
            <Badge tone={r.auto ? "good" : "neutral"}>
              {t(r.auto ? "fibre.ruleAuto" : "fibre.ruleOwn")}
            </Badge>
            <Detail>
              {r.action === "move"
                ? t("fibre.ruleMoved", { host: r.host ?? "–", olt: olt ?? "–" })
                : t(`fibre.rule_${r.action}`)}
            </Detail>
          </>
        )
      )}
      <button
        type="button"
        className="link link-primary link-hover text-xs"
        onClick={(e) => {
          e.stopPropagation();
          onFix();
        }}
      >
        {t(clashes.length > 0 ? "fibre.ruleFix" : "fibre.ruleChange")}
      </button>
    </span>
  );
}

/*
 * What the stick does with one service on its one SFP lane, as a person
 * chooses: the stick's own way (a move to a free VLAN when the service
 * clashes), a move to a VLAN towards the host, off, or one output alone of
 * a service the OLT gave both. A VLAN that another service reaches the host
 * in is refused, as it would clash again.
 */
function RuleForm({
  s,
  olt,
  all,
  onClose,
}: {
  s: Service;
  olt: Service | undefined;
  all: Service[];
  onClose: () => void;
}) {
  const { t } = useI18n();
  const change = useServiceRuleChange();
  const state = useChangeState(change);
  const both =
    (olt ?? s).outputs.some((o) => o.kind === "uni") &&
    (olt ?? s).outputs.some((o) => o.kind === "veip");
  const r = s.rule;
  const taken = new Set(
    all
      .filter((o) => o !== s && !o.off)
      .flatMap((o) => (o.hostVlans.length ? o.hostVlans : [o.vlan])),
  );
  const suggest = () => {
    for (let v = s.vlan ?? 1; v <= 4094; v++) if (v >= 1 && !taken.has(v)) return v;
    return 1;
  };
  const [action, setAction] = useState<"auto" | ServiceRule["action"]>(
    r && !r.auto ? r.action : "auto",
  );
  const [host, setHost] = useState(String(r?.action === "move" && r.host ? r.host : suggest()));
  const vid = Number(host);
  const error =
    action !== "move"
      ? null
      : !/^\d+$/.test(host) || vid < 1 || vid > 4094
        ? t("fibre.ruleHostRange")
        : taken.has(vid)
          ? t("fibre.ruleHostTaken", { vid })
          : null;
  const OPTIONS: { v: typeof action; label: Key; hint: Key; show: boolean }[] = [
    { v: "auto", label: "fibre.ruleOptAuto", hint: "fibre.ruleOptAutoHint", show: true },
    { v: "move", label: "fibre.ruleOptMove", hint: "fibre.ruleOptMoveHint", show: true },
    { v: "off", label: "fibre.ruleOptOff", hint: "fibre.ruleOptOffHint", show: true },
    { v: "only_uni", label: "fibre.ruleOptUni", hint: "fibre.ruleOptUniHint", show: both },
    { v: "only_veip", label: "fibre.ruleOptVeip", hint: "fibre.ruleOptVeipHint", show: both },
  ];
  return (
    <PanelCard
      id="service-rule"
      title={t("fibre.ruleTitle", { vlan: s.vlan ?? t("fibre.untagged") })}
      line={t("fibre.ruleLine")}
      onClose={onClose}
    >
      <form
        noValidate
        className="grid gap-3 p-4"
        onSubmit={(e) => {
          e.preventDefault();
          if (error) return;
          change.mutate(
            {
              FIBRE: s.vlan == null ? "" : String(s.vlan),
              ACTION: action,
              HOST: action === "move" ? host : undefined,
            },
            { onSuccess: onClose },
          );
        }}
      >
        <fieldset className="grid gap-2">
          <legend className="sr-only">{t("fibre.ruleLine")}</legend>
          {OPTIONS.filter((o) => o.show).map((o) => (
            <label key={o.v} className="flex cursor-pointer items-start gap-2.5">
              <input
                type="radio"
                name="service-rule"
                className="radio radio-sm mt-0.5"
                checked={action === o.v}
                onChange={() => setAction(o.v)}
              />
              <span className="grid gap-0.5">
                <span className="text-sm font-medium">{t(o.label)}</span>
                <Detail>{t(o.hint)}</Detail>
              </span>
            </label>
          ))}
        </fieldset>
        {action === "move" && (
          <Field
            label={t("fibre.ruleHost")}
            inputMode="numeric"
            mono
            value={host}
            error={error ?? undefined}
            hint={t("fibre.ruleHostHint")}
            onInput={(e) => setHost(e.currentTarget.value.replace(/\D/g, "").slice(0, 4))}
          />
        )}
        <div className="flex flex-wrap items-center gap-3 border-t border-base-300 pt-3">
          <button
            type="submit"
            className="btn btn-primary btn-sm"
            disabled={change.isPending || !!error}
          >
            {t("internet.save")}
          </button>
          <button type="button" className="btn btn-ghost btn-sm" onClick={onClose}>
            {t("common.cancel")}
          </button>
          <SaveState {...state} />
        </div>
      </form>
    </PanelCard>
  );
}

function ServicesTable({
  list,
  q,
  onFix,
  compact = false,
}: {
  list: Service[];
  q: string;
  onFix: (s: Service) => void;
  /** Beside the form of a rule: no channels nor traffic, and rows of cells only when very narrow. */
  compact?: boolean;
}) {
  const n = compact ? NARROW_SM : NARROW_4XL;
  const row = compact ? NARROW_ROW_SM : NARROW_ROW;
  const cell = compact ? NARROW_CELL_SM : NARROW_CELL;
  const cols = compact ? 3 : 5;
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const rates = useTraffic().at(-1)?.gems;
  const use = useServiceUse();
  const [open, setOpen] = useState<Set<string>>(() => new Set());
  const views = gems(live);
  const needle = q.trim();
  const shown = list.filter(
    (s) =>
      !needle ||
      String(s.vlan ?? "").includes(needle) ||
      s.hostVlans.some((h) => String(h ?? "").includes(needle)) ||
      s.ports.some((p) => p.channels.some((c) => String(c.portId).includes(needle))),
  );
  const pager = usePages(shown);
  const clash = clashesOf(list);
  const key = (s: Service) => String(s.vlan ?? "none");
  const toggle = (k: string) =>
    setOpen((o) => {
      const n = new Set(o);
      if (n.has(k)) n.delete(k);
      else n.add(k);
      return n;
    });

  return (
    <>
      <div className="@container min-h-0 flex-1 overflow-auto">
        <table className={`${TABLE} table-pin-rows ${n.block}`}>
          <thead className={n.hidden}>
            <tr>
              <th className="w-px" />
              <th>VLAN</th>
              <th>{t("fibre.colOutput")}</th>
              <th>{t("fibre.colStick")}</th>
              {!compact && <th>{t("fibre.colChannels")}</th>}
              {!compact && <th className="text-right">{t("fibre.now")}</th>}
            </tr>
          </thead>
          <tbody className={n.block}>
            {pager.shown.map((s) => {
              const k = key(s);
              const ids = [...new Set(s.ports.flatMap((p) => p.channels))];
              let down = 0;
              let up = 0;
              let known = false;
              for (const c of new Set(ids.map((c) => c.portId))) {
                const v = views.find((x) => x.gem.port_id === c);
                const r = v ? rates?.[v.gem.name] : undefined;
                known ||= r != null;
                down += r?.down ?? 0;
                up += r?.up ?? 0;
              }
              const isOpen = open.has(k);
              const more = `service-${k}`;
              const name = vlanName(t, s);
              const u = use(s);
              const channels = [...new Set(ids.map((c) => c.portId))];
              return (
                <Fragment key={k}>
                  <tr
                    className={`cursor-pointer hover:bg-base-200/40 ${isOpen ? "[&>td]:border-b-0" : ""} ${row}`}
                    onClick={opens(() => toggle(k))}
                  >
                    <td className={`w-px ${n.caret}`}>
                      <Caret
                        open={isOpen}
                        controls={more}
                        label={t("fibre.serviceDetails", { vlan: name.main })}
                        onToggle={() => toggle(k)}
                      />
                    </td>
                    <td data-h="VLAN" className={cell}>
                      <span className="grid gap-0.5">
                        <span className="tabular font-semibold">{name.main}</span>
                        {name.fibre != null && (
                          <Detail>{t("internet.onFibre", { vid: name.fibre })}</Detail>
                        )}
                      </span>
                    </td>
                    <td data-h={t("fibre.colOutput")} className={cell}>
                      <Outputs s={s} clashes={clash.get(s.vlan) ?? []} onFix={() => onFix(s)} />
                    </td>
                    <td data-h={t("fibre.colStick")} className={cell}>
                      <span className="grid gap-0.5">
                        {u.to ? (
                          <Link to={u.to} className="link link-hover link-primary">
                            {u.title}
                          </Link>
                        ) : (
                          <span>{u.title}</span>
                        )}
                        {u.more.length > 0 && <Detail>{u.more.join(" · ")}</Detail>}
                      </span>
                    </td>
                    {!compact && (
                      <>
                        <td data-h={t("fibre.colChannels")} className={cell}>
                          {channels.length ? (
                            <Link
                              to="/operator/channels"
                              className="link link-hover link-primary tabular"
                            >
                              GEM {channels.join(", ")}
                            </Link>
                          ) : (
                            "–"
                          )}
                        </td>
                        <td
                          data-h={t("fibre.now")}
                          className={`tabular text-right whitespace-nowrap ${n.left} ${cell}`}
                        >
                          {known ? `↓ ${bitrate(down, lang)} ↑ ${bitrate(up, lang)}` : "–"}
                        </td>
                      </>
                    )}
                  </tr>
                  {isOpen && (
                    <tr id={more} data-more className={`${n.block} ${n.pb}`}>
                      <td className={n.hidden} />
                      <td colSpan={cols} className={`pt-0 ${n.more}`}>
                        <ServiceDetails s={s} />
                      </td>
                    </tr>
                  )}
                </Fragment>
              );
            })}
            {!pager.shown.length && (
              <tr className={n.block}>
                <td colSpan={cols} className={`text-base-content/70 ${n.block}`}>
                  {t("fibre.noMatch")}
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
      <Pager pager={pager} />
    </>
  );
}

/*
 * The VLAN rules, those next to each other that match the same VLAN and do
 * the same with it one row, their priorities listed under it; the default
 * rules, when they do the same, one row too, numbered from the first to the
 * last.
 */
function RulesTable({ mib, q }: { mib: Mib; q: string }) {
  const { t } = useI18n();
  const text = useRuleText();
  const rows: { me: number; first: number; last: number; rules: VlanRule[]; key: string }[] = [];
  for (const set of vlanRules(mib))
    set.rules.forEach((r, i) => {
      const tag = tagsMatched(r) === 2 ? r.outer : r.inner;
      const key = `${isDefaultEntry(r) ? "default" : `${tagsMatched(r)}-${tag.vid}`}|${text.action(r)}`;
      const prev = rows.at(-1);
      if (prev && prev.me === set.me && prev.key === key && prev.last === i - 1) {
        prev.last = i;
        prev.rules.push(r);
      } else rows.push({ me: set.me, first: i, last: i, rules: [r], key });
    });
  const needle = q.trim();
  const shown = rows.filter(
    (x) =>
      !needle ||
      x.rules.some((r) => {
        const tr = translation(r);
        return String(tr?.from ?? "").includes(needle) || String(tr?.to ?? "").includes(needle);
      }),
  );
  const pager = usePages(shown);
  /* What a row matches: the VLAN, the priorities under it; or the default rules by name. */
  const match = (rules: VlanRule[]) => {
    const [r] = rules as [VlanRule, ...VlanRule[]];
    if (isDefaultEntry(r))
      return rules.length > 1 ? (
        <>
          <span>{t("fibre.matchAnyOther")}</span>
          <Detail>
            {t("fibre.ruleDefaults", { list: rules.map((x) => text.match(x)).join(" · ") })}
          </Detail>
        </>
      ) : (
        <>
          <span>{text.match(r)}</span>
          <Detail>{t("fibre.ruleDefault")}</Detail>
        </>
      );
    const tag = (x: VlanRule) => (tagsMatched(x) === 2 ? x.outer : x.inner);
    const prios = rules.map((x) => tag(x).prio);
    return (
      <>
        <span>{text.vlan(r)}</span>
        {prios.every((p) => p <= 7) && <Detail>{t("fibre.prioN", { p: prios.join(", ") })}</Detail>}
      </>
    );
  };
  return (
    <>
      <div className="min-h-0 flex-1 overflow-auto">
        <table className={`${TABLE} table-pin-rows`}>
          <thead>
            <tr>
              <th className="w-14">#</th>
              <th>{t("fibre.ruleFrom")}</th>
              <th>{t("fibre.ruleDo")}</th>
            </tr>
          </thead>
          <tbody>
            {pager.shown.map((x) => (
              <tr key={`${x.me}-${x.first}`}>
                <td className="tabular whitespace-nowrap text-base-content/70">
                  {x.first === x.last ? x.first + 1 : `${x.first + 1}–${x.last + 1}`}
                </td>
                <td>
                  <span className="grid gap-0.5">{match(x.rules)}</span>
                </td>
                <td>{text.action(x.rules[0] as VlanRule)}</td>
              </tr>
            ))}
            {!pager.shown.length && (
              <tr>
                <td colSpan={3} className="text-base-content/70">
                  {rows.length ? t("fibre.noMatch") : t("fibre.rulesNone")}
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
      <Pager pager={pager} />
    </>
  );
}

/*
 * What the OLT set up, or while it is read, that it is; why it could not be
 * read, with the button that reads it again; or, when the stick has none,
 * that it has none.
 */
export function OmciState({ children }: { children: ReactNode }) {
  const { t } = useI18n();
  const { omci, mib } = useMib();
  const absent = (
    <Card>
      <p className="text-sm">{t("fibre.omciAbsent")}</p>
    </Card>
  );
  return (
    <QueryState reads={omci} reading={<Reading>{t("fibre.reading")}</Reading>} missing={absent}>
      {mib.empty ? absent : children}
    </QueryState>
  );
}

/* The services, or the VLAN rules, as the page's tab asks; a card that fits its table. */
function ServiceList({ view }: { view: ServiceTab }) {
  const { t } = useI18n();
  const { omci, mib } = useMib();
  const stick = useStickServices();
  const [q, setQ] = useState("");
  /* The service whose rule is being chosen, by its VLAN on the fibre. */
  const [fixing, setFixing] = useState<{ vlan: number | null } | null>(null);
  const fixed =
    fixing && view === "services" ? stick.list.find((x) => x.vlan === fixing.vlan) : undefined;
  return (
    <OmciState>
      <div className={withPanel(fixed != null)}>
        <section className="card min-h-0 border border-base-300 bg-base-100">
          <div className="card-body min-h-0 gap-3 p-4 sm:p-5">
            <div className="flex justify-end">
              <Filter value={q} onChange={setQ} placeholder={t("fibre.filterVlan")} />
            </div>
            {omci.data?.truncated && (
              <div
                role="alert"
                className="alert alert-warning alert-soft text-sm text-base-content"
              >
                <CircleAlert className="size-4 shrink-0 text-warning" aria-hidden />
                {t("fibre.truncated")}
              </div>
            )}
            {view === "services" ? (
              <ServicesTable
                list={stick.list}
                q={q}
                compact={fixed != null}
                onFix={(x) => setFixing({ vlan: x.vlan })}
              />
            ) : (
              <RulesTable mib={mib} q={q} />
            )}
          </div>
        </section>
        {fixed && (
          <SidePanel onClose={() => setFixing(null)}>
            <RuleForm
              key={String(fixed.vlan)}
              s={fixed}
              olt={stick.olt.find((x) => x.vlan === fixed.vlan)}
              all={stick.list}
              onClose={() => setFixing(null)}
            />
          </SidePanel>
        )}
      </div>
    </OmciState>
  );
}

/* --------------------------------------------------------------- channels */

/*
 * What a channel carries, in the statistics' words, and what more to say of
 * it. A channel that carries a session's VLAN beside the session's own hands
 * its frames to the session (onu-apply's feeds): an operator sends the
 * session's discovery and control frames by such a channel, so it is the
 * session's too.
 */
function useChannelUse() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const views = gems(live);
  const own = sessions(live);
  return (v: GemView, vlans: (number | null)[]): { title: string; to?: string; more?: string } => {
    if (v.gem.multicast) return { title: t("fibre.chanMulticast") };
    const ids = (vid: string | number | null | undefined) =>
      own.filter((x) => vid != null && fibreVlan(x) === String(vid)).map((x) => x.id);
    if (v.use === "session") {
      const list = ids(v.vlan);
      return {
        title: t("fibre.chanSession"),
        to: "/internet/wan",
        more: list.length ? `PPPoE ${list.join(", ")}` : undefined,
      };
    }
    const shared = vlans.length > 1 ? t("fibre.sharedBroadcast") : undefined;
    const feeds = views.find(
      (x) => x.use === "session" && vlans.some((vid) => vid != null && String(vid) === x.vlan),
    );
    if (v.use === "ends" && feeds)
      return {
        title: t("fibre.chanSession"),
        to: "/internet/wan",
        more: t("fibre.chanFeeds", { gem: feeds.gem.port_id }),
      };
    return { title: t(v.use === "bridged" ? "fibre.svcBridged" : "fibre.svcEnds"), more: shared };
  };
}

/*
 * The channels the OLT set up, a table grouped by T-CONT, the upstream
 * allocation, the downstream-only ones last: each with its VLAN as the host
 * knows it, what it carries, its direction, its IEEE 802.1p priorities and
 * its traffic now. The counts since the start are the statistics' page's.
 */
function ChannelList() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const rates = useTraffic().at(-1)?.gems;
  const { omci, mib } = useMib();
  const use = useChannelUse();
  const views = gems(live);
  const info = new Map<number, { c: Channel; services: Service[] }>();
  for (const s of servicesOf(mib))
    for (const p of s.ports)
      for (const c of p.channels) {
        const had = info.get(c.portId);
        info.set(c.portId, { c, services: [...(had?.services ?? []), s] });
      }

  const groups = new Map<number | null, typeof views>();
  for (const v of views) groups.set(v.gem.tcont, [...(groups.get(v.gem.tcont) ?? []), v]);
  const order = [...groups.keys()].sort((a, z) => (a ?? 99) - (z ?? 99));
  const prios = (list: number[]) =>
    !list.length ? "–" : list.length === 8 ? t("fibre.prioAny") : list.join(", ");

  return (
    <Card scroll className="min-h-0">
      <QueryNote reads={omci} aside />
      <table className={`${TABLE} table-pin-rows`}>
        <thead>
          <tr>
            <th className="w-32">{t("fibre.channel")}</th>
            <th className="w-44">VLAN</th>
            <th>{t("switch.vlanUse")}</th>
            <th className="w-40">{t("fibre.direction")}</th>
            <th className="w-28">{t("fibre.colPbits")}</th>
            <th className="w-36 text-right">{t("fibre.now")}</th>
          </tr>
        </thead>
        <tbody>
          {order.map((tc, i) => {
            const list = groups.get(tc) ?? [];
            const alloc = list[0]?.allocId;
            return (
              <Fragment key={String(tc)}>
                <GroupRow span={6} first={i === 0}>
                  {tc == null
                    ? t("fibre.downOnlyGroup")
                    : t("fibre.tcontGroup", { n: tc, alloc: alloc ?? "–" })}
                  {tc == null && (
                    <span className="ml-2 font-normal">{t("fibre.downOnlyHint")}</span>
                  )}
                </GroupRow>
                {list.map((v) => {
                  const g = v.gem;
                  const o = info.get(g.port_id);
                  const names = (o?.services ?? []).map((s) => vlanName(t, s));
                  const mains = [...new Set(names.map((n) => n.main))];
                  const fibres = [
                    ...new Set(names.flatMap((n) => (n.fibre == null ? [] : [n.fibre]))),
                  ];
                  const u = use(
                    v,
                    (o?.services ?? []).map((s) => s.vlan),
                  );
                  const r = rates?.[g.name];
                  const down = g.multicast || g.direction === "downstream";
                  return (
                    <tr key={g.name}>
                      <td>
                        <span className="grid gap-0.5">
                          <span className="tabular font-semibold whitespace-nowrap">
                            GEM {g.port_id}
                          </span>
                          {o?.c.encrypted && (
                            <span title={t("fibre.aes")}>
                              <Detail>{t("fibre.encShort")}</Detail>
                            </span>
                          )}
                        </span>
                      </td>
                      <td>
                        <span className="grid gap-0.5">
                          <span className="tabular">{mains.length ? mains.join(", ") : "–"}</span>
                          {fibres.length > 0 && (
                            <Detail>{t("internet.onFibre", { vid: fibres.join(", ") })}</Detail>
                          )}
                        </span>
                      </td>
                      <td>
                        <span className="grid gap-0.5">
                          {u.to ? (
                            <Link to={u.to} className="link link-hover link-primary">
                              {u.title}
                            </Link>
                          ) : (
                            <span>{u.title}</span>
                          )}
                          {u.more && <Detail>{u.more}</Detail>}
                        </span>
                      </td>
                      <td>
                        {t(
                          g.direction === "both"
                            ? "fibre.both"
                            : g.direction === "upstream"
                              ? "fibre.upstream"
                              : "fibre.downstream",
                        )}
                      </td>
                      <td className="tabular">{prios(o?.c.pbits ?? [])}</td>
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
              </Fragment>
            );
          })}
        </tbody>
      </table>
    </Card>
  );
}

/* --------------------------------------------------------------- identity */

type Group = "unit" | "software";

/* The fields a stick of older software has none of. */
const EXTENDED_FIELDS: string[] = ["sw_version0", "sw_version1", "pack_version"];

/*
 * A field of what OMCI tells the OLT of the stick: where it goes, its words,
 * and what the OLT reads of it now, from the stick's OMCI database; null
 * where no managed entity carries it.
 */
const IDENTITY_FIELDS: {
  field: Exclude<keyof Values, "serial" | "loid" | "ploam" | "loid_secret">;
  key: string;
  label: Key;
  group: Group;
  olt: ((u: Unit) => string | number | null) | null;
  numeric?: boolean;
  mono?: boolean;
}[] = [
  {
    field: "vendor_id",
    key: "VENDOR_ID",
    label: "fibre.vendorId",
    group: "unit",
    olt: (u) => u.vendorId,
    mono: true,
  },
  {
    field: "version",
    key: "VERSION",
    label: "fibre.version",
    group: "unit",
    olt: (u) => u.version,
  },
  {
    field: "equipment_id",
    key: "EQUIPMENT_ID",
    label: "fibre.equipmentId",
    group: "unit",
    olt: (u) => u.equipment,
  },
  { field: "model", key: "MODEL", label: "fibre.model", group: "unit", olt: null },
  {
    field: "equipment_serial",
    key: "EQUIPMENT_SERIAL",
    label: "fibre.equipmentSerial",
    group: "unit",
    olt: null,
  },
  {
    field: "omcc_version",
    key: "OMCC_VERSION",
    label: "fibre.omccVersion",
    group: "software",
    olt: (u) => u.omcc,
    numeric: true,
  },
  {
    field: "vendor_product_code",
    key: "VENDOR_PRODUCT_CODE",
    label: "fibre.productCode",
    group: "software",
    olt: (u) => u.productCode,
    numeric: true,
  },
  {
    field: "tm_option",
    key: "TM_OPTION",
    label: "fibre.tmOption",
    group: "software",
    olt: (u) => u.tmOption,
    numeric: true,
  },
  {
    field: "sw_version0",
    key: "SW_VERSION0",
    label: "fibre.swVersion0",
    group: "software",
    olt: (u) => u.images.find((i) => i.id === 0)?.version ?? null,
  },
  {
    field: "sw_version1",
    key: "SW_VERSION1",
    label: "fibre.swVersion1",
    group: "software",
    olt: (u) => u.images.find((i) => i.id === 1)?.version ?? null,
  },
  {
    field: "pack_version",
    key: "PACK_VERSION",
    label: "fibre.packVersion",
    group: "software",
    olt: (u) => u.packVersion,
  },
];

const numberOrEmpty = (max: number) =>
  z.string().check(
    z.trim(),
    z.refine((v) => v === "" || (/^(0x[0-9a-f]+|\d+)$/i.test(v) && Number(v) <= max), `0…${max}`),
  );

const identitySchema = (rule: string) =>
  z.object({
    serial: z.string().check(z.trim(), z.regex(/^[A-Za-z0-9]{4}[0-9A-Fa-f]{8}$/, rule)),
    loid: upTo(24),
    vendor_id: upTo(4),
    version: upTo(14),
    equipment_id: upTo(20),
    model: upTo(32),
    equipment_serial: upTo(32),
    omcc_version: numberOrEmpty(255),
    vendor_product_code: numberOrEmpty(65535),
    tm_option: numberOrEmpty(2),
    sw_version0: upTo(14),
    sw_version1: upTo(14),
    pack_version: upTo(14),
    ploam: z.string().check(z.maxLength(36)),
    loid_secret: z.string().check(z.maxLength(12)),
  });
type Values = z.infer<ReturnType<typeof identitySchema>>;

/* The checks of the fields @names only: a field a form does not show cannot stop it. */
function identityResolver(rule: string, names: (keyof Values)[]): Resolver<Values> {
  const mask: { [k in keyof Values]?: true } = {};
  for (const n of names) mask[n] = true;
  return zodResolver(z.pick(identitySchema(rule), mask)) as unknown as Resolver<Values>;
}

function initial(i: Identity): Values {
  return {
    serial: i.serial,
    loid: i.loid ?? "",
    vendor_id: i.vendor_id,
    version: i.version,
    equipment_id: i.equipment_id,
    model: i.model,
    equipment_serial: i.equipment_serial,
    omcc_version: i.omcc_version,
    vendor_product_code: i.vendor_product_code,
    tm_option: i.tm_option,
    sw_version0: i.sw_version0 ?? "",
    sw_version1: i.sw_version1 ?? "",
    pack_version: i.pack_version ?? "",
    ploam: "",
    loid_secret: "",
  };
}

/*
 * Whether the stick's value is the one the OLT reads: true or false, null when
 * the field is empty and the stick gives its factory value, undefined when
 * there is nothing to compare with. An empty value the OLT reads is a value
 * too: the same as an empty field.
 */
function sameAsOlt(
  stick: string,
  olt: string | number | null | undefined,
  numeric?: boolean,
): boolean | null | undefined {
  if (olt == null) return undefined;
  if (olt === "") return stick.trim() === "";
  if (stick.trim() === "") return null;
  return numeric
    ? Number(stick) === Number(olt)
    : stick.trim().toUpperCase() === String(olt).trim().toUpperCase();
}

/* A line under a field: what it says, with a mark when it agrees or not. */
function Note({ mark, children }: { mark: "same" | "differs"; children: ReactNode }) {
  return (
    <span className="flex items-start gap-1.5">
      {mark === "same" ? (
        <Check className="mt-px size-3.5 shrink-0 text-success" aria-hidden />
      ) : (
        <CircleAlert className="mt-px size-3.5 shrink-0 text-warning" aria-hidden />
      )}
      {children}
    </span>
  );
}

/*
 * What the OLT sees of a field that holds @stick, the line under the field:
 * the same, another value, the factory value for an empty field, or not yet
 * read. A number above 9 shows in decimal and hexadecimal.
 */
function useOltNote() {
  const { t } = useI18n();
  const shown = (olt: string | number, numeric?: boolean) =>
    numeric && typeof olt === "number" && olt > 9 ? `${olt} (0x${olt.toString(16)})` : String(olt);
  return (stick: string, olt: string | number | null | undefined, numeric?: boolean): ReactNode => {
    const same = sameAsOlt(stick, olt, numeric);
    if (olt == null || same === undefined) return t("fibre.oltNotYet");
    if (same === null) return t("fibre.oltSeesFactory", { v: shown(olt, numeric) });
    if (same)
      return <Note mark="same">{t(olt === "" ? "fibre.oltSeesEmpty" : "fibre.oltSeesSame")}</Note>;
    return (
      <Note mark="differs">
        {olt === "" ? t("fibre.oltSeesNone") : t("fibre.oltSeesOther", { v: shown(olt, numeric) })}
      </Note>
    );
  };
}

/*
 * The foot of a form whose change makes the stick register with the OLT
 * again: the button says so; asked, the foot asks once more where it stood,
 * and the second click of a double one on the button that asked does not
 * answer.
 */
function ReregisterFoot({
  asking,
  dirty,
  pending,
  state,
  onConfirm,
  onCancel,
  onDiscard,
}: {
  asking: boolean;
  dirty: boolean;
  pending: boolean;
  state: ReturnType<typeof useChangeState>;
  onConfirm: () => void;
  onCancel: () => void;
  onDiscard: () => void;
}) {
  const { t } = useI18n();
  const yes = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    if (asking) yes.current?.focus();
  }, [asking]);
  return (
    <div className="flex flex-wrap items-center gap-3 border-t border-base-300 pt-3">
      {asking ? (
        <>
          <button
            ref={yes}
            type="button"
            className="btn btn-primary btn-sm"
            onClick={(e) => e.detail < 2 && onConfirm()}
          >
            {t("fibre.registerAgain")}
          </button>
          <button type="button" className="btn btn-ghost btn-sm" onClick={onCancel}>
            {t("common.cancel")}
          </button>
          <span role="alert" className="flex min-w-0 flex-1 basis-64 items-start gap-1.5 text-sm">
            <CircleAlert className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
            {t("fibre.identityConfirm")}
          </span>
        </>
      ) : (
        <>
          <button type="submit" className="btn btn-primary btn-sm" disabled={!dirty || pending}>
            {t("fibre.saveRegister")}
          </button>
          {dirty && (
            <button type="button" className="btn btn-ghost btn-sm" onClick={onDiscard}>
              {t("form.discard")}
            </button>
          )}
          <SaveState {...state} />
          <span className="flex min-w-0 flex-1 basis-64 items-start gap-1.5 text-xs text-base-content/70">
            <CircleAlert className="mt-px size-3.5 shrink-0 text-warning" aria-hidden />
            {t("fibre.identityWarning")}
          </span>
        </>
      )}
    </div>
  );
}

/* A group of fields' heading: its name and what it holds. */
function FieldsHead({ title, hint }: { title: Key; hint: Key }) {
  const { t } = useI18n();
  return (
    <div>
      <h3 className="text-base font-semibold">{t(title)}</h3>
      <p className="text-xs text-base-content/70">{t(hint)}</p>
    </div>
  );
}

/*
 * The OMCC versions of ITU-T G.988 (ONU2-G), offered in the field: up to
 * 0x85 the editions of G.984.4, then each edition with the baseline message
 * set only, or with the extended one too, which omcid does not handle. The
 * stick gives 0xA3 when the field is empty.
 */
const OMCC_VERSIONS: { v: number; std: string; set?: "base" | "ext" }[] = [
  { v: 0x80, std: "G.984.4 (2004)" },
  { v: 0x81, std: "G.984.4 Amd. 1 (2005)" },
  { v: 0x82, std: "G.984.4 Amd. 2 (2006)" },
  { v: 0x83, std: "G.984.4 Amd. 3 (2006)" },
  { v: 0x84, std: "G.984.4 (2008)" },
  { v: 0x85, std: "G.984.4 (2008) Amd. 1 (2009)" },
  { v: 0x86, std: "G.984.4 (2008) Amd. 2 (2009)", set: "base" },
  { v: 0x96, std: "G.984.4 (2008) Amd. 2 (2009)", set: "ext" },
  { v: 0xa0, std: "G.988 (2010)", set: "base" },
  { v: 0xa1, std: "G.988 Amd. 1 (2011)", set: "base" },
  { v: 0xa2, std: "G.988 Amd. 2 (2012)", set: "base" },
  { v: 0xa3, std: "G.988 (2012)", set: "base" },
  { v: 0xb0, std: "G.988 (2010)", set: "ext" },
  { v: 0xb1, std: "G.988 Amd. 1 (2011)", set: "ext" },
  { v: 0xb2, std: "G.988 Amd. 2 (2012)", set: "ext" },
  { v: 0xb3, std: "G.988 (2012)", set: "ext" },
];

/* The traffic management options of ONU-G, by their value. */
const TM_OPTIONS: Key[] = ["fibre.tm0", "fibre.tm1", "fibre.tm2"];

/*
 * What OMCI tells the OLT of the stick, in its two groups side by side, in
 * rows shared across both, so that the fields of a row are level, as in the
 * registration's form. Under each field one line: what the OLT sees of it,
 * and for the OMCC version first what that version is. The traffic
 * management option is a choice of its three values. Only these fields are
 * checked and sent, and with them the serial number as it is, which every
 * change of the identity carries.
 */
function IdentityForm({ identity }: { identity: Identity }) {
  const { t } = useI18n();
  const change = useIdentityChange();
  const state = useChangeState(change);
  const { mib } = useMib();
  const u = mib.empty ? null : unitOf(mib);
  const note = useOltNote();
  const extended = identity.loid !== undefined;
  const visible = IDENTITY_FIELDS.filter((f) => extended || !EXTENDED_FIELDS.includes(f.field));
  const rule = t("fibre.serialRule");
  const names = visible.map((f) => f.field).join(" ");
  const resolver = useMemo(
    () => identityResolver(rule, names.split(" ") as (keyof Values)[]),
    [rule, names],
  );
  const form = useForm<Values>({ resolver, defaultValues: initial(identity) });
  const { errors, isDirty, dirtyFields } = form.formState;
  const values = form.watch();
  const [asking, setAsking] = useState(false);

  useEffect(() => {
    if (!form.formState.isDirty) form.reset(initial(identity));
  }, [identity, form]);

  const ask = form.handleSubmit(() => setAsking(isDirty));
  const save = form.handleSubmit((v) => {
    setAsking(false);
    const out: Record<string, string> = { SERIAL: identity.serial };
    for (const f of visible) if (dirtyFields[f.field]) out[f.key] = v[f.field];
    change.mutate(out, { onSuccess: () => form.reset(v) });
  });

  /* The OMCC versions of G.988 for the field's list, with their message sets. */
  const omccOptions = OMCC_VERSIONS.map((o) => ({
    value: `0x${o.v.toString(16)}`,
    label: o.std,
    hint: o.set ? t(o.set === "base" ? "fibre.omccBase" : "fibre.omccExt") : undefined,
  }));

  /* The OMCC version a field's value names, in words; nothing for an empty one. */
  const omcc = (value: string) => {
    if (!value.trim()) return null;
    const o = OMCC_VERSIONS.find((x) => x.v === Number(value));
    if (!o) return t("fibre.omccUnknown");
    return [
      o.std,
      o.set && t(o.set === "base" ? "fibre.omccBase" : "fibre.omccExt"),
      o.v === 0xa3 && t("fibre.omccDefault"),
    ]
      .filter(Boolean)
      .join(" · ");
  };
  const line = (f: (typeof IDENTITY_FIELDS)[number]) => {
    const value = values[f.field] ?? "";
    if (!f.olt)
      return [!value.trim() && t("fibre.factoryValue"), t("fibre.oltNone")]
        .filter(Boolean)
        .join(" ");
    return note(value, u ? f.olt(u) : undefined, f.numeric);
  };
  const field = (f: (typeof IDENTITY_FIELDS)[number]) => {
    if (f.field === "tm_option")
      return (
        <div key={f.field} className="grid content-start gap-1">
          <label htmlFor={f.field} className="text-sm font-semibold">
            {t(f.label)}
          </label>
          <select
            id={f.field}
            className={`select w-full ${errors[f.field] ? "select-error" : ""}`}
            {...form.register(f.field)}
          >
            <option value="">{t("fibre.tmFactory")}</option>
            {TM_OPTIONS.map((k, n) => (
              <option key={k} value={String(n)}>
                {t(k)}
              </option>
            ))}
          </select>
          {errors[f.field] ? (
            <p className="text-xs text-error">{errors[f.field]?.message}</p>
          ) : (
            <p className="text-xs text-base-content/70">{line(f)}</p>
          )}
        </div>
      );
    if (f.field === "omcc_version")
      return (
        <Controller
          key={f.field}
          control={form.control}
          name={f.field}
          render={({ field: c, fieldState }) => {
            const named = omcc(c.value ?? "");
            return (
              <Combobox
                label={t(f.label)}
                value={c.value ?? ""}
                onChange={c.onChange}
                onBlur={c.onBlur}
                mono
                options={omccOptions}
                error={fieldState.error?.message}
                listLabel={t("fibre.omccList")}
                none={t("fibre.omccNoMatch")}
                hint={
                  named ? (
                    <span className="grid gap-0.5">
                      <span>{named}</span>
                      <span>{line(f)}</span>
                    </span>
                  ) : (
                    line(f)
                  )
                }
              />
            );
          }}
        />
      );
    return (
      <Field
        key={f.field}
        label={t(f.label)}
        mono={f.mono}
        error={errors[f.field]?.message}
        hint={line(f)}
        {...form.register(f.field)}
      />
    );
  };

  const unit = visible.filter((f) => f.group === "unit");
  const soft = visible.filter((f) => f.group === "software");
  const rows = 1 + Math.max(unit.length, soft.length);
  /* Empty cells fill the shorter group, so that the other's fields keep to their rows. */
  const fill = (n: number, key: string) =>
    Array.from({ length: n }, (_, i) => (
      // biome-ignore lint/suspicious/noArrayIndexKey: empty cells, all alike
      <div key={`${key}${i}`} aria-hidden />
    ));

  return (
    <form onSubmit={ask} noValidate className="grid gap-3">
      <div
        className="grid max-w-5xl gap-x-10 gap-y-2.5 md:grid-flow-col md:grid-cols-2"
        style={{ gridTemplateRows: `repeat(${rows}, auto)` }}
      >
        <FieldsHead title="fibre.groupUnit" hint="fibre.groupUnitHint" />
        {unit.map(field)}
        {fill(rows - 1 - unit.length, "u")}
        <FieldsHead title="fibre.groupSoftware" hint="fibre.groupSoftwareHint" />
        {soft.map(field)}
        {fill(rows - 1 - soft.length, "s")}
      </div>
      <ReregisterFoot
        asking={asking}
        dirty={isDirty}
        pending={change.isPending}
        state={state}
        onConfirm={save}
        onCancel={() => setAsking(false)}
        onDiscard={() => form.reset()}
      />
    </form>
  );
}

/*
 * What the line registers with, in the two ways an operator may ask for:
 * the serial number with the PLOAM password, or the LOID with its password.
 * They stand side by side, in rows shared across both, so that the fields of
 * a row are level. Under each field, one line: what the OLT sees of it, or of
 * a password whether it is kept. A change makes the stick register again, so
 * the button says so, and the form asks once more at its foot before it does.
 */
function RegistrationForm({ identity }: { identity: Identity }) {
  const { t, lang } = useI18n();
  const change = useIdentityChange();
  const state = useChangeState(change);
  const { omci, mib } = useMib();
  const counters = useLive().data?.onu?.counters;
  const u = mib.empty ? null : unitOf(mib);
  const note = useOltNote();
  const loid = identity.loid !== undefined;
  const rule = t("fibre.serialRule");
  const resolver = useMemo(
    () =>
      identityResolver(
        rule,
        loid ? ["serial", "loid", "ploam", "loid_secret"] : ["serial", "ploam"],
      ),
    [rule, loid],
  );
  const form = useForm<Values>({ resolver, defaultValues: initial(identity) });
  const { errors, isDirty, dirtyFields } = form.formState;
  const values = form.watch();
  const [asking, setAsking] = useState(false);

  useEffect(() => {
    if (!form.formState.isDirty) form.reset(initial(identity));
  }, [identity, form]);

  const ask = form.handleSubmit(() => setAsking(isDirty));
  const save = form.handleSubmit((v) => {
    setAsking(false);
    const out: Record<string, string> = { SERIAL: v.serial };
    if (dirtyFields.loid) out.LOID = v.loid;
    if (v.ploam) out.PLOAM = v.ploam;
    if (v.loid_secret) out.LOID_PASSWORD = v.loid_secret;
    change.mutate(out, { onSuccess: () => form.reset({ ...v, ploam: "", loid_secret: "" }) });
  });

  const kept = (set: boolean | undefined) =>
    t(set ? "internet.passwordKept" : "fibre.secretNoneHint");
  const asked = counters?.password_requests;

  return (
    <form onSubmit={ask} noValidate className="grid gap-4">
      <div
        className={`grid gap-x-10 gap-y-4 ${loid ? "max-w-4xl md:grid-flow-col md:grid-cols-2 md:grid-rows-[auto_auto_auto]" : "max-w-md"}`}
      >
        <FieldsHead title="fibre.bySerial" hint="fibre.bySerialHint" />
        <Field
          label={t("fibre.serial")}
          mono
          error={errors.serial?.message}
          hint={note(values.serial, u?.serial ?? undefined)}
          {...form.register("serial")}
        />
        <Field
          label={t("fibre.ploam")}
          secret
          autoComplete="new-password"
          error={errors.ploam?.message}
          hint={[
            kept(identity.ploam_set),
            asked == null
              ? ""
              : asked === 0
                ? t("fibre.ploamNotAsked")
                : t("fibre.ploamAsked", {
                    n: integer(asked, lang),
                    m: integer(counters?.password_replies ?? 0, lang),
                  }),
          ].join(" ")}
          {...form.register("ploam")}
        />
        {loid && (
          <>
            <FieldsHead title="fibre.byLoid" hint="fibre.byLoidHint" />
            <Field
              label={t("fibre.loid")}
              error={errors.loid?.message}
              hint={note(values.loid, u?.loid ?? undefined)}
              {...form.register("loid")}
            />
            <Field
              label={t("fibre.loidPassword")}
              secret
              autoComplete="new-password"
              error={errors.loid_secret?.message}
              hint={[
                kept(identity.loid_password_set),
                u
                  ? t(
                      omci.data?.logical_password_set
                        ? "fibre.oltSeesSecret"
                        : "fibre.oltSeesNoSecret",
                    )
                  : "",
              ].join(" ")}
              {...form.register("loid_secret")}
            />
          </>
        )}
      </div>
      <ReregisterFoot
        asking={asking}
        dirty={isDirty}
        pending={change.isPending}
        state={state}
        onConfirm={save}
        onCancel={() => setAsking(false)}
        onDiscard={() => form.reset()}
      />
    </form>
  );
}

/*
 * A card of the identity, with its @title and @hint, whose values these are,
 * and the @form of them: as tall as the form.
 */
function IdentityCard({
  title,
  hint,
  form,
}: {
  title: Key;
  hint: Key;
  form: (i: Identity) => ReactNode;
}) {
  const { t } = useI18n();
  const identity = useIdentity();
  return (
    <section className="card min-h-0 border border-base-300 bg-base-100">
      <div className="card-body min-h-0 gap-3 p-4 sm:p-5">
        <div className="flex flex-wrap items-start justify-between gap-x-6 gap-y-1">
          <div>
            <h2 className="card-title text-base">{t(title)}</h2>
            <p className="text-sm text-base-content/70">{t(hint)}</p>
          </div>
          {identity.data?.source && (
            <span className="text-xs text-base-content/70">
              {t(identity.data.source === "store" ? "fibre.fromStore" : "fibre.fromFactory")}
            </span>
          )}
        </div>
        <QueryState reads={identity}>{() => identity.data && form(identity.data)}</QueryState>
      </div>
    </section>
  );
}

/* ------------------------------------------------------------------- OMCI */

/* The state of the connection to the operator, at the right of each of these pages' titles. */
export function FibreState() {
  const { t } = useI18n();
  const f = fibre(useLive().data);
  return <Badge tone={f.tone}>{t(`state.${f.stateKey}`)}</Badge>;
}

/* The fibre's tabs: the state of the line, and what it registers with. */
const FIBRE_TABS = ["state", "config"] as const;
type FibreTab = (typeof FIBRE_TABS)[number];

export function FibrePage() {
  const { t } = useI18n();
  const [tab, setTab] = useTab(FIBRE_TABS);
  return (
    <Page
      title={t("nav.fibre")}
      tabs={
        <Tabs
          items={[
            { id: "state", label: t("fibre.tabState") },
            { id: "config", label: t("fibre.tabConfig") },
          ]}
          value={tab}
          onChange={setTab}
          label={t("nav.fibre")}
        />
      }
      actions={<FibreState />}
    >
      {tab === "state" && <LineState />}
      {tab === "config" && (
        <IdentityCard
          title="fibre.regData"
          hint="fibre.regDataHint"
          form={(i) => <RegistrationForm identity={i} />}
        />
      )}
    </Page>
  );
}

/* The services' tabs: the services, and the VLAN rules they come of. */
const SERVICE_TABS = ["services", "rules"] as const;
type ServiceTab = (typeof SERVICE_TABS)[number];

export function ServicesPage() {
  const { t } = useI18n();
  const { mib } = useMib();
  const [tab, setTab] = useTab(SERVICE_TABS);
  const services = mib.empty ? 0 : servicesOf(mib).length;
  const rules = vlanRules(mib).reduce((n, set) => n + set.rules.length, 0);
  return (
    <Page
      title={t("nav.services")}
      hint={t(tab === "services" ? "fibre.servicesHint" : "fibre.rulesHint")}
      tabs={
        <Tabs
          items={[
            { id: "services", label: t("fibre.viewServices", { n: services }) },
            { id: "rules", label: t("fibre.viewRules", { n: rules }) },
          ]}
          value={tab}
          onChange={setTab}
          label={t("nav.services")}
        />
      }
      actions={<FibreState />}
    >
      <ServiceList view={tab} />
    </Page>
  );
}

export function ChannelsPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.channels")} hint={t("fibre.channelsHint")} actions={<FibreState />}>
      <ChannelList />
    </Page>
  );
}

/*
 * The ports the stick tells the OLT of: its Ethernet UNIs, the PPTP, its
 * VEIP, or both, and how many Ethernet UNIs. Each of them is the one SFP
 * lane to the host; an OLT sets services up on the ones it expects, and one
 * that wants nothing more than its profile has refuses a stick that tells of
 * more. A change makes the stick register again, as one of its identity
 * does, so it asks the same way. Under the choice, the ports the OLT gave
 * services to now.
 */
function UniCard() {
  const { t } = useI18n();
  const identity = useIdentity();
  const change = useIdentityChange();
  const state = useChangeState(change);
  const now = identity.data;
  const [report, setReport] = useState<UniReport>(now?.uni_report ?? "both");
  const [ports, setPorts] = useState<UniPorts>(now?.uni_ports ?? 1);
  const [pots, setPots] = useState<PotsPorts>(now?.pots_ports ?? 0);
  const [asking, setAsking] = useState(false);
  useEffect(() => {
    if (now?.uni_report) setReport(now.uni_report);
    if (now?.uni_ports) setPorts(now.uni_ports);
    if (now?.pots_ports !== undefined) setPots(now.pots_ports);
  }, [now?.uni_report, now?.uni_ports, now?.pots_ports]);
  const dirty =
    now != null &&
    (report !== (now.uni_report ?? "both") ||
      ports !== (now.uni_ports ?? 1) ||
      pots !== (now.pots_ports ?? 0));
  const used = useStickServices()
    .list.filter((s) => !s.off)
    .flatMap((s) => s.outputs);
  const names = [
    ...new Set(
      used.map((o) => (o.kind === "uni" ? t("fibre.outEthernet", { n: o.port }) : "VEIP")),
    ),
  ];
  const OPTIONS: { v: UniReport; label: Key; hint: Key }[] = [
    { v: "both", label: "fibre.uniBoth", hint: "fibre.uniBothHint" },
    { v: "pptp", label: "fibre.uniPptp", hint: "fibre.uniPptpHint" },
    { v: "veip", label: "fibre.uniVeip", hint: "fibre.uniVeipHint" },
  ];
  return (
    <Card title={t("fibre.uniTitle")}>
      <p className="text-sm text-base-content/70">{t("fibre.uniHint")}</p>
      {now && now.uni_report === undefined ? (
        <p className="text-sm">{t("fibre.uniOld")}</p>
      ) : (
        <form
          noValidate
          className="grid gap-3"
          onSubmit={(e) => {
            e.preventDefault();
            setAsking(dirty);
          }}
        >
          <fieldset className="grid gap-2">
            <legend className="sr-only">{t("fibre.uniTitle")}</legend>
            {OPTIONS.map((o) => (
              <label key={o.v} className="flex cursor-pointer items-start gap-2.5">
                <input
                  type="radio"
                  name="uni-report"
                  className="radio radio-sm mt-0.5"
                  checked={report === o.v}
                  onChange={() => setReport(o.v)}
                />
                <span className="grid gap-0.5">
                  <span className="text-sm font-medium">{t(o.label)}</span>
                  <Detail>{t(o.hint)}</Detail>
                </span>
              </label>
            ))}
          </fieldset>
          <div className="grid gap-1">
            <span className="text-sm font-medium">{t("fibre.uniPorts")}</span>
            <div className="join">
              {([1, 2, 3, 4, 5, 6, 7, 8] as const).map((n) => (
                <button
                  key={n}
                  type="button"
                  disabled={report === "veip"}
                  aria-pressed={ports === n}
                  className={`btn join-item btn-sm ${ports === n ? "btn-active" : ""}`}
                  onClick={() => setPorts(n)}
                >
                  {n}
                </button>
              ))}
            </div>
            <Detail>{t(report === "veip" ? "fibre.uniPortsVeip" : "fibre.uniPortsHint")}</Detail>
          </div>
          <div className="grid gap-1">
            <span className="text-sm font-medium">{t("fibre.potsPorts")}</span>
            <div className="join">
              {([0, 1, 2] as const).map((n) => (
                <button
                  key={n}
                  type="button"
                  aria-pressed={pots === n}
                  className={`btn join-item btn-sm ${pots === n ? "btn-active" : ""}`}
                  onClick={() => setPots(n)}
                >
                  {n}
                </button>
              ))}
            </div>
            <Detail>{t("fibre.potsPortsHint")}</Detail>
          </div>
          <p className="text-sm">
            <span className="text-base-content/70">{t("fibre.uniNow")} </span>
            <span className="font-medium">
              {names.length ? names.join(", ") : t("fibre.uniNone")}
            </span>
          </p>
          <ReregisterFoot
            asking={asking}
            dirty={dirty}
            pending={change.isPending}
            state={state}
            onConfirm={() => {
              setAsking(false);
              if (!now) return;
              change.mutate({
                SERIAL: now.serial,
                UNI_REPORT: report,
                UNI_PORTS: String(report === "veip" ? (now.uni_ports ?? 1) : ports),
                POTS_PORTS: String(pots),
              });
            }}
            onCancel={() => setAsking(false)}
            onDiscard={() => {
              setReport(now?.uni_report ?? "both");
              setPorts(now?.uni_ports ?? 1);
              setPots(now?.pots_ports ?? 0);
            }}
          />
        </form>
      )}
    </Card>
  );
}

/*
 * The PPPoE login an OLT may send through OMCI (Authentication security
 * method, G.988 9.12.4), so that an ONT dials by itself: its user, and its
 * password, which root's password shows. One button hands both to the form
 * of a new PPPoE connection of the stick's own.
 */
function OltLoginCard() {
  const { t } = useI18n();
  const identity = useIdentity();
  const reveal = useOltLogin();
  const navigate = useNavigate();
  const confirm = useConfirm();
  const [shown, setShown] = useState<OltLogin | null>(null);
  const login = identity.data?.olt_login;
  return (
    <Card title={t("fibre.oltLoginTitle")}>
      <p className="text-sm text-base-content/70">{t("fibre.oltLoginHint")}</p>
      {!login ? (
        <p className="text-sm">
          {t(login === undefined ? "fibre.oltLoginOld" : "fibre.oltLoginNone")}
        </p>
      ) : (
        <>
          <Facts
            rows={[
              [
                t("fibre.oltLoginUser"),
                <span key="user" className="font-mono">
                  {login.user}
                </span>,
              ],
              [
                t("fibre.oltLoginPassword"),
                shown ? (
                  <span key="password" className="font-mono">
                    {shown.secret}
                  </span>
                ) : (
                  t(login.password_set ? "fibre.oltLoginHidden" : "fibre.oltLoginNoPassword")
                ),
              ],
            ]}
          />
          <div className="flex flex-wrap gap-2">
            {login.password_set && !shown && (
              <button
                type="button"
                className="btn btn-sm"
                onClick={() =>
                  confirm.open({
                    title: t("fibre.oltLoginShow"),
                    body: t("fibre.oltLoginShowConfirm"),
                    action: t("fibre.oltLoginShow"),
                    run: async (secret) => setShown(await reveal.mutateAsync({ secret })),
                  })
                }
              >
                <Eye className="size-4" />
                {t("fibre.oltLoginShow")}
              </button>
            )}
            <button
              type="button"
              className="btn btn-primary btn-sm"
              onClick={() => {
                const go = (l: OltLogin) => {
                  handOltLogin(l);
                  void navigate({ to: "/internet/wan" });
                };
                if (shown || !login.password_set) go(shown ?? { user: login.user, secret: "" });
                else
                  confirm.open({
                    title: t("fibre.oltLoginUse"),
                    body: t("fibre.oltLoginUseConfirm"),
                    action: t("fibre.oltLoginUse"),
                    run: async (secret) => go(await reveal.mutateAsync({ secret })),
                  });
              }}
            >
              <Network className="size-4" />
              {t("fibre.oltLoginUse")}
            </button>
          </div>
        </>
      )}
      {confirm.dialog}
    </Card>
  );
}

export function IdentityPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.identity")} actions={<FibreState />}>
      <Columns wide="left">
        <IdentityCard
          title="fibre.identity"
          hint="fibre.identityHint"
          form={(i) => <IdentityForm identity={i} />}
        />
        <Stack>
          <UniCard />
          <OltLoginCard />
        </Stack>
      </Columns>
    </Page>
  );
}
