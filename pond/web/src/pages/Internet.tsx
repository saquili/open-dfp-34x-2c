// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The internet, in three tabs: WAN, the stick's connections to the operator;
 * LAN, the local networks that go out through them; and their clients, the
 * devices in those (pages/Lan.tsx). In the first, the connections, and at their right, while
 * it is open, the form that adds or changes one (components/SidePanel); with
 * no connection yet it is open from the start. A connection is PPPoE, a
 * static address or DHCP, and runs in a VLAN of the operator's; a VLAN
 * without one goes through the stick as a bridge, to the host.
 *
 * Nothing passes without the operator's services: while the fibre is down
 * or the OLT set none, a line over the page says so, and the connections
 * wait; they can still be added, changed and removed.
 */

import { useRef, useState } from "react";
import { useLive } from "../api/live";
import { Page, Tabs, useTab } from "../components/Page";
import { SidePanel, withPanel } from "../components/SidePanel";
import { Dot } from "../components/ui";
import { useI18n } from "../i18n";
import { type FibreView, fibre } from "../lib/state";
import { ConnForm } from "./internet/ConnForm";
import { ConnList } from "./internet/ConnList";
import { type Conn, conns, type Kind, room } from "./internet/conns";
import { useServices } from "./internet/shared";
import { LanDevices, LanNets, useLanCounts } from "./Lan";

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

/* The page's tabs: the connections, the local networks, and their clients. */
const TABS = ["wan", "lan", "clients"] as const;

/** The connections, and at their right while it is open, the form that adds or changes one. */
function Conns() {
  const { data: live } = useLive();
  const list = conns(live);
  const { list: services, ready } = useServices();
  const fib = fibre(live);
  /* Until the OLT's services are read, they are taken as there. */
  const known = !ready || services.length > 0;
  const l2 = !ready || (fib.connected && known);
  const [kind, setKind] = useState<Kind>("pppoe");
  const [editKey, setEditKey] = useState<string | null>(null);
  const editing = list.find((c) => c.key === editKey) ?? null;
  /* The form, shut until asked for while there are connections to look at. */
  const [open, setOpen] = useState(false);
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
            />
          </SidePanel>
        )}
      </div>
    </div>
  );
}

export function Internet() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const [tab, setTab] = useTab(TABS);
  /* The network the devices are narrowed to, from a network's card, or all. */
  const [net, setNet] = useState<number | null>(null);
  const lan = useLanCounts();
  const count = (n: number) => (
    <span className="font-normal tabular-nums text-base-content/60">{n}</span>
  );
  return (
    <Page
      title={t("internet.title")}
      hint={tab === "wan" ? t("internet.hint") : t("lan.hint")}
      tabs={
        <Tabs
          items={[
            { id: "wan", label: t("internet.tabWan"), badge: count(conns(live).length) },
            { id: "lan", label: t("internet.tabLan"), badge: count(lan.nets) },
            { id: "clients", label: t("internet.tabClients"), badge: count(lan.devices) },
          ]}
          value={tab}
          onChange={(v) => {
            setNet(null);
            setTab(v);
          }}
          label={t("internet.title")}
        />
      }
    >
      {tab === "wan" ? (
        <Conns />
      ) : tab === "lan" ? (
        <LanNets
          onDevices={(id) => {
            setNet(id);
            setTab("clients");
          }}
        />
      ) : (
        <LanDevices net={net} onNet={setNet} />
      )}
    </Page>
  );
}
