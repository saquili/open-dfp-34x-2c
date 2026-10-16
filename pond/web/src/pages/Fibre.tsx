// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The fibre and what the operator set up on it, in five pages. Under the
 * switch, the fibre: the state of the connection, the light, the
 * registration with the OLT and what the line registers with (the serial
 * number or the LOID, and the passwords), and the errors on the line. Under
 * the operator, what OMCI carries: the services the OLT set up, each a VLAN
 * with its channels, the host's VLAN for it and multicast; the channels one
 * by one; what the stick tells the OLT of itself, which the owner may change
 * to what the operator expects; and the managed entities as omcid holds
 * them, for the curious.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Link } from "@tanstack/react-router";
import { Check, CircleAlert, CircleX, KeyRound, Lock, Minus, X } from "lucide-react";
import { Fragment, type ReactNode, useEffect, useMemo, useState } from "react";
import { type Resolver, useForm } from "react-hook-form";
import { useIdentity, useIdentityChange, useOmci } from "../api/config";
import { useLive, useTraffic } from "../api/live";
import type { Identity } from "../api/types";
import { Caret, opens } from "../components/CardTable";
import { SaveState, SecretInput, useChangeState } from "../components/Field";
import { useGemText } from "../components/Gem";
import { Page } from "../components/Page";
import { Filter, Pager, usePages } from "../components/Pager";
import { HistoryNote, Sparkline } from "../components/TrafficChart";
import { Badge, Card, Facts, Figure, Stat } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { bitrate, bytes, dbmText, decimal, duration, integer } from "../lib/format";
import {
  type Channel,
  hex,
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
import { CLASSES } from "../lib/omci-classes";
import { upTo, z } from "../lib/schemas";
import {
  fibre,
  fibreVlan,
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
            className="absolute -top-3 size-4 -translate-x-1/2 rounded-full border-2 border-base-100 bg-primary shadow"
            style={{ left: `${at(Math.min(hi, Math.max(lo, rx)))}%` }}
          />
        )}
      </div>
      <div className="relative mt-1 h-4 text-[11px] text-base-content/55">
        {[RX_MIN_DBM, RX_WEAK_DBM, RX_MAX_DBM].map((v) => (
          <span key={v} className="absolute -translate-x-1/2" style={{ left: `${at(v)}%` }}>
            {decimal(v, 0, lang)}
          </span>
        ))}
      </div>
    </div>
  );
}

function useMib() {
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
  wait: "bg-base-200 text-base-content/60 ring-1 ring-base-300",
  off: "bg-base-200 text-base-content/50 ring-1 ring-base-300",
};

/* A step, and the operator's page that shows what the OLT did in it over OMCI. */
const STEP: Record<RegStep, { title: Key; hint: Key; to?: string }> = {
  sync: { title: "fibre.stepSync", hint: "fibre.stepSyncHint" },
  serial: { title: "fibre.stepSerial", hint: "fibre.stepSerialHint" },
  ranging: { title: "fibre.stepRanging", hint: "fibre.stepRangingHint" },
  ploam: { title: "fibre.stepPassword", hint: "fibre.stepPasswordHint" },
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
        <span className="hidden text-xs text-base-content/55 sm:inline">{t("fibre.regHint")}</span>
      }
    >
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
                    className="link link-hover link-primary text-sm font-medium"
                  >
                    {t(STEP[id].title)}
                  </Link>
                ) : (
                  <span className="text-sm font-medium">{t(STEP[id].title)}</span>
                )}
                {detail(id, at).map((line) => (
                  <span key={line} className="tabular truncate text-xs text-base-content/65">
                    {line}
                  </span>
                ))}
                <span className="text-xs text-base-content/55 lg:hidden">{t(STEP[id].hint)}</span>
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
    <div className="grid min-h-0 flex-1 gap-4 lg:grid-rows-[auto_min-content_min-content_minmax(min-content,1fr)]">
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
          <p className="text-xs text-base-content/60">{t("fibre.onuIdHint")}</p>
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
          <p className="text-xs text-base-content/60">
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
              <p className="text-xs text-base-content/60">{t("fibre.sinceStart")}</p>
            </>
          ) : (
            <p className="text-sm">{t("state.noPon")}</p>
          )}
        </Card>
      </div>
      <Registration />
      <IdentityCard groups={REGISTER_GROUPS} title="fibre.regData" hint="fibre.regDataHint" />
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
 * A table of the panel's look (docs/handbook/panel-design.rst, Tables): the
 * names of the columns at 12 px in the secondary colour, the cells at 14 px
 * with their content at their top.
 */
const TABLE = "table [&_thead]:text-xs [&_thead]:text-base-content/70 [&_td]:align-top";

/*
 * A service's row in a card too narrow for the table: the caret, and beside
 * it the cells one under another, each under its column's name.
 */
const NARROW_ROW =
  "@max-4xl:grid @max-4xl:grid-cols-[2.5rem_minmax(0,1fr)] @max-4xl:border-t @max-4xl:border-base-300 @max-4xl:py-2 @max-4xl:first:border-t-0";
