// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The firewall, a tab of the internet page. Over the list, what it does where
 * no rule decides: from the internet in it blocks, but for NAT's forwards and
 * DMZ; from the local networks to the internet it lets through; between the
 * networks it blocks, so that guests do not reach the house; the last two
 * can be changed. The rules are exceptions, in order: the first one traffic
 * matches decides, and the arrows by a rule's number move it. A rule is
 * where traffic comes from and goes to (the internet or a part of it, every
 * local network, one, or a device of one), its protocol and ports, IPv4,
 * IPv6 or both, and whether it lets it through or blocks it. The forms come
 * in at the list's right (components/SidePanel).
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { ChevronDown, ChevronUp, Pencil, Plus, Trash2 } from "lucide-react";
import { useRef, useState } from "react";
import { type UseFormRegisterReturn, useForm } from "react-hook-form";
import { useFwChange, useRuleChange } from "../api/config";
import { useLive } from "../api/live";
import type { FwDefaults, RuleInfo } from "../api/types";
import { Acts, AT, CARD, Cell, HEAD, ROWS } from "../components/CardTable";
import { Field, SaveState, useChangeState } from "../components/Field";
import { PanelCard, SidePanel, withPanel } from "../components/SidePanel";
import { Badge, Dot, NotYet } from "../components/ui";
import { useI18n } from "../i18n";
import { type End, lanOf, readEnd } from "../lib/fw";
import { ip4, net4 } from "../lib/ipv4";
import { ip6, prefix6 } from "../lib/ipv6";
import { readPorts } from "../lib/ports";
import { upTo, z } from "../lib/schemas";
import { fwDefaults, rules, served } from "../lib/state";
import { Sub, type T } from "./internet/shared";
import { Choices, type Device, type Net, netName, useDevices, usePlaces } from "./Lan";

const PROTOS = ["any", "tcp", "udp", "both", "icmp"] as const;
type Proto = (typeof PROTOS)[number];
const FAMILIES = ["any", "ipv4", "ipv6"] as const;

/* The select's value of a part of the internet still to be typed. */
const SOME = "internet:";

/** How many rules there are, for the tab that shows them. */
export function useFwCount() {
  return rules(useLive().data).length;
}

/* The connections that make the stick a router and have a network. */
function useNets() {
  const { places } = usePlaces();
  return places.filter((p): p is Net => p.lan != null);
}

/* An address's family: 4 or 6, of an address or a network, or null for neither. */
function familyOf(s: string): 4 | 6 | null {
  if (ip4(s) != null || net4(s) != null) return 4;
  if (ip6(s) != null || prefix6(s) != null) return 6;
  return null;
}

const protoText = (t: T, p: Proto) =>
  p === "any" ? t("fw.protoAny") : p === "both" ? t("nat.both") : p.toUpperCase();

/** An end as it is read: "internet", "Dom", "laptop (Dom)". */
function endText(t: T, e: End | null, nets: Net[], devices: Device[]) {
  if (!e) return "?";
  const lan = (id: number) => nets.find((p) => p.lan.id === id);
  switch (e.kind) {
    case "internet":
      return e.addr ? t("fw.internetAt", { addr: e.addr }) : t("fw.internet");
    case "lans":
      return t("fw.lans");
    case "lan": {
      const p = lan(e.lan);
      return p ? netName(t, p.lan, nets) : "?";
    }
    case "dev": {
      const d = devices.find((x) => x.lan === e.lan && x.mac === e.mac);
      const p = lan(e.lan);
      const name = d?.name ?? e.mac;
      return p ? `${name} (${netName(t, p.lan, nets)})` : name;
    }
  }
}

/** What a rule's traffic is: its protocol and ports. */
const traffic = (t: T, r: RuleInfo) =>
  r.proto === "any" || r.proto === "icmp" || !r.ports
    ? protoText(t, r.proto)
    : `${protoText(t, r.proto)} ${r.ports.replace("-", "–")}`;

