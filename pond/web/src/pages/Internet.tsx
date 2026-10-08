// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The internet. Without anything set here the OLT makes the stick a
 * transparent bridge, and the host dials the operator through it. With a
 * PPPoE account the stick holds the session itself, in its switch's
 * hardware, up to seven at once, and gives the public address to the host;
 * the other way, the stick routing and translating the addresses itself, is
 * to come. Two tabs: the sessions, and how the host gets the address.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Link } from "@tanstack/react-router";
import { ArrowRight, Globe, Network, Pencil, Plus, Trash2 } from "lucide-react";
import { type ReactNode, useEffect, useRef, useState } from "react";
import { useForm } from "react-hook-form";
import { useLinkChange, useSessionChange } from "../api/config";
import { useLive } from "../api/live";
import type { Family } from "../api/types";
import { Field, SaveState, useChangeState } from "../components/Field";
import { Page, Tabs, useTab } from "../components/Page";
import { Badge, Card, Facts, Stat } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { ipv4, mac, required, upTo, vlan, z } from "../lib/schemas";
import {
  fibreVlan,
  gems,
  hostLink,
  type LinkView,
  MAX_SESSIONS,
  type SessionView,
  session,
  sessions,
} from "../lib/state";

const TABS = ["sessions", "handover"] as const;

/* The rows of the session engine, numbered as the sessions are. */
const SLOT_IDS = Array.from({ length: MAX_SESSIONS }, (_, i) => i + 1);

const FAMILY: Record<Family, Key> = {
  both: "internet.famBoth",
  ipv4: "internet.famV4",
  ipv6: "internet.famV6",
};

/** A dialog that opens while @open holds. */
function Dialog({
  open,
  title,
  onClose,
  children,
}: {
  open: boolean;
  title: ReactNode;
  onClose: () => void;
  children: ReactNode;
}) {
  const ref = useRef<HTMLDialogElement>(null);
  useEffect(() => {
    const d = ref.current;
    if (!d) return;
    if (open && !d.open) d.showModal();
    if (!open && d.open) d.close();
  }, [open]);
  return (
    <dialog ref={ref} className="modal" onClose={onClose}>
      <div className="modal-box max-w-2xl">
        <h3 className="mb-4 text-lg font-semibold">{title}</h3>
        {open && children}
      </div>
      <form method="dialog" className="modal-backdrop">
        <button type="submit" aria-label="close" />
      </form>
    </dialog>
  );
}

