// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What the WAN page's form and list share: the operator's services a
 * connection runs in and their names, a connection's name, and the small
 * pieces values are set in.
 */

import { type ReactNode, useMemo } from "react";
import type { UseFormRegisterReturn } from "react-hook-form";
import { useOmci, useServiceRules } from "../../api/config";
import type { Family } from "../../api/types";
import { type Key, useI18n } from "../../i18n";
import { Mib, type Service, services, withRules } from "../../lib/omci";
import type { Conn } from "./conns";

export const FAMILY: Record<Family, Key> = {
  both: "internet.famBoth",
  ipv4: "internet.famV4",
  ipv6: "internet.famV6",
};

export type T = (k: Key, v?: Record<string, string | number>) => string;

/**
 * The operator's services as they reach the host, the stick's rules applied
 * (lib/omci.ts, withRules): a moved one in its new VLAN, one kept off
 * marked; and the services as the OLT set them up, for what a rule changed.
 */
export function useStickServices() {
  const omci = useOmci();
  const rules = useServiceRules();
  return useMemo(() => {
    const olt = services(new Mib(omci.data));
    return { list: withRules(olt, rules.data?.rules ?? []), olt, ready: omci.isSuccess };
  }, [omci.data, omci.isSuccess, rules.data]);
}

/** The operator's services that can carry a connection: not multicast alone, nor kept off. */
export function useServices() {
  const { list, ready } = useStickServices();
  return {
    list: list.filter(
      (s) =>
        !s.off &&
        s.ports.some((p) => p.channels.some((c) => !c.multicast && c.direction !== "down")),
    ),
    ready,
  };
}

/*
 * The VID a connection is given for service @s: the one by which the
 * operator names the service towards the host, which the OLT translates,
 * else the VID on the fibre; onu-apply follows it from there.
 */
export function serviceVid(s: Service): string {
  return String(s.hostVlans.find((v) => v != null) ?? s.vlan ?? "");
}

/* The VIDs the OLT translates service @s to towards the host, when they are not its own. */
function hostNames(s: Service) {
  return s.hostVlans.filter((v) => v != null && v !== s.vlan);
}

/** What service @s is named by towards the host. */
function hostServiceName(t: T, s: Service) {
  const host = hostNames(s);
  if (host.length) return `VLAN ${host.join(", ")}`;
  return s.vlan == null ? t("internet.serviceNoVlan") : `VLAN ${s.vlan}`;
}

/** What service @s is named by: its VID towards the host, and on the fibre (upstream). */
export function serviceName(t: T, s: Service) {
  const host = hostNames(s);
  if (host.length)
    return t("internet.serviceVia", {
      host: host.join(", "),
      fibre: s.vlan ?? t("internet.serviceNoVlan"),
    });
  return hostServiceName(t, s);
}

/* The VIDs service @s comes in on the host's side. */
export function hostSide(s: Service): number[] {
  const host = s.hostVlans.filter((v): v is number => v != null);
  return host.length ? host : s.vlan != null ? [s.vlan] : [];
}

/* One answer of the form's: a radio of @field with @value. */
export function Choice({
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
        <span className="block text-sm font-semibold">{title}</span>
        <span className="mt-1 block text-xs text-base-content/70">{text}</span>
      </span>
    </label>
  );
}

/** The families of addresses a connection asks for: a row of three buttons. */
export function FamilyField({ field, hint }: { field: UseFormRegisterReturn; hint: string }) {
  const { t } = useI18n();
  return (
    <div className="grid gap-1">
      <span className="text-sm font-semibold">{t("internet.family")}</span>
      <div className="join">
        {(["both", "ipv4", "ipv6"] as const).map((f) => (
          <label key={f} className="join-item btn btn-sm has-[:checked]:btn-primary">
            <input type="radio" value={f} className="hidden" {...field} />
            {t(FAMILY[f])}
          </label>
        ))}
      </div>
      <p className="text-xs text-base-content/70">{hint}</p>
    </div>
  );
}

/* A line of detail under a cell's value, quieter. */
export function Sub({ children }: { children: ReactNode }) {
  return <div className="text-xs text-base-content/70">{children}</div>;
}

/* What is read character by character: an address, an interface. */
export function Mono({ children }: { children: ReactNode }) {
  return <span className="font-mono">{children}</span>;
}

/** Addresses one after another, apart by commas, or a dash for none. */
export function MonoList({ items }: { items: string[] }) {
  if (!items.length) return <>–</>;
  return (
    <>
      {items.map((d, i) => (
        <span key={d}>
          {i > 0 && ", "}
          <Mono>{d}</Mono>
        </span>
      ))}
    </>
  );
}

/** A connection's name: PPPoE with its session's number, or its kind. */
export function connName(t: T, c: Conn) {
  return c.kind === "pppoe"
    ? `PPPoE ${c.x.id}`
    : t(c.kind === "static" ? "internet.tabStatic" : "internet.tabDhcp");
}
