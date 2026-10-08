// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The internet: the seven slots of the session engine, one under another,
 * each with all there is to know of its session. Without anything set
 * here the OLT makes the stick a transparent bridge, and the host dials the
 * operator through it. A slot takes one service: the stick holds its PPPoE
 * session itself, in its switch's hardware, and hands the public address to
 * the host or keeps it. A slot is bound to the service, by the VID the
 * operator names it with: when the OLT moves the service to another VID on
 * the fibre or another channel, the session follows. The address goes down
 * to the host in that same VID, unless the slot names another, or none; in
 * one VLAN towards the host one slot at most hands its IPv4 address down,
 * and one its IPv6 prefix.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Pencil, Plus, Trash2 } from "lucide-react";
import { type ReactNode, useEffect, useMemo, useRef, useState } from "react";
import { type UseFormRegisterReturn, useForm } from "react-hook-form";
import { useNetwork, useOmci, useSessionChange } from "../api/config";
import { useLive } from "../api/live";
import type { Family } from "../api/types";
import { Field, SaveState, useChangeState } from "../components/Field";
import { Page } from "../components/Page";
import { Badge, Facts } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { Mib, type Service, services } from "../lib/omci";
import { mac, required, upTo, vlan, z } from "../lib/schemas";
import {
  downVlan,
  fibreVlan,
  gems,
  hostLink,
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

/* One answer of the form's: a radio of @field with @value. */
function Choice({
  title,
  text,
  on,
  field,
  value,
}: {
  title: string;
  text: string;
  on: boolean;
  field: UseFormRegisterReturn;
  value: string;
}) {
  return (
    <label
      className={`flex cursor-pointer items-start gap-3 rounded-box border p-3 ${
        on ? "border-primary/50 bg-primary/5" : "border-base-300"
      }`}
    >
      <input
        type="radio"
        value={value}
        className="radio radio-sm radio-primary mt-0.5"
        {...field}
      />
      <span>
        <span className="block text-sm font-medium">{title}</span>
        <span className="block text-xs text-base-content/65">{text}</span>
      </span>
    </label>
  );
}

/* The VIDs service @s comes in on the host's side. */
function hostSide(s: Service): number[] {
  const host = s.hostVlans.filter((v): v is number => v != null);
  return host.length ? host : s.vlan != null ? [s.vlan] : [];
}

/*
 * The slots other than @id that already hand down into VLAN @down what
 * families @f would: in one VLAN one slot at most hands IPv4, and one IPv6.
 */
function clashes(all: SessionView[], id: number, down: string, f: Family) {
  const there = all.filter((y) => y.id !== id && y.pass && downVlan(y) === down);
  return {
    v4: f === "ipv6" ? undefined : there.find((y) => y.family !== "ipv6")?.id,
    v6: f === "ipv4" ? undefined : there.find((y) => y.family !== "ipv4")?.id,
  };
}

/* What the host is given with families @f. */
function downWhat(f: Family): Key {
  return f === "both"
    ? "internet.kindDownBoth"
    : f === "ipv4"
      ? "internet.kindDownV4"
      : "internet.kindDownV6";
}

/** A slot's session and settings: a new one in @slot without @x, or @x changed. */
function SessionForm({ x, slot, onDone }: { x?: SessionView; slot?: number; onDone: () => void }) {
  const { t } = useI18n();
  const change = useSessionChange();
  const state = useChangeState(change);
  const all = sessions(useLive().data);
  const { list } = useServices();
  const id = x?.id ?? slot ?? 0;
  const bound = x ? serviceOf(list, x) : undefined;
  /* A session whose service the OLT no longer provisions keeps its VID. */
  const gone = x && !bound ? x.vlan : null;
  const first = bound ? serviceVid(bound) : (gone ?? (list[0] ? serviceVid(list[0]) : ""));
  /* A new slot hands its address down unless its service's VLAN has one that does. */
  const free = Object.values(clashes(all, id, first, "both")).every((v) => v == null);
  const schema = z.object({
    user: x ? upTo(63) : required(t("form.required")),
    secret: x ? z.string().check(z.maxLength(255)) : required(t("internet.secretRequired")),
    service: required(t("form.required")),
    pass: z.enum(["on", "off"]),
    hostVlan: vlan(t),
    mac: mac(t),
    family: z.enum(["both", "ipv4", "ipv6"]),
  });
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: {
      user: x?.user ?? "",
      secret: "",
      service: first,
      pass: (x ? x.pass : free) ? "on" : "off",
      hostVlan: x ? downVlan(x) : first,
      mac: x?.mac ?? "",
      family: x?.family ?? "both",
    },
  });
  const { errors, isDirty } = form.formState;
  const [family, service, pass, down] = form.watch(["family", "service", "pass", "hostVlan"]);
  /* The VLAN to the host follows the service while it is the service's own. */
  const prevService = useRef(first);
  const picked = list.find((s) => serviceVid(s) === service);
  /* What stands in the way of the address in the VLAN it goes down in. */
  const clash = pass === "on" ? clashes(all, id, down, family) : { v4: undefined, v6: undefined };
  const foreign =
    pass === "on" && down
      ? list.find((s) => s !== picked && hostSide(s).includes(Number(down)))
      : undefined;
  const blocked = clash.v4 != null || clash.v6 != null || foreign != null;

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
        PASS: v.pass,
        /* The service's own VLAN is kept as nothing, to follow the service. */
        HOST_VLAN: !v.hostVlan ? "untagged" : v.hostVlan === v.service ? "" : v.hostVlan,
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
          placeholder={x?.secretSet ? t("internet.passwordSaved") : t("internet.passwordNone")}
          hint={x?.secretSet ? t("internet.passwordKept") : undefined}
          error={errors.secret?.message}
          {...form.register("secret")}
        />
      </div>
      <div className="grid gap-1">
        <label htmlFor="service" className="text-sm font-medium">
          {t("internet.service")}
        </label>
        <select
          id="service"
          className="select w-full"
          {...form.register("service", {
            onChange: (e) => {
              if (form.getValues("hostVlan") === prevService.current)
                form.setValue("hostVlan", e.target.value, { shouldDirty: true });
              prevService.current = e.target.value;
            },
          })}
        >
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
      <fieldset className="grid gap-2">
        <legend className="mb-1 text-sm font-medium">{t("internet.kind")}</legend>
        <div className="grid gap-2 sm:grid-cols-2">
          <Choice
            value="off"
            on={pass === "off"}
            field={form.register("pass")}
            title={t("internet.kindStick")}
            text={t("internet.kindStickHint")}
          />
          <Choice
            value="on"
            on={pass === "on"}
            field={form.register("pass")}
            title={t("internet.kindDown")}
            text={t("internet.kindDownHint", { what: t(downWhat(family)) })}
          />
        </div>
        {pass === "on" && (
          <div className="grid gap-2 pt-1">
            <Field
              label={t("internet.hostVlan")}
              inputMode="numeric"
              placeholder={t("switch.untagged")}
              hint={t("internet.hostVlanHint")}
              error={errors.hostVlan?.message}
              {...form.register("hostVlan")}
            />
            {clash.v4 != null && (
              <p className="text-xs text-error">{t("internet.clashV4", { id: clash.v4 })}</p>
            )}
            {clash.v6 != null && (
              <p className="text-xs text-error">{t("internet.clashV6", { id: clash.v6 })}</p>
            )}
            {foreign && (
              <p className="text-xs text-error">
                {t("internet.clashService", { vid: down, name: serviceName(t, foreign) })}
              </p>
            )}
          </div>
        )}
      </fieldset>
      <div className="flex flex-wrap items-center gap-3">
        <button
          type="submit"
          className="btn btn-primary"
          disabled={(x && !isDirty) || change.isPending || !service || blocked}
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

