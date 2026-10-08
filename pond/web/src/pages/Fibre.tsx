// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The fibre, in five tabs: the state of the connection, the light and the
 * errors on the line; the services the OLT set up, each a VLAN with its
 * channels, the host's VLAN for it and multicast; the channels one by one;
 * how the stick introduces itself to the OLT, which the owner may change to
 * what the operator expects; and the managed entities as omcid holds them,
 * for the curious.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Activity, Database, Fingerprint, Layers, ListTree } from "lucide-react";
import { type ReactNode, useEffect, useMemo, useState } from "react";
import { useForm } from "react-hook-form";
import { useIdentity, useIdentityChange, useOmci } from "../api/config";
import { useLive, useTraffic } from "../api/live";
import type { Identity } from "../api/types";
import { Field, SaveState, useChangeState } from "../components/Field";
import { useGemText } from "../components/Gem";
import { Page, Tabs, useTab } from "../components/Page";
import { Sparkline } from "../components/TrafficChart";
import { Badge, Card, Facts, Figure, Stat } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { bitrate, bytes, dbmText, decimal, integer } from "../lib/format";
import {
  type Channel,
  hex,
  isDefault,
  Mib,
  type Multicast,
  num,
  type Service,
  services as servicesOf,
  tagsMatched,
  translation,
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
  RX_MAX_DBM,
  RX_MIN_DBM,
  RX_WEAK_DBM,
  sessions,
} from "../lib/state";

const TABS = ["state", "services", "channels", "identity", "omci"] as const;
type Tab = (typeof TABS)[number];

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