const familyText = (t: T, f: RuleInfo["family"]) =>
  f === "ipv4" ? t("fw.only4") : f === "ipv6" ? t("fw.only6") : t("fw.both");

/** What the firewall does where no rule decides, over the rules, and the button that changes it. */
function DefaultsBar({ d, onEdit }: { d: FwDefaults; onEdit: () => void }) {
  const { t } = useI18n();
  const act = (a: "allow" | "block") => (a === "allow" ? t("fw.allowed") : t("fw.blocked"));
  return (
    <section
      aria-label={t("fw.defaults")}
      className="flex shrink-0 flex-wrap items-center gap-x-8 gap-y-2 rounded-box border border-base-300 bg-base-100 px-4 py-2.5 text-sm"
    >
      <span className="font-semibold">{t("fw.defaults")}</span>
      <span>
        <span className="text-base-content/70">{t("fw.in")}:</span> {t("fw.blocked")}{" "}
        <span className="text-base-content/60">{t("fw.inBut")}</span>
      </span>
      <span>
        <span className="text-base-content/70">{t("fw.out")}:</span> {act(d.out)}
      </span>
      <span>
        <span className="text-base-content/70">{t("fw.between")}:</span> {act(d.between)}
      </span>
      <button
        type="button"
        className="btn btn-ghost btn-xs btn-square ml-auto text-base-content/70"
        onClick={onEdit}
        aria-label={t("fw.defaultsEdit")}
        title={t("fw.defaultsEdit")}
      >
        <Pencil className="size-4" />
      </button>
    </section>
  );
}

/** The defaults' form: to the internet, and between the networks. */
function DefaultsForm({ d, onClose }: { d: FwDefaults; onClose: () => void }) {
  const { t } = useI18n();
  const change = useFwChange();
  const state = useChangeState(change);
  const form = useForm<FwDefaults>({ defaultValues: d });
  const { isDirty } = form.formState;
  const both = [
    { value: "allow" as const, label: t("fw.allow") },
    { value: "block" as const, label: t("fw.block") },
  ];
  return (
    <PanelCard
      id="fw-defaults"
      title={t("fw.defaults")}
      line={t("fw.defaultsLine")}
      onClose={onClose}
    >
      <form
        onSubmit={form.handleSubmit((v) =>
          change.mutate({ OUT: v.out, BETWEEN: v.between }, { onSuccess: onClose }),
        )}
        noValidate
        className="flex min-h-0 flex-1 flex-col"
      >
        <div className="grid min-h-0 flex-1 content-start gap-4 p-4 xl:overflow-auto">
          <div className="grid gap-1">
            <span className="text-sm font-semibold">{t("fw.in")}</span>
            <p className="text-xs text-base-content/70">{t("fw.inHint")}</p>
          </div>
          <Choices
            label={t("fw.out")}
            hint={t("fw.outHint")}
            field={form.register("out")}
            options={both}
          />
          <Choices
            label={t("fw.between")}
            hint={t("fw.betweenHint")}
            field={form.register("between")}
            options={both}
          />
        </div>
        <div className="flex min-h-16 shrink-0 flex-wrap items-center gap-3 border-t border-base-300 px-4 py-2.5">
          <button type="submit" className="btn btn-primary" disabled={!isDirty || change.isPending}>
            {t("internet.save")}
          </button>
          <SaveState {...state} />
        </div>
      </form>
    </PanelCard>
  );
}

