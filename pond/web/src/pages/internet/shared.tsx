// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What the internet's tabs share: the operator's services a connection runs
 * in and their names, the dialog a connection is set in, and the pieces of a
 * connection's row.
 */

import { Pencil, Trash2, X } from "lucide-react";
import { type ReactNode, useEffect, useMemo, useRef } from "react";
import type { UseFormRegisterReturn } from "react-hook-form";
import { useOmci } from "../../api/config";
import type { Family } from "../../api/types";
import { SaveState } from "../../components/Field";
import { Cell, Dot } from "../../components/ui";
import { type Key, useI18n } from "../../i18n";
import { Mib, type Service, services } from "../../lib/omci";
import { fibreVlan } from "../../lib/state";

export const FAMILY: Record<Family, Key> = {
  both: "internet.famBoth",
  ipv4: "internet.famV4",
  ipv6: "internet.famV6",
};

export type T = (k: Key, v?: Record<string, string | number>) => string;

/** The operator's services that can carry a connection: not multicast alone. */
export function useServices() {
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
 * The VID a connection is given for service @s: the one by which the
 * operator names the service towards the host, which the OLT translates,
 * else the VID on the fibre; onu-mode follows it from there.
 */
export function serviceVid(s: Service): string {
  return String(s.hostVlans.find((v) => v != null) ?? s.vlan ?? "");
}

/** The service a connection runs in: the one whose VID on the fibre it runs in. */
export function serviceOf(list: Service[], x: { dev: string | null; vlan: string }) {
  return list.find((s) => s.vlan != null && String(s.vlan) === fibreVlan(x));
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

/** A dialog that opens while @open holds. */
export function Dialog({
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
      <div className="modal-box max-w-2xl p-6">
        <div className="mb-6 flex items-center gap-4">
          <h3 className="text-base font-semibold">{title}</h3>
          <form method="dialog" className="ml-auto">
            <button
              type="submit"
              className="btn btn-ghost btn-sm btn-square text-base-content/70"
              aria-label="close"
            >
              <X className="size-4" />
            </button>
          </form>
        </div>
        {open && children}
      </div>
      <form method="dialog" className="modal-backdrop">
        <button type="submit" aria-label="close" />
      </form>
    </dialog>
  );
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

/**
 * The service a connection is given, by the VID its operator names it with;
 * @gone keeps the VID of a service the OLT no longer provisions.
 */
export function ServiceSelect({
  field,
  list,
  gone,
  picked,
}: {
  field: UseFormRegisterReturn;
  list: Service[];
  gone: string | null;
  picked: Service | undefined;
}) {
  const { t } = useI18n();
  return (
    <div className="grid gap-1">
      <label htmlFor="service" className="text-sm font-semibold">
        {t("internet.service")}
      </label>
      <select id="service" className="select w-full" {...field}>
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
      <p className="text-xs text-base-content/70">
        {picked
          ? t("internet.serviceFollows", { vid: serviceVid(picked) })
          : list.length === 0
            ? t("internet.servicesNone")
            : null}
      </p>
    </div>
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

/**
 * A connection's service: the name it has towards the host, and under it its
 * VID on the fibre, when the OLT translates it, and the channel it rides.
 */
export function ServiceCell({
  service,
  vlan,
  channel,
}: {
  service: Service | undefined;
  vlan: string;
  channel: string;
}) {
  const { t } = useI18n();
  const fibre =
    service && hostNames(service).length ? (service.vlan ?? t("internet.serviceNoVlan")) : null;
  return (
    <Cell head={t("internet.colService")}>
      <span>
        {service ? hostServiceName(t, service) : vlan ? `VLAN ${vlan}` : t("switch.untagged")}
      </span>
      <Sub>
        {[fibre != null && t("internet.upstream", { vid: fibre }), channel]
          .filter(Boolean)
          .join(" · ")}
      </Sub>
    </Cell>
  );
}

/** What stands behind the stick where it is the router. */
export function RouterCell() {
  const { t } = useI18n();
  return (
    <Cell head={t("internet.colHost")}>
      <span>{t("internet.routerNat")}</span>
      <Sub>{t("internet.routerLan")}</Sub>
    </Cell>
  );
}

/** A connection's number and its state, a dot and a word. */
export function StateCell({ id, up, word }: { id: number; up: boolean; word: string }) {
  const { t } = useI18n();
  return (
    <Cell head={t("internet.colConn")}>
      <span className="text-base font-semibold">{id}</span>
      <span className="flex items-center gap-2">
        <Dot tone={up ? "good" : "warn"} />
        {word}
      </span>
    </Cell>
  );
}

/** A row's change and remove, and how its last change went. */
export function ActionsCell({
  state,
  onEdit,
  onRemove,
  busy,
  removeLabel,
}: {
  state: Parameters<typeof SaveState>[0];
  onEdit: () => void;
  onRemove: () => void;
  busy: boolean;
  removeLabel: string;
}) {
  const { t } = useI18n();
  return (
    <Cell actions>
      <span className="flex items-center justify-end gap-1 max-md:justify-start">
        <SaveState {...state} />
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
          onClick={onRemove}
          disabled={busy}
          aria-label={removeLabel}
          title={removeLabel}
        >
          <Trash2 className="size-4" />
        </button>
      </span>
    </Cell>
  );
}
