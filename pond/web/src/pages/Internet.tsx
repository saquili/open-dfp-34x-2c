// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The internet: how the stick takes its address from the operator, a tab
 * for each way. Without anything set here the OLT makes the stick a
 * transparent bridge, and the host dials the operator through it.
 *
 * PPPoE: the seven slots of the session engine, a row each. A slot takes one
 * service: the stick holds its PPPoE session itself, in its switch's
 * hardware, and is the router of the devices behind it, or hands the public
 * address down to the host. A slot is bound to the service, by the VID the
 * operator names it with: when the OLT moves the service to another VID on
 * the fibre or another channel, the session follows. The address goes down
 * to the host in that same VID, unless the slot names another, or none; in
 * one VLAN towards the host one slot at most hands its IPv4 address down,
 * and one its IPv6 prefix.
 *
 * Static IP and DHCP: a connection whose address is typed in or comes from
 * the operator's DHCP; the stick is the router (internet/Routed.tsx).
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Plus } from "lucide-react";
import { useRef, useState } from "react";
import { useForm } from "react-hook-form";
import { useNetwork, useSessionChange } from "../api/config";
import { useLive } from "../api/live";
import type { Family } from "../api/types";
import { Field, SaveState, useChangeState } from "../components/Field";
import { Page, Tabs, useTab } from "../components/Page";
import { Cell, Row, Table } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { mac, required, upTo, vlan, z } from "../lib/schemas";
import {
  downVlan,
  gems,
  hostLink,
  ipoes,
  MAX_SESSIONS,
  type SessionView,
  session,
  sessions,
} from "../lib/state";
import { RoutedTab } from "./internet/Routed";
import {
  ActionsCell,
  Choice,
  Dialog,
  FAMILY,
  FamilyField,
  hostSide,
  Mono,
  MonoList,
  RouterCell,
  ServiceCell,
  ServiceSelect,
  StateCell,
  Sub,
  serviceName,
  serviceOf,
  serviceVid,
  useServices,
} from "./internet/shared";

/* The rows of the session engine, numbered as the sessions are. */
const SLOT_IDS = Array.from({ length: MAX_SESSIONS }, (_, i) => i + 1);

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
      <ServiceSelect
        field={form.register("service", {
          onChange: (e) => {
            if (form.getValues("hostVlan") === prevService.current)
              form.setValue("hostVlan", e.target.value, { shouldDirty: true });
            prevService.current = e.target.value;
          },
        })}
        list={list}
        gone={gone}
        picked={picked}
      />
      <FamilyField field={form.register("family")} hint={t("internet.familyHint")} />
      <Field
        label={t("internet.mac")}
        hint={t("internet.macHint")}
        error={errors.mac?.message}
        {...form.register("mac")}
      />
      <fieldset className="grid gap-2">
        <legend className="mb-1 text-sm font-semibold">{t("internet.kind")}</legend>
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

/** A slot with its session: one row, with all there is to know of it at once. */
function SlotRow({ x, onEdit }: { x: SessionView; onEdit: () => void }) {
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
  const shared = first && first.id !== x.id ? first : undefined;
  const macAddr = shared ? shared.mac : x.mac;
  /* The MTU the session came to, which the host is given: 1500 when the concentrator took RFC 4638. */
  const mtu = (x.up && net.data?.links.find((l) => l.name === x.ppp)?.mtu) || null;
  const v4 = x.family !== "ipv6";
  const v6 = x.family !== "ipv4";
  /* The session's own, else, from a stick of before, the one handed on. */
  const dns = x.dns.length ? x.dns : x.handsOn ? s.dns : [];
  const gateway = x.up ? (x.gateway ?? (x.handsOn ? link.hostGateway : null)) : null;
  const channel =
    portId != null ? `GEM ${portId}` : x.gem === "auto" ? t("internet.autoGem") : `GEM ${x.gem}`;
  const down = downVlan(x);

  const remove = () => {
    if (!window.confirm(t("internet.removeSessionConfirm", { id: x.id }))) return;
    change.mutate({ ID: String(x.id), USER: "" });
  };

  return (
    <Row>
      <StateCell id={x.id} up={x.up} word={x.up ? t("internet.up") : t("internet.connecting")} />
      <ServiceCell service={bound} vlan={x.vlan} channel={channel} />
      <Cell head={t("internet.colSession")}>
        <span className="break-all">{x.user}</span>
        <Sub>
          {t(FAMILY[x.family])}
          {x.dev && (
            <>
              {" · "}
              <Mono>{[x.dev, x.ppp].filter(Boolean).join(" → ")}</Mono>
            </>
          )}
        </Sub>
        <Sub>
          MAC {macAddr ? <Mono>{macAddr}</Mono> : t("internet.macStick")}
          {shared && `, ${t("internet.macShared", { id: shared.id })}`}
        </Sub>
      </Cell>
      <Cell head={t("internet.colOperator")}>
        {v4 && <span>{x.ipv4 ? <Mono>{x.ipv4}</Mono> : "IPv4 –"}</span>}
        {v6 && (
          <span>
            {x.prefix ? <Mono>{x.prefix}</Mono> : x.ipv6 ? t("internet.prefixNone") : "IPv6 –"}
          </span>
        )}
        {v4 && (
          <Sub>
            DNS <MonoList items={dns} />
          </Sub>
        )}
        <Sub>
          {mtu ? t(mtu >= 1500 ? "internet.mtuRfc" : "internet.mtuPlain", { mtu }) : "MTU –"}
        </Sub>
      </Cell>
      {x.pass ? (
        <Cell head={t("internet.colHost")}>
          <span>{down ? `VLAN ${down}` : t("switch.untagged")}</span>
          {v4 && <span>{x.handsOn && x.ipv4 ? <Mono>{x.ipv4}</Mono> : "IPv4 –"}</span>}
          {v6 && <span>{x.prefix ? <Mono>{x.prefix}</Mono> : "IPv6 –"}</span>}
          {v6 && x.prefix && !x.prefixHost && <Sub>{t("internet.prefixWaiting")}</Sub>}
          {v4 && (
            <Sub>
              {t("internet.gatewayIs")} {x.handsOn && gateway ? <Mono>{gateway}</Mono> : "–"}
            </Sub>
          )}
        </Cell>
      ) : (
        <RouterCell />
      )}
      <ActionsCell
        state={state}
        onEdit={onEdit}
        onRemove={remove}
        busy={change.isPending}
        removeLabel={t("internet.removeSession")}
      />
    </Row>
  );
}

