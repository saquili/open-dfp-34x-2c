// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * NAT, a tab of the internet page: what comes in from the internet to the
 * devices of the local networks. The devices go out with their connection's
 * address by themselves (the stick translates them, in silicon); in, only
 * what a forward lets through: a port, or a range, of a connection to a
 * device of that connection's network, by its MAC, whose address is fixed so
 * that a new lease does not send it elsewhere. Over the forwards, each
 * connection's DMZ: the one device that takes whatever no forward takes.
 * The forms come in at the list's right (components/SidePanel). It is IPv4:
 * a device's IPv6 address is its own, which the firewall opens.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Pencil, Plus, Trash2 } from "lucide-react";
import { type ReactNode, useRef, useState } from "react";
import { type UseFormRegisterReturn, useForm } from "react-hook-form";
import { useDmzChange, useForwardChange } from "../api/config";
import { useLive } from "../api/live";
import type { ForwardInfo } from "../api/types";
import { Acts, AT, CARD, Cell, HEAD, ROWS } from "../components/CardTable";
import { Field, SaveState, useChangeState } from "../components/Field";
import { PanelCard, SidePanel, withPanel } from "../components/SidePanel";
import { Dot } from "../components/ui";
import { useI18n } from "../i18n";
import { ports, readPorts } from "../lib/ports";
import { upTo, z } from "../lib/schemas";
import { dmzs, forwards } from "../lib/state";
import { connName, Mono, Sub, type T } from "./internet/shared";
import { Choices, type Device, type Net, netName, useDevices, usePlaces } from "./Lan";

const PROTOS = ["tcp", "udp", "both"] as const;
type Proto = (typeof PROTOS)[number];

const proto = (t: T, p: Proto) => (p === "both" ? t("nat.both") : p.toUpperCase());

/* A device as a choice: its name and its address, the fixed one if it has one. */
const deviceLabel = (t: T, d: Device) =>
  `${d.name ?? t("lan.noName")} · ${d.fixed?.ip ?? d.now ?? d.mac}`;

/* Devices by name, the ones without a name last. */
const byName = (list: Device[]) =>
  list
    .slice()
    .sort((a, b) =>
      a.name && b.name ? a.name.localeCompare(b.name) : a.name ? -1 : b.name ? 1 : 0,
    );

/* The connections that make the stick a router and have a network: where NAT is. */
function useNets() {
  const { places } = usePlaces();
  return places.filter((p): p is Net => p.lan != null);
}

/** How many forwards there are, for the tab that shows them. */
export function useNatCount() {
  return forwards(useLive().data).length;
}

/** A connection's public IPv4 address, the one forwards come in on, or null. */
function wanAddress(p: Net) {
  const a = (p.c.x as { ipv4?: string | null }).ipv4;
  return a ? (a.split("/")[0] ?? null) : null;
}

/** The line under a form's title: the address what comes in comes to. */
function Strip({ p }: { p: Net | undefined }) {
  const { t } = useI18n();
  const ip = p ? wanAddress(p) : null;
  if (!p) return null;
  return ip ? (
    <span>
      {t("nat.strip", { name: connName(t, p.c) })} <Mono>{ip}</Mono>
    </span>
  ) : (
    t("nat.stripNone", { name: connName(t, p.c) })
  );
}

/** A form's frame: its title and X, the address it is about, and what it holds. */
function Frame({
  id,
  title,
  p,
  onClose,
  children,
}: {
  id: string;
  title: string;
  p: Net | undefined;
  onClose: () => void;
  children: ReactNode;
}) {
  return (
    <PanelCard id={id} title={title} line={<Strip p={p} />} onClose={onClose}>
      {children}
    </PanelCard>
  );
}

