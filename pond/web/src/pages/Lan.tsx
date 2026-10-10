// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The local networks: the stick's own networks towards the devices behind
 * it, each out through one connection that makes the stick their router,
 * with that connection's address. A router of a few ports has one or two
 * addresses from the operator; the stick has up to seven connections, and
 * each can have a network of its own, apart from the others by its VLAN to
 * the switch behind the stick, or untagged, for one of them.
 *
 * Two tabs of the internet page (pages/Internet.tsx). The networks: the
 * networks there are, one at most for each connection that makes the stick a
 * router, and at their right, while it is open (components/SidePanel), the
 * form that adds a network or changes one, the connection first: its name,
 * its VLAN to the switch, the stick's address in it, and the DHCP server that
 * gives the devices theirs, with the operator's name servers; the panels
 * stand on the lines of the connections' tab.
 * The devices: every device of the networks, a line each, to be searched and
 * narrowed to one network, since a big house has fifty of them, which no card
 * would hold; and the form of a fixed address beside them while it is open.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Pencil, Pin, Plus, Trash2, X } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { type UseFormRegisterReturn, useForm } from "react-hook-form";
import { useFixedChange, useLanChange } from "../api/config";
import { useLive } from "../api/live";
import type { FixedInfo } from "../api/types";
import { Acts, AT, CARD, Cell, HEAD, ROWS } from "../components/CardTable";
import { Field, SaveState, useChangeState } from "../components/Field";
import { Filter } from "../components/Pager";
import { SidePanel, withPanel } from "../components/SidePanel";
import { useI18n } from "../i18n";
import { duration } from "../lib/format";
import { ip4, net4 } from "../lib/ipv4";
import { subnet64, subnets64 } from "../lib/ipv6";
import { ipv4, ipv4Cidr, mac, required, upTo, vlan, z } from "../lib/schemas";
import { downVlan, fixed, type LanView, lans, leases, sessions } from "../lib/state";
import { type Conn, conns } from "./internet/conns";
import { connName, hostSide, Mono, Sub, type T, useServices } from "./internet/shared";

/* How long a device keeps its address: the choices, in seconds. */
const LEASES = ["3600", "43200", "86400", "604800"] as const;

/** A connection that makes the stick a router, and its network if it has one. */
export interface Place {
  c: Conn;
  lan: LanView | undefined;
}

/* A connection's name with the VLAN it runs in. */
const wan = (t: T, c: Conn) => `${connName(t, c)} · VLAN ${c.vlan}`;

/* A network's name: the one given it, else its connection's, for one named before names were. */
export function netName(t: T, l: LanView, places: Place[]) {
  if (l.name) return l.name;
  const p = places.find((q) => q.c.key === l.conn);
  return p ? connName(t, p.c) : l.conn;
}

/* A name for a new network: the first "Sieć N" no other one has. */
function suggestName(t: T, taken: LanView[]) {
  const names = new Set(taken.map((l) => l.name.toLowerCase()));
  for (let n = 1; ; n++) {
    const name = t("lan.nameDefault", { n });
    if (!names.has(name.toLowerCase())) return name;
  }
}

/* A pool as it is read: its last address cut to what differs from its first. */
function pool(from: string, to: string) {
  const a = from.split(".");
  const z = to.split(".");
  return a.slice(0, 3).join(".") === z.slice(0, 3).join(".") ? `${from}–${z[3]}` : `${from}–${to}`;
}

/* A network for a new one: the first 192.168.N.0/24 that no other one takes. */
function suggest(taken: LanView[]) {
  const nets = taken.map((l) => net4(l.address)).filter((n) => n != null);
  for (let n = 1; n < 255; n++) {
    const first = ip4(`192.168.${n}.0`) ?? 0;
    if (!nets.some((x) => first <= x.last && first + 255 >= x.first))
      return {
        address: `192.168.${n}.1/24`,
        poolFrom: `192.168.${n}.100`,
        poolTo: `192.168.${n}.199`,
      };
  }
  return { address: "", poolFrom: "", poolTo: "" };
}

/* A few short answers as a row of buttons, the chosen one filled. */
export function Choices<V extends string>({
  label,
  hint,
  field,
  options,
}: {
  label: string;
  hint?: string;
  field: UseFormRegisterReturn;
  options: { value: V; label: string }[];
}) {
  return (
    <div className="grid gap-1">
      <span className="text-sm font-semibold">{label}</span>
      <div className="join">
        {options.map((o) => (
          <label key={o.value} className="join-item btn btn-sm has-[:checked]:btn-primary">
            <input type="radio" value={o.value} className="hidden" {...field} />
            {o.label}
          </label>
        ))}
      </div>
      {hint && <p className="text-xs text-base-content/70">{hint}</p>}
    </div>
  );
}

