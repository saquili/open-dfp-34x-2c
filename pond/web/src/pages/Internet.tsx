// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The internet. On top, the operator's services, as the OLT set them up;
 * under them the seven slots of the session engine. Without anything set
 * here the OLT makes the stick a transparent bridge, and the host dials the
 * operator through it. A slot takes one service: the stick holds its PPPoE
 * session itself, in its switch's hardware, and hands the public address to
 * the host, whose link is set with the slot that hands it on. A slot is
 * bound to the service, by the VID the operator names it with: when the OLT
 * moves the service to another VID on the fibre or another channel, the
 * session follows; the VLAN towards the host is the OLT's, never set here.
 * Whether a slot hands its address on or keeps it on the stick is, for now,
 * the order of the slots: the first that carries IPv4 hands its address on,
 * the first that carries IPv6 its prefix.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Link } from "@tanstack/react-router";
import { ArrowRight, Pencil, Plus, Trash2 } from "lucide-react";
import { type ReactNode, useEffect, useMemo, useRef, useState } from "react";
import { useForm } from "react-hook-form";
import { useLinkChange, useOmci, useSessionChange } from "../api/config";
import { useLive } from "../api/live";
import type { Family } from "../api/types";
import { Field, SaveState, useChangeState } from "../components/Field";
import { Page } from "../components/Page";
import { Badge, Card, Facts, Stat } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { Mib, type Service, services } from "../lib/omci";
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

/* The rows of the session engine, numbered as the sessions are. */
const SLOT_IDS = Array.from({ length: MAX_SESSIONS }, (_, i) => i + 1);

const FAMILY: Record<Family, Key> = {
  both: "internet.famBoth",
  ipv4: "internet.famV4",
  ipv6: "internet.famV6",
};

/* --------------------------------------------------------------- services */

/** The operator's services that can carry a session: not multicast alone. */
function useServices() {
  const omci = useOmci();
  const list = useMemo(
    () =>
      services(new Mib(omci.data)).filter((s) =>
        s.ports.some((p) => p.channels.some((c) => !c.multicast && c.direction !== "down")),
      ),
    [omci.data],
  );
  return { list, ready: omci.isSuccess };
}

/** The Port-IDs of the channels of service @s. */
function channelIds(s: Service): number[] {
  return [...new Set(s.ports.flatMap((p) => p.channels.map((c) => c.portId)))].sort(
    (a, z) => a - z,
  );
}

/*
 * The VID a session is given for service @s: the one by which the operator
 * names the service towards the host, which the OLT translates, else the VID
 * on the fibre; onu-mode follows it from there.
 */
function serviceVid(s: Service): string {
  return String(s.hostVlans.find((v) => v != null) ?? s.vlan ?? "");
}

/** The service a session runs in: the one whose VID on the fibre it runs in. */
function serviceOf(list: Service[], x: SessionView): Service | undefined {
  return list.find((s) => s.vlan != null && String(s.vlan) === fibreVlan(x));
}

/** What service @s is named by: its VID towards the host, and on the fibre (upstream). */
function serviceName(t: (k: Key, v?: Record<string, string | number>) => string, s: Service) {
  const host = s.hostVlans.filter((v) => v != null && v !== s.vlan);
  if (host.length)
    return t("internet.serviceVia", {
      host: host.join(", "),
      fibre: s.vlan ?? t("internet.serviceNoVlan"),
    });
  return s.vlan == null ? t("internet.serviceNoVlan") : `VLAN ${s.vlan}`;
}

