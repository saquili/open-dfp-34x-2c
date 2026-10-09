// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The form beside the connections: a connection added, or one changed. A tab
 * for each kind, and in each the VLAN first, as every connection runs in one
 * of the operator's.
 *
 * PPPoE: the stick holds the session and is the router of the devices behind
 * it, or hands the public address down to the host, in the service's own
 * VLAN unless the session names another, or none; in one VLAN towards the
 * host one session at most hands its IPv4 address down, and one its IPv6
 * prefix. Static IP and DHCP: the stick keeps the address and is the router;
 * a VLAN has one such connection at most.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { X } from "lucide-react";
import { type KeyboardEvent, useEffect, useMemo, useRef, useState } from "react";
import { Controller, type FieldValues, type UseFormReturn, useForm } from "react-hook-form";
import { useIpoeChange, useSessionChange } from "../../api/config";
import { useLive } from "../../api/live";
import type { Family } from "../../api/types";
import { Field, SaveState, useChangeState } from "../../components/Field";
import { type Key, useI18n } from "../../i18n";
import {
  addresses,
  ipv4,
  ipv4Cidr,
  ipv6,
  ipv6Cidr,
  mac,
  required,
  upTo,
  vlan,
  z,
} from "../../lib/schemas";
import { downVlan, type IpoeView, MAX_CONNS, type SessionView, sessions } from "../../lib/state";
import { type Conn, ipoeIn, KINDS, type Kind, room, serviceIn } from "./conns";
import { Choice, connName, FamilyField, hostSide, serviceName, useServices } from "./shared";
import { VlanField, type VlanOption, type VlanUse, vlanOptions } from "./VlanField";

/** What a new connection keeps when its kind is changed: its VLAN, MAC address and families. */
interface Draft {
  vlan: string;
  mac: string;
  family: Family;
}

const EMPTY: Draft = { vlan: "", mac: "", family: "both" };

/* The tabs' words. */
const TAB: Record<Kind, Key> = {
  pppoe: "internet.tabPppoe",
  static: "internet.tabStatic",
  dhcp: "internet.tabDhcp",
};

interface Common {
  list: Conn[];
  options: VlanOption[];
  known: boolean;
  draft: Draft;
  onDraft: (d: Draft) => void;
  onDone: () => void;
  onCancel?: () => void;
}

/* Reports what a new connection's form holds, so that another tab starts from it. */
function useDraft<V extends FieldValues>(
  form: UseFormReturn<V>,
  onDraft: (d: Draft) => void,
  on: boolean,
) {
  useEffect(() => {
    if (!on) return;
    const sub = form.watch((v) => {
      const d = v as unknown as Partial<Draft>;
      onDraft({ vlan: d.vlan ?? "", mac: d.mac ?? "", family: d.family ?? "both" });
    });
    return () => sub.unsubscribe();
  }, [form, onDraft, on]);
}

/** What a VLAN carries already, as the list of a connection of @kind shows it. */
function useVlanUse(list: Conn[], kind: Kind, self?: string): (vid: string) => VlanUse {
  const { t } = useI18n();
  return (vid) => {
    const here = list.filter((c) => c.vlan === vid && c.key !== self);
    const other = here.find((c) => c.kind !== "pppoe");
    if (kind !== "pppoe" && other)
      return { text: t("internet.vlanTaken", { name: connName(t, other) }), closed: true };
    const ids = here.filter((c) => c.kind === "pppoe").map((c) => c.x.id);
    const names = [
      ...(ids.length ? [`PPPoE ${ids.join(", ")}`] : []),
      ...(other ? [connName(t, other)] : []),
    ];
    return names.length
      ? { text: t("internet.vlanHas", { list: names.join(t("internet.and")) }), closed: false }
      : null;
  };
}