/** The form's last row: the button that saves, and the one that gives up a change. */
function Foot({
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
    <div className="flex min-h-16 shrink-0 flex-wrap items-center gap-3 border-t border-base-300 px-4 py-2.5">
      <button type="submit" className="btn btn-primary" disabled={disabled}>
        {editing ? t("internet.save") : t("lan.add")}
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

/** A network added out through @conn, or @x changed: its fields scroll between its head and foot. */
function LanFields({
  x,
  conn,
  free,
  all,
  places,
  onDone,
  onCancel,
}: {
  x: LanView | undefined;
  conn: string;
  /** The connections a new network can go out through. */
  free: Place[];
  all: LanView[];
  places: Place[];
  onDone: () => void;
  onCancel: () => void;
}) {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const { list: services } = useServices();
  const change = useLanChange();
  const state = useChangeState(change);
  const others = all.filter((l) => l.id !== x?.id);
  const pins = x ? fixed(live).filter((f) => f.lan === x.id) : [];
  const nameOf = (l: LanView) => netName(t, l, places);
  /* The connection a network goes out through, whose IPv6 prefix it takes a /64 of. */
  const wanOf = (key: string) => places.find((p) => p.c.key === key)?.c;
  const schema = z
    .object({
      conn: z.string().check(z.minLength(1, t("form.required"))),
      name: required(t("form.required")).check(z.maxLength(24, t("lan.nameLong"))),
      vlan: vlan(t),
      address: ipv4Cidr(t).check(z.minLength(1, t("form.required"))),
      dhcp: z.enum(["on", "off"]),
      poolFrom: ipv4(t),
      poolTo: ipv4(t),
      lease: z.enum(LEASES),
      ipv6: z.enum(["on", "off"]),
      subnet: z.string().check(z.trim()),
    })
    .check(
      /*
       * The network takes none of another's addresses; its pool lies in it,
       * in order, and leaves the stick's address out.
       */
      z.superRefine((v, ctx) => {
        const net = net4(v.address);
        const fail = (
          path: "name" | "address" | "poolFrom" | "poolTo" | "subnet",
          message: string,
        ) => ctx.addIssue({ code: "custom", path: [path], message, input: v[path] });
        /* The /64 network is one the connection's prefix holds: 0 to ff of a /56. */
        const w = wanOf(v.conn);
        if (v.ipv6 === "on" && w && w.x.family !== "ipv4") {
          const room = w.x.prefix ? subnets64(w.x.prefix) : 256;
          const id = /^[0-9a-f]{1,4}$/i.test(v.subnet) ? Number.parseInt(v.subnet, 16) : -1;
          if (room > 1 && (id < 0 || id >= room))
            fail("subnet", t("lan.subnetBad", { max: (room - 1).toString(16) }));
        }
        /* Two networks of one name would be one in every list. */
        if (others.some((l) => nameOf(l).toLowerCase() === v.name.toLowerCase()))
          fail("name", t("lan.nameTaken"));
        const over = net
          ? others.find((l) => {
              const o = net4(l.address);
              return o != null && net.first <= o.last && net.last >= o.first;
            })
          : undefined;
        if (net && over)
          fail("address", t("lan.netOverlap", { net: net.text, name: nameOf(over) }));
        /* The network's fixed addresses stay in it, none of them the stick's. */
        const stick = ip4(v.address.split("/")[0] ?? "");
        const out = net
          ? pins.find((f) => {
              const a = ip4(f.ip);
              return a == null || a <= net.first || a >= net.last || a === stick;
            })
          : undefined;
        if (out) fail("address", t("lan.netFixed", { ip: out.ip, name: out.name || out.mac }));
        if (v.dhcp === "off") return;
        if (!v.poolFrom) return fail("poolFrom", t("form.required"));
        if (!v.poolTo) return fail("poolTo", t("form.required"));
        const a = ip4(v.address.split("/")[0] ?? "");
        const from = ip4(v.poolFrom);
        const to = ip4(v.poolTo);
        if (!net || a == null || from == null || to == null) return;
        if (from <= net.first || from >= net.last)
          fail("poolFrom", t("lan.poolOut", { net: net.text }));
        if (to <= net.first || to >= net.last) fail("poolTo", t("lan.poolOut", { net: net.text }));
        if (from > to) fail("poolTo", t("lan.poolOrder"));
        else if (a >= from && a <= to) fail("poolFrom", t("lan.poolStick"));
      }),
    );
  type Values = z.infer<typeof schema>;
  /*
   * A new network goes untagged while none does; else in the first tenth
   * VLAN nothing else goes to the switch in.
   */
  const used = new Set([
    ...all.map((l) => l.vlan),
    ...services.flatMap((s) => hostSide(s).map(String)),
    ...sessions(live)
      .filter((y) => y.pass)
      .map(downVlan),
  ]);
  const freeVid = used.has("")
    ? (Array.from({ length: 409 }, (_, i) => String((i + 1) * 10)).find((v) => !used.has(v)) ?? "")
    : "";
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: x
      ? {
          conn: x.conn,
          name: nameOf(x),
          vlan: x.vlan,
          address: x.address,
          dhcp: x.dhcp ? "on" : "off",
          poolFrom: x.poolFrom,
          poolTo: x.poolTo,
          lease: LEASES.find((l) => Number(l) === x.leaseS) ?? "86400",
          ipv6: x.ipv6 ? "on" : "off",
          subnet: x.subnet6.toString(16),
        }
      : {
          conn,
          name: suggestName(t, all),
          vlan: freeVid,
          dhcp: "on",
          lease: "86400",
          ipv6: wanOf(conn)?.x.family === "ipv4" ? "off" : "on",
          subnet: "0",
          ...suggest(all),
        },
  });
  const { errors, isDirty } = form.formState;
  const [vid, address, dhcp, connKey, ipv6, subnet] = form.watch([
    "vlan",
    "address",
    "dhcp",
    "conn",
    "ipv6",
    "subnet",
  ]);
  const w = wanOf(connKey);
  const v6 = !!w && w.x.family !== "ipv4";
  const prefix = w?.x.prefix ?? null;
  const room = prefix ? subnets64(prefix) : 256;
  const net6 = prefix ? subnet64(prefix, Number.parseInt(subnet || "0", 16)) : null;
  const net = net4(address);

  /*
   * What else goes to the switch in the VLAN, or untagged: another network,
   * a service of the operator's, an address a session hands down.
   */
  const lan = others.find((l) => l.vlan === vid);
  const service = vid ? services.find((s) => hostSide(s).includes(Number(vid))) : undefined;
  const down = sessions(live).find((y) => y.pass && downVlan(y) === vid);
  const clash = lan
    ? t(vid ? "lan.vlanLan" : "lan.vlanLanNone", { vid, name: nameOf(lan) })
    : service
      ? t("lan.vlanService", { vid })
      : down
        ? t(vid ? "lan.vlanDown" : "lan.vlanDownNone", { vid, name: `PPPoE ${down.id}` })
        : undefined;

  const send = (v: Values) =>
    change.mutate(
      {
        ID: x ? String(x.id) : undefined,
        CONN: v.conn,
        NAME: v.name,
        VLAN: v.vlan,
        ADDRESS: v.address,
        DHCP: v.dhcp,
        POOL_FROM: v.poolFrom,
        POOL_TO: v.poolTo,
        LEASE: v.lease,
        IPV6: v6 ? v.ipv6 : "off",
        IPV6_SUBNET: (Number.parseInt(v.subnet || "0", 16) || 0).toString(16),
      },
      { onSuccess: onDone },
    );

  const choices = x ? places.filter((p) => p.c.key === x.conn) : free;
  return (
    <>
      {/* What the address makes of the network, as high as the internet form's tabs. */}
      <p className="flex h-11 shrink-0 items-center border-b border-base-300 px-4 text-xs text-base-content/70">
        {net ? t(vid ? "lan.netIn" : "lan.netNone", { net: net.text, vid }) : t("lan.netUnknown")}
      </p>
      <form onSubmit={form.handleSubmit(send)} noValidate className="flex min-h-0 flex-1 flex-col">
        <div className="grid min-h-0 flex-1 content-start gap-4 p-4 xl:overflow-auto">
          <div className="grid content-start gap-1">
            <label htmlFor="lan-conn" className="text-sm font-semibold">
              {t("lan.conn")}
            </label>
            <select
              id="lan-conn"
              className="select w-full"
              disabled={!!x}
              {...form.register("conn")}
            >
              {choices.map((p) => (
                <option key={p.c.key} value={p.c.key}>
                  {wan(t, p.c)}
                </option>
              ))}
            </select>
            <p className="text-xs text-base-content/70">{t("lan.connHint")}</p>
          </div>
          <Field
            label={t("lan.name")}
            placeholder={t("lan.nameDefault", { n: 1 })}
            hint={t("lan.nameHint")}
            error={errors.name?.message}
            {...form.register("name")}
          />
          <Field
            label={t("lan.vlan")}
            inputMode="numeric"
            placeholder={t("switch.untagged")}
            hint={t("lan.vlanHint")}
            error={errors.vlan?.message ?? clash}
            {...form.register("vlan")}
          />
          <Field
            label={t("lan.address")}
            placeholder="192.168.1.1/24"
            hint={t("lan.addressHint")}
            error={errors.address?.message}
            {...form.register("address")}
          />
          <Choices
            label={t("lan.dhcp")}
            hint={t(dhcp === "on" ? "lan.dhcpOnHint" : "lan.dhcpOffHint")}
            field={form.register("dhcp")}
            options={[
              { value: "on", label: t("lan.dhcpOn") },
              { value: "off", label: t("lan.dhcpOff") },
            ]}
          />
          {dhcp === "on" && (
            <>
              <div className="grid grid-cols-2 gap-4">
                <Field
                  label={t("lan.poolFrom")}
                  placeholder="192.168.1.100"
                  error={errors.poolFrom?.message}
                  {...form.register("poolFrom")}
                />
                <Field
                  label={t("lan.poolTo")}
                  placeholder="192.168.1.199"
                  error={errors.poolTo?.message}
                  {...form.register("poolTo")}
                />
              </div>
              <Choices
                label={t("lan.lease")}
                field={form.register("lease")}
                options={LEASES.map((l) => ({ value: l, label: duration(Number(l), lang) }))}
              />
            </>
          )}
          {/* IPv6 by SLAAC: a /64 of the prefix the connection has from the operator. */}
          {v6 && w ? (
            <>
              <Choices
                label={t("lan.ipv6")}
                hint={t(ipv6 === "on" ? "lan.ipv6OnHint" : "lan.ipv6OffHint")}
                field={form.register("ipv6")}
                options={[
                  { value: "on", label: t("lan.ipv6On") },
                  { value: "off", label: t("lan.ipv6Off") },
                ]}
              />
              {ipv6 === "on" &&
                (room === 1 && prefix ? (
                  <p className="-mt-2 text-xs text-base-content/70">
                    {t("lan.subnetWhole", { net: subnet64(prefix, 0) ?? prefix })}
                  </p>
                ) : (
                  <Field
                    label={t("lan.subnet")}
                    placeholder="0"
                    hint={
                      net6
                        ? t("lan.subnetHint", { max: (room - 1).toString(16), net: net6 })
                        : t("lan.subnetWait", { name: connName(t, w) })
                    }
                    error={errors.subnet?.message}
                    {...form.register("subnet")}
                  />
                ))}
            </>
          ) : (
            <div className="grid gap-1">
              <span className="text-sm font-semibold">{t("lan.ipv6")}</span>
              <p className="text-xs text-base-content/70">
                {t("lan.ipv6None", { name: w ? connName(t, w) : "" })}
              </p>
            </div>
          )}
        </div>
        <Foot
          editing={!!x}
          disabled={(!!x && !isDirty) || change.isPending || !!clash}
          state={state}
          onCancel={onCancel}
        />
      </form>
    </>
  );
}