/** The choices of a rule's end: the internet, the networks, and each network's devices. */
function EndSelect({
  id,
  label,
  nets,
  devices,
  error,
  field,
}: {
  id: string;
  label: string;
  nets: Net[];
  devices: Device[];
  error: string | undefined;
  field: UseFormRegisterReturn;
}) {
  const { t } = useI18n();
  return (
    <div className="grid content-start gap-1">
      <label htmlFor={id} className="text-sm font-semibold">
        {label}
      </label>
      <select id={id} className={`select w-full ${error ? "select-error" : ""}`} {...field}>
        <optgroup label={t("fw.internet")}>
          <option value="internet">{t("fw.internetAll")}</option>
          <option value={SOME}>{t("fw.internetSome")}</option>
        </optgroup>
        <optgroup label={t("fw.nets")}>
          <option value="lans">{t("fw.lans")}</option>
          {nets.map((p) => (
            <option key={p.lan.id} value={`lan:${p.lan.id}`}>
              {t("fw.lanWhole", { name: netName(t, p.lan, nets) })}
            </option>
          ))}
        </optgroup>
        {nets.map((p) => (
          <optgroup key={p.lan.id} label={t("fw.devicesOf", { name: netName(t, p.lan, nets) })}>
            {devices
              .filter((d) => d.lan === p.lan.id)
              .map((d) => (
                <option key={d.mac} value={`dev:${d.lan}:${d.mac}`}>
                  {`${d.name ?? t("lan.noName")} · ${d.fixed?.ip ?? d.now ?? d.mac}`}
                </option>
              ))}
          </optgroup>
        ))}
      </select>
      {error && <p className="text-xs text-error">{error}</p>}
    </div>
  );
}

