// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The connections by which the stick takes its address on a service as a
 * router does: typed in (static IP), or from the operator's DHCP. The stick
 * keeps the address and is the router of the devices behind the switch on
 * its SFP port. Static and DHCP connections share the rows the hardware has
 * for them.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { Plus } from "lucide-react";
import { useState } from "react";
import { useForm } from "react-hook-form";
import { useIpoeChange } from "../../api/config";
import { useLive } from "../../api/live";
import { Field, SaveState, useChangeState } from "../../components/Field";
import { Cell, Row, Table } from "../../components/ui";
import { useI18n } from "../../i18n";
import { duration } from "../../lib/format";
import { addresses, ipv4, ipv4Cidr, ipv6, ipv6Cidr, mac, required, z } from "../../lib/schemas";
import { gems, type IpoeView, ipoes, MAX_IPOE } from "../../lib/state";
import {
  ActionsCell,
  Dialog,
  FamilyField,
  Mono,
  MonoList,
  RouterCell,
  ServiceCell,
  ServiceSelect,
  StateCell,
  Sub,
  serviceOf,
  serviceVid,
  useServices,
} from "./shared";

type Kind = IpoeView["kind"];

/** A connection: one row, with all there is to know of it at once. */
function IpoeRow({ x, onEdit }: { x: IpoeView; onEdit: () => void }) {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const { list } = useServices();
  const change = useIpoeChange();
  const state = useChangeState(change);
  const portId = gems(live).find((g) => g.gem.name === x.gemDev)?.gem.port_id;
  const channel =
    portId != null ? `GEM ${portId}` : x.gem === "auto" ? t("internet.autoGem") : `GEM ${x.gem}`;
  const v4 = x.family !== "ipv6";
  const v6 = x.family !== "ipv4";
  const word = x.up
    ? t("internet.up")
    : x.kind === "dhcp"
      ? t("internet.waitingAddress")
      : t("internet.noLink");

  const remove = () => {
    if (!window.confirm(t("internet.connRemoveConfirm", { id: x.id }))) return;
    change.mutate({ ID: String(x.id), KIND: "" });
  };

  return (
    <Row>
      <StateCell id={x.id} up={x.up} word={word} />
      <ServiceCell service={serviceOf(list, x)} vlan={x.vlan} channel={channel} />
      <Cell head={t("internet.colAddress")}>
        {v4 && <span>{x.ipv4 ? <Mono>{x.ipv4}</Mono> : "IPv4 –"}</span>}
        {v6 && <span>{x.ipv6 ? <Mono>{x.ipv6}</Mono> : "IPv6 –"}</span>}
        {v6 && x.prefix && (
          <Sub>
            {t("internet.prefix6")}: <Mono>{x.prefix}</Mono>
          </Sub>
        )}
        <Sub>
          MAC {x.mac ? <Mono>{x.mac}</Mono> : t("internet.macStick")}
          {x.kind === "dhcp" &&
            x.lease != null &&
            ` · ${t("internet.lease", { time: duration(x.lease, lang) })}`}
        </Sub>
      </Cell>
      <Cell head={t("internet.colGateway")}>
        <span>{x.router ? <Mono>{x.router}</Mono> : "–"}</span>
        {v6 && x.kind === "static" && x.gateway6 && (
          <Sub>
            {t("internet.gateway6")} <Mono>{x.gateway6}</Mono>
          </Sub>
        )}
        <Sub>
          DNS <MonoList items={x.dns} />
        </Sub>
      </Cell>
      <RouterCell />
      <ActionsCell
        state={state}
        onEdit={onEdit}
        onRemove={remove}
        busy={change.isPending}
        removeLabel={t("internet.connRemove")}
      />
    </Row>
  );
}

