// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The local network: the stick's own network towards the devices behind it,
 * where a connection makes it their router. The devices that have an
 * address from it on the left, and on the right its settings: the VLAN it
 * goes in to the switch behind the stick, or none, the stick's address in
 * it, and the DHCP server that gives the devices theirs, the operator's name
 * servers with them. The two panels stand on the same lines as those of the
 * internet page.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { type ReactNode, useEffect } from "react";
import { type UseFormRegisterReturn, useForm } from "react-hook-form";
import { useLanChange } from "../api/config";
import { useLive } from "../api/live";
import { Field, SaveState, useChangeState } from "../components/Field";
import { Page } from "../components/Page";
import { Dot } from "../components/ui";
import { useI18n } from "../i18n";
import { duration } from "../lib/format";
import { ip4, net4 } from "../lib/ipv4";
import { ipv4, ipv4Cidr, vlan, z } from "../lib/schemas";
import { downVlan, type LanView, lan, leases, sessions } from "../lib/state";
import { conns } from "./internet/conns";
import { hostSide, Mono, Sub, useServices } from "./internet/shared";

/* How long a device keeps its address: the choices, in seconds. */
const LEASES = ["3600", "43200", "86400", "604800"] as const;

/* The form's values for the settings @y. */
function values(y: LanView) {
  return {
    vlan: y.vlan,
    address: y.address,
    dhcp: (y.dhcp ? "on" : "off") as "on" | "off",
    poolFrom: y.poolFrom,
    poolTo: y.poolTo,
    lease: LEASES.find((l) => Number(l) === y.leaseS) ?? "86400",
  };
}

/* A few short answers as a row of buttons, the chosen one filled. */
function Buttons<V extends string>({
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

/** The settings: a form whose fields scroll between its head and its button. */
function LanForm({ x }: { x: LanView }) {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const { list: services } = useServices();
  const change = useLanChange();
  const state = useChangeState(change);
  const schema = z
    .object({
      vlan: vlan(t),
      address: ipv4Cidr(t).check(z.minLength(1, t("form.required"))),
      dhcp: z.enum(["on", "off"]),
      poolFrom: ipv4(t),
      poolTo: ipv4(t),
      lease: z.enum(LEASES),
    })
    .check(
      /* The pool lies in the stick's network, in order, and leaves the stick's address out. */
      z.superRefine((v, ctx) => {
        if (v.dhcp === "off") return;
        const net = net4(v.address);
        const a = ip4(v.address.split("/")[0] ?? "");
        const from = ip4(v.poolFrom);
        const to = ip4(v.poolTo);
        const fail = (path: "poolFrom" | "poolTo", message: string) =>
          ctx.addIssue({ code: "custom", path: [path], message, input: v[path] });
        if (!v.poolFrom) return fail("poolFrom", t("form.required"));
        if (!v.poolTo) return fail("poolTo", t("form.required"));
        if (!net || a == null || from == null || to == null) return;
        if (from <= net.first || from >= net.last)
          fail("poolFrom", t("lan.poolOut", { net: net.text }));
        if (to <= net.first || to >= net.last) fail("poolTo", t("lan.poolOut", { net: net.text }));
        if (from > to) fail("poolTo", t("lan.poolOrder"));
        else if (a >= from && a <= to) fail("poolFrom", t("lan.poolStick"));
      }),
    );
  type Values = z.infer<typeof schema>;
  const form = useForm<Values>({ resolver: zodResolver(schema), defaultValues: values(x) });
  const { errors, isDirty } = form.formState;
  /* What the stick has, while nothing is changed here. */
  const saved = JSON.stringify(x);
  useEffect(() => {
    if (!form.formState.isDirty) form.reset(values(JSON.parse(saved)));
  }, [saved, form]);
  const [vid, address, dhcp] = form.watch(["vlan", "address", "dhcp"]);
  const net = net4(address);

  /*
   * What else goes to the host in the VLAN: a service of the operator's,
   * or an address a session hands down; in none, an address handed down
   * untagged.
   */
  const service = vid ? services.find((s) => hostSide(s).includes(Number(vid))) : undefined;
  const down = sessions(live).find((y) => y.pass && downVlan(y) === vid);
  const clash = service
    ? t("lan.vlanService", { vid })
    : down
      ? vid
        ? t("lan.vlanDown", { vid, name: `PPPoE ${down.id}` })
        : t("lan.vlanDownNone", { name: `PPPoE ${down.id}` })
      : undefined;

  const send = (v: Values) =>
    change.mutate(
      {
        VLAN: v.vlan,
        ADDRESS: v.address,
        DHCP: v.dhcp,
        POOL_FROM: v.poolFrom,
        POOL_TO: v.poolTo,
        LEASE: v.lease,
      },
      { onSuccess: () => form.reset(v) },
    );

  return (
    <section
      className="flex flex-col rounded-box border border-base-300 bg-base-100 xl:h-full xl:min-h-0"
      aria-labelledby="lan-form"
    >
      <div className="flex min-h-14 shrink-0 items-center px-4 pt-4 pb-2">
        <h2 id="lan-form" className="text-base font-semibold">
          {t("lan.settings")}
        </h2>
      </div>
      {/* What the address makes of the network, as high as the internet form's tabs. */}
      <p className="flex h-11 shrink-0 items-center border-b border-base-300 px-4 text-xs text-base-content/70">
        {net ? t(vid ? "lan.netIn" : "lan.netNone", { net: net.text, vid }) : t("lan.netUnknown")}
      </p>
      <form onSubmit={form.handleSubmit(send)} noValidate className="flex min-h-0 flex-1 flex-col">
        <div className="grid min-h-0 flex-1 content-start gap-4 p-4 xl:overflow-auto">
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
          <Buttons
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
              <Buttons
                label={t("lan.lease")}
                field={form.register("lease")}
                options={LEASES.map((l) => ({ value: l, label: duration(Number(l), lang) }))}
              />
            </>
          )}
        </div>
        <div className="flex min-h-16 shrink-0 flex-wrap items-center gap-3 border-t border-base-300 px-4 py-2.5">
          <button
            type="submit"
            className="btn btn-primary"
            disabled={!isDirty || change.isPending || !!clash}
          >
            {t("internet.save")}
          </button>
          <SaveState {...state} />
        </div>
      </form>
    </section>
  );
}