/** The operator's services, each with the slots it carries. */
function ServicesStrip({ list, ready }: { list: Service[]; ready: boolean }) {
  const { t } = useI18n();
  const { data: live } = useLive();
  const all = sessions(live);
  const views = gems(live);
  return (
    <section className="grid gap-2">
      <div className="flex flex-wrap items-baseline justify-between gap-2">
        <h2 className="text-sm font-semibold">{t("internet.servicesTitle")}</h2>
        <Link to="/fibre" search={{ tab: "services" } as never} className="link text-xs">
          {t("internet.seeServices")} <ArrowRight className="inline size-3" />
        </Link>
      </div>
      {!ready || list.length === 0 ? (
        <p className="text-sm text-base-content/60">
          {ready ? t("internet.servicesNone") : t("internet.servicesLoading")}
        </p>
      ) : (
        <div className="grid grid-cols-[repeat(auto-fill,minmax(14rem,1fr))] gap-3">
          {list.map((s) => {
            const ids = channelIds(s);
            const slots = all.filter((x) => serviceOf(list, x) === s).map((x) => x.id);
            const bridged = views.some(
              (v) => v.gem.port_id != null && ids.includes(v.gem.port_id) && v.use === "bridged",
            );
            return (
              <div
                key={String(s.vlan ?? "none")}
                className={`grid gap-1 rounded-box border bg-base-100 p-3 text-sm ${slots.length ? "border-primary/60" : "border-base-300"}`}
              >
                <span className="flex flex-wrap items-center justify-between gap-1">
                  <span className="font-semibold">{serviceName(t, s)}</span>
                  {slots.length > 0 ? (
                    <Badge tone="info">
                      {t(slots.length === 1 ? "internet.serviceSlot" : "internet.serviceSlots", {
                        list: slots.join(", "),
                      })}
                    </Badge>
                  ) : (
                    <Badge tone={bridged ? "good" : "neutral"}>
                      {t(bridged ? "internet.serviceBridged" : "internet.serviceUnused")}
                    </Badge>
                  )}
                </span>
                <span className="text-xs text-base-content/70">
                  {ids.map((id) => `GEM ${id}`).join(" · ")}
                  {s.multicast ? ` · ${t("internet.serviceMulticast")}` : ""}
                </span>
              </div>
            );
          })}
        </div>
      )}
    </section>
  );
}

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

/* One answer of the form's: the one the stick gives, or one it cannot yet. */
function Choice({ title, text, on }: { title: string; text: ReactNode; on: boolean }) {
  return (
    <label
      className={`flex items-start gap-3 rounded-box border p-3 ${
        on ? "border-primary/50 bg-primary/5" : "border-base-300 opacity-60"
      }`}
    >
      <input
        type="radio"
        className="radio radio-sm radio-primary mt-0.5"
        checked={on}
        disabled={!on}
        readOnly
      />
      <span>
        <span className="block text-sm font-medium">{title}</span>
        <span className="block text-xs text-base-content/65">{text}</span>
      </span>
    </label>
  );
}

/*
 * What slot @id hands down with families @f, as onu-mode decides it: the
 * first session that carries IPv4 hands its address on, the first that
 * carries IPv6 its prefix; with the slots that already do, if any.
 */
function handing(all: SessionView[], id: number, f: Family) {
  const before = all.filter((y) => y.id < id).sort((a, z) => a.id - z.id);
  const v4By = f === "ipv6" ? undefined : before.find((y) => y.family !== "ipv6")?.id;
  const v6By = f === "ipv4" ? undefined : before.find((y) => y.family !== "ipv4")?.id;
  return { v4: f !== "ipv6" && v4By == null, v6: f !== "ipv4" && v6By == null, v4By, v6By };
}

/* What the host is given with families @v4 and @v6. */
function downWhat(v4: boolean, v6: boolean): Key {
  return v4 && v6 ? "internet.kindDownBoth" : v4 ? "internet.kindDownV4" : "internet.kindDownV6";
}