/** The device of a forward or a DMZ, and what keeps its address: fixed already, or fixed by this. */
function DeviceSelect({
  devices,
  mac,
  error,
  field,
}: {
  devices: Device[];
  mac: string;
  error: string | undefined;
  field: UseFormRegisterReturn;
}) {
  const { t } = useI18n();
  const d = devices.find((x) => x.mac === mac);
  return (
    <div className="grid content-start gap-1">
      <label htmlFor="nat-device" className="text-sm font-semibold">
        {t("nat.device")}
      </label>
      <select id="nat-device" className={`select w-full ${error ? "select-error" : ""}`} {...field}>
        <option value="">{t("nat.devicePick")}</option>
        {devices.map((x) => (
          <option key={x.mac} value={x.mac}>
            {deviceLabel(t, x)}
          </option>
        ))}
      </select>
      {error ? (
        <p className="text-xs text-error">{error}</p>
      ) : d ? (
        <p className="text-xs text-base-content/70">
          {d.fixed ? t("nat.pinned", { ip: d.fixed.ip }) : t("nat.pinWill", { ip: d.now ?? "" })}
        </p>
      ) : null}
    </div>
  );
}

/** A forward added, or @x changed. */
function ForwardForm({
  x,
  nets,
  list,
  onClose,
}: {
  x: ForwardInfo | null;
  nets: Net[];
  list: ForwardInfo[];
  onClose: () => void;
}) {
  const { t } = useI18n();
  const change = useForwardChange();
  const state = useChangeState(change);
  const all = useDevices(nets.map((p) => p.lan.id));
  const others = list.filter((f) => f.id !== x?.id);
  const netOf = (key: string) => nets.find((p) => p.c.key === key);
  const schema = z
    .object({
      name: upTo(32),
      conn: z.string().check(z.minLength(1, t("form.required"))),
      proto: z.enum(PROTOS),
      ext: z.string(),
      mac: z.string().check(z.minLength(1, t("nat.devicePick"))),
      int: z.string(),
      on: z.enum(["on", "off"]),
    })
    .check(
      z.superRefine((v, ctx) => {
        const fail = (path: "ext" | "mac" | "int", message: string) =>
          ctx.addIssue({ code: "custom", path: [path], message, input: v[path] });
        const p = netOf(v.conn);
        const ext = readPorts(v.ext);
        if (!ext) return fail("ext", t("nat.extBad"));
        /* Ports of another forward of the connection, of a protocol of this one. */
        const taken = others.find(
          (f) =>
            f.conn === v.conn &&
            (f.proto === "both" || v.proto === "both" || f.proto === v.proto) &&
            f.ext_from <= ext.to &&
            ext.from <= f.ext_to,
        );
        if (taken)
          fail(
            "ext",
            t("nat.extTaken", {
              ports: ports(taken.ext_from, taken.ext_to),
              proto: proto(t, taken.proto),
              name: taken.name || t("nat.unnamed", { ports: ports(taken.ext_from, taken.ext_to) }),
            }),
          );
        const int = v.int.trim() ? Number(v.int) : ext.from;
        if (!/^\d*$/.test(v.int.trim()) || int < 1 || int + (ext.to - ext.from) > 65535)
          fail("int", t("nat.intBad"));
        if (p && v.mac && !all.some((d) => d.lan === p.lan.id && d.mac === v.mac))
          fail("mac", t("nat.deviceOther", { name: netName(t, p.lan, nets) }));
      }),
    );
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: x
      ? {
          name: x.name,
          conn: x.conn,
          proto: x.proto,
          ext: x.ext_from === x.ext_to ? String(x.ext_from) : `${x.ext_from}-${x.ext_to}`,
          mac: x.mac,
          int: x.int_port === x.ext_from ? "" : String(x.int_port),
          on: x.on ? "on" : "off",
        }
      : { name: "", conn: nets[0]?.c.key ?? "", proto: "tcp", ext: "", mac: "", int: "", on: "on" },
  });
  const { errors, isDirty } = form.formState;
  const [connKey, mac] = form.watch(["conn", "mac"]);
  const p = netOf(connKey);
  const devices = byName(all.filter((d) => d.lan === p?.lan.id));
  const send = (v: Values) => {
    const ext = readPorts(v.ext);
    const net = netOf(v.conn);
    if (!ext || !net) return;
    change.mutate(
      {
        ID: x ? String(x.id) : undefined,
        NAME: v.name,
        CONN: v.conn,
        PROTO: v.proto,
        EXT_FROM: String(ext.from),
        EXT_TO: String(ext.to),
        LAN: String(net.lan.id),
        MAC: v.mac,
        INT_PORT: v.int.trim() || String(ext.from),
        ON: v.on,
      },
      { onSuccess: onClose },
    );
  };
  const name = x ? x.name || t("nat.unnamed", { ports: ports(x.ext_from, x.ext_to) }) : "";
  return (
    <Frame
      id="forward-form"
      title={x ? t("nat.editTitle", { name }) : t("nat.addTitle")}
      p={p}
      onClose={onClose}
    >
      <form onSubmit={form.handleSubmit(send)} noValidate className="flex min-h-0 flex-1 flex-col">
        <div className="grid min-h-0 flex-1 content-start gap-4 p-4 xl:overflow-auto">
          <Field
            label={t("nat.name")}
            placeholder={t("nat.namePlaceholder")}
            error={errors.name?.message}
            {...form.register("name")}
          />
          <div className="grid content-start gap-1">
            <label htmlFor="nat-conn" className="text-sm font-semibold">
              {t("nat.conn")}
            </label>
            <select id="nat-conn" className="select w-full" {...form.register("conn")}>
              {nets.map((n) => (
                <option key={n.c.key} value={n.c.key}>
                  {`${connName(t, n.c)} · ${netName(t, n.lan, nets)}`}
                </option>
              ))}
            </select>
            <p className="text-xs text-base-content/70">{t("nat.connHint")}</p>
          </div>
          <Choices
            label={t("nat.proto")}
            field={form.register("proto")}
            options={PROTOS.map((v) => ({ value: v, label: proto(t, v) }))}
          />
          <div className="grid grid-cols-2 gap-4">
            <Field
              label={t("nat.ext")}
              placeholder="443"
              inputMode="numeric"
              hint={t("nat.extHint")}
              error={errors.ext?.message}
              {...form.register("ext")}
            />
            <Field
              label={t("nat.intPort")}
              placeholder={readPorts(form.watch("ext"))?.from.toString() ?? ""}
              inputMode="numeric"
              hint={t("nat.intHint")}
              error={errors.int?.message}
              {...form.register("int")}
            />
          </div>
          <DeviceSelect
            devices={devices}
            mac={mac}
            error={errors.mac?.message}
            field={form.register("mac")}
          />
          <Choices
            label={t("nat.state")}
            field={form.register("on")}
            options={[
              { value: "on", label: t("nat.on") },
              { value: "off", label: t("nat.off") },
            ]}
          />
        </div>
        <div className="flex min-h-16 shrink-0 flex-wrap items-center gap-3 border-t border-base-300 px-4 py-2.5">
          <button
            type="submit"
            className="btn btn-primary"
            disabled={(!!x && !isDirty) || change.isPending}
          >
            {x ? t("internet.save") : t("nat.add")}
          </button>
          {x && (
            <button type="button" className="btn btn-ghost" onClick={onClose}>
              {t("internet.cancel")}
            </button>
          )}
          <SaveState {...state} />
        </div>
      </form>
    </Frame>
  );
}