/** A session's account and settings: a new one in @slot without @x, or @x changed. */
function SessionForm({ x, slot, onDone }: { x?: SessionView; slot?: number; onDone: () => void }) {
  const { t } = useI18n();
  const change = useSessionChange();
  const state = useChangeState(change);
  const all = sessions(useLive().data);
  /* The first session that carries IPv4 is the one that hands its address on. */
  const handsOn = all.find((y) => y.family !== "ipv6");
  const schema = z.object({
    user: x ? upTo(63) : required(t("form.required")),
    secret: x ? z.string().check(z.maxLength(255)) : required(t("internet.secretRequired")),
    vlan: vlan(t),
    mac: mac(t),
    family: z.enum(["both", "ipv4", "ipv6"]),
  });
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: {
      user: x?.user ?? "",
      secret: "",
      vlan: x?.vlan ?? "",
      mac: x?.mac ?? "",
      family: x?.family ?? "both",
    },
  });
  const { errors, isDirty } = form.formState;
  const family = form.watch("family");
  const willHandOn = family !== "ipv6" && (!handsOn || handsOn.id === x?.id);

  const send = (v: Values) =>
    change.mutate(
      {
        ID: x ? String(x.id) : slot ? String(slot) : undefined,
        USER: v.user,
        SECRET: v.secret || undefined,
        VLAN: v.vlan,
        MAC: v.mac,
        FAMILY: v.family,
      },
      { onSuccess: onDone },
    );

  return (
    <form onSubmit={form.handleSubmit(send)} noValidate className="grid gap-4">
      <div className="grid gap-4 sm:grid-cols-2">
        <Field
          label={t("internet.user")}
          autoComplete="username"
          error={errors.user?.message}
          {...form.register("user")}
        />
        <Field
          label={t("internet.passwordField")}
          secret
          autoComplete="new-password"
          placeholder={x?.secretSet ? t("internet.passwordKept") : t("internet.passwordNone")}
          error={errors.secret?.message}
          {...form.register("secret")}
        />
        <Field
          label={t("internet.vlan")}
          hint={t("internet.vlanHint")}
          inputMode="numeric"
          error={errors.vlan?.message}
          {...form.register("vlan")}
        />
        <Field
          label={t("internet.mac")}
          hint={t("internet.macHint")}
          error={errors.mac?.message}
          {...form.register("mac")}
        />
      </div>
      <div className="grid gap-1">
        <span className="text-sm font-medium">{t("internet.family")}</span>
        <div className="join">
          {(["both", "ipv4", "ipv6"] as const).map((f) => (
            <label key={f} className="join-item btn btn-sm has-[:checked]:btn-primary">
              <input type="radio" value={f} className="hidden" {...form.register("family")} />
              {t(FAMILY[f])}
            </label>
          ))}
        </div>
        <p className="text-xs text-base-content/60">{t("internet.familyHint")}</p>
      </div>
      <fieldset className="grid gap-2">
        <legend className="mb-1 text-sm font-medium">{t("internet.addressUse")}</legend>
        <label className="flex cursor-pointer items-start gap-3 rounded-box border border-primary/50 bg-primary/5 p-3">
          <input
            type="radio"
            name="use"
            className="radio radio-sm radio-primary mt-0.5"
            defaultChecked
          />
          <span>
            <span className="block text-sm font-medium">{t("internet.useHandover")}</span>
            <span className="block text-xs text-base-content/65">
              {family === "ipv6"
                ? t("internet.useHandoverV6")
                : willHandOn
                  ? t("internet.useHandoverHint")
                  : t("internet.useHandoverTaken", { id: handsOn?.id ?? 1 })}
            </span>
          </span>
        </label>
        <label className="flex items-start gap-3 rounded-box border border-base-300 p-3 opacity-60">
          <input type="radio" name="use" className="radio radio-sm mt-0.5" disabled />
          <span>
            <span className="flex items-center gap-2 text-sm font-medium">
              {t("internet.useNat")}
              <Badge tone="neutral">{t("internet.soon")}</Badge>
            </span>
            <span className="block text-xs text-base-content/65">{t("internet.useNatHint")}</span>
          </span>
        </label>
      </fieldset>
      <div className="flex flex-wrap items-center gap-3">
        <button
          type="submit"
          className="btn btn-primary"
          disabled={(x && !isDirty) || change.isPending}
        >
          {x ? t("internet.save") : t("internet.add")}
        </button>
        <button type="button" className="btn btn-ghost" onClick={onDone}>
          {t("internet.cancel")}
        </button>
        <SaveState {...state} />
      </div>
    </form>
  );
}