const NARROW_CELL =
  "@max-4xl:col-start-2 @max-4xl:border-0 @max-4xl:px-0 @max-4xl:py-1 @max-4xl:before:block @max-4xl:before:text-xs @max-4xl:before:font-normal @max-4xl:before:text-base-content/70 @max-4xl:before:content-[attr(data-h)]";

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
  const match = (r: VlanRule) => {
    const tags = tagsMatched(r);
    if (isDefault(r)) return tags === 1 ? t("fibre.matchOtherSingle") : t("fibre.matchOtherDouble");
    if (tags === 0) return t("fibre.matchUntagged");
    const tag = tags === 1 ? r.inner : r.outer;
    const vid = tag.vid === 4096 ? t("fibre.anyVlan") : t("switch.vlanN", { id: tag.vid });
    return [vid, prio(tag.prio)].filter(Boolean).join(", ");
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
  return { match, action };
}

/** What the stick does with a service: the channels' uses, as words, each a neutral pill. */
function useServiceUse() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const views = gems(live);
  return (s: Service) => {
    const ports = new Set(s.ports.flatMap((p) => p.channels.map((c) => c.portId)));
    const uses = new Set(views.filter((v) => ports.has(v.gem.port_id)).map((v) => v.use));
    const own = sessions(live).filter((x) => s.vlan != null && fibreVlan(x) === String(s.vlan));
    const tags: string[] = [];
    if (own.length) tags.push(t("fibre.tagSession", { list: own.map((x) => x.id).join(", ") }));
    if (uses.has("bridged")) tags.push(t("fibre.tagBridged"));
    if (s.multicast) tags.push(t("fibre.tagMulticast"));
    if (!tags.length) tags.push(t("fibre.tagStick"));
    return tags;
  };
}

function hostText(t: (k: Key, v?: Record<string, string | number>) => string, s: Service) {
  return s.hostVlans.length === 0
    ? s.vlan == null
      ? t("fibre.hostAll")
      : t("fibre.hostSame", { vid: s.vlan })
    : s.hostVlans.map((v) => (v == null ? t("switch.untagged") : String(v))).join(", ");
}

