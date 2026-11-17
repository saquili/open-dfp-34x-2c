// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The SFP port, towards the host: the lane, its mode and the traffic on it,
 * and the management VLAN and address the stick is reached on. Both changes
 * can cut the panel off, as the panel comes over this very port, so neither
 * is final at once: a new lane mode is kept only when it is confirmed from
 * the panel within a minute, else the stick goes back to the old one; new
 * management settings (untagged or a VLAN, the address, a gateway) are added
 * beside the old ones and replace them only when confirmed from a panel
 * opened at the new address.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import {
  Check,
  CircleAlert,
  ExternalLink,
  Info,
  RotateCcw,
  TriangleAlert,
  Undo2,
} from "lucide-react";
import { useState } from "react";
import { useForm } from "react-hook-form";
import {
  useLaneChange,
  useMgmt,
  useMgmtChange,
  usePvidChange,
  useServiceRules,
  useSfpLane,
  useSfpPvid,
} from "../api/config";
import { useLive, useTraffic } from "../api/live";
import type { LaneMode } from "../api/types";
import { Field, SaveState, useChangeState } from "../components/Field";
import { Page } from "../components/Page";
import { QueryNote } from "../components/Read";
import { HistoryNote, TrafficChart } from "../components/TrafficChart";
import { Badge, Card, Facts, Figure, Stat } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { useCountdown } from "../lib/countdown";
import { bitrate, integer } from "../lib/format";
import { ip4, net4 } from "../lib/ipv4";
import { vlan, z } from "../lib/schemas";
import { hostLink, laneMode, linkSpeed, port } from "../lib/state";
import { hostSide, useStickServices } from "./internet/shared";

/*
 * The lane's modes: the stick's own search, and the four it keeps to, each
 * speed with in-band negotiation and without, named by their speed with
 * their standard's name and the negotiation under it. Each was tested with a
 * host in the same mode.
 */
const MODES: {
  id: LaneMode;
  label: Key;
  hint: Key;
  standard?: string;
  an?: boolean;
}[] = [
  { id: "auto", label: "sfp.modeAuto", hint: "sfp.modeAutoHint" },
  {
    id: "2500basex",
    label: "sfp.mode2500",
    hint: "sfp.mode2500Hint",
    standard: "2500BASE-X",
    an: false,
  },
  {
    id: "2500basex-an",
    label: "sfp.mode2500",
    hint: "sfp.mode2500anHint",
    standard: "2500BASE-X",
    an: true,
  },
  {
    id: "1000basex",
    label: "sfp.mode1000",
    hint: "sfp.mode1000Hint",
    standard: "1000BASE-X",
    an: false,
  },
  {
    id: "1000basex-an",
    label: "sfp.mode1000",
    hint: "sfp.mode1000anHint",
    standard: "1000BASE-X",
    an: true,
  },
];