/** A group of a slot's facts, under a small heading. */
function Group({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="grid min-w-0 content-start gap-2">
      <h3 className="text-xs font-semibold uppercase tracking-wide text-base-content/50">
        {title}
      </h3>
      {children}
    </div>
  );
}

/** A slot with its session: all there is to know of it at once. */
function SlotCard({ x, onEdit }: { x: SessionView; onEdit: () => void }) {
  const { t } = useI18n();
  const { data: live } = useLive();
  const net = useNetwork();
  const { list } = useServices();
  const change = useSessionChange();
  const state = useChangeState(change);
  const s = session(live);
  const link = hostLink(live);
  const bound = serviceOf(list, x);
  const portId = gems(live).find((g) => g.gem.name === x.gemDev)?.gem.port_id;
  /* Sessions in one VLAN send from the MAC address of the first of them. */
  const first = sessions(live).find((y) => y.vlan === x.vlan && y.gem === x.gem);
  const macText =
    first && first.id !== x.id
      ? `${first.mac || t("internet.macStick")} (${t("internet.macShared", { id: first.id })})`
      : x.mac || t("internet.macStick");
  /* The MTU the session came to, which the host is given: 1500 when the concentrator took RFC 4638. */
  const mtu = (x.up && net.data?.links.find((l) => l.name === x.ppp)?.mtu) || null;
  const v4 = x.family !== "ipv6";
  const v6 = x.family !== "ipv4";
  /* The session's own, else, from a stick of before, the one handed on. */
  const dns = x.dns.length ? x.dns : x.handsOn ? s.dns : [];
  const gateway = x.up ? (x.gateway ?? (x.handsOn ? link.hostGateway : null)) : null;

  const remove = () => {
    if (!window.confirm(t("internet.removeSessionConfirm", { id: x.id }))) return;
    change.mutate({ ID: String(x.id), USER: "" });
  };

  return (
    <section className="rounded-box border border-base-300 bg-base-100 p-4">
      <div className="flex flex-wrap items-center gap-x-3 gap-y-1">
        <h2 className="text-base font-semibold">{t("internet.slotN", { id: x.id })}</h2>
        <Badge tone={x.up ? "good" : "warn"}>
          {x.up ? t("internet.up") : t("internet.connecting")}
        </Badge>
        <span className="text-sm font-medium">
          {bound ? serviceName(t, bound) : x.vlan ? `VLAN ${x.vlan}` : t("switch.untagged")}
        </span>
        <Badge tone="neutral">{t(FAMILY[x.family])}</Badge>
        <span className="ml-auto flex items-center gap-1">
          <SaveState {...state} />
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
      </div>
      <div className="mt-3 grid gap-x-8 gap-y-4 md:grid-cols-3">
        <Group title={t("internet.colSession")}>
          <Facts
            rows={[
              [
                t("internet.account"),
                <span key="u" className="break-all">
                  {x.user}
                </span>,
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
                t("internet.macShort"),
                <span key="m" className="font-mono text-xs">
                  {macText}
                </span>,
              ],
              x.dev ? [t("internet.iface"), [x.dev, x.ppp].filter(Boolean).join(" → ")] : null,
            ]}
          />
        </Group>
        <Group title={t("internet.colOperator")}>
          <Facts
            rows={[
              v4 && ["IPv4", x.ipv4 ?? "–"],
              v6 && [t("internet.prefix"), x.prefix ?? (x.ipv6 ? t("internet.prefixNone") : "–")],
              v4 && [t("internet.dns"), dns.length ? dns.join(", ") : "–"],
              [
                t("internet.mtu"),
                mtu ? t(mtu >= 1500 ? "internet.mtuRfc" : "internet.mtuPlain", { mtu }) : "–",
              ],
            ]}
          />
        </Group>
        <Group title={t("internet.colHost")}>
          {x.pass ? (
            <Facts
              rows={[
                ["VLAN", downVlan(x) || t("switch.untagged")],
                v4 && [t("internet.hostAddress"), x.handsOn && x.ipv4 ? x.ipv4 : "–"],
                v4 && [t("internet.hostGateway"), x.handsOn && gateway ? gateway : "–"],
                v6 && [
                  t("internet.prefix"),
                  !x.prefix
                    ? "–"
                    : x.prefixHost
                      ? x.prefix
                      : t("internet.prefixWaiting", { prefix: x.prefix }),
                ],
              ]}
            />
          ) : (
            <p className="text-sm text-base-content/70">{t("internet.kindStickHint")}</p>
          )}
        </Group>
      </div>
    </section>
  );
}

