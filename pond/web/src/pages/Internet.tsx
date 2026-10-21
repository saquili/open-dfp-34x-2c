// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The internet, in pages under one entry of the menu: WAN, the stick's
 * connections to the operator; LAN, the local networks that go out through
 * them; their clients, the devices in those (pages/Lan.tsx); NAT, what comes
 * in to them from the internet (pages/Nat.tsx); and the firewall's rules
 * (pages/Firewall.tsx). On the first, the connections, and at their right,
 * while it is open, the form that adds or changes one (components/SidePanel);
 * with no connection yet it is open from the start. A connection is PPPoE, a
 * static address or DHCP, and runs in a VLAN of the operator's; a VLAN
 * without one goes through the stick as a bridge, to the host.
 *
 * Nothing passes without the operator's services: while the fibre is down
 * or the OLT set none, a line over the page says so, and the connections
 * wait; they can still be added, changed and removed.
 */

import { useNavigate, useSearch } from "@tanstack/react-router";
import { useRef, useState } from "react";
import { useNetwork, useOmci } from "../api/config";
import { useLive } from "../api/live";
import { Page } from "../components/Page";
import { QueryNote } from "../components/Read";
import { SidePanel, withPanel } from "../components/SidePanel";
import { Dot } from "../components/ui";
import { useI18n } from "../i18n";
import { takeOltLogin } from "../lib/handoff";
import { type FibreView, fibre, served } from "../lib/state";
import { FirewallSection, useFwCount } from "./Firewall";
import { ConnForm } from "./internet/ConnForm";
import { ConnList } from "./internet/ConnList";
import { type Conn, conns, type Kind, room } from "./internet/conns";
import { useServices } from "./internet/shared";
import { LanDevices, LanNets, useLanCounts } from "./Lan";
import { NatSection, useNatCount } from "./Nat";

/** Why there are no services: the fibre, or the OLT. */
function NoServices({ fib }: { fib: FibreView }) {
  const { t } = useI18n();
  return (
    <div
      role="status"
      className="flex items-start gap-3 rounded-box border border-warning/60 bg-warning/10 px-4 py-3"
    >
      <span className="mt-1.5">
        <Dot tone="warn" />
      </span>
      <div>
        <p className="font-semibold">{t("internet.noL2")}</p>
        <p className="mt-1">
          {fib.connected
            ? t("internet.noL2Olt")
            : t("internet.noL2Fibre", {
                state: fib.present ? fib.state.toUpperCase() : t("internet.noPon"),
              })}
        </p>
      </div>
    </div>
  );
}

/** The connections, and at their right while it is open, the form that adds or changes one. */
function Conns() {
  const { data: live } = useLive();
  const list = conns(live);
  const { list: services, ready } = useServices();
  /* The OLT's services name the connections' VLANs; the network, their interfaces. */
  const omci = useOmci();
  const net = useNetwork();
  const fib = fibre(live);
  /* Until the OLT's services are read, they are taken as there. */
  const known = !ready || services.length > 0;
  const l2 = !ready || (fib.connected && known);
  const [kind, setKind] = useState<Kind>("pppoe");
  const [editKey, setEditKey] = useState<string | null>(null);
  const editing = list.find((c) => c.key === editKey) ?? null;
  /* The PPPoE login the identity page handed over: a new session's form opens with it. */
  const [login] = useState(takeOltLogin);
  /* The form, shut until asked for while there are connections to look at. */
  const [open, setOpen] = useState(login != null);
  const form = useRef<HTMLDivElement>(null);
  const show = (key: string | null) => {
    setEditKey(key);
    setOpen(true);
    requestAnimationFrame(() => form.current?.scrollIntoView({ block: "nearest" }));
  };
  const close = () => {
    setEditKey(null);
    setOpen(false);
  };
  const shown = open || editing != null || list.length === 0;

  return (
    <div className="flex min-h-0 flex-1 flex-col gap-4">
      <QueryNote reads={[omci, net]} aside />
      {!l2 && <NoServices fib={fib} />}
      <div className={withPanel(shown)}>
        <div className="min-h-0">
          <ConnList
            list={list}
            services={services}
            l2={l2}
            editing={editKey}
            onEdit={(c: Conn) => show(c.key)}
            onAdd={room(list) ? () => show(null) : undefined}
          />
        </div>
        {shown && (
          <SidePanel ref={form} onClose={list.length ? close : undefined}>
            <ConnForm
              list={list}
              kind={kind}
              onKind={setKind}
              editing={editing}
              onEditDone={close}
              onClose={list.length ? close : undefined}
              login={login}
            />
          </SidePanel>
        )}
      </div>
    </div>
  );
}

/** The pages of the internet, by the name each has in the menu and in its address. */
export const INTERNET_PAGES = ["wan", "lan", "clients", "nat", "fw", "routes"] as const;
export type InternetPage = (typeof INTERNET_PAGES)[number];

/**
 * How many things each page of the internet holds, for the menu; null for a
 * page the stick has no backend for yet, which shows no count.
 */
export function useInternetCounts(): Record<InternetPage, number | null> {
  const { data: live } = useLive();
  const lan = useLanCounts();
  const nat = useNatCount();
  const fw = useFwCount();
  const lans = served(live, "lans");
  return {
    wan: conns(live).length,
    lan: lans ? lan.nets : null,
    clients: lans ? lan.devices : null,
    nat: served(live, "forwards") ? nat : null,
    fw: served(live, "rules") ? fw : null,
    routes: null,
  };
}

export function WanPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.wan")} hint={t("internet.hint")}>
      <Conns />
    </Page>
  );
}

export function LanPage() {
  const { t } = useI18n();
  const navigate = useNavigate();
  return (
    <Page title={t("nav.lan")} hint={t("lan.hint")}>
      <LanNets
        onDevices={(id) => navigate({ to: "/internet/clients", search: { net: id } as never })}
      />
    </Page>
  );
}

/* The devices, narrowed to one network from that network's card, or all; the address keeps which. */
export function ClientsPage() {
  const { t } = useI18n();
  const navigate = useNavigate();
  const search = useSearch({ strict: false }) as { net?: unknown };
  const net = typeof search.net === "number" ? search.net : null;
  return (
    <Page title={t("nav.clients")} hint={t("lan.hint")}>
      <LanDevices
        net={net}
        onNet={(n) =>
          navigate({ to: ".", search: { net: n ?? undefined } as never, replace: true })
        }
      />
    </Page>
  );
}

export function NatPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.nat")} hint={t("nat.hint")}>
      <NatSection />
    </Page>
  );
}

export function FirewallPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.fw")} hint={t("fw.hint")}>
      <FirewallSection />
    </Page>
  );
}