function StateTab() {
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
    <div className="grid min-h-0 flex-1 gap-4 lg:grid-rows-[auto_minmax(10rem,1fr)]">
      <div className="grid gap-4 lg:grid-cols-3 lg:items-start">
        <Card
          title={t("fibre.status")}
          action={<Badge tone={f.tone}>{t(`state.${f.stateKey}`)}</Badge>}
        >
          <Facts
            rows={[
              [t("fibre.onuId"), f.onuId ?? "–"],
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
    <Card title={t("fibre.history")}>
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

function VlanName({ vid }: { vid: number | null }) {
  const { t } = useI18n();
  return <>{vid == null ? t("switch.untagged") : t("switch.vlanN", { id: vid })}</>;
}

function MulticastFacts({ m }: { m: Multicast }) {
  const { t, lang } = useI18n();
  const version =
    m.version === 16 ? "MLDv1" : m.version === 17 ? "MLDv2" : `IGMPv${m.version ?? "?"}`;
  const fn = [t("fibre.mcSnoop"), t("fibre.mcSnoopReport"), t("fibre.mcProxy")][m.fn ?? 0];
  return (
    <div className="rounded-box bg-base-200/60 px-3 py-2 text-sm">
      <div className="font-medium">
        {t("fibre.multicastTitle")} · {version} · {fn}
      </div>
      <div className="text-xs text-base-content/70">
        {[
          m.immediateLeave && t("fibre.immediateLeave"),
          m.rate ? t("fibre.igmpRate", { n: integer(m.rate, lang) }) : null,
          m.downstreamVid != null && t("fibre.mcastVlan", { vid: m.downstreamVid }),
        ]
          .filter(Boolean)
          .join(" · ")}
      </div>
      {m.acl.map((a) => (
        <div key={`${a.portId}-${a.from}`} className="tabular text-xs text-base-content/70">
          {t("fibre.mcastGroups", { from: a.from, to: a.to, gem: a.portId, vlan: a.vlan })}
        </div>
      ))}
    </div>
  );
}

function ServiceCard({ s }: { s: Service }) {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const rates = useTraffic().at(-1)?.gems;
  const ct = useChannelText();
  const gem = useGemText();
  const views = gems(live);
  const own = sessions(live).filter((x) => s.vlan != null && fibreVlan(x) === String(s.vlan));
  const channels = s.ports.flatMap((p) => p.channels.map((c) => ({ c, port: p })));
  const seen = new Set<number>();

  const host =
    s.hostVlans.length === 0
      ? s.vlan == null
        ? t("fibre.hostAll")
        : t("fibre.hostSame", { vid: s.vlan })
      : s.hostVlans
          .map((v) => (v == null ? t("switch.untagged") : t("switch.vlanN", { id: v })))
          .join(", ");

  return (
    <Card
      title={
        <span className="flex items-baseline gap-2">
          {s.vlan == null ? t("fibre.noVlan") : <VlanName vid={s.vlan} />}
          <span className="text-xs font-normal text-base-content/55">{t("fibre.onFibre")}</span>
        </span>
      }
      action={
        <span className="flex flex-wrap justify-end gap-1">
          {own.length > 0 && <Badge tone="info">{t("fibre.tagSession")}</Badge>}
          {s.multicast && <Badge tone="neutral">{t("fibre.tagMulticast")}</Badge>}
        </span>
      }
    >
      <div className="flex items-center gap-2 text-sm">
        <span className="text-base-content/65">{t("fibre.atHost")}</span>
        <span className="font-medium">{host}</span>
      </div>
      <ul className="grid gap-1.5">
        {channels.map(({ c, port }) => {
          if (seen.has(c.portId)) return null;
          seen.add(c.portId);
          const v = views.find((x) => x.gem.port_id === c.portId);
          const r = v ? rates?.[v.gem.name] : undefined;
          const shared = (port.vlans?.length ?? 0) > 1;
          return (
            <li
              key={c.portId}
              className="grid gap-0.5 rounded-box border border-base-300 px-3 py-2 text-sm"
            >
              <div className="flex flex-wrap items-baseline gap-x-3">
                <span className="tabular font-semibold">GEM {c.portId}</span>
                <span className="text-base-content/70">{ct.dir(c)}</span>
                {ct.pbits(c) && <span className="text-base-content/70">{ct.pbits(c)}</span>}
                {c.allocId != null && (
                  <span className="tabular ml-auto text-xs text-base-content/60">
                    T-CONT · Alloc-ID {c.allocId}
                  </span>
                )}
              </div>
              <div className="flex flex-wrap items-baseline gap-x-3 text-xs text-base-content/65">
                {shared && <span>{t("fibre.sharedBroadcast")}</span>}
                {c.encrypted && <span>AES</span>}
                {c.queue && (
                  <span>{t("fibre.queue", { p: c.queue.priority, w: c.queue.weight ?? "–" })}</span>
                )}
                {ct.rate(c.up) && <span>↑ {ct.rate(c.up)}</span>}
                {ct.rate(c.down) && <span>↓ {ct.rate(c.down)}</span>}
                {v && <span>{gem.use(v)}</span>}
                {r && (
                  <span className="tabular ml-auto">
                    ↓ {bitrate(r.down, lang)} ↑ {bitrate(r.up, lang)}
                  </span>
                )}
              </div>
            </li>
          );
        })}
      </ul>
      {own.length > 0 && (
        <p className="text-xs text-base-content/70">
          {t("fibre.sessionsHere", { list: own.map((x) => x.id).join(", ") })}
        </p>
      )}
      {s.multicast && <MulticastFacts m={s.multicast} />}
    </Card>
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

function RulesCard({ mib }: { mib: Mib }) {
  const { t } = useI18n();
  const text = useRuleText();
  const all = vlanRules(mib);
  return (
    <Card title={t("fibre.rulesTitle")} scroll>
      <p className="text-xs text-base-content/65">{t("fibre.rulesHint")}</p>
      {all.length === 0 && <p className="text-sm">{t("fibre.rulesNone")}</p>}
      {all.map((set) => (
        <table key={set.me} className="table table-sm">
          <thead>
            <tr>
              <th>{t("fibre.ruleFrom")}</th>
              <th>{t("fibre.ruleDo")}</th>
            </tr>
          </thead>
          <tbody>
            {set.rules.map((r, i) => (
              // biome-ignore lint/suspicious/noArrayIndexKey: the rules have no other key
              <tr key={i} className={r.remove === 3 ? "text-base-content/60" : ""}>
                <td>{text.match(r)}</td>
                <td>{text.action(r)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      ))}
    </Card>
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

function ServicesTab() {
  const { t } = useI18n();
  const { omci, mib } = useMib();
  const list = servicesOf(mib);
  return (
    <OmciState>
      <div className="grid min-h-0 flex-1 gap-4 lg:grid-cols-[minmax(0,1fr)_24rem]">
        <div className="grid min-h-0 content-start gap-4 lg:overflow-auto">
          <p className="text-sm text-base-content/70">{t("fibre.servicesHint")}</p>
          {omci.data?.truncated && (
            <div role="alert" className="alert alert-warning alert-soft text-sm">
              {t("fibre.truncated")}
            </div>
          )}
          <div className="grid gap-4 xl:grid-cols-2">
            {list.map((s) => (
              <ServiceCard key={s.vlan ?? "none"} s={s} />
            ))}
          </div>
        </div>
        <RulesCard mib={mib} />
      </div>
    </OmciState>
  );
}

/* --------------------------------------------------------------- channels */

function ChannelsTab() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const rates = useTraffic().at(-1)?.gems;
  const { mib } = useMib();
  const gem = useGemText();
  const ct = useChannelText();
  const list = servicesOf(mib);
  const byPort = new Map<number, { c: Channel; vlans: (number | null)[] }>();
  for (const s of list)
    for (const p of s.ports)
      for (const c of p.channels) {
        const had = byPort.get(c.portId);
        byPort.set(c.portId, { c, vlans: [...(had?.vlans ?? []), s.vlan] });
      }
  const tconts = (live?.tconts ?? []).filter((x) => x.alloc_id != null);

  return (
    <div className="grid min-h-0 flex-1 gap-4 lg:grid-rows-[minmax(0,1fr)_auto]">
      <Card title={t("fibre.gemPorts")} scroll>
        <table className="table table-sm table-pin-rows">
          <thead>
            <tr>
              <th>{t("fibre.channel")}</th>
              <th>{t("fibre.direction")}</th>
              <th>{t("fibre.serviceCol")}</th>
              <th>{t("fibre.pbitsCol")}</th>
              <th>{t("fibre.allocId")}</th>
              <th>{t("fibre.use")}</th>
              <th className="text-right">{t("fibre.now")}</th>
              <th className="text-right">{t("fibre.received")}</th>
              <th className="text-right">{t("fibre.sent")}</th>
            </tr>
          </thead>
          <tbody>
            {gems(live).map((v) => {
              const g = v.gem;
              const o = byPort.get(g.port_id);
              const c = live?.gem_counters?.[g.name];
              const r = rates?.[g.name];
              return (
                <tr key={g.name}>
                  <td className="tabular font-medium whitespace-nowrap">
                    GEM {g.port_id}
                    {o?.c.encrypted && (
                      <span className="ml-1 text-xs text-base-content/55">AES</span>
                    )}
                  </td>
                  <td>{gem.direction(g)}</td>
                  <td className="tabular">
                    {o ? o.vlans.map((x) => (x == null ? "–" : x)).join(", ") : "–"}
                  </td>
                  <td>{o ? (ct.pbits(o.c) ?? "–") : "–"}</td>
                  <td className="tabular">{v.allocId ?? "–"}</td>
                  <td>{gem.use(v)}</td>
                  <td className="tabular text-right">
                    {r ? `↓ ${bitrate(r.down, lang)} ↑ ${bitrate(r.up, lang)}` : "–"}
                  </td>
                  <td className="tabular text-right">{bytes(c?.pon_rx_octets, lang)}</td>
                  <td className="tabular text-right">{bytes(c?.pon_tx_octets, lang)}</td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </Card>
      <Card title={t("fibre.tconts")}>
        <p className="text-xs text-base-content/65">{t("fibre.tcontsHint")}</p>
        <div className="flex flex-wrap gap-2">
          {tconts.length
            ? tconts.map((x) => (
                <span key={x.index} className="badge badge-ghost tabular gap-1">
                  T-CONT {x.index} · Alloc-ID {x.alloc_id}
                  <span className="text-base-content/55">
                    {gems(live)
                      .filter((g) => g.gem.tcont === x.index)
                      .map((g) => `GEM ${g.gem.port_id}`)
                      .join(", ")}
                  </span>
                </span>
              ))
            : t("common.none")}
        </div>
      </Card>
    </div>
  );
}

/* --------------------------------------------------------------- identity */

const IDENTITY_FIELDS: {
  field: keyof Values;
  key: string;
  label: Key;
  hint?: Key;
  group: "register" | "unit" | "software";
}[] = [
  {
    field: "serial",
    key: "SERIAL",
    label: "fibre.serial",
    hint: "fibre.serialHint",
    group: "register",
  },
  { field: "loid", key: "LOID", label: "fibre.loid", hint: "fibre.loidHint", group: "register" },
  { field: "vendor_id", key: "VENDOR_ID", label: "fibre.vendorId", group: "unit" },
  { field: "version", key: "VERSION", label: "fibre.version", group: "unit" },
  { field: "equipment_id", key: "EQUIPMENT_ID", label: "fibre.equipmentId", group: "unit" },
  { field: "model", key: "MODEL", label: "fibre.model", group: "unit" },
  {
    field: "equipment_serial",
    key: "EQUIPMENT_SERIAL",
    label: "fibre.equipmentSerial",
    group: "unit",
  },
  {
    field: "omcc_version",
    key: "OMCC_VERSION",
    label: "fibre.omccVersion",
    hint: "fibre.omccHint",
    group: "software",
  },
  {
    field: "vendor_product_code",
    key: "VENDOR_PRODUCT_CODE",
    label: "fibre.productCode",
    group: "software",
  },
  {
    field: "tm_option",
    key: "TM_OPTION",
    label: "fibre.tmOption",
    hint: "fibre.tmHint",
    group: "software",
  },
  { field: "sw_version0", key: "SW_VERSION0", label: "fibre.swVersion0", group: "software" },
  { field: "sw_version1", key: "SW_VERSION1", label: "fibre.swVersion1", group: "software" },
  { field: "pack_version", key: "PACK_VERSION", label: "fibre.packVersion", group: "software" },
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

function IdentityForm({ identity }: { identity: Identity }) {
  const { t } = useI18n();
  const change = useIdentityChange();
  const state = useChangeState(change);
  const form = useForm<Values>({
    resolver: zodResolver(identitySchema(t("fibre.serialRule"))),
    defaultValues: initial(identity),
  });
  const { errors, isDirty, dirtyFields } = form.formState;
  const extended = identity.loid !== undefined;

  useEffect(() => {
    if (!form.formState.isDirty) form.reset(initial(identity));
  }, [identity, form]);

  const submit = form.handleSubmit((v) => {
    if (!window.confirm(t("fibre.identityConfirm"))) return;
    const out: Record<string, string> = {};
    for (const f of IDENTITY_FIELDS)
      if (dirtyFields[f.field] || f.field === "serial") out[f.key] = v[f.field];
    if (v.ploam) out.PLOAM = v.ploam;
    if (v.loid_secret) out.LOID_PASSWORD = v.loid_secret;
    change.mutate(out, { onSuccess: () => form.reset({ ...v, ploam: "", loid_secret: "" }) });
  });

  const group = (g: (typeof IDENTITY_FIELDS)[number]["group"]) =>
    IDENTITY_FIELDS.filter(
      (f) =>
        f.group === g &&
        (extended || !["loid", "sw_version0", "sw_version1", "pack_version"].includes(f.field)),
    ).map((f) => (
      <Field
        key={f.field}
        label={t(f.label)}
        hint={f.hint ? t(f.hint) : undefined}
        error={errors[f.field]?.message}
        {...form.register(f.field)}
      />
    ));

  return (
    <form onSubmit={submit} noValidate className="grid min-h-0 flex-1 gap-4">
      <div className="grid min-h-0 gap-4 lg:grid-cols-3 lg:overflow-auto">
        <fieldset className="grid content-start gap-3">
          <legend className="mb-2 text-sm font-semibold">{t("fibre.groupRegister")}</legend>
          {group("register")}
          <Field
            label={t("fibre.ploam")}
            secret
            autoComplete="new-password"
            placeholder={
              identity.ploam_set ? t("internet.passwordKept") : t("internet.passwordNone")
            }
            hint={t("fibre.ploamHint")}
            {...form.register("ploam")}
          />
          {extended && (
            <Field
              label={t("fibre.loidPassword")}
              secret
              autoComplete="new-password"
              placeholder={
                identity.loid_password_set ? t("internet.passwordKept") : t("internet.passwordNone")
              }
              {...form.register("loid_secret")}
            />
          )}
        </fieldset>
        <fieldset className="grid content-start gap-3">
          <legend className="mb-2 text-sm font-semibold">{t("fibre.groupUnit")}</legend>
          {group("unit")}
        </fieldset>
        <fieldset className="grid content-start gap-3">
          <legend className="mb-2 text-sm font-semibold">{t("fibre.groupSoftware")}</legend>
          {group("software")}
        </fieldset>
      </div>
      <div className="flex flex-wrap items-center gap-3">
        <button type="submit" className="btn btn-primary" disabled={!isDirty || change.isPending}>
          {t("internet.save")}
        </button>
        {isDirty && (
          <button type="button" className="btn btn-ghost" onClick={() => form.reset()}>
            {t("form.discard")}
          </button>
        )}
        <SaveState {...state} />
        <span className="ml-auto max-w-xl text-xs text-warning">{t("fibre.identityWarning")}</span>
      </div>
    </form>
  );
}

function OltView() {
  const { t } = useI18n();
  const { mib } = useMib();
  const u = unitOf(mib);
  if (mib.empty) return null;
  return (
    <Card title={t("fibre.oltSees")} className="lg:w-96">
      <Facts
        rows={[
          [t("fibre.vendorId"), u.vendorId ?? "–"],
          [t("fibre.version"), u.version ?? "–"],
          [t("fibre.equipmentId"), u.equipment ?? "–"],
          [t("fibre.loid"), u.loid ?? "–"],
          [t("fibre.omccVersion"), u.omcc != null ? `0x${u.omcc.toString(16)}` : "–"],
          [t("fibre.productCode"), u.productCode ?? "–"],
          [t("fibre.tmOption"), u.tmOption ?? "–"],
          ...u.images.map(
            (i) =>
              [
                t("fibre.imageN", { n: i.id }),
                <span key={i.id}>
                  {i.version || "–"}
                  <span className="block text-xs font-normal text-base-content/60">
                    {[
                      i.active && t("fibre.imageActive"),
                      i.committed && t("fibre.imageCommitted"),
                      i.valid && t("fibre.imageValid"),
                    ]
                      .filter(Boolean)
                      .join(", ")}
                  </span>
                </span>,
              ] as [ReactNode, ReactNode],
          ),
        ]}
      />
      <p className="text-xs text-base-content/60">{t("fibre.oltSeesHint")}</p>
    </Card>
  );
}

function IdentityTab() {
  const { t } = useI18n();
  const identity = useIdentity();
  return (
    <div className="grid min-h-0 flex-1 gap-4 lg:grid-cols-[minmax(0,1fr)_auto]">
      <Card
        title={t("fibre.identity")}
        action={
          identity.data?.source && (
            <Badge tone={identity.data.source === "store" ? "good" : "info"}>
              {t(identity.data.source === "store" ? "fibre.fromStore" : "fibre.fromFactory")}
            </Badge>
          )
        }
      >
        <p className="text-sm text-base-content/70">{t("fibre.identityHint")}</p>
        {identity.data ? (
          <IdentityForm identity={identity.data} />
        ) : identity.isError ? (
          <p className="text-sm text-error">{identity.error.message}</p>
        ) : (
          <span className="loading loading-spinner" />
        )}
      </Card>
      <OltView />
    </div>
  );
}

/* ------------------------------------------------------------------- OMCI */

function decoded(b: Uint8Array | null): string {
  if (b == null) return "••••";
  const printable = b.length > 1 && [...b].every((c) => c === 0 || (c >= 32 && c < 127));
  if (printable && b.some((c) => c !== 0)) return `"${String.fromCharCode(...b.filter((c) => c))}"`;
  if (b.length <= 4) {
    let v = 0;
    for (const c of b) v = v * 256 + c;
    return String(v);
  }
  return "";
}

function OmciTab() {
  const { t } = useI18n();
  const { omci, mib } = useMib();
  const classes = [...mib.byClass.keys()];
  const [cls, setCls] = useState<number | null>(null);
  const shown = cls ?? classes[0] ?? null;
  return (
    <OmciState>
      <div className="grid min-h-0 flex-1 gap-4 lg:grid-cols-[17rem_minmax(0,1fr)]">
        <Card title={t("fibre.omciClasses")} scroll>
          <ul className="menu menu-sm w-full p-0">
            {classes.map((c) => (
              <li key={c}>
                <button
                  type="button"
                  className={c === shown ? "menu-active" : ""}
                  onClick={() => setCls(c)}
                >
                  <span className="tabular w-8 text-base-content/55">{c}</span>
                  <span className="flex-1 truncate">{CLASSES[c]?.name ?? c}</span>
                  <span className="badge badge-ghost badge-sm">{mib.all(c).length}</span>
                </button>
              </li>
            ))}
          </ul>
          {omci.data?.frames && (
            <p className="text-xs text-base-content/60">
              {t("fibre.omciFramesValue", {
                handled: omci.data.frames.handled,
                rejected: omci.data.frames.rejected,
              })}
            </p>
          )}
        </Card>
        <Card title={shown != null ? `${shown} · ${CLASSES[shown]?.name ?? ""}` : ""} scroll>
          {shown != null &&
            mib.all(shown).map((me) => (
              <section key={me.id} className="grid gap-1">
                <h3 className="tabular text-sm font-semibold">
                  {t("fibre.instance")} 0x{me.id.toString(16).padStart(4, "0")} ({me.id})
                </h3>
                {me.error ? (
                  <p className="text-xs text-warning">{t("fibre.omciTooBig")}</p>
                ) : (
                  <table className="table table-xs">
                    <tbody>
                      {[...me.attrs.entries()].map(([n, b]) => (
                        <tr key={n}>
                          <td className="tabular w-6 text-base-content/55">{n}</td>
                          <td className="w-64">
                            {CLASSES[shown]?.attrs[n] ?? n}
                            {me.tables.has(n) && (
                              <span className="ml-1 badge badge-ghost badge-xs">
                                {t("fibre.table")}
                              </span>
                            )}
                          </td>
                          <td className="font-mono text-xs break-all">{hex(b) || "–"}</td>
                          <td className="text-xs text-base-content/70">{decoded(b)}</td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                )}
              </section>
            ))}
        </Card>
      </div>
    </OmciState>
  );
}

export function Fibre() {
  const { t } = useI18n();
  const [tab, setTab] = useTab(TABS);
  const { data: live } = useLive();
  const f = fibre(live);
  const tabs: { id: Tab; label: string; icon: ReactNode }[] = [
    { id: "state", label: t("fibre.tabState"), icon: <Activity className="size-4" /> },
    { id: "services", label: t("fibre.tabServices"), icon: <Layers className="size-4" /> },
    { id: "channels", label: t("fibre.tabChannels"), icon: <ListTree className="size-4" /> },
    { id: "identity", label: t("fibre.tabIdentity"), icon: <Fingerprint className="size-4" /> },
    { id: "omci", label: t("fibre.tabOmci"), icon: <Database className="size-4" /> },
  ];
  return (
    <Page
      title={t("fibre.title")}
      tabs={<Tabs items={tabs} value={tab} onChange={setTab} label={t("fibre.title")} />}
      actions={<Badge tone={f.tone}>{t(`state.${f.stateKey}`)}</Badge>}
    >
      {tab === "state" && <StateTab />}
      {tab === "services" && <ServicesTab />}
      {tab === "channels" && <ChannelsTab />}
      {tab === "identity" && <IdentityTab />}
      {tab === "omci" && <OmciTab />}
    </Page>
  );
}