/* A line of detail under a value: 12 px, in the secondary colour. */
function Detail({ children }: { children: ReactNode }) {
  return <span className="text-xs text-base-content/70">{children}</span>;
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

function ServicesTable({ list, q }: { list: Service[]; q: string }) {
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
        <table className={`${TABLE} table-pin-rows @max-4xl:block`}>
          <thead className="@max-4xl:hidden">
            <tr>
              <th className="w-px" />
              <th>{t("fibre.colVlan")}</th>
              <th>{t("fibre.colHost")}</th>
              <th>{t("fibre.colChannels")}</th>
              <th>{t("fibre.colStick")}</th>
              <th className="text-right">{t("fibre.now")}</th>
            </tr>
          </thead>
          <tbody className="@max-4xl:block">
            {pager.shown.map((s) => {
              const k = key(s);
              const ids = [...new Set(s.ports.flatMap((p) => p.channels))];
              let down = 0;
              let up = 0;
              for (const c of new Set(ids.map((c) => c.portId))) {
                const v = views.find((x) => x.gem.port_id === c);
                const r = v ? rates?.[v.gem.name] : undefined;
                down += r?.down ?? 0;
                up += r?.up ?? 0;
              }
              const isOpen = open.has(k);
              const more = `service-${k}`;
              const vlan = s.vlan == null ? t("fibre.noVlan") : String(s.vlan);
              return (
                <Fragment key={k}>
                  <tr
                    className={`cursor-pointer hover:bg-base-200/40 ${isOpen ? "[&>td]:border-b-0" : ""} ${NARROW_ROW}`}
                    onClick={opens(() => toggle(k))}
                  >
                    <td className="w-px @max-4xl:col-start-1 @max-4xl:row-start-1 @max-4xl:p-0">
                      <Caret
                        open={isOpen}
                        controls={more}
                        label={t("fibre.serviceDetails", { vlan })}
                        onToggle={() => toggle(k)}
                      />
                    </td>
                    <td
                      data-h={t("fibre.colVlan")}
                      className={`tabular font-semibold ${NARROW_CELL}`}
                    >
                      {vlan}
                    </td>
                    <td data-h={t("fibre.colHost")} className={`tabular ${NARROW_CELL}`}>
                      {hostText(t, s)}
                    </td>
                    <td data-h={t("fibre.colChannels")} className={NARROW_CELL}>
                      <span className="flex flex-wrap gap-1">
                        {s.ports.flatMap((p) =>
                          p.channels.map((c) => (
                            <span
                              key={`${p.id}-${c.portId}`}
                              className={`badge badge-sm tabular ${(p.vlans?.length ?? 0) > 1 ? "badge-ghost" : "badge-outline"}`}
                              title={
                                (p.vlans?.length ?? 0) > 1 ? t("fibre.sharedBroadcast") : undefined
                              }
                            >
                              {c.portId}
                              {c.pbits.length > 0 && c.pbits.length < 8 && (
                                <span className="text-base-content/70">
                                  {" "}
                                  p
                                  {c.pbits.length > 1
                                    ? `${c.pbits[0]}–${c.pbits.at(-1)}`
                                    : c.pbits[0]}
                                </span>
                              )}
                            </span>
                          )),
                        )}
                      </span>
                    </td>
                    <td data-h={t("fibre.colStick")} className={NARROW_CELL}>
                      <span className="flex flex-wrap gap-1 *:whitespace-nowrap">
                        {use(s).map((u) => (
                          <Badge key={u} tone="neutral">
                            {u}
                          </Badge>
                        ))}
                      </span>
                    </td>
                    <td
                      data-h={t("fibre.now")}
                      className={`tabular text-right whitespace-nowrap @max-4xl:text-left ${NARROW_CELL}`}
                    >
                      ↓ {bitrate(down, lang)} ↑ {bitrate(up, lang)}
                    </td>
                  </tr>
                  {isOpen && (
                    <tr id={more} data-more className="@max-4xl:block @max-4xl:pb-2">
                      <td className="@max-4xl:hidden" />
                      <td colSpan={5} className="pt-0 @max-4xl:block @max-4xl:p-0 @max-4xl:pl-10">
                        <ServiceDetails s={s} />
                      </td>
                    </tr>
                  )}
                </Fragment>
              );
            })}
            {!pager.shown.length && (
              <tr className="@max-4xl:block">
                <td colSpan={6} className="text-base-content/70 @max-4xl:block">
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

function RulesTable({ mib, q }: { mib: Mib; q: string }) {
  const { t } = useI18n();
  const text = useRuleText();
  const rows = vlanRules(mib).flatMap((set) => set.rules.map((r, i) => ({ me: set.me, i, r })));
  const needle = q.trim();
  const shown = rows.filter((x) => {
    if (!needle) return true;
    const tr = translation(x.r);
    return String(tr?.from ?? "").includes(needle) || String(tr?.to ?? "").includes(needle);
  });
  const pager = usePages(shown);
  return (
    <>
      <div className="min-h-0 flex-1 overflow-auto">
        <table className={`${TABLE} table-pin-rows`}>
          <thead>
            <tr>
              <th className="w-10">#</th>
              <th>{t("fibre.ruleFrom")}</th>
              <th>{t("fibre.ruleDo")}</th>
            </tr>
          </thead>
          <tbody>
            {pager.shown.map((x) => (
              <tr key={`${x.me}-${x.i}`}>
                <td className="tabular text-base-content/70">{x.i + 1}</td>
                <td>
                  <span className="grid gap-1">
                    {text.match(x.r)}
                    {isDefaultEntry(x.r) && <Detail>{t("fibre.ruleDefault")}</Detail>}
                  </span>
                </td>
                <td>{text.action(x.r)}</td>
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

function OmciState({ children }: { children: ReactNode }) {
  const { t } = useI18n();
  const { omci, mib } = useMib();
  if (omci.isPending)
    return (
      <p className="flex items-center gap-2 text-sm">
        <span className="loading loading-spinner loading-sm" />
        {t("fibre.reading")}
      </p>
    );
  if (omci.isError || mib.empty)
    return (
      <Card>
        <p className="text-sm">{t("fibre.omciAbsent")}</p>
      </Card>
    );
  return <>{children}</>;
}

function ServiceList() {
  const { t } = useI18n();
  const { omci, mib } = useMib();
  const [view, setView] = useState<"services" | "rules">("services");
  const [q, setQ] = useState("");
  const list = servicesOf(mib);
  const rules = vlanRules(mib).reduce((n, set) => n + set.rules.length, 0);
  return (
    <OmciState>
      <section className="card flex min-h-0 flex-1 border border-base-300 bg-base-100">
        <div className="card-body min-h-0 gap-3 p-4 sm:p-5">
          <div className="flex flex-wrap items-center justify-between gap-3">
            <div role="tablist" className="tabs tabs-border">
              <button
                type="button"
                role="tab"
                className={`tab ${view === "services" ? "tab-active" : ""}`}
                onClick={() => setView("services")}
              >
                {t("fibre.viewServices", { n: list.length })}
              </button>
              <button
                type="button"
                role="tab"
                className={`tab ${view === "rules" ? "tab-active" : ""}`}
                onClick={() => setView("rules")}
              >
                {t("fibre.viewRules", { n: rules })}
              </button>
            </div>
            <Filter value={q} onChange={setQ} placeholder={t("fibre.filterVlan")} />
          </div>
          <p className="text-xs text-base-content/70">
            {view === "services" ? t("fibre.servicesHint") : t("fibre.rulesHint")}
          </p>
          {omci.data?.truncated && (
            <div role="alert" className="alert alert-warning alert-soft text-sm">
              {t("fibre.truncated")}
            </div>
          )}
          {view === "services" ? (
            <ServicesTable list={list} q={q} />
          ) : (
            <RulesTable mib={mib} q={q} />
          )}
        </div>
      </section>
    </OmciState>
  );
}

/* --------------------------------------------------------------- channels */

/** The eight priorities of IEEE 802.1p, those a channel carries filled in. */
function Pbits({ list }: { list: number[] }) {
  const { t } = useI18n();
  if (!list.length) return <span>–</span>;
  return (
    <span className="flex gap-0.5" title={t("fibre.pbits", { list: list.join(", ") })}>
      {Array.from({ length: 8 }, (_, p) => (
        <span
          // biome-ignore lint/suspicious/noArrayIndexKey: the eight priorities are fixed
          key={p}
          className={`grid size-4 place-items-center rounded-[3px] text-xs leading-none tabular ${list.includes(p) ? "bg-primary text-primary-content" : "bg-base-200 text-base-content/70"}`}
        >
          {p}
        </span>
      ))}
    </span>
  );
}

/* The columns of a channel's row, its heading's as well. */
const CHANNEL_COLS = "md:grid-cols-[6.5rem_9rem_minmax(0,1fr)_9.5rem_8rem_8rem]";

function ChannelList() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const rates = useTraffic().at(-1)?.gems;
  const { mib } = useMib();
  const gem = useGemText();
  const views = gems(live);
  const info = new Map<number, { c: Channel; vlans: (number | null)[] }>();
  for (const s of servicesOf(mib))
    for (const p of s.ports)
      for (const c of p.channels) {
        const had = info.get(c.portId);
        info.set(c.portId, { c, vlans: [...(had?.vlans ?? []), s.vlan] });
      }

  const groups = new Map<number | null, typeof views>();
  for (const v of views) groups.set(v.gem.tcont, [...(groups.get(v.gem.tcont) ?? []), v]);
  const order = [...groups.keys()].sort((a, z) => (a ?? 99) - (z ?? 99));

  return (
    <section className="card flex min-h-0 flex-1 border border-base-300 bg-base-100">
      <div className="card-body min-h-0 gap-3 p-4 sm:p-5">
        <div className="grid min-h-0 flex-1 content-start gap-4 overflow-auto pr-1">
          <div
            className={`sticky top-0 z-10 hidden gap-x-4 border-b border-base-300 bg-base-100 px-2 pb-1.5 text-xs font-semibold text-base-content/70 md:grid ${CHANNEL_COLS}`}
          >
            <span>{t("fibre.channel")}</span>
            <span>{t("fibre.direction")}</span>
            <span>{t("fibre.colUse")}</span>
            <span>{t("fibre.colPbits")}</span>
            <span className="text-right">{t("fibre.now")}</span>
            <span className="text-right">{t("fibre.colTotal")}</span>
          </div>
          {order.map((tc) => {
            const list = groups.get(tc) ?? [];
            const alloc = list[0]?.allocId;
            return (
              <div key={String(tc)} className="grid gap-1.5">
                <div className="flex flex-wrap items-baseline gap-x-2 border-b border-base-300 pb-1">
                  <span className="text-sm font-semibold whitespace-nowrap">
                    {tc == null
                      ? t("fibre.downOnlyGroup")
                      : t("fibre.tcontGroup", { n: tc, alloc: alloc ?? "–" })}
                  </span>
                  {tc == null && (
                    <span className="text-xs text-base-content/70">{t("fibre.downOnlyHint")}</span>
                  )}
                </div>
                {list.map((v) => {
                  const g = v.gem;
                  const o = info.get(g.port_id);
                  const r = rates?.[g.name];
                  const c = live?.gem_counters?.[g.name];
                  return (
                    <div
                      key={g.name}
                      className={`grid items-center gap-x-4 gap-y-1 rounded-field px-2 py-1.5 hover:bg-base-200/50 ${CHANNEL_COLS}`}
                    >
                      <span className="flex items-center gap-1.5">
                        <span className="tabular font-semibold">GEM {g.port_id}</span>
                        {o?.c.encrypted && (
                          <Lock className="size-3.5 text-base-content/70" aria-label="AES" />
                        )}
                      </span>
                      <span className="text-sm">{gem.direction(g)}</span>
                      <span className="flex flex-wrap items-center gap-1 text-xs">
                        {(o?.vlans ?? []).map((x) => (
                          <span key={String(x)} className="badge badge-sm badge-outline tabular">
                            {x == null ? "–" : `VLAN ${x}`}
                          </span>
                        ))}
                        <span className="text-base-content/70">{gem.use(v)}</span>
                      </span>
                      <Pbits list={o?.c.pbits ?? []} />
                      <span className="tabular text-right text-sm whitespace-nowrap">
                        <span className="block">↓ {bitrate(r?.down ?? 0, lang)}</span>
                        {g.direction !== "downstream" && (
                          <span className="block">↑ {bitrate(r?.up ?? 0, lang)}</span>
                        )}
                      </span>
                      <span className="tabular text-right text-sm">
                        <span className="block">↓ {bytes(c?.pon_rx_octets, lang)}</span>
                        {g.direction !== "downstream" && (
                          <span className="block">↑ {bytes(c?.pon_tx_octets, lang)}</span>
                        )}
                      </span>
                    </div>
                  );
                })}
              </div>
            );
          })}
        </div>
      </div>
    </section>
  );
}

/* --------------------------------------------------------------- identity */

type Group = "register" | "unit" | "software";

/* The fields a stick of older software has none of. */
const EXTENDED_FIELDS: string[] = ["loid", "sw_version0", "sw_version1", "pack_version"];

/*
 * A field of the identity: where it goes, its words, and what the OLT reads
 * of it now, from the stick's OMCI database; null where no managed entity
 * carries it.
 */
const IDENTITY_FIELDS: {
  field: Exclude<keyof Values, "ploam" | "loid_secret">;
  key: string;
  label: Key;
  hint?: Key;
  group: Group;
  olt: ((u: Unit) => string | number | null) | null;
  numeric?: boolean;
  mono?: boolean;
}[] = [
  {
    field: "serial",
    key: "SERIAL",
    label: "fibre.serial",
    hint: "fibre.serialHint",
    group: "register",
    olt: (u) => u.serial,
    mono: true,
  },
  {
    field: "loid",
    key: "LOID",
    label: "fibre.loid",
    hint: "fibre.loidHint",
    group: "register",
    olt: (u) => u.loid,
  },
  {
    field: "vendor_id",
    key: "VENDOR_ID",
    label: "fibre.vendorId",
    hint: "fibre.vendorIdHint",
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
    hint: "fibre.omccHint",
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
    hint: "fibre.tmHint",
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

const GROUPS: { id: Group; label: Key; hint: Key }[] = [
  { id: "register", label: "fibre.groupRegister", hint: "fibre.groupRegisterHint" },
  { id: "unit", label: "fibre.groupUnit", hint: "fibre.groupUnitHint" },
  { id: "software", label: "fibre.groupSoftware", hint: "fibre.groupSoftwareHint" },
];

/*
 * Where the groups stand: what the line registers with, beside the
 * registration under the switch, since the port does it with PLOAM; what
 * OMCI tells the OLT of the stick, under the operator.
 */
type Groups = [Group, ...Group[]];
const REGISTER_GROUPS: Groups = ["register"];
const OMCI_GROUPS: Groups = ["unit", "software"];

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

/* The identity form's columns: the field, the stick's value, what the OLT reads. */
const IDENTITY_COLS = "md:grid-cols-[minmax(11rem,16rem)_minmax(12rem,26rem)_minmax(10rem,1fr)]";

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

/* What the OLT reads beside what the stick holds: the same, another value, or a default. */
function OltValue({
  stick,
  olt,
  numeric,
  mono,
}: {
  stick: string;
  olt: string | number | null | undefined;
  numeric?: boolean;
  mono?: boolean;
}) {
  const { t } = useI18n();
  if (olt === undefined)
    return <span className="text-xs text-base-content/70">{t("fibre.oltNotRead")}</span>;
  if (olt === null)
    return <span className="text-xs text-base-content/70">{t("fibre.oltNone")}</span>;
  const empty = olt === "";
  const shown = empty
    ? t("fibre.oltEmpty")
    : numeric && typeof olt === "number"
      ? `${olt} (0x${olt.toString(16)})`
      : String(olt);
  const same = sameAsOlt(stick, olt, numeric);
  return (
    <span className="grid min-w-0 gap-0.5">
      <span className="flex min-w-0 items-center gap-1.5">
        {same === true && (
          <Check className="size-3.5 shrink-0 text-success" aria-label={t("fibre.oltSame")} />
        )}
        {same === false && <CircleAlert className="size-3.5 shrink-0 text-warning" aria-hidden />}
        <span
          className={`min-w-0 break-words text-sm ${mono && !empty ? "font-mono" : ""} ${empty ? "italic text-base-content/70" : ""}`}
          title={same === null ? t("fibre.oltDefaultHint") : undefined}
        >
          {shown}
        </span>
      </span>
      {same === false && (
        <span className="text-xs text-base-content/70" title={t("fibre.oltDiffersHint")}>
          {t("fibre.oltDiffers")}
        </span>
      )}
    </span>
  );
}

/*
 * The identity's fields of @groups, one group at a time, chosen from tabs
 * beside them when there are more. Only those fields are checked and sent,
 * and with them the serial number, which every change of the identity carries.
 */
function IdentityForm({ identity, groups }: { identity: Identity; groups: Groups }) {
  const { t } = useI18n();
  const change = useIdentityChange();
  const state = useChangeState(change);
  const { omci, mib } = useMib();
  const u = mib.empty ? null : unitOf(mib);
  const extended = identity.loid !== undefined;
  const registers = groups.includes("register");
  const visible = IDENTITY_FIELDS.filter(
    (f) => groups.includes(f.group) && (extended || !EXTENDED_FIELDS.includes(f.field)),
  );
  const secrets: (keyof Values)[] = !registers
    ? []
    : extended
      ? ["ploam", "loid_secret"]
      : ["ploam"];
  const rule = t("fibre.serialRule");
  const names = [...visible.map((f) => f.field), ...secrets].join(" ");
  const resolver = useMemo(
    () => identityResolver(rule, names.split(" ") as (keyof Values)[]),
    [rule, names],
  );
  const form = useForm<Values>({ resolver, defaultValues: initial(identity) });
  const { errors, isDirty, dirtyFields } = form.formState;
  const values = form.watch();
  const changed = Object.keys(dirtyFields).length;
  const [group, setGroup] = useState<Group>(groups[0]);
  const groupOf = (name: string): Group | undefined =>
    secrets.includes(name as keyof Values)
      ? "register"
      : IDENTITY_FIELDS.find((f) => f.field === name)?.group;
  /* What a group holds that the reader is to see before opening it. */
  const tally = (g: Group) => {
    const names = [
      ...visible.filter((f) => f.group === g).map((f) => f.field),
      ...(g === "register" ? secrets : []),
    ];
    return {
      changed: names.filter((n) => dirtyFields[n]).length,
      errors: names.filter((n) => errors[n]).length,
      differs: visible.filter(
        (f) =>
          f.group === g &&
          u &&
          f.olt &&
          sameAsOlt(values[f.field] ?? "", f.olt(u), f.numeric) === false,
      ).length,
    };
  };

  useEffect(() => {
    if (!form.formState.isDirty) form.reset(initial(identity));
  }, [identity, form]);

  const submit = form.handleSubmit(
    (v) => {
      if (!window.confirm(t("fibre.identityConfirm"))) return;
      const out: Record<string, string> = { SERIAL: registers ? v.serial : identity.serial };
      for (const f of visible) if (dirtyFields[f.field]) out[f.key] = v[f.field];
      if (v.ploam) out.PLOAM = v.ploam;
      if (v.loid_secret) out.LOID_PASSWORD = v.loid_secret;
      change.mutate(out, { onSuccess: () => form.reset({ ...v, ploam: "", loid_secret: "" }) });
    },
    (bad) => {
      /* An error in a group not shown opens that group. */
      const g = Object.keys(bad).map(groupOf).find(Boolean);
      if (g) setGroup(g);
    },
  );

  const row = (
    label: string,
    hint: string | undefined,
    input: ReactNode,
    olt: ReactNode,
    key: string,
  ) => (
    <div
      key={key}
      className={`grid items-center gap-x-6 gap-y-1 border-b border-base-200 py-2 last:border-0 ${IDENTITY_COLS}`}
    >
      <div className="min-w-0">
        <div className="text-sm font-semibold">{label}</div>
        {hint && <div className="text-xs text-base-content/70">{hint}</div>}
      </div>
      <div className="min-w-0">{input}</div>
      <div className="min-w-0">
        <span className="block text-xs text-base-content/70 md:hidden">{t("fibre.colOlt")}</span>
        {olt}
      </div>
    </div>
  );

  const secret = (name: "ploam" | "loid_secret", set: boolean | undefined) => (
    <SecretInput
      small
      icon={<KeyRound className="size-3.5 shrink-0 opacity-50" />}
      invalid={!!errors[name]}
      autoComplete="new-password"
      placeholder={set ? t("internet.passwordSaved") : t("internet.passwordNone")}
      title={set ? t("internet.passwordKept") : undefined}
      {...form.register(name)}
    />
  );

  const fieldRow = (f: (typeof IDENTITY_FIELDS)[number]) =>
    row(
      t(f.label),
      f.hint ? t(f.hint) : undefined,
      <>
        <input
          className={`input input-sm w-full ${f.mono ? "font-mono" : ""} ${errors[f.field] ? "input-error" : ""} ${dirtyFields[f.field] ? "border-primary" : ""}`}
          spellcheck={false}
          autoComplete="off"
          placeholder={t("fibre.factoryValue")}
          {...form.register(f.field)}
        />
        {errors[f.field] && <p className="mt-0.5 text-xs text-error">{errors[f.field]?.message}</p>}
      </>,
      <OltValue
        stick={values[f.field] ?? ""}
        olt={!u ? undefined : f.olt ? f.olt(u) : null}
        numeric={f.numeric}
        mono={f.mono}
      />,
      f.field,
    );

  const secretRows = () => (
    <>
      {row(
        t("fibre.ploam"),
        t("fibre.ploamHint"),
        secret("ploam", identity.ploam_set),
        <span className="text-xs text-base-content/70">{t("fibre.oltNone")}</span>,
        "ploam",
      )}
      {extended &&
        row(
          t("fibre.loidPassword"),
          undefined,
          secret("loid_secret", identity.loid_password_set),
          u ? (
            <span className="text-sm">
              {omci.data?.logical_password_set ? t("fibre.secretSet") : t("fibre.secretNone")}
            </span>
          ) : (
            <span className="text-xs text-base-content/70">{t("fibre.oltNotRead")}</span>
          ),
          "loid_secret",
        )}
    </>
  );

  return (
    <form onSubmit={submit} noValidate className="flex min-h-0 flex-1 flex-col gap-3">
      <div
        className={`grid min-h-0 flex-1 gap-4 ${groups.length > 1 ? "lg:grid-cols-[15rem_minmax(0,1fr)]" : ""}`}
      >
        {groups.length > 1 && (
          <div
            role="tablist"
            aria-orientation="vertical"
            className="flex gap-1 overflow-x-auto lg:flex-col lg:overflow-visible lg:border-r lg:border-base-200 lg:pr-4"
          >
            {GROUPS.filter((g) => groups.includes(g.id)).map((g) => {
              const n = tally(g.id);
              const active = g.id === group;
              return (
                <button
                  key={g.id}
                  type="button"
                  role="tab"
                  aria-selected={active}
                  onClick={() => setGroup(g.id)}
                  className={`flex min-w-44 shrink-0 flex-col items-start gap-0.5 rounded-field px-3 py-2 text-left transition-colors lg:min-w-0 ${active ? "bg-primary/10 text-primary" : "hover:bg-base-200"}`}
                >
                  <span className="flex w-full items-center gap-2 text-sm font-semibold">
                    {t(g.label)}
                    <span className="ml-auto flex items-center gap-1.5">
                      {n.errors > 0 && (
                        <span title={t("fibre.groupErrors")}>
                          <CircleX
                            className="size-3.5 text-error"
                            aria-label={t("fibre.groupErrors")}
                          />
                        </span>
                      )}
                      {n.differs > 0 && (
                        <span
                          className="tabular flex items-center gap-0.5 text-xs font-normal text-base-content"
                          title={t("fibre.differsN", { n: n.differs })}
                        >
                          <CircleAlert className="size-3.5 text-warning" />
                          {n.differs}
                        </span>
                      )}
                      {n.changed > 0 && (
                        <span
                          className="badge badge-primary badge-sm tabular"
                          title={t("fibre.changedN", { n: n.changed })}
                        >
                          {n.changed}
                        </span>
                      )}
                    </span>
                  </span>
                  <span className="hidden text-xs font-normal text-base-content/70 lg:block">
                    {t(g.hint)}
                  </span>
                </button>
              );
            })}
          </div>
        )}
        <div className="flex min-h-0 flex-col">
          <div
            className={`hidden gap-x-6 border-b border-base-300 pb-1.5 text-xs font-semibold text-base-content/70 md:grid ${IDENTITY_COLS}`}
          >
            <span>{t("fibre.colField")}</span>
            <span>{t("fibre.colOnStick")}</span>
            <span>{t("fibre.colOlt")}</span>
          </div>
          <div
            role={groups.length > 1 ? "tabpanel" : undefined}
            className="min-h-0 flex-1 overflow-auto"
          >
            {visible.filter((f) => f.group === group).map(fieldRow)}
            {group === "register" && secretRows()}
          </div>
        </div>
      </div>
      <div className="flex flex-wrap items-center gap-3 border-t border-base-300 pt-3">
        <button
          type="submit"
          className="btn btn-primary btn-sm"
          disabled={!isDirty || change.isPending}
        >
          {t("internet.save")}
        </button>
        {isDirty && (
          <button type="button" className="btn btn-ghost btn-sm" onClick={() => form.reset()}>
            {t("form.discard")}
          </button>
        )}
        {changed > 0 && (
          <span className="text-sm text-base-content/70">
            {t("fibre.changedN", { n: changed })}
          </span>
        )}
        <SaveState {...state} />
        <span className="ml-auto flex max-w-xl items-start gap-1.5 text-xs text-base-content/70">
          <CircleAlert className="mt-px size-3.5 shrink-0 text-warning" aria-hidden />
          {t("fibre.identityWarning")}
        </span>
      </div>
    </form>
  );
}

/* A card of the identity's @groups, with its @title and @hint. */
function IdentityCard({ groups, title, hint }: { groups: Groups; title: Key; hint: Key }) {
  const { t } = useI18n();
  const identity = useIdentity();
  return (
    <section className="card flex min-h-0 flex-1 border border-base-300 bg-base-100">
      <div className="card-body min-h-0 gap-3 p-4 sm:p-5">
        <div className="flex flex-wrap items-center justify-between gap-3">
          <div>
            <h2 className="card-title text-base">{t(title)}</h2>
            <p className="text-sm text-base-content/70">{t(hint)}</p>
          </div>
          {identity.data?.source && (
            <Badge tone="neutral">
              {t(identity.data.source === "store" ? "fibre.fromStore" : "fibre.fromFactory")}
            </Badge>
          )}
        </div>
        {identity.data ? (
          <IdentityForm identity={identity.data} groups={groups} />
        ) : identity.isError ? (
          <p className="text-sm text-error">{identity.error.message}</p>
        ) : (
          <span className="loading loading-spinner" />
        )}
      </div>
    </section>
  );
}

/* ------------------------------------------------------------------- OMCI */

function decoded(b: Uint8Array | null): string {
  if (b == null) return "••••";
  const printable = b.length > 1 && [...b].every((c) => c === 0 || (c >= 32 && c < 127));
  /* Text, an empty text as well: a field of zeros longer than a number. */
  if (printable && (b.some((c) => c !== 0) || b.length > 4))
    return `"${String.fromCharCode(...b.filter((c) => c))}"`;
  if (b.length <= 4) {
    let v = 0;
    for (const c of b) v = v * 256 + c;
    return String(v);
  }
  return "";
}

/* The instances of a class, a page at a time: a class may have hundreds. */
function Instances({ cls }: { cls: number }) {
  const { t } = useI18n();
  const { mib } = useMib();
  const pager = usePages(mib.all(cls), 10);
  return (
    <>
      <div className="grid min-h-0 flex-1 auto-rows-max content-start gap-4 overflow-auto pr-1">
        {pager.shown.map((me) => (
          <div key={me.id} className="overflow-hidden rounded-box border border-base-300">
            <div className="flex items-baseline gap-2 bg-base-200/70 px-3 py-1.5 text-sm">
              <span className="font-semibold">{t("fibre.instance")}</span>
              <span className="tabular font-mono">0x{me.id.toString(16).padStart(4, "0")}</span>
              <span className="tabular text-xs text-base-content/70">({me.id})</span>
            </div>
            {me.error ? (
              <p className="flex items-center gap-1.5 px-3 py-2 text-xs text-base-content/70">
                <CircleAlert className="size-3.5 shrink-0 text-warning" aria-hidden />
                {t("fibre.omciTooBig")}
              </p>
            ) : (
              <table className={TABLE}>
                <thead>
                  <tr>
                    <th className="w-8">#</th>
                    <th className="w-72">{t("fibre.colAttr")}</th>
                    <th>{t("fibre.colRaw")}</th>
                    <th className="w-40">{t("fibre.colRead")}</th>
                  </tr>
                </thead>
                <tbody>
                  {[...me.attrs.entries()].map(([n, b]) => (
                    <tr key={n}>
                      <td className="tabular w-8 text-base-content/70">{n}</td>
                      <td className="w-72">
                        {CLASSES[cls]?.attrs[n] ?? n}
                        {me.tables.has(n) && (
                          <span className="ml-1.5 badge badge-ghost badge-sm">
                            {t("fibre.table")}
                          </span>
                        )}
                      </td>
                      <td className="font-mono break-all">{hex(b) || "–"}</td>
                      <td className="w-40">{decoded(b)}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            )}
          </div>
        ))}
      </div>
      <Pager pager={pager} />
    </>
  );
}

function OmciBrowser() {
  const { t } = useI18n();
  const { omci, mib } = useMib();
  const [cls, setCls] = useState<number | null>(null);
  const [q, setQ] = useState("");
  const classes = [...mib.byClass.keys()].filter(
    (c) =>
      !q ||
      String(c).includes(q) ||
      (CLASSES[c]?.name ?? "").toLowerCase().includes(q.toLowerCase()),
  );
  const shown = cls != null && mib.byClass.has(cls) ? cls : (classes[0] ?? null);
  return (
    <OmciState>
      <div className="grid min-h-0 flex-1 gap-4 lg:grid-cols-[22rem_minmax(0,1fr)]">
        <section className="card flex min-h-0 border border-base-300 bg-base-100">
          <div className="card-body min-h-0 gap-3 p-4">
            <h2 className="card-title text-base">{t("fibre.omciClasses")}</h2>
            <Filter
              value={q}
              onChange={setQ}
              placeholder={t("fibre.classFilter")}
              className="sm:w-full"
            />
            <ul className="grid min-h-0 flex-1 content-start gap-0.5 overflow-auto pr-1">
              {classes.map((c) => (
                <li key={c}>
                  <button
                    type="button"
                    onClick={() => setCls(c)}
                    className={`grid w-full grid-cols-[2.5rem_minmax(0,1fr)_auto] items-center gap-2 rounded-field px-2.5 py-2 text-left text-sm transition-colors ${c === shown ? "bg-primary/10 font-semibold text-primary" : "hover:bg-base-200"}`}
                  >
                    <span
                      className={`tabular text-xs ${c === shown ? "text-primary" : "text-base-content/70"}`}
                    >
                      {c}
                    </span>
                    <span className="leading-snug">{CLASSES[c]?.name ?? c}</span>
                    <span className="badge badge-sm badge-ghost tabular">{mib.all(c).length}</span>
                  </button>
                </li>
              ))}
            </ul>
            {omci.data?.frames && (
              <p className="border-t border-base-200 pt-2 text-xs text-base-content/70">
                {t("fibre.omciFramesValue", {
                  handled: omci.data.frames.handled,
                  rejected: omci.data.frames.rejected,
                })}
              </p>
            )}
          </div>
        </section>
        <section className="card flex min-h-0 border border-base-300 bg-base-100">
          <div className="card-body min-h-0 gap-3 p-4">
            <h2 className="card-title text-base">
              {shown != null && (
                <>
                  <span className="tabular text-base-content/70">{shown}</span>
                  {CLASSES[shown]?.name ?? ""}
                  <span className="badge badge-sm badge-ghost">
                    {t("fibre.instancesN", { n: mib.all(shown).length })}
                  </span>
                </>
              )}
            </h2>
            {shown != null && <Instances key={shown} cls={shown} />}
          </div>
        </section>
      </div>
    </OmciState>
  );
}

/* The state of the connection to the operator, at the right of each of these pages' titles. */
function FibreState() {
  const { t } = useI18n();
  const f = fibre(useLive().data);
  return <Badge tone={f.tone}>{t(`state.${f.stateKey}`)}</Badge>;
}

export function FibrePage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.fibre")} actions={<FibreState />}>
      <LineState />
    </Page>
  );
}

export function ServicesPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.services")} actions={<FibreState />}>
      <ServiceList />
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

export function IdentityPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.identity")} actions={<FibreState />}>
      <IdentityCard groups={OMCI_GROUPS} title="fibre.identity" hint="fibre.identityHint" />
    </Page>
  );
}

export function OmciPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.omci")} actions={<FibreState />}>
      <OmciBrowser />
    </Page>
  );
}