/** A free slot: one row, and a way to take it. */
function FreeRow({ id, onUse }: { id: number; onUse: () => void }) {
  const { t } = useI18n();
  return (
    <Row>
      <Cell head={t("internet.colSlot")}>
        <span className="text-base font-semibold text-base-content/70">{id}</span>
      </Cell>
      <Cell span={5}>
        <span className="flex flex-wrap items-center gap-2">
          <span className="text-base-content/70">{t("internet.slotFree")}</span>
          <button
            type="button"
            className="btn btn-ghost btn-sm ml-auto text-primary"
            onClick={onUse}
          >
            <Plus className="size-4" />
            {t("internet.slotUse")}
          </button>
        </span>
      </Cell>
    </Row>
  );
}

/** The PPPoE tab: the seven slots, a row each, and the dialog that sets one. */
function PppoeTab() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const list = sessions(live);
  /* A session to change, or the free slot a new one takes. */
  const [editing, setEditing] = useState<SessionView | number | null>(null);
  return (
    <>
      <Table
        head={[
          t("internet.colSlot"),
          t("internet.colService"),
          t("internet.colSession"),
          t("internet.colOperator"),
          t("internet.colHost"),
          "",
        ]}
      >
        {SLOT_IDS.map((id) => {
          const x = list.find((y) => y.id === id);
          return x ? (
            <SlotRow key={id} x={x} onEdit={() => setEditing(x)} />
          ) : (
            <FreeRow key={id} id={id} onUse={() => setEditing(id)} />
          );
        })}
      </Table>
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
    </>
  );
}

const TABS = ["pppoe", "static", "dhcp"] as const;
type Tab = (typeof TABS)[number];

export function Internet() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const [tab, setTab] = useTab(TABS);
  const n: Record<Tab, number> = {
    pppoe: sessions(live).length,
    static: ipoes(live).filter((x) => x.kind === "static").length,
    dhcp: ipoes(live).filter((x) => x.kind === "dhcp").length,
  };
  const label: Record<Tab, Key> = {
    pppoe: "internet.tabPppoe",
    static: "internet.tabStatic",
    dhcp: "internet.tabDhcp",
  };
  const hint =
    tab === "pppoe"
      ? t("internet.slots", { n: n.pppoe, max: MAX_SESSIONS })
      : t(tab === "static" ? "internet.staticHint" : "internet.dhcpHint");
  return (
    <Page
      title={t("internet.title")}
      hint={hint}
      tabs={
        <Tabs
          items={TABS.map((id) => ({
            id,
            label: t(label[id]),
            badge: n[id] ? (
              <span className="text-xs font-normal text-base-content/70">{n[id]}</span>
            ) : undefined,
          }))}
          value={tab}
          onChange={setTab}
          label={t("internet.title")}
        />
      }
    >
      <div className="min-h-0 flex-1 lg:overflow-auto">
        {tab === "pppoe" ? <PppoeTab /> : <RoutedTab key={tab} kind={tab} />}
      </div>
    </Page>
  );
}