/** A rule added at the end, or @x changed. */
function RuleForm({
  x,
  list,
  nets,
  devices,
  onClose,
}: {
  x: RuleInfo | null;
  list: RuleInfo[];
  nets: Net[];
  devices: Device[];
  onClose: () => void;
}) {
  const { t } = useI18n();
  const change = useRuleChange();
  const state = useChangeState(change);
  /* An end as the select holds it, and the address of a part of the internet. */
  const split = (s: string) => {
    const e = readEnd(s);
    return e?.kind === "internet" && e.addr ? { sel: SOME, addr: e.addr } : { sel: s, addr: "" };
  };
  const join = (sel: string, addr: string) => (sel === SOME ? `internet:${addr.trim()}` : sel);
  const schema = z
    .object({
      name: upTo(40),
      action: z.enum(["allow", "block"]),
      from: z.string(),
      fromAddr: z.string(),
      to: z.string(),
      toAddr: z.string(),
      proto: z.enum(PROTOS),
      ports: z.string(),
      family: z.enum(FAMILIES),
      on: z.enum(["on", "off"]),
    })
    .check(
      z.superRefine((v, ctx) => {
        const fail = (path: "from" | "to" | "fromAddr" | "toAddr" | "ports", message: string) =>
          ctx.addIssue({ code: "custom", path: [path], message, input: v[path] });
        /* A part of the internet is an address or a network of the rule's family. */
        for (const [sel, path] of [
          [v.from, "fromAddr"],
          [v.to, "toAddr"],
        ] as const) {
          if (sel !== SOME) continue;
          const f = familyOf(v[path].trim());
          if (!f) fail(path, t("fw.addrBad"));
          else if ((f === 4 && v.family === "ipv6") || (f === 6 && v.family === "ipv4"))
            fail(path, t("fw.addrFamily"));
        }
        const a = readEnd(join(v.from, v.fromAddr));
        const b = readEnd(join(v.to, v.toAddr));
        if (a?.kind === "internet" && b?.kind === "internet") fail("to", t("fw.bothInternet"));
        else if (a && b && lanOf(a) != null && lanOf(a) === lanOf(b)) fail("to", t("fw.sameLan"));
        if ((v.proto === "tcp" || v.proto === "udp" || v.proto === "both") && v.ports.trim())
          if (!readPorts(v.ports)) fail("ports", t("nat.extBad"));
      }),
    );
  type Values = z.infer<typeof schema>;
  const from = split(x?.from ?? "lans");
  const to = split(x?.to ?? "internet");
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: {
      name: x?.name ?? "",
      action: x?.action ?? "block",
      from: from.sel,
      fromAddr: from.addr,
      to: to.sel,
      toAddr: to.addr,
      proto: x?.proto ?? "tcp",
      ports: x?.ports ?? "",
      family: x?.family ?? "any",
      on: x?.on === false ? "off" : "on",
    },
  });
  const { errors, isDirty } = form.formState;
  const [fromSel, toSel, proto, family] = form.watch(["from", "to", "proto", "family"]);
  const send = (v: Values) => {
    const p = readPorts(v.ports);
    change.mutate(
      {
        ID: x ? String(x.id) : undefined,
        NAME: v.name,
        ACTION: v.action,
        FROM: join(v.from, v.fromAddr),
        TO: join(v.to, v.toAddr),
        PROTO: v.proto,
        PORTS:
          v.proto === "any" || v.proto === "icmp" || !p
            ? ""
            : p.from === p.to
              ? String(p.from)
              : `${p.from}-${p.to}`,
        FAMILY: v.family,
        ON: v.on,
      },
      { onSuccess: onClose },
    );
  };
  const n = x ? list.findIndex((r) => r.id === x.id) + 1 : list.length + 1;
  /* In from the internet to a device by IPv4 comes only what NAT lets in. */
  const v4in =
    (fromSel === "internet" || fromSel === SOME) &&
    !toSel.startsWith("internet") &&
    family !== "ipv6";
  return (
    <PanelCard
      id="rule-form"
      title={x ? t("fw.editTitle", { n }) : t("fw.addTitle")}
      line={x ? t("fw.lineAt", { n, all: list.length }) : t("fw.lineNew", { n })}
      onClose={onClose}
    >
      <form onSubmit={form.handleSubmit(send)} noValidate className="flex min-h-0 flex-1 flex-col">
        <div className="grid min-h-0 flex-1 content-start gap-4 p-4 xl:overflow-auto">
          <Field
            label={t("fw.name")}
            placeholder={t("fw.namePlaceholder")}
            error={errors.name?.message}
            {...form.register("name")}
          />
          <Choices
            label={t("fw.action")}
            field={form.register("action")}
            options={[
              { value: "allow", label: t("fw.allow") },
              { value: "block", label: t("fw.block") },
            ]}
          />
          <EndSelect
            id="rule-from"
            label={t("fw.from")}
            nets={nets}
            devices={devices}
            error={errors.from?.message}
            field={form.register("from")}
          />
          {fromSel === SOME && (
            <Field
              label={t("fw.addr")}
              placeholder="203.0.113.0/24"
              hint={t("fw.addrHint")}
              error={errors.fromAddr?.message}
              {...form.register("fromAddr")}
            />
          )}
          <EndSelect
            id="rule-to"
            label={t("fw.to")}
            nets={nets}
            devices={devices}
            error={errors.to?.message}
            field={form.register("to")}
          />
          {toSel === SOME && (
            <Field
              label={t("fw.addr")}
              placeholder="203.0.113.0/24"
              hint={t("fw.addrHint")}
              error={errors.toAddr?.message}
              {...form.register("toAddr")}
            />
          )}
          {v4in && <p className="-mt-2 text-xs text-base-content/70">{t("fw.v4in")}</p>}
          <Choices
            label={t("fw.proto")}
            field={form.register("proto")}
            options={PROTOS.map((p) => ({ value: p, label: protoText(t, p) }))}
          />
          {(proto === "tcp" || proto === "udp" || proto === "both") && (
            <Field
              label={t("fw.ports")}
              placeholder={t("fw.portsAll")}
              inputMode="numeric"
              hint={t("fw.portsHint")}
              error={errors.ports?.message}
              {...form.register("ports")}
            />
          )}
          <Choices
            label={t("fw.family")}
            field={form.register("family")}
            options={FAMILIES.map((f) => ({ value: f, label: familyText(t, f) }))}
          />
          <Choices
            label={t("fw.state")}
            field={form.register("on")}
            options={[
              { value: "on", label: t("fw.on") },
              { value: "off", label: t("fw.off") },
            ]}
          />
        </div>
        <div className="flex min-h-16 shrink-0 flex-wrap items-center gap-3 border-t border-base-300 px-4 py-2.5">
          <button
            type="submit"
            className="btn btn-primary"
            disabled={(!!x && !isDirty) || change.isPending}
          >
            {x ? t("internet.save") : t("fw.add")}
          </button>
          {x && (
            <button type="button" className="btn btn-ghost" onClick={onClose}>
              {t("internet.cancel")}
            </button>
          )}
          <SaveState {...state} />
        </div>
      </form>
    </PanelCard>
  );
}