/** One session: its state, what it carries, and its account. */
function SessionCard({ x, onEdit }: { x: SessionView; onEdit: () => void }) {
  const { t } = useI18n();
  const { data: live } = useLive();
  const change = useSessionChange();
  const state = useChangeState(change);
  const portId = gems(live).find((g) => g.gem.name === x.gemDev)?.gem.port_id;
  /* Sessions in one VLAN send from the MAC address of the first of them. */
  const first = sessions(live).find((y) => y.vlan === x.vlan && y.gem === x.gem);
  const macText =
    first && first.id !== x.id
      ? `${first.mac || t("internet.macStick")} (${t("internet.macShared", { id: first.id })})`
      : x.mac || t("internet.macStick");

  const remove = () => {
    if (!window.confirm(t("internet.removeSessionConfirm", { id: x.id }))) return;
    change.mutate({ ID: String(x.id), USER: "" });
  };

  return (
    <Card
      title={
        <span className="flex items-center gap-2">
          {t("internet.slotN", { id: x.id })}
          <Badge tone={x.up ? "good" : "warn"}>
            {x.up ? t("internet.up") : t("internet.connecting")}
          </Badge>
        </span>
      }
      action={
        <span className="flex gap-1">
          <button
            type="button"
            className="btn btn-ghost btn-sm btn-square"
            onClick={onEdit}
            aria-label={t("internet.edit")}
            title={t("internet.edit")}
          >
            <Pencil className="size-4" />
          </button>
          <button
            type="button"
            className="btn btn-ghost btn-sm btn-square text-error"
            onClick={remove}
            disabled={change.isPending}
            aria-label={t("internet.removeSession")}
            title={t("internet.removeSession")}
          >
            <Trash2 className="size-4" />
          </button>
        </span>
      }
    >
      <div className="flex flex-wrap gap-1">
        <Badge tone="neutral">{t(FAMILY[x.family])}</Badge>
        {x.handsOn && <Badge tone="info">{t("internet.handsOn")}</Badge>}
        {x.prefixHost && <Badge tone="info">{t("internet.prefixToHost")}</Badge>}
      </div>
      <Facts
        rows={[
          [
            t("internet.account"),
            <span key="u" className="break-all">
              {x.user}
            </span>,
          ],
          [
            t("internet.vlan"),
            !x.vlan
              ? t("switch.untagged")
              : fibreVlan(x) !== x.vlan
                ? t("internet.vlanOnFibre", { vlan: x.vlan, fibre: fibreVlan(x) })
                : x.vlan,
          ],
          [
            t("internet.channel"),
            portId != null
              ? `GEM ${portId}`
              : x.gem === "auto"
                ? t("internet.autoGem")
                : `GEM ${x.gem}`,
          ],
          [
            t("internet.mac"),
            <span key="m" className="font-mono text-xs">
              {macText}
            </span>,
          ],
          x.family !== "ipv6" && [
            "IPv4",
            x.ipv4
              ? t(x.handsOn ? "internet.ipv4Host" : "internet.ipv4Stick", { addr: x.ipv4 })
              : "–",
          ],
          x.family !== "ipv4" && [
            t("internet.prefix"),
            x.prefix
              ? t(x.prefixHost ? "internet.prefixHost" : "internet.prefixWaiting", {
                  prefix: x.prefix,
                })
              : x.ipv6
                ? t("internet.prefixNone")
                : "–",
          ],
          x.dev ? [t("internet.iface"), [x.dev, x.ppp].filter(Boolean).join(" → ")] : null,
        ]}
      />
      <SaveState {...state} />
    </Card>
  );
}

/** How many of the session engine's seven rows are in use. */
function Slots({ used }: { used: number }) {
  const { t } = useI18n();
  return (
    <span className="text-sm">
      <span className="tabular font-semibold">
        {t("internet.slots", { n: used, max: MAX_SESSIONS })}
      </span>{" "}
      <span className="text-base-content/70">{t("internet.slotsHint")}</span>
    </span>
  );
}

/** One row of the session engine, as a small card: its session in short, or free. */
function SlotTile({
  id,
  x,
  selected,
  onSelect,
}: {
  id: number;
  x?: SessionView;
  selected: boolean;
  onSelect: () => void;
}) {
  const { t } = useI18n();
  const { data: live } = useLive();
  const portId = x && gems(live).find((g) => g.gem.name === x.gemDev)?.gem.port_id;
  const service = x
    ? [portId != null ? `GEM ${portId}` : null, fibreVlan(x) ? `VLAN ${fibreVlan(x)}` : null]
        .filter(Boolean)
        .join(" · ")
    : null;
  const where = x
    ? [
        x.family !== "ipv6" && (x.handsOn ? t("internet.tileV4Host") : t("internet.tileV4Stick")),
        x.family !== "ipv4" && (x.prefixHost ? t("internet.tileV6Host") : "IPv6"),
      ]
        .filter(Boolean)
        .join(" · ")
    : null;
  return (
    <button
      type="button"
      onClick={onSelect}
      aria-pressed={selected}
      className={`flex min-h-24 flex-col gap-1 rounded-box border bg-base-100 p-3 text-left text-sm ${
        selected
          ? "border-2 border-primary"
          : x
            ? "border-base-300"
            : "border-dashed border-base-300"
      } hover:border-primary`}
    >
      <span className="flex flex-wrap items-center justify-between gap-1 font-semibold">
        <span className="whitespace-nowrap">{t("internet.slotN", { id })}</span>
        {x ? (
          <Badge tone={x.up ? "good" : "warn"}>
            {x.up ? t("internet.up") : t("internet.connecting")}
          </Badge>
        ) : (
          <Badge tone="neutral">{t("internet.slotFree")}</Badge>
        )}
      </span>
      {x ? (
        <>
          <span className="text-xs text-base-content/70">{service || t("internet.autoGem")}</span>
          <span className="text-xs">{where}</span>
        </>
      ) : (
        <span className="mt-auto flex items-center gap-1 text-xs text-base-content/60">
          <Plus className="size-3" />
          {t("internet.slotUse")}
        </span>
      )}
    </button>
  );
}