/** The DMZ of the connection @conn: its device, or none. */
function DmzForm({ conn, nets, onClose }: { conn: string; nets: Net[]; onClose: () => void }) {
  const { t } = useI18n();
  const { data: live } = useLive();
  const change = useDmzChange();
  const state = useChangeState(change);
  const p = nets.find((n) => n.c.key === conn);
  const now = dmzs(live).find((y) => y.conn === conn);
  const all = useDevices(p ? [p.lan.id] : []);
  const devices = byName(all);
  const form = useForm<{ mac: string }>({ defaultValues: { mac: now?.mac ?? "" } });
  const mac = form.watch("mac");
  const send = (v: { mac: string }) =>
    p && change.mutate({ CONN: conn, LAN: String(p.lan.id), MAC: v.mac }, { onSuccess: onClose });
  return (
    <Frame
      id="dmz-form"
      title={t("nat.dmzTitle", { name: p ? connName(t, p.c) : conn })}
      p={p}
      onClose={onClose}
    >
      <form onSubmit={form.handleSubmit(send)} noValidate className="flex min-h-0 flex-1 flex-col">
        <div className="grid min-h-0 flex-1 content-start gap-4 p-4 xl:overflow-auto">
          <p className="rounded-box border border-warning/60 bg-warning/10 px-3 py-2 text-xs">
            {t("nat.dmzWarn")}
          </p>
          <DeviceSelect
            devices={devices}
            mac={mac}
            error={undefined}
            field={form.register("mac")}
          />
        </div>
        <div className="flex min-h-16 shrink-0 flex-wrap items-center gap-3 border-t border-base-300 px-4 py-2.5">
          <button
            type="submit"
            className="btn btn-primary"
            disabled={!mac || mac === now?.mac || change.isPending}
          >
            {t("internet.save")}
          </button>
          {now && (
            <button
              type="button"
              className="btn btn-ghost hover:text-error"
              disabled={change.isPending}
              onClick={() => send({ mac: "" })}
            >
              {t("nat.dmzDisable")}
            </button>
          )}
          <SaveState {...state} />
        </div>
      </form>
    </Frame>
  );
}

