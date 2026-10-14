// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The SFP port, towards the host: the lane, its mode and the traffic on it,
 * and the management VLAN and address the stick is reached on. Both changes
 * can cut the panel off, as the panel comes over this very port, so neither
 * is final at once: a new lane mode is kept only when it is confirmed from
 * the panel within a minute, else the stick goes back to the old one; a new
 * management address is added beside the old one and replaces it only when
 * confirmed from a panel opened at the new address.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Check, ExternalLink, RotateCcw, Undo2 } from "lucide-react";
import { useState } from "react";
import { useForm } from "react-hook-form";
import { useLaneChange, useMgmt, useMgmtChange, useSfpLane } from "../api/config";
import { useLive, useTraffic } from "../api/live";
import type { LaneMode } from "../api/types";
import { Field, SaveState, useChangeState } from "../components/Field";
import { Page } from "../components/Page";
import { HistoryNote, TrafficChart } from "../components/TrafficChart";
import { Badge, Card, Facts, Figure, Stat } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { useCountdown } from "../lib/countdown";
import { bitrate, integer } from "../lib/format";
import { vlan, z } from "../lib/schemas";
import { hostLink, laneMode, linkSpeed, port } from "../lib/state";

/*
 * The lane's modes: the stick's own search, and the four it keeps to, each
 * speed with in-band negotiation and without, named by their speed with
 * their standard's name and the negotiation under it. The two not tested
 * with a host say so.
 */
const MODES: {
  id: LaneMode;
  label: Key;
  hint: Key;
  standard?: string;
  an?: boolean;
  tested?: boolean;
}[] = [
  { id: "auto", label: "sfp.modeAuto", hint: "sfp.modeAutoHint", tested: true },
  {
    id: "2500basex",
    label: "sfp.mode2500",
    hint: "sfp.mode2500Hint",
    standard: "2500BASE-X",
    an: false,
    tested: true,
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
    tested: true,
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
          className="alert alert-warning alert-soft grid-flow-row gap-2 sm:grid-flow-col"
        >
          <span className="text-sm">
            {t("sfp.trial", {
              mode: modeName(MODES.find((m) => m.id === l.trial?.mode)),
              left: left ?? "…",
            })}
          </span>
          <span className="flex gap-2">
            <button
              type="button"
              className="btn btn-sm btn-success"
              onClick={() => change.mutate({ KEEP: "1" })}
            >
              <Check className="size-4" />
              {t("sfp.keep")}
            </button>
            <button
              type="button"
              className="btn btn-sm"
              onClick={() => change.mutate({ REVERT: "1" })}
            >
              <Undo2 className="size-4" />
              {t("sfp.revert")}
            </button>
          </span>
        </div>
      ) : (
        <>
          {/* The search across, then a row for each speed: without negotiation, with it. */}
          <div className="grid gap-2 sm:grid-cols-2">
            {MODES.map((m) => (
              <label
                key={m.id}
                title={t(m.hint)}
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
                  {m.standard && (
                    <span className="text-xs whitespace-nowrap text-base-content/60">
                      {m.standard} · {t(m.an ? "sfp.withAn" : "sfp.withoutAn")}
                    </span>
                  )}
                </span>
                {!m.tested && <Badge tone="neutral">{t("sfp.untested")}</Badge>}
                {m.id === active && <Badge tone="good">{t("sfp.modeActive")}</Badge>}
              </label>
            ))}
          </div>
          <p className="text-xs text-base-content/70">
            {t(MODES.find((m) => m.id === chosen)?.hint ?? "sfp.modeAutoHint")}
          </p>
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
            {!l && lane.isFetched && (
              <span className="text-xs text-base-content/60">{t("sfp.modeUnavailable")}</span>
            )}
          </div>
          <p className="text-xs text-base-content/60">{t("sfp.modeSafety")}</p>
        </>
      )}
    </Card>
  );
}

function MgmtCard() {
  const { t } = useI18n();
  const mgmt = useMgmt();
  const change = useMgmtChange();
  const state = useChangeState(change);
  const m = mgmt.data;
  const schema = z.object({
    vlan: vlan(t),
    address: z.string().check(
      z.trim(),
      z.refine(
        (v) =>
          /^(\d{1,3}\.){3}\d{1,3}\/\d{1,2}$/.test(v) &&
          v
            .split("/")[0]
            ?.split(".")
            .every((o) => +o <= 255) === true &&
          +(v.split("/")[1] ?? 99) <= 32,
        t("sfp.mgmtAddressRule"),
      ),
    ),
  });
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    values: { vlan: m?.vlan != null ? String(m.vlan) : "", address: m?.address ?? "" },
  });
  const { errors, isDirty } = form.formState;
  const here = window.location.hostname;
  const pending = m?.pending;
  const onNew = pending && here === pending.address.split("/")[0];

  return (
    <Card title={t("sfp.mgmtTitle")}>
      <p className="text-sm text-base-content/70">{t("sfp.mgmtHint")}</p>
      {m ? (
        <Facts
          rows={[
            [t("sfp.mgmtVlan"), m.vlan ?? "–"],
            [
              t("sfp.mgmtAddress"),
              <span key="a" className="tabular">
                {m.address ?? "–"}
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
      ) : (
        <p className="text-xs text-base-content/60">{t("sfp.mgmtUnavailable")}</p>
      )}

      {pending ? (
        <div role="alert" className="alert alert-info alert-soft grid-flow-row gap-2">
          <span className="text-sm">
            {onNew
              ? t("sfp.mgmtConfirmHere")
              : t("sfp.mgmtPending", { vlan: pending.vlan, address: pending.address })}
          </span>
          <span className="flex flex-wrap gap-2">
            {onNew ? (
              <button
                type="button"
                className="btn btn-sm btn-success"
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
              onClick={() => change.mutate({ CANCEL: "1" })}
            >
              <Undo2 className="size-4" />
              {t("sfp.mgmtCancel")}
            </button>
          </span>
        </div>
      ) : (
        <form
          onSubmit={form.handleSubmit((v) => {
            if (!window.confirm(t("sfp.mgmtConfirm"))) return;
            change.mutate({ VLAN: v.vlan, ADDRESS: v.address });
          })}
          noValidate
          className="grid gap-3"
        >
          <div className="grid gap-3 sm:grid-cols-2">
            <Field
              label={t("sfp.mgmtVlan")}
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
          </div>
        </form>
      )}
      <p className="text-xs text-base-content/60">{t("sfp.mgmtSafety")}</p>
    </Card>
  );
}

function PortCounters() {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const c = live?.port_counters?.sfp;
  const n = (k: string) => integer(c?.[k], lang);
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
          <LaneCard />
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