/* How the stick handles the internet now: the bridge alone, or its own sessions as well. */
function Ways({ count }: { count: number }) {
  const { t } = useI18n();
  const way = (active: boolean, icon: ReactNode, title: string, text: string) => (
    <div
      className={`flex gap-3 rounded-box border p-3 ${active ? "border-primary bg-primary/5" : "border-base-300 opacity-70"}`}
    >
      <span className={active ? "text-primary" : "text-base-content/50"}>{icon}</span>
      <span>
        <span className="flex items-center gap-2 text-sm font-semibold">
          {title}
          {active && <Badge tone="info">{t("internet.now")}</Badge>}
        </span>
        <span className="block text-xs text-base-content/70">{text}</span>
      </span>
    </div>
  );
  return (
    <div className="grid gap-3 md:grid-cols-2">
      {way(
        count === 0,
        <Network className="size-5" />,
        t("internet.wayBridge"),
        t("internet.wayBridgeText"),
      )}
      {way(
        count > 0,
        <Globe className="size-5" />,
        t("internet.waySession"),
        t("internet.waySessionText"),
      )}
    </div>
  );
}

function SessionsTab() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const list = sessions(live);
  /* The row looked at, and a session to change or the free row a new one takes. */
  const [picked, setPicked] = useState<number | null>(null);
  const [editing, setEditing] = useState<SessionView | number | null>(null);
  const shown = picked ?? list[0]?.id ?? 1;
  const x = list.find((y) => y.id === shown);

  return (
    <div className="grid min-h-0 flex-1 content-start gap-4 lg:grid-rows-[auto_auto_auto_minmax(0,1fr)]">
      <Ways count={list.length} />
      <div className="flex flex-wrap items-center justify-between gap-3">
        <Slots used={list.length} />
        <Link to="/fibre" search={{ tab: "services" } as never} className="link text-sm">
          {t("internet.seeServices")} <ArrowRight className="inline size-3" />
        </Link>
      </div>
      <div className="grid grid-cols-2 gap-3 sm:grid-cols-4 xl:grid-cols-7">
        {SLOT_IDS.map((id) => (
          <SlotTile
            key={id}
            id={id}
            x={list.find((y) => y.id === id)}
            selected={shown === id}
            onSelect={() => setPicked(id)}
          />
        ))}
      </div>
      <div className="min-h-0 lg:overflow-auto">
        {x ? (
          <div className="max-w-3xl">
            <SessionCard x={x} onEdit={() => setEditing(x)} />
          </div>
        ) : (
          <Card title={t("internet.slotN", { id: shown })}>
            <p className="text-sm text-base-content/70">{t("internet.slotFreeText")}</p>
            <div>
              <button
                type="button"
                className="btn btn-sm btn-primary"
                onClick={() => setEditing(shown)}
              >
                <Plus className="size-4" />
                {t("internet.slotUse")}
              </button>
            </div>
          </Card>
        )}
      </div>
      <Dialog
        open={editing != null}
        title={
          typeof editing === "number"
            ? t("internet.addTitle", { id: editing })
            : t("internet.editTitle", { id: editing?.id ?? 0 })
        }
        onClose={() => setEditing(null)}
      >
        <SessionForm
          x={typeof editing === "number" || editing == null ? undefined : editing}
          slot={typeof editing === "number" ? editing : undefined}
          onDone={() => setEditing(null)}
        />
      </Dialog>
    </div>
  );
}

/* ---------------------------------------------------------------- handover */