/** A rule: its number and the arrows that move it, its ends, its traffic, its action and state. */
function RuleCard({
  r,
  n,
  last,
  nets,
  devices,
  editing,
  onEdit,
}: {
  r: RuleInfo;
  n: number;
  last: boolean;
  nets: Net[];
  devices: Device[];
  editing: boolean;
  onEdit: () => void;
}) {
  const { t } = useI18n();
  const change = useRuleChange();
  const ends = `${endText(t, readEnd(r.from), nets, devices)} → ${endText(t, readEnd(r.to), nets, devices)}`;
  const name = r.name || ends;
  const move = (dir: "up" | "down") => change.mutate({ ID: String(r.id), MOVE: dir });
  const remove = () => {
    if (!window.confirm(t("fw.removeConfirm", { n, name }))) return;
    change.mutate({ ID: String(r.id), ACTION: "" });
  };
  return (
    <div
      className={`${CARD} ${editing ? "border-primary ring-1 ring-primary" : "border-base-300"} ${r.on ? "" : "text-base-content/60"}`}
    >
      {/* Its number, between the arrows that move it up and down the order. */}
      <div className={`-my-2 -ml-2 flex flex-col items-center ${AT.caret}`}>
        <button
          type="button"
          className="btn btn-ghost btn-xs btn-square h-5 min-h-5 text-base-content/60"
          disabled={n === 1 || change.isPending}
          onClick={() => move("up")}
          aria-label={t("fw.up", { n })}
          title={t("fw.up", { n })}
        >
          <ChevronUp className="size-4" />
        </button>
        <span className="text-xs font-semibold tabular-nums">{n}</span>
        <button
          type="button"
          className="btn btn-ghost btn-xs btn-square h-5 min-h-5 text-base-content/60"
          disabled={last || change.isPending}
          onClick={() => move("down")}
          aria-label={t("fw.down", { n })}
          title={t("fw.down", { n })}
        >
          <ChevronDown className="size-4" />
        </button>
      </div>
      <Cell at={AT.name}>
        <span className="truncate font-semibold">{name}</span>
        <Sub>{r.name ? ends : familyText(t, r.family)}</Sub>
      </Cell>
      <Cell head={t("fw.colTraffic")} at={AT.first}>
        <span>{traffic(t, r)}</span>
        {r.name && <Sub>{familyText(t, r.family)}</Sub>}
      </Cell>
      <Cell head={t("fw.colAction")} at={AT.second}>
        <span>
          <Badge tone={r.action === "allow" ? "good" : "bad"}>
            {r.action === "allow" ? t("fw.allow") : t("fw.block")}
          </Badge>
        </span>
      </Cell>
      <Cell head={t("fw.colState")} at={AT.third}>
        <span className="flex items-center gap-2">
          <Dot tone={r.on ? "good" : "neutral"} />
          {r.on ? t("fw.on") : t("fw.off")}
        </span>
      </Cell>
      <Acts>
        <button
          type="button"
          className="btn btn-ghost btn-sm btn-square text-base-content/70"
          onClick={onEdit}
          aria-label={t("internet.edit")}
          title={t("internet.edit")}
        >
          <Pencil className="size-4" />
        </button>
        <button
          type="button"
          className="btn btn-ghost btn-sm btn-square text-base-content/70 hover:text-error"
          onClick={remove}
          disabled={change.isPending}
          aria-label={t("internet.remove")}
          title={t("internet.remove")}
        >
          <Trash2 className="size-4" />
        </button>
      </Acts>
    </div>
  );
}