/**
 * The form: a network added, out through the first connection without one
 * unless another is chosen, or @editing changed. Once every connection has
 * its network, it says so in place of its fields, as the internet page's does
 * at its seven connections.
 */
function LanForm({
  places,
  all,
  editing,
  onDone,
  onClose,
}: {
  places: Place[];
  all: LanView[];
  editing: LanView | null;
  onDone: () => void;
  /** Shuts the form; none while it is the page's only content, with no network yet. */
  onClose: (() => void) | undefined;
}) {
  const { t } = useI18n();
  /* A new form after each network added. */
  const [round, setRound] = useState(0);
  const free = places.filter((p) => !p.lan);
  const conn = free[0]?.c.key ?? "";
  return (
    <section
      className="flex flex-col rounded-box border border-base-300 bg-base-100 xl:h-full xl:min-h-0"
      aria-labelledby="lan-form"
    >
      <div className="flex min-h-14 shrink-0 items-center gap-2 px-4 pt-4 pb-2">
        <h2 id="lan-form" className="text-base font-semibold">
          {editing ? t("lan.editTitle", { name: netName(t, editing, places) }) : t("lan.addTitle")}
        </h2>
        {(editing || onClose) && (
          <button
            type="button"
            className="btn btn-ghost btn-sm btn-square ml-auto text-base-content/70"
            onClick={editing ? onDone : onClose}
            aria-label={t("internet.cancel")}
            title={t("internet.cancel")}
          >
            <X className="size-4" />
          </button>
        )}
      </div>
      {editing || conn ? (
        <LanFields
          key={`${editing?.id ?? "new"}:${conn}:${round}`}
          x={editing ?? undefined}
          conn={conn}
          free={free}
          all={all}
          places={places}
          onDone={() => {
            if (!editing) setRound((r) => r + 1);
            onDone();
          }}
          onCancel={onDone}
        />
      ) : (
        /* The buttons stay, the one that adds off, so that nothing moves. */
        <>
          <p className="flex h-11 shrink-0 items-center border-b border-base-300 px-4" />
          <div className="flex min-h-0 flex-1 flex-col">
            <div className="min-h-0 flex-1 p-4 xl:overflow-auto">
              <div className="rounded-box border border-dashed border-base-300 p-6 text-center">
                <p className="font-semibold">
                  {places.length ? t("lan.allHave") : t("lan.noRouters")}
                </p>
                <p className="mt-1 text-xs text-base-content/70">
                  {places.length ? t("lan.allHaveHint") : t("lan.noRoutersHint")}
                </p>
              </div>
            </div>
            <Foot editing={false} disabled state={{ state: "idle" }} />
          </div>
        </>
      )}
    </section>
  );
}