/*
 * The sessions other than @id that already hand down into VLAN @down what
 * families @f would: in one VLAN one session at most hands IPv4, and one IPv6.
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

/** The form's last row: the button that saves, and the one that gives up a change. */
function Buttons({
  editing,
  disabled,
  state,
  onCancel,
}: {
  editing: boolean;
  disabled: boolean;
  state: Parameters<typeof SaveState>[0];
  onCancel?: () => void;
}) {
  const { t } = useI18n();
  return (
    <div className="flex min-h-16 shrink-0 flex-wrap items-center gap-3 border-t border-base-300 px-4 py-3">
      <button type="submit" className="btn btn-primary" disabled={disabled}>
        {editing ? t("internet.save") : t("internet.addConn")}
      </button>
      {editing && (
        <button type="button" className="btn btn-ghost" onClick={onCancel}>
          {t("internet.cancel")}
        </button>
      )}
      <SaveState {...state} />
    </div>
  );
}

/** A PPPoE session: a new one without @x, or @x changed. */
function PppoeForm({ x, ...c }: Common & { x?: SessionView }) {
  const { t } = useI18n();
  const change = useSessionChange();
  const state = useChangeState(change);
  const all = sessions(useLive().data);
  const { list: services } = useServices();
  const id = x?.id ?? 0;
  const first = x?.vlan ?? c.draft.vlan;
  /* A new session hands its address down unless its VLAN has one that does. */
  const free = (v: string) => Object.values(clashes(all, id, v, "both")).every((y) => y == null);
  const schema = z.object({
    vlan: vlan(t).check(z.minLength(1, t("internet.vlanRequired"))),
    user: x ? upTo(63) : required(t("form.required")),
    secret: x ? z.string().check(z.maxLength(255)) : required(t("internet.secretRequired")),
    pass: z.enum(["on", "off"]),
    hostVlan: vlan(t),
    mac: mac(t),
    family: z.enum(["both", "ipv4", "ipv6"]),
  });
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: {
      vlan: first,
      user: x?.user ?? "",
      secret: "",
      pass: (x ? x.pass : free(first)) ? "on" : "off",
      hostVlan: x ? downVlan(x) : first,
      mac: x?.mac ?? c.draft.mac,
      family: x?.family ?? c.draft.family,
    },
  });
  useDraft(form, c.onDraft, !x);
  const { errors, isDirty } = form.formState;
  const [family, service, pass, down] = form.watch(["family", "vlan", "pass", "hostVlan"]);
  const use = useVlanUse(c.list, "pppoe", x ? `pppoe:${x.id}` : undefined);
  const picked = serviceIn(services, service);
  /* What stands in the way of the address in the VLAN it goes down in. */
  const clash = pass === "on" ? clashes(all, id, down, family) : { v4: undefined, v6: undefined };
  const foreign =
    pass === "on" && down
      ? services.find((s) => s !== picked && hostSide(s).includes(Number(down)))
      : undefined;
  const blocked = clash.v4 != null || clash.v6 != null || foreign != null;

  /* What the session does with its address, once chosen by hand, stays as chosen. */
  const chosen = useRef(!!x);
  const passField = form.register("pass", {
    onChange: () => {
      chosen.current = true;
    },
  });
  /* The VLAN to the host follows the service's while it is the same; a new session's choice too. */
  const onVlan = (v: string) => {
    const was = form.getValues("vlan");
    if (form.getValues("hostVlan") === was) form.setValue("hostVlan", v, { shouldDirty: true });
    if (!chosen.current) form.setValue("pass", free(v) ? "on" : "off");
  };

  const send = (v: Values) =>
    change.mutate(
      {
        ID: x ? String(x.id) : undefined,
        USER: v.user,
        SECRET: v.secret || undefined,
        VLAN: v.vlan,
        /* The channel follows the service: onu-mode finds it. */
        GEM: "auto",
        MAC: v.mac,
        FAMILY: v.family,
        PASS: v.pass,
        /* The service's own VLAN is kept as nothing, to follow the service. */
        HOST_VLAN: !v.hostVlan ? "untagged" : v.hostVlan === v.vlan ? "" : v.hostVlan,
      },
      { onSuccess: c.onDone },
    );

  return (
    <form onSubmit={form.handleSubmit(send)} noValidate className="flex min-h-0 flex-1 flex-col">
      {/* The fields scroll between the tabs and the buttons, which stay. */}
      <div className="grid min-h-0 flex-1 content-start gap-4 p-4 xl:overflow-auto">
        <Controller
          control={form.control}
          name="vlan"
          render={({ field, fieldState }) => (
            <VlanField
              value={field.value}
              onChange={(v) => {
                onVlan(v);
                field.onChange(v);
              }}
              onBlur={field.onBlur}
              options={c.options}
              use={use}
              known={c.known}
              error={fieldState.error?.message}
            />
          )}
        />
        <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-1">
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
        <FamilyField field={form.register("family")} hint={t("internet.familyHint")} />
        <fieldset className="grid gap-2">
          <legend className="mb-1 text-sm font-semibold">{t("internet.kind")}</legend>
          <Choice
            value="off"
            on={pass === "off"}
            field={passField}
            title={t("internet.kindStick")}
            text={t("internet.kindStickHint")}
          />
          <Choice
            value="on"
            on={pass === "on"}
            field={passField}
            title={t("internet.kindDown")}
            text={t("internet.kindDownHint", { what: t(downWhat(family)) })}
          />
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
        <Field
          label={t("internet.mac")}
          hint={t("internet.macHint")}
          error={errors.mac?.message}
          {...form.register("mac")}
        />
      </div>
      <Buttons
        editing={!!x}
        disabled={(!!x && !isDirty) || change.isPending || blocked}
        state={state}
        onCancel={c.onCancel}
      />
    </form>
  );
}