function LaneCard() {
  const { t, lang } = useI18n();
  /* A mode's whole name: its speed, and with negotiation or without. */
  const modeName = (m: (typeof MODES)[number] | undefined) =>
    !m || m.an == null
      ? t(m?.label ?? "sfp.modeAuto")
      : `${t(m.label)} ${t(m.an ? "sfp.withAn" : "sfp.withoutAn")}`;
  const { data: live, dataUpdatedAt } = useLive();
  const lane = useSfpLane();
  const change = useLaneChange();
  const state = useChangeState(change);
  const link = hostLink(live);
  const sfp = port(live, "sfp");
  const l = lane.data;
  const [pick, setPick] = useState<LaneMode | null>(null);
  const left = useCountdown(l?.trial?.until_s, live?.uptime.up_s, dataUpdatedAt);
  const chosen = pick ?? l?.choice ?? "auto";
  const active = l?.active ?? laneMode(live)?.toLowerCase().replace("base-x", "basex") ?? null;
  const activeMode = MODES.find((m) => m.id === active);

  const apply = () => {
    if (!window.confirm(t("sfp.modeConfirm"))) return;
    change.mutate({ MODE: chosen }, { onSuccess: () => setPick(null) });
  };

  return (
    <Card
      title={t("sfp.link")}
      action={
        <Badge tone={link.up ? "good" : "bad"}>{link.up ? t("sfp.up") : t("sfp.down")}</Badge>
      }
    >
      <div className="grid grid-cols-2 gap-3 sm:grid-cols-4">
        <Figure value={linkSpeed(link.speedMbit, lang) ?? "–"} caption={t("sfp.speed")} />
        <Stat
          label={t("sfp.modeNow")}
          value={activeMode ? t(activeMode.label) : (laneMode(live) ?? "–")}
          sub={
            l?.choice === "auto"
              ? t("sfp.foundByAuto")
              : activeMode?.an != null
                ? t(activeMode.an ? "sfp.withAn" : "sfp.withoutAn")
                : undefined
          }
        />
        <Stat label={t("sfp.mtu")} value={sfp?.mtu ?? "–"} />
        <Stat
          label={t("sfp.stickMac")}
          value={<span className="font-mono text-sm">{sfp?.mac ?? "–"}</span>}
        />
      </div>

      {l?.trial ? (
        <div
          role="alert"
          className="alert alert-warning alert-soft grid-flow-row gap-2 text-base-content sm:grid-flow-col"
        >
          <span className="flex items-start gap-2 text-sm">
            <CircleAlert className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
            {t("sfp.trial", {
              mode: modeName(MODES.find((m) => m.id === l.trial?.mode)),
              left: left ?? "…",
            })}
          </span>
          <span className="flex flex-wrap items-center gap-2">
            <button
              type="button"
              className="btn btn-sm btn-primary"
              disabled={change.isPending}
              onClick={() => change.mutate({ KEEP: "1" })}
            >
              <Check className="size-4" />
              {t("sfp.keep")}
            </button>
            <button
              type="button"
              className="btn btn-sm"
              disabled={change.isPending}
              onClick={() => change.mutate({ REVERT: "1" })}
            >
              <Undo2 className="size-4" />
              {t("sfp.revert")}
            </button>
            <SaveState {...state} />
          </span>
        </div>
      ) : (
        <>
          {/* The search across, then a row for each speed: without negotiation, with it. */}
          <div className="grid gap-2 sm:grid-cols-2">
            {MODES.map((m) => (
              <label
                key={m.id}
                title={m.id === "auto" ? undefined : t(m.hint)}
                className={`flex cursor-pointer items-center gap-2 rounded-box border px-3 py-2 ${m.id === "auto" ? "sm:col-span-2" : ""} ${chosen === m.id ? "border-primary bg-primary/5" : "border-base-300"}`}
              >
                <input
                  type="radio"
                  name="lane"
                  className="radio radio-sm radio-primary"
                  checked={chosen === m.id}
                  disabled={!l}
                  onChange={() => setPick(m.id)}
                />
                <span className="grid min-w-0 flex-1 leading-tight">
                  <span className="text-sm font-medium">{t(m.label)}</span>
                  {m.standard ? (
                    <span className="text-xs whitespace-nowrap text-base-content/70">
                      {m.standard} · {t(m.an ? "sfp.withAn" : "sfp.withoutAn")}
                    </span>
                  ) : (
                    <span className="text-xs text-base-content/70">{t(m.hint)}</span>
                  )}
                </span>
                {m.id === active && <Badge tone="good">{t("sfp.modeActive")}</Badge>}
              </label>
            ))}
          </div>
          <div className="flex flex-wrap items-center gap-3">
            <button
              type="button"
              className="btn btn-primary btn-sm"
              disabled={!l || chosen === l.choice || change.isPending}
              onClick={apply}
            >
              <RotateCcw className="size-4" />
              {t("sfp.modeTry")}
            </button>
            <SaveState {...state} />
            <QueryNote reads={lane} missing={t("sfp.modeUnavailable")} />
            <span className="min-w-0 flex-1 basis-64 text-xs text-base-content/70">
              {t("sfp.modeSafety")}
            </span>
          </div>
        </>
      )}
    </Card>
  );
}

/** Whether @cidr is an IPv4 address with its prefix. */
const isCidr = (v: string) => {
  const m = /^((\d{1,3}\.){3}\d{1,3})\/(\d{1,2})$/.exec(v);
  return m != null && ip4(m[1] ?? "") != null && +(m[3] ?? 99) <= 32;
};

/*
 * Whether @gateway can be the gateway of @cidr: an address of its network,
 * not the stick's own, nor, on a network of more than two addresses, the
 * network's first or last, which no host holds.
 */