/*
 * The devices' table: its columns, the same in its head and in every line,
 * as wide as their longest values (a MAC, 192.168.255.254, a network's name cut short,
 * "22 godz. 30 min", a button), so that they stand still as a search
 * narrows the list. Below 56rem, about what their least widths and gaps
 * take with a scroll bar, a device takes two lines: never a scroll across.
 */
const DEVICE_COLS = "grid-cols-[minmax(8rem,1fr)_8rem_8rem_7rem_8rem_6rem]";
/*
 * In a narrow list: the name, the address and the button, the same in the
 * head and every line; as narrow as a phone, the button is its icon alone.
 */
const NARROW =
  "@max-4xl:grid-cols-[minmax(0,1fr)_8rem_5.5rem] @max-4xl:gap-x-3 @max-lg:grid-cols-[minmax(0,1fr)_7.5rem_1.5rem]";

/** A device of the table: one with an address now, one with a fixed address, or both. */
export interface Device {
  lan: number;
  mac: string;
  /** The name given it by hand, else its own, if any. */
  name: string | null;
  /** Its own name, as it gives it. */
  own: string | null;
  /** The address it has now, and how long it keeps it, while it is on. */
  now: string | null;
  left: number;
  fixed: FixedInfo | undefined;
  /** The IPv6 addresses the stick sees it use. */
  v6: string[];
}

/** The devices of the networks @ids: those on now, and those with a fixed address that are off. */
export function useDevices(ids: number[]): Device[] {
  const { data: live } = useLive();
  const pins = fixed(live).filter((f) => ids.includes(f.lan));
  const on = leases(live).filter((l) => ids.includes(l.lan));
  const pinOf = (lan: number, m: string) => pins.find((f) => f.lan === lan && f.mac === m);
  const list: Device[] = [
    ...on.map((l) => {
      const f = pinOf(l.lan, l.mac);
      return {
        lan: l.lan,
        mac: l.mac,
        name: f?.name || l.name,
        own: l.name,
        now: l.ip,
        left: l.left_s,
        fixed: f,
        v6: l.ipv6 ?? [],
      };
    }),
    ...pins
      .filter((f) => !on.some((l) => l.lan === f.lan && l.mac === f.mac))
      .map((f) => ({
        lan: f.lan,
        mac: f.mac,
        name: f.name || null,
        own: null,
        now: null,
        left: 0,
        fixed: f,
        v6: [],
      })),
  ];
  const at = (d: Device) => ip4(d.fixed?.ip ?? d.now ?? "") ?? 0;
  return list.sort((x, y) => at(x) - at(y));
}

/**
 * Every device of @list, a line each: its name, MAC, address, network, its
 * lease, and the button that fixes its address or changes the fixed one; in
 * a narrow list two lines, the name, the address and the button first. A
 * fixed address shows where the device has it, or will have it at its next
 * renewal.
 */