/** A connection whose address is typed in (static) or comes from DHCP: a new one, or @x changed. */
function IpoeForm({ kind, x, ...c }: Common & { kind: "static" | "dhcp"; x?: IpoeView }) {
  const { t } = useI18n();
  const change = useIpoeChange();
  const state = useChangeState(change);
  const self = x ? `ipoe:${x.id}` : undefined;
  const fixed = kind === "static";
  const schema = z.object({
    vlan: vlan(t).check(z.minLength(1, t("internet.vlanRequired"))),
    family: z.enum(["both", "ipv4", "ipv6"]),
    address: fixed ? ipv4Cidr(t).check(z.minLength(1, t("form.required"))) : z.string(),
    gateway: fixed ? ipv4(t).check(z.minLength(1, t("form.required"))) : z.string(),
    dns: addresses(t),
    address6: ipv6Cidr(t),
    gateway6: ipv6(t),
    prefix6: ipv6Cidr(t),
    mac: mac(t),
  });
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: {
      vlan: x?.vlan ?? c.draft.vlan,
      family: x?.family ?? (fixed ? "ipv4" : c.draft.family),
      address: x?.address ?? "",
      gateway: x?.gateway ?? "",
      dns: x?.dnsSet ?? "",
      address6: x?.address6 ?? "",
      gateway6: x?.gateway6 ?? "",
      prefix6: x?.prefix6 ?? "",
      mac: x?.mac ?? c.draft.mac,
    },
  });
  useDraft(form, c.onDraft, !x);
  const { errors, isDirty } = form.formState;
  const service = form.watch("vlan");
  const use = useVlanUse(c.list, kind, self);
  /* A VLAN has one connection that is not PPPoE at most. */
  const taken = service ? ipoeIn(c.list, service, self) : undefined;

  const send = (v: Values) => {
    /* A static connection has the families it is given addresses of. */
    const family = fixed ? (v.address6 ? "both" : "ipv4") : v.family;
    change.mutate(
      {
        ID: x ? String(x.id) : undefined,
        KIND: kind,
        VLAN: v.vlan,
        GEM: "auto",
        MAC: v.mac,
        FAMILY: family,
        ...(fixed
          ? {
              ADDRESS: v.address,
              GATEWAY: v.gateway,
              DNS: v.dns
                .split(/[\s,]+/)
                .filter(Boolean)
                .join(" "),
              ADDRESS6: v.address6,
              GATEWAY6: v.gateway6,
              PREFIX6: v.prefix6,
            }
          : {}),
      },
      { onSuccess: c.onDone },
    );
  };

  return (
    <form onSubmit={form.handleSubmit(send)} noValidate className="flex min-h-0 flex-1 flex-col">
      {/* The fields scroll between the tabs and the buttons, which stay. */}
      <div className="grid min-h-0 flex-1 content-start gap-4 p-4 xl:overflow-auto">
        <Controller
          control={form.control}
          name="vlan"
          render={({ field, fieldState }) => (
            <VlanField
              value={field.value}
              onChange={field.onChange}
              onBlur={field.onBlur}
              options={c.options}
              use={use}
              known={c.known}
              error={
                fieldState.error?.message ??
                (taken
                  ? t("internet.ipoeTaken", { vid: service, name: connName(t, taken) })
                  : undefined)
              }
            />
          )}
        />
        {fixed ? (
          <>
            <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-1">
              <Field
                label={t("internet.address4")}
                placeholder="192.0.2.10/24"
                error={errors.address?.message}
                {...form.register("address")}
              />
              <Field
                label={t("internet.gateway4")}
                placeholder="192.0.2.1"
                error={errors.gateway?.message}
                {...form.register("gateway")}
              />
            </div>
            <Field
              label={t("internet.dnsServers")}
              placeholder="198.51.100.53 198.51.100.54"
              hint={t("internet.dnsHint")}
              error={errors.dns?.message}
              {...form.register("dns")}
            />
            <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-1">
              <Field
                label={t("internet.address6")}
                placeholder="2001:db8::2/64"
                hint={t("internet.address6Hint")}
                error={errors.address6?.message}
                {...form.register("address6")}
              />
              <Field
                label={t("internet.gateway6")}
                placeholder="2001:db8::1"
                error={errors.gateway6?.message}
                {...form.register("gateway6")}
              />
            </div>
            <Field
              label={t("internet.prefix6")}
              placeholder="2001:db8:1200::/56"
              hint={t("internet.prefix6Hint")}
              error={errors.prefix6?.message}
              {...form.register("prefix6")}
            />
          </>
        ) : (
          <FamilyField field={form.register("family")} hint={t("internet.dhcpFamilyHint")} />
        )}
        <Field
          label={t("internet.mac")}
          hint={t("internet.macHint")}
          error={errors.mac?.message}
          {...form.register("mac")}
        />
        <p className="text-xs text-base-content/70">{t("internet.kindStickHint")}</p>
      </div>
      <Buttons
        editing={!!x}
        disabled={(!!x && !isDirty) || change.isPending || !!taken}
        state={state}
        onCancel={c.onCancel}
      />
    </form>
  );
}