/** A connection's settings: a new one of @kind without @x, or @x changed. */
function IpoeForm({ kind, x, onDone }: { kind: Kind; x?: IpoeView; onDone: () => void }) {
  const { t } = useI18n();
  const change = useIpoeChange();
  const state = useChangeState(change);
  const { list } = useServices();
  const bound = x ? serviceOf(list, x) : undefined;
  /* A connection whose service the OLT no longer provisions keeps its VID. */
  const gone = x && !bound ? x.vlan : null;
  const first = bound ? serviceVid(bound) : (gone ?? (list[0] ? serviceVid(list[0]) : ""));
  const fixed = kind === "static";
  const schema = z.object({
    service: required(t("form.required")),
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
      service: first,
      family: x?.family ?? (fixed ? "ipv4" : "both"),
      address: x?.address ?? "",
      gateway: x?.gateway ?? "",
      dns: x?.dnsSet ?? "",
      address6: x?.address6 ?? "",
      gateway6: x?.gateway6 ?? "",
      prefix6: x?.prefix6 ?? "",
      mac: x?.mac ?? "",
    },
  });
  const { errors, isDirty } = form.formState;
  const service = form.watch("service");
  const picked = list.find((s) => serviceVid(s) === service);

  const send = (v: Values) => {
    /* A static connection has the families it is given addresses of. */
    const family = fixed ? (v.address6 ? "both" : "ipv4") : v.family;
    change.mutate(
      {
        ID: x ? String(x.id) : undefined,
        KIND: kind,
        VLAN: v.service,
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
      { onSuccess: onDone },
    );
  };

  return (
    <form onSubmit={form.handleSubmit(send)} noValidate className="grid gap-4">
      <ServiceSelect field={form.register("service")} list={list} gone={gone} picked={picked} />
      {fixed ? (
        <>
          <div className="grid gap-4 sm:grid-cols-2">
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
          <div className="grid gap-4 sm:grid-cols-2">
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
      <div className="flex flex-wrap items-center gap-3">
        <button
          type="submit"
          className="btn btn-primary"
          disabled={(x && !isDirty) || change.isPending || !service}
        >
          {x ? t("internet.save") : t("internet.connAdd")}
        </button>
        <button type="button" className="btn btn-ghost" onClick={onDone}>
          {t("internet.cancel")}
        </button>
        <SaveState {...state} />
      </div>
    </form>
  );
}

/** The tab of static connections, or of DHCP ones: their table and the dialog that sets one. */
export function RoutedTab({ kind }: { kind: Kind }) {
  const { t } = useI18n();
  const { data: live } = useLive();
  const all = ipoes(live);
  const list = all.filter((x) => x.kind === kind);
  const full = all.length >= MAX_IPOE;
  /* A connection to change, or a new one. */
  const [editing, setEditing] = useState<IpoeView | "new" | null>(null);
  return (
    <>
      <Table
        head={[
          t("internet.colConn"),
          t("internet.colService"),
          t("internet.colAddress"),
          t("internet.colGateway"),
          t("internet.colHost"),
          "",
        ]}
      >
        {list.map((x) => (
          <IpoeRow key={x.id} x={x} onEdit={() => setEditing(x)} />
        ))}
        <Row>
          <Cell span={6}>
            <span className="flex flex-wrap items-center gap-2">
              <span className="text-base-content/70">
                {full
                  ? t("internet.connFull", { max: MAX_IPOE })
                  : list.length
                    ? ""
                    : t(kind === "static" ? "internet.staticNone" : "internet.dhcpNone")}
              </span>
              <button
                type="button"
                className="btn btn-ghost btn-sm ml-auto text-primary"
                disabled={full}
                onClick={() => setEditing("new")}
              >
                <Plus className="size-4" />
                {t("internet.connAdd")}
              </button>
            </span>
          </Cell>
        </Row>
      </Table>
      <Dialog
        open={editing != null}
        title={
          editing === "new"
            ? t(kind === "static" ? "internet.connAddStatic" : "internet.connAddDhcp")
            : t("internet.connEdit", { id: editing?.id ?? 0 })
        }
        onClose={() => setEditing(null)}
      >
        <IpoeForm
          kind={kind}
          x={editing === "new" || editing == null ? undefined : editing}
          onDone={() => setEditing(null)}
        />
      </Dialog>
    </>
  );
}