function DeviceTable({
  list,
  places,
  onFix,
}: {
  list: Device[];
  places: Place[];
  onFix: (d: Device) => void;
}) {
  const { t, lang } = useI18n();
  const netOf = (id: number) => {
    const l = places.find((x) => x.lan?.id === id)?.lan;
    return l ? netName(t, l, places) : "–";
  };
  /* The devices whose IPv6 addresses are open under their line. */
  const [open, setOpen] = useState<string[]>([]);
  const toggle = (key: string) =>
    setOpen((o) => (o.includes(key) ? o.filter((k) => k !== key) : [...o, key]));
  return (
    <div className={`grid ${DEVICE_COLS} gap-x-6 gap-y-2 pb-2 @max-4xl:flex @max-4xl:flex-col`}>
      {/* In a narrow list the head names the first line's columns alone. */}
      <div
        className={`sticky top-0 z-10 col-span-full mb-2 grid h-11 grid-cols-subgrid items-center border-x border-b border-x-transparent border-b-base-300 bg-base-200 px-4 text-xs font-semibold text-base-content/70 @max-4xl:mb-1 ${NARROW}`}
      >
        <span className="pl-5">{t("lan.colDevice")}</span>
        <span className="@max-4xl:hidden">{t("lan.colMac")}</span>
        <span className="@max-4xl:col-start-2 @max-4xl:text-right">{t("lan.colIp")}</span>
        <span className="@max-4xl:hidden">{t("lan.colNet")}</span>
        <span className="@max-4xl:hidden">{t("lan.colLease")}</span>
        <span />
      </div>
      <div className="col-span-full grid grid-cols-subgrid rounded-box border border-base-300 bg-base-100">
        {list.map((d) => {
          const name = d.name ?? t("lan.noName");
          const lease = d.fixed
            ? d.now
              ? t("lan.fixedOn")
              : t("lan.off")
            : duration(d.left, lang);
          const moved = d.fixed && d.now && d.now !== d.fixed.ip;
          const key = `${d.lan}:${d.mac}`;
          const v6 = open.includes(key);
          return (
            <div
              key={key}
              className={`col-span-full grid grid-cols-subgrid items-baseline border-b border-base-300 px-4 py-2.5 text-sm last:border-b-0 @max-4xl:gap-y-0.5 ${NARROW}`}
            >
              <span className="flex min-w-0 items-baseline gap-1.5">
                {d.fixed ? (
                  <Pin
                    className="size-3.5 shrink-0 self-center text-primary"
                    aria-label={t("lan.fixedMark")}
                  />
                ) : (
                  <span className="size-3.5 shrink-0" />
                )}
                <span className={`truncate ${d.name ? "" : "text-base-content/60"}`}>{name}</span>
              </span>
              <span className="font-mono text-xs text-base-content/70 @max-4xl:hidden">
                {d.mac}
              </span>
              <span className="font-mono @max-4xl:col-start-2 @max-4xl:row-start-1 @max-4xl:text-right">
                {d.fixed?.ip ?? d.now}
                {moved && (
                  <span className="block font-sans text-xs text-base-content/60">
                    {t("lan.now", { ip: d.now ?? "" })}
                  </span>
                )}
                {d.v6.length > 0 && (
                  <span className="block font-sans text-xs">
                    <button
                      type="button"
                      className="link link-hover text-base-content/60"
                      aria-expanded={v6}
                      aria-controls={`v6-${key}`}
                      onClick={() => toggle(key)}
                    >
                      {t("lan.v6Count", { n: d.v6.length })}
                    </button>
                  </span>
                )}
              </span>
              <span className="truncate @max-4xl:hidden">{netOf(d.lan)}</span>
              <span
                className={`whitespace-nowrap @max-4xl:hidden ${d.fixed && !d.now ? "text-base-content/60" : ""}`}
              >
                {lease}
              </span>
              <span className="-my-1 flex justify-end @max-4xl:col-start-3 @max-4xl:row-start-1">
                {d.fixed ? (
                  <button
                    type="button"
                    className="btn btn-ghost btn-xs btn-square text-base-content/70"
                    onClick={() => onFix(d)}
                    aria-label={t("lan.fixedEdit", { name })}
                    title={t("lan.fixedEdit", { name })}
                  >
                    <Pencil className="size-4" />
                  </button>
                ) : (
                  <button
                    type="button"
                    className="btn btn-ghost btn-xs text-primary @max-lg:btn-square"
                    onClick={() => onFix(d)}
                    title={t("lan.pinTitle")}
                  >
                    <Pin className="size-3.5" />
                    <span className="@max-lg:sr-only">{t("lan.pin")}</span>
                  </button>
                )}
              </span>
              <span className="col-span-full hidden pl-5 text-xs text-base-content/70 @max-4xl:block">
                <span className="font-mono">{d.mac}</span> · {netOf(d.lan)} · {lease}
              </span>
              {/* Its IPv6 addresses, under the address column, or across a narrow line. */}
              {v6 && (
                <div
                  id={`v6-${key}`}
                  className="col-span-3 col-start-3 grid gap-0.5 pt-1 font-mono text-xs text-base-content/70 @max-4xl:col-span-full @max-4xl:col-start-1 @max-4xl:pl-5"
                >
                  {d.v6.map((a) => (
                    <span key={a} className="break-all">
                      {a}
                    </span>
                  ))}
                </div>
              )}
            </div>
          );
        })}
      </div>
    </div>
  );
}

/** What the fixed-address form is open on: a device's address to fix, or a fixed one, or a new one. */
interface FixTarget {
  /** A device with an address now, or with a fixed one; none for a device not seen yet. */
  d: Device | null;
}

/* The first address of @lan that no device, fixed address or pool takes, for a new fixed one. */
function freeIp(lan: LanView, taken: string[]) {
  const net = net4(lan.address);
  if (!net) return "";
  const stick = ip4(lan.address.split("/")[0] ?? "");
  const from = lan.dhcp ? ip4(lan.poolFrom) : null;
  const to = lan.dhcp ? ip4(lan.poolTo) : null;
  const used = new Set(taken.map((x) => ip4(x)));
  for (let a = net.first + 1; a < net.last; a++) {
    if (a === stick || used.has(a) || (from != null && to != null && a >= from && a <= to))
      continue;
    return [a >>> 24, (a >>> 16) & 255, (a >>> 8) & 255, a & 255].join(".");
  }
  return "";
}

/**
 * The form of a fixed address, at the devices' right while it is open: a
 * device's address fixed, a fixed one changed or taken away, or one added for
 * a device not seen yet, by its MAC. The address lies in the network, is not
 * the stick's, and no other device has it, fixed or now.
 */