/* The kinds as tabs; the one of a connection being changed alone, the others off. */
function KindTabs({
  value,
  onChange,
  only,
}: {
  value: Kind;
  onChange: (k: Kind) => void;
  only?: Kind;
}) {
  const { t } = useI18n();
  const ref = useRef<HTMLDivElement>(null);
  const move = (e: KeyboardEvent<HTMLDivElement>) => {
    if (only || (e.key !== "ArrowRight" && e.key !== "ArrowLeft")) return;
    const next = KINDS[(KINDS.indexOf(value) + (e.key === "ArrowRight" ? 1 : 2)) % KINDS.length];
    if (!next) return;
    onChange(next);
    ref.current?.querySelector<HTMLButtonElement>(`[data-kind="${next}"]`)?.focus();
  };
  return (
    <div
      ref={ref}
      role="tablist"
      aria-label={t("internet.kindLabel")}
      className="flex shrink-0 gap-6 border-b border-base-300 px-4"
      onKeyDown={move}
    >
      {KINDS.map((k) => {
        const on = k === value;
        return (
          <button
            key={k}
            type="button"
            role="tab"
            data-kind={k}
            aria-selected={on}
            tabIndex={on ? 0 : -1}
            disabled={only != null && only !== k}
            onClick={() => onChange(k)}
            className={`-mb-px grid border-b-2 pt-2.5 pb-3 text-sm ${
              on
                ? "border-primary text-primary"
                : "border-transparent text-base-content/70 enabled:hover:text-base-content disabled:text-base-content/45"
            }`}
          >
            {/* The label set bold, unseen, holds the tab's width whether or not it is chosen. */}
            <span className={`col-start-1 row-start-1 ${on ? "font-semibold" : ""}`}>
              {t(TAB[k])}
            </span>
            <span aria-hidden="true" className="invisible col-start-1 row-start-1 font-semibold">
              {t(TAB[k])}
            </span>
          </button>
        );
      })}
    </div>
  );
}