function gatewayFits(gateway: string, cidr: string) {
  const g = ip4(gateway);
  const n = net4(cidr);
  const own = ip4(cidr.split("/")[0] ?? "");
  if (g == null || n == null || own == null || g === own) return false;
  if (g < n.first || g > n.last) return false;
  return n.last - n.first < 2 || (g !== n.first && g !== n.last);
}

function MgmtCard() {
  const { t } = useI18n();
  const mgmt = useMgmt();
  const pvid = useSfpPvid();
  const change = useMgmtChange();
  const state = useChangeState(change);
  const m = mgmt.data;
  const schema = z
    .object({
      vlan: vlan(t),
      address: z.string().check(z.trim(), z.refine(isCidr, t("sfp.mgmtAddressRule"))),
      gateway: z.string().check(z.trim()),
    })
    .check(
      z.refine((v) => v.vlan !== "", { message: t("sfp.mgmtVlanRequired"), path: ["vlan"] }),
      z.refine((v) => v.gateway === "" || !isCidr(v.address) || gatewayFits(v.gateway, v.address), {
        message: t("sfp.mgmtGatewayRule"),
        path: ["gateway"],
      }),
    );
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    values: {
      vlan: m?.vlan != null ? String(m.vlan) : "",
      address: m?.address ?? "",
      gateway: m?.gateway ?? "",
    },
  });
  const { errors, isDirty } = form.formState;
  const here = window.location.hostname;
  const pending = m?.pending;
  const onNew = pending && here === pending.address.split("/")[0];
  /* The panel answers untagged too while the port's PVID is the management VLAN. */
  const untagged = m != null && pvid.data?.pvid === m.vlan;

  return (
    <Card title={t("sfp.mgmtTitle")}>
      <p className="text-sm text-base-content/70">{t("sfp.mgmtHint")}</p>
      <QueryNote reads={mgmt} missing={t("sfp.mgmtUnavailable")} />
      {m && (
        <Facts
          rows={[
            [t("sfp.mgmtVlan"), untagged ? t("sfp.mgmtVlanAndUntagged", { vlan: m.vlan }) : m.vlan],
            [
              t("sfp.mgmtAddress"),
              <span key="a" className="tabular">
                {m.address ?? "–"}
              </span>,
            ],
            [
              t("sfp.mgmtGatewayShown"),
              <span key="g" className="tabular">
                {m.gateway ?? t("sfp.mgmtNoGateway")}
              </span>,
            ],
            m.data_address ? [t("sfp.dataAddress"), m.data_address] : null,
            [
              t("sfp.mgmtHere"),
              <span key="h" className="tabular">
                {here}
              </span>,
            ],
          ]}
        />
      )}

      {pending ? (
        <div
          role="alert"
          className="alert alert-info alert-soft grid-flow-row grid-cols-1 gap-2 text-base-content sm:grid-cols-1"
        >
          <span className="flex items-start gap-2 text-sm">
            <Info className="mt-0.5 size-4 shrink-0 text-info" aria-hidden />
            {onNew
              ? t("sfp.mgmtConfirmHere")
              : t("sfp.mgmtPending", {
                  address: pending.address,
                  where:
                    t("sfp.mgmtPendingVlan", { vlan: pending.vlan }) +
                    (pending.gateway
                      ? t("sfp.mgmtPendingGateway", { gateway: pending.gateway })
                      : ""),
                })}
          </span>
          <span className="flex flex-wrap gap-2">
            {onNew ? (
              <button
                type="button"
                className="btn btn-sm btn-primary"
                disabled={change.isPending}
                onClick={() => change.mutate({ COMMIT: "1" })}
              >
                <Check className="size-4" />
                {t("sfp.mgmtCommit")}
              </button>
            ) : (
              <a
                className="btn btn-sm btn-primary"
                href={`${window.location.protocol}//${pending.address.split("/")[0]}:${window.location.port || 8080}/sfp`}
                target="_blank"
                rel="noreferrer"
              >
                <ExternalLink className="size-4" />
                {t("sfp.mgmtOpenNew")}
              </a>
            )}
            <button
              type="button"
              className="btn btn-sm"
              disabled={change.isPending}
              onClick={() => change.mutate({ CANCEL: "1" })}
            >
              <Undo2 className="size-4" />
              {t("sfp.mgmtCancel")}
            </button>
            <SaveState {...state} />
          </span>
        </div>
      ) : (
        <form
          onSubmit={form.handleSubmit((v) => {
            if (!window.confirm(t("sfp.mgmtConfirm"))) return;
            change.mutate({ VLAN: v.vlan, ADDRESS: v.address, GATEWAY: v.gateway });
          })}
          noValidate
          className="grid gap-3"
        >
          <div className="grid gap-3 sm:grid-cols-2">
            <Field
              label={t("sfp.mgmtVlan")}
              hint={t("sfp.mgmtVlanHint")}
              inputMode="numeric"
              error={errors.vlan?.message}
              disabled={!m}
              {...form.register("vlan")}
            />
            <Field
              label={t("sfp.mgmtAddress")}
              hint={t("sfp.mgmtAddressHint")}
              error={errors.address?.message}
              disabled={!m}
              {...form.register("address")}
            />
            <Field
              label={t("sfp.mgmtGateway")}
              hint={t("sfp.mgmtGatewayHint")}
              error={errors.gateway?.message}
              disabled={!m}
              {...form.register("gateway")}
            />
          </div>
          <div className="flex flex-wrap items-center gap-3">
            <button
              type="submit"
              className="btn btn-primary btn-sm"
              disabled={!m || !isDirty || change.isPending}
            >
              {t("sfp.mgmtAdd")}
            </button>
            <SaveState {...state} />
            <span className="min-w-0 flex-1 basis-64 text-xs text-base-content/70">
              {t("sfp.mgmtSafety")}
            </span>
          </div>
        </form>
      )}
    </Card>
  );
}