/** Each connection's DMZ, over the forwards: its device, or "wyłączona", and the button that sets it. */
function DmzBar({ nets, onSet }: { nets: Net[]; onSet: (conn: string) => void }) {
  const { t } = useI18n();
  const { data: live } = useLive();
  const all = useDevices(nets.map((p) => p.lan.id));
  return (
    <section
      aria-label={t("nat.dmz")}
      className="flex shrink-0 flex-wrap items-center gap-x-8 gap-y-2 rounded-box border border-base-300 bg-base-100 px-4 py-2.5 text-sm"
    >
      <span className="font-semibold">{t("nat.dmz")}</span>
      {nets.map((p) => {
        const z = dmzs(live).find((y) => y.conn === p.c.key);
        const d = z && all.find((x) => x.lan === z.lan && x.mac === z.mac);
        const name = connName(t, p.c);
        return (
          <span key={p.c.key} className="flex items-center gap-2">
            <span className="text-base-content/70">{name}:</span>
            {z ? (
              <>
                <span>{d?.name ?? z.mac}</span>
                <Mono>{d?.fixed?.ip ?? d?.now ?? ""}</Mono>
              </>
            ) : (
              <span className="text-base-content/60">{t("nat.dmzOff")}</span>
            )}
            <button
              type="button"
              className={`btn btn-ghost btn-xs ${z ? "btn-square text-base-content/70" : "text-primary"}`}
              onClick={() => onSet(p.c.key)}
              aria-label={z ? t("nat.dmzEdit", { name }) : undefined}
              title={z ? t("nat.dmzEdit", { name }) : undefined}
            >
              {z ? <Pencil className="size-4" /> : t("nat.dmzSet")}
            </button>
          </span>
        );
      })}
    </section>
  );
}

