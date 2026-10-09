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
 * On the left the connections that make the stick a router, each with its
 * network, or a place for one; a network opens on its devices. On the right
 * the form that adds a network or changes one, the connection first: its
 * VLAN to the switch, the stick's address in it, and the DHCP server that
 * gives the devices theirs, with the operator's name servers. The panels
 * stand on the internet page's lines.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Pencil, Plus, Trash2, X } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { type UseFormRegisterReturn, useForm } from "react-hook-form";
import { useLanChange } from "../api/config";
import { useLive } from "../api/live";
import type { LeaseInfo } from "../api/types";
import { Acts, AT, CARD, Caret, Cell, HEAD, opens, ROWS } from "../components/CardTable";
import { Field, SaveState, useChangeState } from "../components/Field";
import { Page } from "../components/Page";
import { useI18n } from "../i18n";
import { duration } from "../lib/format";
import { ip4, net4 } from "../lib/ipv4";
import { ipv4, ipv4Cidr, vlan, z } from "../lib/schemas";
import { downVlan, type LanView, lans, leases, sessions } from "../lib/state";
import { type Conn, conns } from "./internet/conns";
import { connName, hostSide, Mono, Sub, type T, useServices } from "./internet/shared";

/* How long a device keeps its address: the choices, in seconds. */
const LEASES = ["3600", "43200", "86400", "604800"] as const;

/** A connection that makes the stick a router, and its network if it has one. */
interface Place {
  c: Conn;
  lan: LanView | undefined;
}