/** A VLAN of the stick's switch view: its members, each tagged or not, and what it is. */
interface VlanRow {
  vlan: number;
  what: string;
  /** On the PON side: untagged, tagged, or no member. */
  pon: "U" | "T" | "-";
  /** The stick itself (the CPU) is a member: management, or a session of its own. */
  stick: boolean;
  /** The router's side reaches it tagged even while it is not the PVID. */
  sfp: boolean;
}

/*
 * The stick as an 802.1Q switch (ADR 0023): the SFP port to the router with a
 * PVID a person sets, the PON port to the operator with PVID 1, the operator's
 * untagged traffic, and the stick itself; a table of the VLANs and their
 * members, untagged (U), tagged (T) or not (–), as on a switch's own page. The
 * SFP port's PVID decides where the router's untagged frames go: the
 * management VLAN, as a new stick starts, 1 to carry them to the operator as
 * they are, or a service's VLAN. A change runs on trial, as the lane's does.
 */
function PvidCard() {
  const { t } = useI18n();
  const { data: live, dataUpdatedAt } = useLive();
  const mgmt = useMgmt();
  const pvid = useSfpPvid();
  const rules = useServiceRules();
  const { list } = useStickServices();
  const change = usePvidChange();
  const state = useChangeState(change);
  const p = pvid.data;
  const m = mgmt.data;
  const u = rules.data?.untagged;
  const left = useCountdown(p?.trial?.until_s, live?.uptime.up_s, dataUpdatedAt);
  const [pick, setPick] = useState<string | null>(null);
  const [other, setOther] = useState("");

  const rows: VlanRow[] = [];
  const seen = new Set<number>();
  const add = (r: VlanRow) => {
    if (seen.has(r.vlan)) return;
    seen.add(r.vlan);
    rows.push(r);
  };
  add({ vlan: 1, what: t("sfp.vlanOperatorUntagged"), pon: "U", stick: false, sfp: false });
  if (m) add({ vlan: m.vlan, what: t("sfp.vlanMgmt"), pon: "-", stick: true, sfp: true });
  if (u?.host != null)
    add({ vlan: u.host, what: t("sfp.vlanOltUntagged"), pon: "T", stick: false, sfp: true });
  for (const s of list)
    if (!s.off)
      for (const v of hostSide(s)) {
        const what = t(s.multicast ? "sfp.pvidWhatTv" : "sfp.pvidWhatService");
        add({
          vlan: v,
          what: s.vlan != null && s.vlan !== v ? t("sfp.vlanVia", { what, fibre: s.vlan }) : what,
          pon: "T",
          stick: false,
          sfp: true,
        });
      }
  rows.sort((a, b) => a.vlan - b.vlan);

  const current = p ? String(p.pvid) : "";
  const chosen = pick ?? current;
  const value = chosen === "other" ? other.trim() : chosen;
  const valid = /^\d+$/.test(value) && +value >= 1 && +value <= 4094;
  const mgmtOff = m != null && p != null && valid && +value !== m.vlan;
  const what = (v: number) => rows.find((r) => r.vlan === v)?.what ?? t("sfp.vlanNothing");
  /* The SFP port's membership of a VLAN, as the PVID in force makes it. */
  const sfpCell = (r: VlanRow) => (p?.pvid === r.vlan ? "U" : r.sfp ? "T" : "-");

  const apply = () => {
    if (
      !window.confirm(
        t(mgmtOff ? "sfp.pvidConfirmTagged" : "sfp.pvidConfirm", { vlan: m?.vlan ?? "" }),
      )
    )
      return;
    change.mutate({ PVID: value }, { onSuccess: () => setPick(null) });
  };

  const cell = (c: "U" | "T" | "-") =>
    c === "U" ? (
      <Badge tone="good">U</Badge>
    ) : c === "T" ? (
      <span className="font-mono text-sm">T</span>
    ) : (
      <span className="text-base-content/40">–</span>
    );

  return (
    <Card title={t("sfp.vlansTitle")}>
      <p className="text-sm text-base-content/70">{t("sfp.vlansHint")}</p>
      <div className="overflow-x-auto">
        <table className="table table-sm">
          <thead>
            <tr>
              <th>{t("sfp.vlansPort")}</th>
              <th>PVID</th>
            </tr>
          </thead>
          <tbody>
            <tr>
              <td>{t("sfp.portSfp")}</td>
              <td className="tabular font-medium">
                {p ? (p.trial ? t("sfp.pvidOnTrial", { pvid: p.pvid }) : p.pvid) : "–"}
              </td>
            </tr>
            <tr>
              <td>{t("sfp.portPon")}</td>
              <td className="tabular">1</td>
            </tr>
          </tbody>
        </table>
      </div>
      <div className="overflow-x-auto">
        <table className="table table-sm">
          <thead>
            <tr>
              <th>VLAN</th>
              <th>SFP</th>
              <th>PON</th>
              <th>{t("sfp.vlansStick")}</th>
              <th>{t("sfp.vlansWhat")}</th>
            </tr>
          </thead>
          <tbody>
            {rows.map((r) => (
              <tr key={r.vlan}>
                <td className="tabular font-medium">{r.vlan}</td>
                <td>{cell(sfpCell(r))}</td>
                <td>{cell(r.pon)}</td>
                <td>{r.stick ? <Check className="size-4 text-success" /> : cell("-")}</td>
                <td className="text-sm">{r.what}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <p className="text-xs text-base-content/70">{t("sfp.vlansLegend")}</p>

      {p && m && u?.clash && u.host != null && (
        <div role="alert" className="alert alert-warning alert-soft text-base-content">
          <TriangleAlert className="size-4 shrink-0 text-warning" aria-hidden />
          <span className="text-sm">{t("sfp.pvidOltService", { vlan: u.host, mgmt: m.vlan })}</span>
        </div>
      )}
      {p && m && u?.bridged && p.pvid !== 1 && (
        <div role="alert" className="alert alert-warning alert-soft text-base-content">
          <TriangleAlert className="size-4 shrink-0 text-warning" aria-hidden />
          <span className="text-sm">{t("sfp.pvidOltBridged", { mgmt: m.vlan })}</span>
        </div>
      )}

      {p?.trial ? (
        <div
          role="alert"
          className="alert alert-warning alert-soft grid-flow-row gap-2 text-base-content sm:grid-flow-col"
        >
          <span className="flex items-start gap-2 text-sm">
            <CircleAlert className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
            {t("sfp.pvidTrial", { now: p.trial.pvid, before: p.trial.previous, left: left ?? "…" })}
          </span>
          <span className="flex flex-wrap items-center gap-2">
            <button
              type="button"
              className="btn btn-sm btn-primary"
              disabled={change.isPending}
              onClick={() => change.mutate({ KEEP: "1" })}
            >
              <Check className="size-4" />
              {t("sfp.pvidKeep")}
            </button>
            <button
              type="button"
              className="btn btn-sm"
              disabled={change.isPending}
              onClick={() => change.mutate({ REVERT: "1" })}
            >
              <Undo2 className="size-4" />
              {t("sfp.revert")}
            </button>
            <SaveState {...state} />
          </span>
        </div>
      ) : (
        <div className="grid gap-3">
          <label className="grid gap-1">
            <span className="text-sm font-medium">{t("sfp.pvidLabel")}</span>
            <select
              className="select select-sm w-full"
              disabled={!p}
              value={chosen}
              onChange={(e) => setPick(e.currentTarget.value)}
            >
              {rows.map((r) => (
                <option key={r.vlan} value={String(r.vlan)}>
                  {`${r.vlan} – ${r.what}`}
                </option>
              ))}
              {p && !seen.has(p.pvid) && (
                <option value={String(p.pvid)}>{`${p.pvid} – ${what(p.pvid)}`}</option>
              )}
              <option value="other">{t("sfp.pvidOtherChoice")}</option>
            </select>
          </label>
          {chosen === "other" && (
            <Field
              label={t("sfp.pvidOtherLabel")}
              hint={t("sfp.pvidOtherHint")}
              inputMode="numeric"
              value={other}
              onInput={(e) => setOther(e.currentTarget.value)}
              error={other !== "" && !valid ? t("sfp.mgmtVlanRequired") : undefined}
            />
          )}
          {mgmtOff && (
            <span className="text-xs text-warning">
              {t("sfp.pvidTaggedOnly", { vlan: m?.vlan ?? "" })}
            </span>
          )}
          <div className="flex flex-wrap items-center gap-3">
            <button
              type="button"
              className="btn btn-primary btn-sm"
              disabled={!p || !valid || value === current || change.isPending}
              onClick={apply}
            >
              <RotateCcw className="size-4" />
              {t("sfp.pvidTry")}
            </button>
            <SaveState {...state} />
            <span className="min-w-0 flex-1 basis-64 text-xs text-base-content/70">
              {t("sfp.pvidSafety")}
            </span>
          </div>
        </div>
      )}
    </Card>
  );
}

function PortCounters() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const c = live?.port_counters?.sfp;
  /* Every counter is a number (pond/openapi.json); Orval types the ones it does not name as unknown. */
  const n = (k: string) => integer(c?.[k] as number | undefined, lang);
  return (
    <div className="grid content-start grid-cols-2 gap-3 lg:grid-cols-1">
      <Stat
        label={t("sfp.crcErrors")}
        value={n("rx_crc_errors")}
        tone={c?.rx_crc_errors ? "warn" : undefined}
      />
      <Stat label={t("sfp.drops")} value={n("rx_drops")} />
      <Stat label={t("sfp.pauseIn")} value={n("rx_pause")} />
      <Stat label={t("sfp.pauseOut")} value={n("tx_pause")} />
    </div>
  );
}

export function SfpPage() {
  const { t, lang } = useI18n();
  const traffic = useTraffic();
  const now = traffic.at(-1);

  return (
    <Page title={t("sfp.title")}>
      <div className="grid min-h-0 flex-1 gap-4 lg:grid-rows-[auto_minmax(min-content,1fr)]">
        <div className="grid gap-4 xl:grid-cols-[minmax(0,3fr)_minmax(0,2fr)]">
          <div className="grid content-start gap-4">
            <LaneCard />
            <PvidCard />
          </div>
          <MgmtCard />
        </div>
        <Card title={t("sfp.traffic")} action={<HistoryNote />}>
          <div className="grid min-h-0 flex-1 gap-4 sm:grid-cols-[10rem_minmax(0,1fr)] lg:grid-cols-[10rem_minmax(0,1fr)_12rem]">
            <div className="grid content-start grid-cols-2 gap-3 sm:grid-cols-1">
              <Figure value={bitrate(now?.lanDown, lang)} caption={`↓ ${t("sfp.toHost")}`} />
              <Figure value={bitrate(now?.lanUp, lang)} caption={`↑ ${t("sfp.fromHost")}`} />
            </div>
            <TrafficChart data={traffic} down={(r) => r.lanDown} up={(r) => r.lanUp} fill />
            <PortCounters />
          </div>
        </Card>
      </div>
    </Page>
  );
}