/** A forward: its name and connection, its ports, its device and whether it is on. */
function ForwardCard({
  f,
  nets,
  devices,
  editing,
  onEdit,
}: {
  f: ForwardInfo;
  nets: Net[];
  devices: Device[];
  editing: boolean;
  onEdit: () => void;
}) {
  const { t } = useI18n();
  const change = useForwardChange();
  const p = nets.find((n) => n.c.key === f.conn);
  const d = devices.find((x) => x.lan === f.lan && x.mac === f.mac);
  const name = f.name || t("nat.unnamed", { ports: ports(f.ext_from, f.ext_to) });
  const span = f.ext_to - f.ext_from;
  const remove = () => {
    if (!window.confirm(t("nat.removeConfirm", { name }))) return;
    change.mutate({ ID: String(f.id), CONN: "" });
  };
  return (
    <div
      className={`${CARD} ${editing ? "border-primary ring-1 ring-primary" : "border-base-300"} ${f.on ? "" : "text-base-content/60"}`}
    >
      <span className={AT.caret} />
      <Cell at={AT.name}>
        <span className="truncate font-semibold">{name}</span>
        <Sub>{p ? `${connName(t, p.c)} · ${netName(t, p.lan, nets)}` : f.conn}</Sub>
      </Cell>
      <Cell head={t("nat.colFrom")} at={AT.first}>
        <span>
          {proto(t, f.proto)} <Mono>{ports(f.ext_from, f.ext_to)}</Mono>
        </span>
      </Cell>
      <Cell head={t("nat.colTo")} at={AT.second}>
        <span className="truncate">{d?.name ?? f.mac}</span>
        <Sub>
          <Mono>{`${d?.fixed?.ip ?? d?.now ?? "?"}:${ports(f.int_port, f.int_port + span)}`}</Mono>
        </Sub>
      </Cell>
      <Cell head={t("nat.colState")} at={AT.third}>
        <span className="flex items-center gap-2">
          <Dot tone={f.on ? "good" : "neutral"} />
          {f.on ? t("nat.on") : t("nat.off")}
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

/** What the panel at the list's right is open on: a forward (none for a new one), or a connection's DMZ. */
type Open = { forward: ForwardInfo | null } | { dmz: string };

/** The NAT tab: each connection's DMZ, the forwards, and their forms. */
export function NatTab() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const nets = useNets();
  const list = forwards(live);
  const devices = useDevices(nets.map((p) => p.lan.id));
  const [open, setOpen] = useState<Open | null>(null);
  const form = useRef<HTMLDivElement>(null);
  const show = (o: Open) => {
    setOpen(o);
    requestAnimationFrame(() => form.current?.scrollIntoView({ block: "nearest" }));
  };
  const close = () => setOpen(null);
  const editing = open && "forward" in open ? (open.forward?.id ?? null) : null;
  return (
    <div className="flex min-h-0 flex-1 flex-col gap-4">
      {nets.length > 0 && <DmzBar nets={nets} onSet={(conn) => show({ dmz: conn })} />}
      <div className={withPanel(open != null)}>
        <section
          aria-labelledby="forwards"
          className="@container flex flex-col xl:h-full xl:min-h-0"
        >
          <div className="mt-px flex min-h-14 shrink-0 items-center gap-3 pt-4 pb-2">
            <h2 id="forwards" className="text-base font-semibold">
              {t("nat.forwards")}
            </h2>
            <button
              type="button"
              className="btn btn-primary btn-sm ml-auto"
              onClick={() => show({ forward: null })}
              disabled={!nets.length}
              title={nets.length ? undefined : t("nat.noNets")}
            >
              <Plus className="size-4" />
              {t("nat.add")}
            </button>
          </div>
          <div className="min-h-0 flex-1 px-px xl:overflow-auto">
            {list.length === 0 ? (
              <div className="mt-4 rounded-box border border-dashed border-base-300 px-6 py-8 text-center">
                <p className="text-base font-semibold">{t("nat.empty")}</p>
                <p className="mx-auto mt-1 max-w-prose text-base-content/70">
                  {nets.length ? t("nat.emptyHint") : t("nat.noNets")}
                </p>
              </div>
            ) : (
              <div className={`grid grid-cols-[2rem_minmax(8rem,1fr)_auto_auto_auto_auto] ${ROWS}`}>
                <div className={HEAD}>
                  <span />
                  <span className="@max-3xl:hidden">{t("nat.colName")}</span>
                  <span>{t("nat.colFrom")}</span>
                  <span>{t("nat.colTo")}</span>
                  <span>{t("nat.colState")}</span>
                  <span />
                </div>
                {list.map((f) => (
                  <ForwardCard
                    key={f.id}
                    f={f}
                    nets={nets}
                    devices={devices}
                    editing={editing === f.id}
                    onEdit={() => show({ forward: f })}
                  />
                ))}
              </div>
            )}
          </div>
          <p className="mb-px flex min-h-16 shrink-0 items-center border-t border-base-300 text-xs text-base-content/70">
            {t("nat.count", { n: list.length, on: list.filter((f) => f.on).length })}
          </p>
        </section>
        {open && (
          <SidePanel ref={form} onClose={close}>
            {"dmz" in open ? (
              <DmzForm key={`dmz:${open.dmz}`} conn={open.dmz} nets={nets} onClose={close} />
            ) : (
              <ForwardForm
                key={`fw:${open.forward?.id ?? "new"}`}
                x={open.forward}
                nets={nets}
                list={list}
                onClose={close}
              />
            )}
          </SidePanel>
        )}
      </div>
    </div>
  );
}