function FixedForm({
  target,
  nets,
  onClose,
}: {
  target: FixTarget;
  nets: Net[];
  onClose: () => void;
}) {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const change = useFixedChange();
  const state = useChangeState(change);
  const d = target.d;
  const editing = !!d?.fixed;
  const pins = fixed(live);
  const on = leases(live);
  /* A new fixed address goes in a network whose DHCP server gives addresses. */
  const choices = d ? nets.filter((p) => p.lan.id === d.lan) : nets.filter((p) => p.lan.dhcp);
  const lanOf = (id: string) => choices.find((p) => String(p.lan.id) === id)?.lan;
  const first = choices[0]?.lan;
  const schema = z
    .object({
      lan: z.string().check(z.minLength(1, t("form.required"))),
      mac: mac(t).check(z.minLength(1, t("form.required"))),
      name: upTo(63),
      ip: ipv4(t).check(z.minLength(1, t("form.required"))),
    })
    .check(
      z.superRefine((v, ctx) => {
        const lan = lanOf(v.lan);
        const net = lan ? net4(lan.address) : null;
        const a = ip4(v.ip);
        const m = v.mac.toLowerCase();
        const fail = (path: "ip" | "mac", message: string) =>
          ctx.addIssue({ code: "custom", path: [path], message, input: v[path] });
        if (!lan || !net || a == null) return;
        if (!d) {
          const dup = pins.find((f) => f.lan === lan.id && f.mac === m);
          if (dup) fail("mac", t("lan.fixedDup", { ip: dup.ip }));
        }
        if (a <= net.first || a >= net.last)
          return fail("ip", t("lan.fixedOut", { net: net.text }));
        if (a === ip4(lan.address.split("/")[0] ?? "")) return fail("ip", t("lan.fixedStick"));
        const other = pins.find((f) => f.lan === lan.id && f.ip === v.ip && f.mac !== m);
        const own = other && on.find((l) => l.lan === other.lan && l.mac === other.mac)?.name;
        if (other) return fail("ip", t("lan.fixedTaken", { name: other.name || own || other.mac }));
        const busy = on.find((l) => l.lan === lan.id && l.ip === v.ip && l.mac !== m);
        if (busy) fail("ip", t("lan.fixedBusy", { name: busy.name ?? busy.mac }));
      }),
    );
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({
    resolver: zodResolver(schema),
    defaultValues: d
      ? {
          lan: String(d.lan),
          mac: d.mac,
          name: d.fixed?.name ?? "",
          ip: d.fixed?.ip ?? d.now ?? "",
        }
      : {
          lan: first ? String(first.id) : "",
          mac: "",
          name: "",
          ip: first
            ? freeIp(first, [
                ...pins.filter((f) => f.lan === first.id).map((f) => f.ip),
                ...on.filter((l) => l.lan === first.id).map((l) => l.ip),
              ])
            : "",
        },
  });
  const { errors, isDirty } = form.formState;
  const [lanId, ip] = form.watch(["lan", "ip"]);
  const lan = lanOf(lanId);
  const net = lan ? net4(lan.address) : null;
  const send = (v: Values) =>
    change.mutate(
      { LAN: v.lan, MAC: v.mac.toLowerCase(), IP: v.ip, NAME: v.name },
      { onSuccess: onClose },
    );
  const unpin = () =>
    d && change.mutate({ LAN: String(d.lan), MAC: d.mac, IP: "" }, { onSuccess: onClose });
  const title = editing
    ? t("lan.fixedEditTitle", { name: d?.name ?? d?.mac ?? "" })
    : d
      ? t("lan.pinTitle")
      : t("lan.fixedAdd");
  return (
    <section
      className="flex flex-col rounded-box border border-base-300 bg-base-100 xl:h-full xl:min-h-0"
      aria-labelledby="fixed-form"
    >
      <div className="flex min-h-14 shrink-0 items-center gap-2 px-4 pt-4 pb-2">
        <h2 id="fixed-form" className="min-w-0 truncate text-base font-semibold">
          {title}
        </h2>
        <button
          type="button"
          className="btn btn-ghost btn-sm btn-square ml-auto text-base-content/70"
          onClick={onClose}
          aria-label={t("internet.cancel")}
          title={t("internet.cancel")}
        >
          <X className="size-4" />
        </button>
      </div>
      {/* The network the address lies in, and its pool, as high as the list's head. */}
      <p className="flex h-11 shrink-0 items-center border-b border-base-300 px-4 text-xs text-base-content/70">
        {lan && net
          ? lan.dhcp
            ? t("lan.fixedStrip", { net: net.text, pool: pool(lan.poolFrom, lan.poolTo) })
            : t("lan.fixedStripOff", { net: net.text })
          : ""}
      </p>
      <form onSubmit={form.handleSubmit(send)} noValidate className="flex min-h-0 flex-1 flex-col">
        <div className="grid min-h-0 flex-1 content-start gap-4 p-4 xl:overflow-auto">
          <div className="grid content-start gap-1">
            <label htmlFor="fixed-lan" className="text-sm font-semibold">
              {t("lan.colNet")}
            </label>
            <select
              id="fixed-lan"
              className="select w-full"
              disabled={!!d}
              {...form.register("lan")}
            >
              {choices.map((p) => (
                <option key={p.lan.id} value={p.lan.id}>
                  {`${netName(t, p.lan, nets)} · ${net4(p.lan.address)?.text ?? p.lan.address}`}
                </option>
              ))}
            </select>
          </div>
          <Field
            label={t("lan.fixedMac")}
            placeholder="00:00:5e:00:53:00"
            hint={d ? undefined : t("lan.fixedMacHint")}
            disabled={!!d}
            error={errors.mac?.message}
            {...form.register("mac")}
          />
          <Field
            label={t("lan.fixedName")}
            placeholder={d?.own ?? ""}
            hint={t("lan.fixedNameHint")}
            error={errors.name?.message}
            {...form.register("name")}
          />
          <Field
            label={t("lan.fixedIp")}
            placeholder={net ? net.text.split("/")[0] : "192.168.1.10"}
            hint={
              d?.now && ip !== d.now
                ? t("lan.fixedMoves", { time: duration(d.left, lang) })
                : t("lan.fixedIpHint")
            }
            error={errors.ip?.message}
            {...form.register("ip")}
          />
        </div>
        <div className="flex min-h-16 shrink-0 flex-wrap items-center gap-3 border-t border-base-300 px-4 py-2.5">
          <button
            type="submit"
            className="btn btn-primary"
            disabled={(editing && !isDirty) || change.isPending || !lan}
          >
            {editing ? t("internet.save") : d ? t("lan.pin") : t("lan.fixedAddShort")}
          </button>
          {editing && (
            <button
              type="button"
              className="btn btn-ghost hover:text-error"
              onClick={unpin}
              disabled={change.isPending}
            >
              {t("lan.unpin")}
            </button>
          )}
          <SaveState {...state} />
        </div>
      </form>
    </section>
  );
}