function HandoverForm({ link }: { link: LinkView }) {
  const { t } = useI18n();
  const change = useLinkChange();
  const state = useChangeState(change);
  const schema = z.object({ vlan: vlan(t), gateway: ipv4(t), peer: z.boolean() });
  type Values = z.infer<typeof schema>;
  const current = { vlan: link.vlan, gateway: link.gateway, peer: link.peer };
  const form = useForm<Values>({ resolver: zodResolver(schema), defaultValues: current });
  const { errors, isDirty } = form.formState;

  // biome-ignore lint/correctness/useExhaustiveDependencies: refill only when the stick's values change
  useEffect(() => {
    if (!form.formState.isDirty) form.reset(current);
  }, [link.vlan, link.gateway, link.peer]);

  return (
    <form
      onSubmit={form.handleSubmit((v) =>
        change.mutate(
          { VLAN: v.vlan, GATEWAY: v.gateway, PEER: v.peer ? "on" : "off" },
          { onSuccess: () => form.reset(v) },
        ),
      )}
      noValidate
      className="grid gap-4"
    >
      <div className="grid gap-4 sm:grid-cols-2">
        <Field
          label={t("sfp.vlan")}
          hint={t("sfp.vlanHint")}
          inputMode="numeric"
          error={errors.vlan?.message}
          {...form.register("vlan")}
        />
        <Field
          label={t("sfp.gateway")}
          hint={t("sfp.gatewayHint")}
          error={errors.gateway?.message}
          {...form.register("gateway")}
        />
      </div>
      <label className="label cursor-pointer justify-start gap-3 whitespace-normal">
        <input type="checkbox" className="toggle toggle-sm" {...form.register("peer")} />
        <span>
          <span className="text-sm">{t("sfp.peer")}</span>
          <span className="block text-xs text-base-content/60">{t("sfp.peerHint")}</span>
        </span>
      </label>
      <div className="flex flex-wrap items-center gap-3">
        <button type="submit" className="btn btn-primary" disabled={!isDirty || change.isPending}>
          {t("internet.save")}
        </button>
        {isDirty && (
          <button type="button" className="btn btn-ghost" onClick={() => form.reset(current)}>
            {t("form.discard")}
          </button>
        )}
        <SaveState {...state} />
      </div>
    </form>
  );
}

function HandoverTab() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const s = session(live);
  const link = hostLink(live);
  return (
    <div className="grid min-h-0 flex-1 content-start gap-4 lg:grid-cols-[minmax(0,1fr)_22rem]">
      <Card title={t("internet.handoverTitle")}>
        <p className="text-sm text-base-content/70">{t("internet.handoverHint")}</p>
        {!s.configured && (
          <div role="alert" className="alert alert-info alert-soft text-sm">
            {t("internet.handoverIdle")}
          </div>
        )}
        <HandoverForm link={link} />
      </Card>
      <Card title={t("internet.hostGets")}>
        <Facts
          rows={[
            [t("sfp.lease"), s.up && link.lease ? link.lease : t("sfp.noLease")],
            [t("sfp.hostGateway"), s.up && link.hostGateway ? link.hostGateway : "–"],
            [t("internet.hostPrefix"), s.prefix ?? "–"],
            [t("internet.dns"), s.dns.length ? s.dns.join(", ") : "–"],
            [t("internet.mtu"), "1500"],
          ]}
        />
        <p className="text-xs text-base-content/60">{t("internet.hostGetsHint")}</p>
      </Card>
    </div>
  );
}

export function Internet() {
  const { t } = useI18n();
  const [tab, setTab] = useTab(TABS);
  const { data: live } = useLive();
  const s = session(live);
  return (
    <Page
      title={t("internet.title")}
      tabs={
        <Tabs
          items={[
            {
              id: "sessions",
              label: t("internet.tabSessions"),
              badge: (
                <span className="badge badge-ghost badge-xs tabular">
                  {s.count}/{MAX_SESSIONS}
                </span>
              ),
            },
            { id: "handover", label: t("internet.tabHandover") },
          ]}
          value={tab}
          onChange={setTab}
          label={t("internet.title")}
        />
      }
      actions={
        <div className="hidden items-center gap-6 md:flex">
          <Stat label="IPv4" value={s.address ?? "–"} />
          <Stat label="IPv6" value={s.prefix ?? "–"} />
          <Badge tone={s.configured ? (s.up ? "good" : "warn") : "neutral"}>
            {s.configured
              ? s.up
                ? t("internet.up")
                : t("internet.connecting")
              : t("internet.off")}
          </Badge>
        </div>
      }
    >
      {tab === "sessions" ? <SessionsTab /> : <HandoverTab />}
    </Page>
  );
}