/** A slot's session and settings: a new one in @slot without @x, or @x changed. */
function SessionForm({ x, slot, onDone }: { x?: SessionView; slot?: number; onDone: () => void }) {
  const { t } = useI18n();
  const change = useSessionChange();
  const state = useChangeState(change);
  const all = sessions(useLive().data);
  const { list } = useServices();
  const bound = x ? serviceOf(list, x) : undefined;
  /* A session whose service the OLT no longer provisions keeps its VID. */
  const gone = x && !bound ? x.vlan : null;
  const schema = z.object({
    user: x ? upTo(63) : required(t("form.required")),
    secret: x ? z.string().check(z.maxLength(255)) : required(t("internet.secretRequired")),
    service: required(t("form.required")),
    mac: mac(t),
    family: z.enum(["both", "ipv4", "ipv6"]),
  });
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: {
      user: x?.user ?? "",
      secret: "",
      service: bound ? serviceVid(bound) : (gone ?? (list[0] ? serviceVid(list[0]) : "")),
      mac: x?.mac ?? "",
      family: x?.family ?? "both",
    },
  });
  const { errors, isDirty } = form.formState;
  const family = form.watch("family");
  const service = form.watch("service");
  const picked = list.find((s) => serviceVid(s) === service);
  const h = handing(all, x?.id ?? slot ?? 0, family);
  const down = h.v4 || h.v6;

  const send = (v: Values) =>
    change.mutate(
      {
        ID: x ? String(x.id) : slot ? String(slot) : undefined,
        USER: v.user,
        SECRET: v.secret || undefined,
        VLAN: v.service,
        /* The channel follows the service: onu-mode finds it. */
        GEM: "auto",
        MAC: v.mac,
        FAMILY: v.family,
      },
      { onSuccess: onDone },
    );

  return (
    <form onSubmit={form.handleSubmit(send)} noValidate className="grid gap-4">
      <div className="grid gap-1">
        <label htmlFor="service" className="text-sm font-medium">
          {t("internet.service")}
        </label>
        <select id="service" className="select w-full" {...form.register("service")}>
          {list.map((s) => (
            <option key={serviceVid(s)} value={serviceVid(s)}>
              {serviceName(t, s)}
            </option>
          ))}
          {gone != null && (
            <option value={gone}>
              {gone ? t("internet.serviceGone", { vid: gone }) : t("switch.untagged")}
            </option>
          )}
        </select>
        <p className="text-xs text-base-content/60">
          {picked
            ? t("internet.serviceFollows", { vid: serviceVid(picked) })
            : list.length === 0
              ? t("internet.servicesNone")
              : null}
        </p>
      </div>
      <fieldset className="grid gap-2">
        <legend className="mb-1 text-sm font-medium">{t("internet.kind")}</legend>
        <div className="grid gap-2 sm:grid-cols-2">
          <Choice on={!down} title={t("internet.kindStick")} text={t("internet.kindStickHint")} />
          <Choice
            on={down}
            title={t("internet.kindDown")}
            text={
              <>
                {t("internet.kindDownHint", {
                  what: t(
                    down ? downWhat(h.v4, h.v6) : downWhat(family !== "ipv6", family !== "ipv4"),
                  ),
                })}
                {h.v4By != null && (
                  <span className="block">{t("internet.kindTakenV4", { id: h.v4By })}</span>
                )}
                {h.v6By != null && (
                  <span className="block">{t("internet.kindTakenV6", { id: h.v6By })}</span>
                )}
              </>
            }
          />
        </div>
        <p className="text-xs text-base-content/60">{t("internet.kindByOrder")}</p>
      </fieldset>
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
          placeholder={x?.secretSet ? t("internet.passwordSaved") : t("internet.passwordNone")}
          hint={x?.secretSet ? t("internet.passwordKept") : undefined}
          error={errors.secret?.message}
          {...form.register("secret")}
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
      <Field
        label={t("internet.mac")}
        hint={t("internet.macHint")}
        error={errors.mac?.message}
        {...form.register("mac")}
      />
      <div className="flex flex-wrap items-center gap-3">
        <button
          type="submit"
          className="btn btn-primary"
          disabled={(x && !isDirty) || change.isPending || !service}
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
      <span className="text-base-content/70">
        {t("internet.slotsHint")} {t("internet.restBridge")}
      </span>
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

function SessionsTab() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const list = sessions(live);
  const services = useServices();
  /* The row looked at, and a session to change or the free row a new one takes. */
  const [picked, setPicked] = useState<number | null>(null);
  const [editing, setEditing] = useState<SessionView | number | null>(null);
  const shown = picked ?? list[0]?.id ?? 1;
  const x = list.find((y) => y.id === shown);

  return (
    <div className="grid min-h-0 flex-1 content-start gap-4 lg:grid-rows-[auto_auto_auto_minmax(0,1fr)]">
      <ServicesStrip list={services.list} ready={services.ready} />
      <Slots used={list.length} />
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
          <div className="grid items-start gap-4 xl:grid-cols-[minmax(0,1fr)_minmax(0,1fr)]">
            <SessionCard x={x} onEdit={() => setEditing(x)} />
            {x.handsOn && <HostCard />}
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

/** How the host gets the address the slot hands on, and what it gets. */
function HostCard() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const s = session(live);
  const link = hostLink(live);
  return (
    <Card title={t("internet.handoverTitle")}>
      <p className="text-sm text-base-content/70">{t("internet.handoverHint")}</p>
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
      <HandoverForm link={link} />
    </Card>
  );
}

export function Internet() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const s = session(live);
  return (
    <Page
      title={t("internet.title")}
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
      <SessionsTab />
    </Page>
  );
}