/** A free slot: one line, and a way to take it. */
function FreeSlot({ id, onUse }: { id: number; onUse: () => void }) {
  const { t } = useI18n();
  return (
    <section className="flex flex-wrap items-center gap-3 rounded-box border border-dashed border-base-300 bg-base-100 px-4 py-2">
      <h2 className="text-base font-semibold">{t("internet.slotN", { id })}</h2>
      <Badge tone="neutral">{t("internet.slotFree")}</Badge>
      <button type="button" className="btn btn-ghost btn-sm ml-auto" onClick={onUse}>
        <Plus className="size-4" />
        {t("internet.slotUse")}
      </button>
    </section>
  );
}

export function Internet() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const list = sessions(live);
  /* A session to change, or the free slot a new one takes. */
  const [editing, setEditing] = useState<SessionView | number | null>(null);
  return (
    <Page
      title={t("internet.title")}
      hint={t("internet.slots", { n: list.length, max: MAX_SESSIONS })}
    >
      <div className="grid min-h-0 flex-1 content-start gap-3 lg:overflow-auto">
        {SLOT_IDS.map((id) => {
          const x = list.find((y) => y.id === id);
          return x ? (
            <SlotCard key={id} x={x} onEdit={() => setEditing(x)} />
          ) : (
            <FreeSlot key={id} id={id} onUse={() => setEditing(id)} />
          );
        })}
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
    </Page>
  );
}