/** What the panel at the list's right is open on: a rule (none for a new one), or the defaults. */
type Open = { rule: RuleInfo | null } | { defaults: true };

/** The firewall tab: the defaults, the rules in order, and their forms. */
function Firewall() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const nets = useNets();
  const list = rules(live);
  const d = fwDefaults(live);
  const devices = useDevices(nets.map((p) => p.lan.id));
  const [open, setOpen] = useState<Open | null>(null);
  const form = useRef<HTMLDivElement>(null);
  const show = (o: Open) => {
    setOpen(o);
    requestAnimationFrame(() => form.current?.scrollIntoView({ block: "nearest" }));
  };
  const close = () => setOpen(null);
  const editing = open && "rule" in open ? (open.rule?.id ?? null) : null;
  return (
    <div className="flex min-h-0 flex-1 flex-col gap-4">
      <DefaultsBar d={d} onEdit={() => show({ defaults: true })} />
      <div className={withPanel(open != null)}>
        <section aria-labelledby="rules" className="@container flex flex-col xl:h-full xl:min-h-0">
          <div className="mt-px flex min-h-14 shrink-0 items-center gap-3 pt-4 pb-2">
            <h2 id="rules" className="text-base font-semibold">
              {t("fw.rules")}
            </h2>
            <button
              type="button"
              className="btn btn-primary btn-sm ml-auto"
              onClick={() => show({ rule: null })}
            >
              <Plus className="size-4" />
              {t("fw.add")}
            </button>
          </div>
          <div className="min-h-0 flex-1 px-px xl:overflow-auto">
            {list.length === 0 ? (
              <div className="mt-4 rounded-box border border-dashed border-base-300 px-6 py-8 text-center">
                <p className="text-base font-semibold">{t("fw.empty")}</p>
                <p className="mx-auto mt-1 max-w-prose text-base-content/70">{t("fw.emptyHint")}</p>
              </div>
            ) : (
              <div className={`grid grid-cols-[2rem_minmax(8rem,1fr)_auto_auto_auto_auto] ${ROWS}`}>
                <div className={HEAD}>
                  <span className="text-center">#</span>
                  <span className="@max-3xl:hidden">{t("fw.colRule")}</span>
                  <span>{t("fw.colTraffic")}</span>
                  <span>{t("fw.colAction")}</span>
                  <span>{t("fw.colState")}</span>
                  <span />
                </div>
                {list.map((r, i) => (
                  <RuleCard
                    key={r.id}
                    r={r}
                    n={i + 1}
                    last={i === list.length - 1}
                    nets={nets}
                    devices={devices}
                    editing={editing === r.id}
                    onEdit={() => show({ rule: r })}
                  />
                ))}
              </div>
            )}
          </div>
          <p className="mb-px flex min-h-16 shrink-0 items-center border-t border-base-300 text-xs text-base-content/70">
            {t("fw.count", { n: list.length, on: list.filter((r) => r.on).length })}
          </p>
        </section>
        {open && (
          <SidePanel ref={form} onClose={close}>
            {"defaults" in open ? (
              <DefaultsForm key="defaults" d={d} onClose={close} />
            ) : (
              <RuleForm
                key={`rule:${open.rule?.id ?? "new"}`}
                x={open.rule}
                list={list}
                nets={nets}
                devices={devices}
                onClose={close}
              />
            )}
          </SidePanel>
        )}
      </div>
    </div>
  );
}

/* The firewall tab, or that the stick has no firewall yet. */
export function FirewallTab() {
  const { t } = useI18n();
  const { data: live } = useLive();
  if (!live) return null;
  if (!served(live, "rules")) return <NotYet>{t("notYet.fw")}</NotYet>;
  return <Firewall />;
}