/* A connection's name with the VLAN it runs in. */
const wan = (t: T, c: Conn) => `${connName(t, c)} · VLAN ${c.vlan}`;

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
function Choices<V extends string>({
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
  const nameOf = (l: LanView) => {
    const p = places.find((q) => q.c.key === l.conn);
    return p ? connName(t, p.c) : l.conn;
  };
  const schema = z
    .object({
      conn: z.string().check(z.minLength(1, t("form.required"))),
      vlan: vlan(t),
      address: ipv4Cidr(t).check(z.minLength(1, t("form.required"))),
      dhcp: z.enum(["on", "off"]),
      poolFrom: ipv4(t),
      poolTo: ipv4(t),
      lease: z.enum(LEASES),
    })
    .check(
      /*
       * The network takes none of another's addresses; its pool lies in it,
       * in order, and leaves the stick's address out.
       */
      z.superRefine((v, ctx) => {
        const net = net4(v.address);
        const fail = (path: "address" | "poolFrom" | "poolTo", message: string) =>
          ctx.addIssue({ code: "custom", path: [path], message, input: v[path] });
        const over = net
          ? others.find((l) => {
              const o = net4(l.address);
              return o != null && net.first <= o.last && net.last >= o.first;
            })
          : undefined;
        if (net && over)
          fail("address", t("lan.netOverlap", { net: net.text, name: nameOf(over) }));
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
          vlan: x.vlan,
          address: x.address,
          dhcp: x.dhcp ? "on" : "off",
          poolFrom: x.poolFrom,
          poolTo: x.poolTo,
          lease: LEASES.find((l) => Number(l) === x.leaseS) ?? "86400",
        }
      : { conn, vlan: freeVid, dhcp: "on", lease: "86400", ...suggest(all) },
  });
  const { errors, isDirty } = form.formState;
  const [vid, address, dhcp] = form.watch(["vlan", "address", "dhcp"]);
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
        VLAN: v.vlan,
        ADDRESS: v.address,
        DHCP: v.dhcp,
        POOL_FROM: v.poolFrom,
        POOL_TO: v.poolTo,
        LEASE: v.lease,
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

/** The form: a network added, out through @pick when one was asked for, or @editing changed. */
function LanForm({
  places,
  all,
  editing,
  pick,
  onDone,
  onClose,
}: {
  places: Place[];
  all: LanView[];
  editing: LanView | null;
  pick: string;
  onDone: () => void;
  onClose: () => void;
}) {
  const { t } = useI18n();
  /* A new form after each network added. */
  const [round, setRound] = useState(0);
  const free = places.filter((p) => !p.lan);
  const conn = free.find((p) => p.c.key === pick)?.c.key ?? free[0]?.c.key ?? "";
  const of = editing ? places.find((p) => p.c.key === editing.conn) : undefined;
  return (
    <section
      className="flex flex-col rounded-box border border-base-300 bg-base-100 xl:h-full xl:min-h-0"
      aria-labelledby="lan-form"
    >
      <div className="flex min-h-14 shrink-0 items-center gap-2 px-4 pt-4 pb-2">
        <h2 id="lan-form" className="text-base font-semibold">
          {editing
            ? t("lan.editTitle", { name: of ? connName(t, of.c) : editing.conn })
            : t("lan.addTitle")}
        </h2>
        <button
          type="button"
          className={`btn btn-ghost btn-sm btn-square ml-auto text-base-content/70 ${editing ? "" : "xl:hidden"}`}
          onClick={editing ? onDone : onClose}
          aria-label={t("internet.cancel")}
          title={t("internet.cancel")}
        >
          <X className="size-4" />
        </button>
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

/** The devices of a network: what a network's card opens on. */
function Devices({ list }: { list: LeaseInfo[] }) {
  const { t, lang } = useI18n();
  if (!list.length) return <p className="text-base-content/70">{t("lan.devicesNone")}</p>;
  return (
    <div className="grid grid-cols-[minmax(0,1fr)_auto_auto] gap-x-6 gap-y-3">
      <span className="text-xs text-base-content/70">{t("lan.colDevice")}</span>
      <span className="text-xs text-base-content/70">{t("lan.colIp")}</span>
      <span className="text-xs text-base-content/70">{t("lan.colLease")}</span>
      {list.map((l) => (
        <div key={l.mac} className="contents">
          <div className="min-w-0">
            <div>{l.name ?? t("lan.noName")}</div>
            <Sub>
              <Mono>{l.mac}</Mono>
            </Sub>
          </div>
          <Mono>{l.ip}</Mono>
          <span>{t("lan.leaseLeft", { time: duration(l.left_s, lang) })}</span>
        </div>
      ))}
    </div>
  );
}

/** A connection that makes the stick a router: its network, or a place for one. */
function PlaceCard({
  p,
  open,
  onToggle,
  editing,
  onEdit,
  onAdd,
}: {
  p: Place;
  open: boolean;
  onToggle: () => void;
  editing: boolean;
  onEdit: () => void;
  onAdd: () => void;
}) {
  const { t } = useI18n();
  const change = useLanChange();
  const state = useChangeState(change);
  const list = leases(useLive().data).filter((l) => l.lan === p.lan?.id);
  const name = (
    <Cell at={AT.name}>
      <span className="font-semibold">{connName(t, p.c)}</span>
      <Sub>{`VLAN ${p.c.vlan}`}</Sub>
    </Cell>
  );
  if (!p.lan)
    return (
      <div className={`${CARD} border-dashed border-base-300`}>
        <span className={AT.caret} />
        {name}
        <Cell head={t("lan.colNet")} at={AT.first}>
          <span className="text-base-content/70">{t("lan.noLan")}</span>
        </Cell>
        <span className={AT.second} />
        <div className={`-my-1.5 ${AT.third}`}>
          <button type="button" className="btn btn-ghost btn-sm -ml-3 text-primary" onClick={onAdd}>
            <Plus className="size-4" />
            {t("lan.add")}
          </button>
        </div>
        <span className={AT.acts} />
      </div>
    );
  const x = p.lan;
  const net = net4(x.address);
  const more = `lan-${x.id}`;
  const remove = () => {
    if (!window.confirm(t("lan.removeConfirm", { name: connName(t, p.c) }))) return;
    change.mutate({ ID: String(x.id), CONN: "" });
  };
  return (
    // biome-ignore lint/a11y/useKeyWithClickEvents lint/a11y/noStaticElementInteractions: the caret is its key
    <div
      className={`${CARD} cursor-pointer ${editing ? "border-primary ring-1 ring-primary" : "border-base-300"}`}
      onClick={opens(onToggle)}
    >
      <Caret
        open={open}
        controls={more}
        label={t("lan.details", { name: connName(t, p.c) })}
        onToggle={onToggle}
      />
      {name}
      <Cell head={t("lan.colNet")} at={AT.first}>
        <Mono>{net?.text ?? x.address}</Mono>
        <Sub>
          {t("lan.stickAt")} <Mono>{x.address.split("/")[0]}</Mono>
        </Sub>
      </Cell>
      <Cell head={t("lan.colVlan")} at={AT.second}>
        <span>{x.vlan ? `VLAN ${x.vlan}` : t("switch.untagged")}</span>
      </Cell>
      <Cell head={t("lan.colDevices")} at={AT.third}>
        <span>{list.length}</span>
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
      {open && (
        <div id={more} data-more className={`col-span-5 col-start-2 cursor-auto pb-1 ${AT.more}`}>
          <Devices list={list} />
        </div>
      )}
    </div>
  );
}

/** The connections that make the stick a router, each with its network or a place for one. */
function Places({
  places,
  editing,
  onEdit,
  onAdd,
}: {
  places: Place[];
  editing: number | null;
  onEdit: (l: LanView) => void;
  onAdd: (conn: string) => void;
}) {
  const { t } = useI18n();
  const all = leases(useLive().data);
  const [open, setOpen] = useState<number[]>([]);
  const toggle = (id: number) =>
    setOpen((o) => (o.includes(id) ? o.filter((k) => k !== id) : [...o, id]));
  const nets = places.filter((p) => p.lan).length;
  const devices = all.filter((l) => places.some((p) => p.lan?.id === l.lan)).length;
  return (
    <section aria-labelledby="lans" className="@container flex flex-col xl:h-full xl:min-h-0">
      <div className="mt-px flex min-h-14 shrink-0 items-center pt-4 pb-2">
        <h2 id="lans" className="text-base font-semibold">
          {t("lan.lans")}
        </h2>
      </div>
      <div className="min-h-0 flex-1 px-px xl:overflow-auto">
        {places.length === 0 ? (
          <div className="mt-4 rounded-box border border-dashed border-base-300 px-6 py-8 text-center">
            <p className="text-base font-semibold">{t("lan.noRouters")}</p>
            <p className="mx-auto mt-1 max-w-prose text-base-content/70">
              {t("lan.noRoutersHint")}
            </p>
          </div>
        ) : (
          <div className={`grid grid-cols-[2rem_minmax(8rem,1fr)_auto_auto_auto_auto] ${ROWS}`}>
            <div className={HEAD}>
              <span />
              <span className="@max-3xl:hidden">{t("lan.colConn")}</span>
              <span>{t("lan.colNet")}</span>
              <span>{t("lan.colVlan")}</span>
              <span>{t("lan.colDevices")}</span>
              <span />
            </div>
            {places.map((p) => (
              <PlaceCard
                key={p.c.key}
                p={p}
                open={!!p.lan && open.includes(p.lan.id)}
                onToggle={() => p.lan && toggle(p.lan.id)}
                editing={!!p.lan && editing === p.lan.id}
                onEdit={() => p.lan && onEdit(p.lan)}
                onAdd={() => onAdd(p.c.key)}
              />
            ))}
          </div>
        )}
      </div>
      <p className="mb-px flex min-h-16 shrink-0 items-center border-t border-base-300 text-xs text-base-content/70">
        {t("lan.count", { n: nets, d: devices })}
      </p>
    </section>
  );
}

export function Lan() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const all = lans(live);
  /* The connections that make the stick a router: whatever does not hand its address down. */
  const places: Place[] = conns(live)
    .filter((c) => c.kind !== "pppoe" || !c.x.pass)
    .map((c) => ({ c, lan: all.find((l) => l.conn === c.key) }));
  const [editId, setEditId] = useState<number | null>(null);
  const editing = all.find((l) => l.id === editId) ?? null;
  const [pick, setPick] = useState("");
  /* The form on a narrow screen, shut until asked for. */
  const [open, setOpen] = useState(false);
  const form = useRef<HTMLDivElement>(null);
  const show = () => {
    setOpen(true);
    requestAnimationFrame(() => form.current?.scrollIntoView({ block: "nearest" }));
  };
  /* A network gone elsewhere leaves the form. */
  useEffect(() => {
    if (editId != null && !editing) setEditId(null);
  }, [editId, editing]);
  const shown = open || editing != null;

  return (
    <Page title={t("lan.title")} hint={t("lan.hint")}>
      <div className="grid min-h-0 flex-1 content-start gap-6 xl:grid-cols-[minmax(0,1fr)_25rem] xl:content-stretch xl:overflow-hidden">
        <div className="min-h-0">
          <Places
            places={places}
            editing={editId}
            onEdit={(l) => {
              setEditId(l.id);
              show();
            }}
            onAdd={(conn) => {
              setEditId(null);
              setPick(conn);
              show();
            }}
          />
        </div>
        {/* On a narrow screen the form, or the button that opens it, comes first. */}
        <div ref={form} className="min-h-0 max-xl:order-first xl:flex xl:flex-col">
          {!shown && (
            <button type="button" className="btn btn-primary w-full xl:hidden" onClick={show}>
              <Plus className="size-4" />
              {t("lan.addTitle")}
            </button>
          )}
          <div className={`xl:min-h-0 xl:flex-1 ${shown ? "" : "max-xl:hidden"}`}>
            <LanForm
              places={places}
              all={all}
              editing={editing}
              pick={pick}
              onDone={() => {
                setEditId(null);
                setPick("");
                setOpen(false);
              }}
              onClose={() => setOpen(false)}
            />
          </div>
        </div>
      </div>
    </Page>
  );
}