/**
 * The form: a new connection of @kind, or @editing changed; @onClose shuts
 * it where it can be shut, on a narrow screen.
 */
export function ConnForm({
  list,
  kind,
  onKind,
  editing,
  onEditDone,
  onClose,
}: {
  list: Conn[];
  kind: Kind;
  onKind: (k: Kind) => void;
  editing: Conn | null;
  onEditDone: () => void;
  onClose: () => void;
}) {
  const { t } = useI18n();
  const { list: services, ready } = useServices();
  const options = useMemo(() => vlanOptions(services), [services]);
  const [draft, setDraft] = useState<Draft>(EMPTY);
  /* A new form after each connection added. */
  const [round, setRound] = useState(0);
  const k = editing?.kind ?? kind;
  const full = !editing && !room(list);
  const common: Common = {
    list,
    options,
    known: !ready || options.length > 0,
    draft,
    onDraft: setDraft,
    onDone: editing
      ? onEditDone
      : () => {
          setDraft(EMPTY);
          setRound((r) => r + 1);
        },
    onCancel: onEditDone,
  };
  const key = `${editing?.key ?? "new"}:${k}:${round}`;

  return (
    <section
      className="flex flex-col rounded-box border border-base-300 bg-base-100 xl:h-full xl:min-h-0"
      aria-labelledby="conn-form"
    >
      <div className="flex min-h-14 shrink-0 items-center gap-2 px-4 pt-4 pb-2">
        <h2 id="conn-form" className="text-base font-semibold">
          {editing
            ? t("internet.editConn", { name: connName(t, editing), vid: editing.vlan })
            : t("internet.addConn")}
        </h2>
        <button
          type="button"
          className={`btn btn-ghost btn-sm btn-square ml-auto text-base-content/70 ${editing ? "" : "xl:hidden"}`}
          onClick={editing ? onEditDone : onClose}
          aria-label={t("internet.cancel")}
          title={t("internet.cancel")}
        >
          <X className="size-4" />
        </button>
      </div>
      <KindTabs value={k} onChange={onKind} only={editing?.kind} />
      {full ? (
        <div className="m-4 rounded-box border border-dashed border-base-300 p-6 text-center">
          <p className="font-semibold">{t("internet.full", { max: MAX_CONNS })}</p>
          <p className="mt-1 text-xs text-base-content/70">{t("internet.fullHint")}</p>
        </div>
      ) : k === "pppoe" ? (
        <PppoeForm key={key} x={editing?.kind === "pppoe" ? editing.x : undefined} {...common} />
      ) : (
        <IpoeForm
          key={key}
          kind={k}
          x={editing && editing.kind !== "pppoe" ? editing.x : undefined}
          {...common}
        />
      )}
    </section>
  );
}