/** A connection that makes the stick a router, with the network it has. */
export type Net = Place & { lan: LanView };

/** A network: the connection it goes out through, its addresses, VLAN and devices. */
function NetCard({
  p,
  editing,
  onEdit,
  onDevices,
}: {
  p: Net;
  editing: boolean;
  onEdit: () => void;
  onDevices: () => void;
}) {
  const { t } = useI18n();
  const change = useLanChange();
  const state = useChangeState(change);
  const x = p.lan;
  const list = leases(useLive().data).filter((l) => l.lan === x.id);
  const net = net4(x.address);
  const remove = () => {
    if (!window.confirm(t("lan.removeConfirm", { name: netName(t, x, [p]) }))) return;
    change.mutate({ ID: String(x.id), CONN: "" });
  };
  return (
    <div
      className={`${CARD} ${editing ? "border-primary ring-1 ring-primary" : "border-base-300"}`}
    >
      <span className={AT.caret} />
      <Cell at={AT.name}>
        <span className="truncate font-semibold">{netName(t, x, [p])}</span>
        <Sub>{wan(t, p.c)}</Sub>
      </Cell>
      <Cell head={t("lan.colAddr")} at={AT.first}>
        <Mono>{net?.text ?? x.address}</Mono>
        <Sub>
          {t("lan.stickAt")} <Mono>{x.address.split("/")[0]}</Mono>
        </Sub>
        <Sub>
          {!x.ipv6 || p.c.x.family === "ipv4" ? (
            t("lan.ipv6OffShort")
          ) : p.c.x.prefix ? (
            <Mono>{subnet64(p.c.x.prefix, x.subnet6) ?? p.c.x.prefix}</Mono>
          ) : (
            t("lan.ipv6Wait")
          )}
        </Sub>
      </Cell>
      <Cell head={t("lan.colVlan")} at={AT.second}>
        <span>{x.vlan ? `VLAN ${x.vlan}` : t("switch.untagged")}</span>
      </Cell>
      <Cell head={t("lan.colDevices")} at={AT.third}>
        <button
          type="button"
          className="link link-hover -my-0.5 w-fit text-left"
          onClick={onDevices}
          title={t("lan.showDevices", { name: netName(t, x, [p]) })}
        >
          {list.length}
        </button>
        <Sub>
          {x.dhcp ? (
            <>
              DHCP <Mono>{pool(x.poolFrom, x.poolTo)}</Mono>
            </>
          ) : (
            t("lan.dhcpOffShort")
          )}
        </Sub>
        {state.state === "bad" && <p className="text-xs text-error">{state.message}</p>}
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

/**
 * The networks there are, of the @places a network can go out through: one
 * at most for each connection that makes the stick a router.
 */
function Places({
  places,
  editing,
  onEdit,
  onDevices,
  onAdd,
}: {
  places: Place[];
  editing: number | null;
  onEdit: (l: LanView) => void;
  onDevices: (lan: number) => void;
  /** Opens the form on a new network; none while every connection has one. */
  onAdd: (() => void) | undefined;
}) {
  const { t } = useI18n();
  const nets = places.filter((p): p is Net => p.lan != null);
  const devices = leases(useLive().data).filter((l) => nets.some((p) => p.lan.id === l.lan)).length;
  return (
    <section aria-labelledby="lans" className="@container flex flex-col xl:h-full xl:min-h-0">
      <div className="mt-px flex min-h-14 shrink-0 items-center gap-3 pt-4 pb-2">
        <h2 id="lans" className="text-base font-semibold">
          {t("lan.lans")}
        </h2>
        <button
          type="button"
          className="btn btn-primary btn-sm ml-auto"
          onClick={onAdd}
          disabled={!onAdd}
          title={onAdd ? undefined : places.length ? t("lan.allHave") : t("lan.noRouters")}
        >
          <Plus className="size-4" />
          {t("lan.addTitle")}
        </button>
      </div>
      <div className="min-h-0 flex-1 px-px xl:overflow-auto">
        {nets.length === 0 ? (
          <div className="mt-4 rounded-box border border-dashed border-base-300 px-6 py-8 text-center">
            <p className="text-base font-semibold">
              {places.length ? t("lan.empty") : t("lan.noRouters")}
            </p>
            <p className="mx-auto mt-1 max-w-prose text-base-content/70">
              {places.length ? t("lan.emptyHint") : t("lan.noRoutersHint")}
            </p>
          </div>
        ) : (
          <div className={`grid grid-cols-[2rem_minmax(8rem,1fr)_auto_auto_auto_auto] ${ROWS}`}>
            <div className={HEAD}>
              <span />
              <span className="@max-3xl:hidden">{t("lan.colNet")}</span>
              <span>{t("lan.colAddr")}</span>
              <span>{t("lan.colVlan")}</span>
              <span>{t("lan.colDevices")}</span>
              <span />
            </div>
            {nets.map((p) => (
              <NetCard
                key={p.c.key}
                p={p}
                editing={editing === p.lan.id}
                onEdit={() => onEdit(p.lan)}
                onDevices={() => onDevices(p.lan.id)}
              />
            ))}
          </div>
        )}
      </div>
      <p className="mb-px flex min-h-16 shrink-0 items-center border-t border-base-300 text-xs text-base-content/70">
        {t("lan.count", { n: nets.length, max: places.length, d: devices })}
      </p>
    </section>
  );
}

/**
 * Every device of the networks across the page, searched by name, address
 * or MAC, narrowed to the network @net, or none for all of them, or to the
 * fixed addresses; with the form of a fixed address at the right while one
 * is being fixed, changed or added.
 */
function Devices({
  places,
  net,
  onNet,
}: {
  places: Place[];
  net: number | null;
  onNet: (lan: number | null) => void;
}) {
  const { t } = useI18n();
  const nets = places.filter((p): p is Net => p.lan != null);
  const ids = nets.map((p) => p.lan.id);
  const all = useDevices(ids);
  const [q, setQ] = useState("");
  const [only, setOnly] = useState(false);
  const [fix, setFix] = useState<FixTarget | null>(null);
  const on = net != null && ids.includes(net) ? net : null;
  const words = q.trim().toLowerCase();
  const shown = all.filter(
    (d) =>
      (on == null || d.lan === on) &&
      (!only || d.fixed) &&
      (!words ||
        `${d.name ?? ""} ${d.own ?? ""} ${d.now ?? ""} ${d.fixed?.ip ?? ""} ${d.mac} ${d.v6.join(" ")}`
          .toLowerCase()
          .includes(words)),
  );
  const pinned = all.filter((d) => d.fixed).length;
  const canAdd = nets.some((p) => p.lan.dhcp);
  return (
    <div className={withPanel(fix != null)}>
      <section
        aria-label={t("lan.colDevices")}
        className="@container flex min-h-0 flex-col xl:h-full"
      >
        <div className="mt-px flex min-h-14 shrink-0 flex-wrap items-center gap-3 pt-4 pb-2">
          <Filter
            value={q}
            onChange={setQ}
            placeholder={t("lan.search")}
            className="@max-lg:basis-full @lg:w-72"
          />
          {ids.length > 1 && (
            <select
              className="select select-sm w-auto"
              aria-label={t("lan.colNet")}
              value={on ?? ""}
              onChange={(e) => onNet(e.currentTarget.value ? Number(e.currentTarget.value) : null)}
            >
              <option value="">{t("lan.allNets")}</option>
              {nets.map((p) => (
                <option key={p.lan.id} value={p.lan.id}>
                  {netName(t, p.lan, nets)}
                </option>
              ))}
            </select>
          )}
          <label className="label cursor-pointer gap-2 text-sm">
            <input
              type="checkbox"
              className="toggle toggle-sm"
              checked={only}
              onChange={(e) => setOnly(e.currentTarget.checked)}
            />
            {t("lan.onlyFixed")}
          </label>
          {canAdd && (
            <button
              type="button"
              className="btn btn-primary btn-sm ml-auto"
              onClick={() => setFix({ d: null })}
            >
              <Plus className="size-4" />
              {t("lan.fixedAdd")}
            </button>
          )}
        </div>
        <div className="min-h-0 flex-1 px-px xl:overflow-auto">
          {shown.length ? (
            <DeviceTable list={shown} places={places} onFix={(d) => setFix({ d })} />
          ) : (
            <p className="mt-4 rounded-box border border-dashed border-base-300 px-6 py-8 text-center text-base-content/70">
              {words
                ? t("lan.noMatch")
                : only
                  ? t("lan.fixedNone")
                  : all.length
                    ? t("lan.noMatch")
                    : t("lan.devicesNone")}
            </p>
          )}
        </div>
        <p className="mb-px flex min-h-16 shrink-0 items-center border-t border-base-300 text-xs text-base-content/70">
          {shown.length === all.length
            ? t("lan.devicesCount", { n: all.length, f: pinned })
            : t("lan.shown", { n: shown.length, all: all.length, f: pinned })}
        </p>
      </section>
      {fix && (
        <SidePanel onClose={() => setFix(null)}>
          <FixedForm
            key={fix.d ? `${fix.d.lan}:${fix.d.mac}` : "new"}
            target={fix}
            nets={nets}
            onClose={() => setFix(null)}
          />
        </SidePanel>
      )}
    </div>
  );
}

/** The connections that make the stick a router, each with its network if it has one. */
export function usePlaces() {
  const { data: live } = useLive();
  const all = lans(live);
  /* The connections that make the stick a router: whatever does not hand its address down. */
  const places: Place[] = conns(live)
    .filter((c) => c.kind !== "pppoe" || !c.x.pass)
    .map((c) => ({ c, lan: all.find((l) => l.conn === c.key) }));
  return { all, places };
}

/** How many networks there are, and how many devices in them, for the tabs that show them. */
export function useLanCounts() {
  const { places } = usePlaces();
  const ids = places.flatMap((p) => (p.lan ? [p.lan.id] : []));
  return { nets: ids.length, devices: useDevices(ids).length };
}

/**
 * The networks, and at their right while it is open, the form that adds one
 * or changes one; @onDevices opens a network's devices.
 */
export function LanNets({ onDevices }: { onDevices: (lan: number) => void }) {
  const { all, places } = usePlaces();
  const [editId, setEditId] = useState<number | null>(null);
  const editing = all.find((l) => l.id === editId) ?? null;
  /*
   * The form, shut until asked for while there are networks to look at; with
   * none yet, and a connection to make one for, it is open from the start.
   */
  const [open, setOpen] = useState(false);
  const form = useRef<HTMLDivElement>(null);
  const show = (id: number | null) => {
    setEditId(id);
    setOpen(true);
    requestAnimationFrame(() => form.current?.scrollIntoView({ block: "nearest" }));
  };
  const close = () => {
    setEditId(null);
    setOpen(false);
  };
  /* A network gone elsewhere leaves the form. */
  useEffect(() => {
    if (editId != null && !editing) setEditId(null);
  }, [editId, editing]);
  const free = places.some((p) => !p.lan);
  const first = free && !places.some((p) => p.lan);
  const shown = open || editing != null || first;
  return (
    <div className={withPanel(shown)}>
      <div className="min-h-0">
        <Places
          places={places}
          editing={editId}
          onEdit={(l) => show(l.id)}
          onDevices={onDevices}
          onAdd={free ? () => show(null) : undefined}
        />
      </div>
      {shown && (
        <SidePanel ref={form} onClose={first ? undefined : close}>
          <LanForm
            places={places}
            all={all}
            editing={editing}
            onDone={close}
            onClose={first ? undefined : close}
          />
        </SidePanel>
      )}
    </div>
  );
}

/** Every device of the networks, narrowed to the network @net, or all with none. */
export function LanDevices({
  net,
  onNet,
}: {
  net: number | null;
  onNet: (lan: number | null) => void;
}) {
  const { places } = usePlaces();
  return <Devices places={places} net={net} onNet={onNet} />;
}