/* A cell; in a narrow list, under its column's name. */
function Cell({ head, children }: { head?: string; children: ReactNode }) {
  return (
    <div
      data-h={head}
      className={`grid min-w-0 content-start gap-1 ${
        head
          ? "@max-lg:before:text-xs @max-lg:before:text-base-content/70 @max-lg:before:content-[attr(data-h)]"
          : ""
      }`}
    >
      {children}
    </div>
  );
}

/** The devices with an address from the stick, and how much of its pool they take. */
function Devices({ x }: { x: LanView }) {
  const { t, lang } = useI18n();
  const list = leases(useLive().data);
  const from = ip4(x.poolFrom);
  const to = ip4(x.poolTo);
  const size = from != null && to != null && to >= from ? to - from + 1 : 0;
  const taken = list.filter((l) => {
    const n = ip4(l.ip);
    return n != null && from != null && to != null && n >= from && n <= to;
  }).length;
  return (
    <section aria-labelledby="devices" className="@container flex flex-col xl:h-full xl:min-h-0">
      <div className="mt-px flex min-h-14 shrink-0 items-center pt-4 pb-2">
        <h2 id="devices" className="text-base font-semibold">
          {t("lan.devices")}
        </h2>
      </div>
      <div className="min-h-0 flex-1 px-px xl:overflow-auto">
        {list.length === 0 ? (
          <div className="mt-4 rounded-box border border-dashed border-base-300 px-6 py-8 text-center">
            <p className="text-base font-semibold">{t("lan.empty")}</p>
            <p className="mx-auto mt-1 max-w-prose text-base-content/70">
              {t(x.dhcp ? "lan.emptyHint" : "lan.emptyOff")}
            </p>
          </div>
        ) : (
          <div className="grid grid-cols-[minmax(0,1fr)_auto_auto] gap-x-8 gap-y-2 pb-2 @max-lg:flex @max-lg:flex-col @max-lg:gap-3">
            <div className="sticky top-0 z-10 col-span-full mb-2 grid h-11 grid-cols-subgrid items-center border-x border-b border-x-transparent border-b-base-300 bg-base-200 px-4 text-xs font-semibold text-base-content/70 @max-lg:hidden">
              <span>{t("lan.colDevice")}</span>
              <span>{t("lan.colIp")}</span>
              <span>{t("lan.colLease")}</span>
            </div>
            {list.map((l) => (
              <div
                key={l.mac}
                className="col-span-full grid grid-cols-subgrid items-start rounded-box border border-base-300 bg-base-100 px-4 py-3 @max-lg:flex @max-lg:flex-col @max-lg:gap-3"
              >
                <Cell>
                  <span className="font-semibold">{l.name ?? t("lan.noName")}</span>
                  <Sub>
                    <Mono>{l.mac}</Mono>
                  </Sub>
                </Cell>
                <Cell head={t("lan.colIp")}>
                  <Mono>{l.ip}</Mono>
                </Cell>
                <Cell head={t("lan.colLease")}>
                  <span>{t("lan.leaseLeft", { time: duration(l.left_s, lang) })}</span>
                </Cell>
              </div>
            ))}
          </div>
        )}
      </div>
      <p className="mb-px flex min-h-16 shrink-0 items-center border-t border-base-300 text-xs text-base-content/70">
        {x.dhcp ? t("lan.poolUse", { n: taken, size }) : t("lan.dhcpOffFoot")}
      </p>
    </section>
  );
}

export function Lan() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const x = lan(live);
  /* The connections that make the stick a router, whose network this is. */
  const routers = conns(live).filter((c) => c.kind !== "pppoe" || !c.x.pass);
  return (
    <Page title={t("lan.title")} hint={t("lan.hint")}>
      <div className="flex min-h-0 flex-1 flex-col gap-4">
        {routers.length === 0 && (
          <div
            role="status"
            className="flex items-start gap-3 rounded-box border border-base-300 bg-base-100 px-4 py-3"
          >
            <span className="mt-1.5">
              <Dot tone="neutral" />
            </span>
            <p>{t("lan.unused")}</p>
          </div>
        )}
        <div className="grid min-h-0 flex-1 content-start gap-6 xl:grid-cols-[minmax(0,1fr)_25rem] xl:content-stretch xl:overflow-hidden">
          <div className="min-h-0">
            <Devices x={x} />
          </div>
          <div className="min-h-0 max-xl:order-first xl:flex xl:flex-col">
            <LanForm x={x} />
          </div>
        </div>
      </div>
    </Page>
  );
}
