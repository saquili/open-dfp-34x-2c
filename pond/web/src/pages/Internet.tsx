// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The internet: the stick's connections to the operator on the left, and on
 * the right the form that adds or changes one. A
 * connection is PPPoE, a static address or DHCP, and runs in a VLAN of the
 * operator's; a VLAN without one goes through the stick as a bridge, to the
 * host. On a narrow screen the form stands above the list, shut until asked
 * for.
 *
 * Nothing passes without the operator's services: while the fibre is down
 * or the OLT set none, a line over the page says so, and the connections
 * wait; they can still be added, changed and removed.
 */

import { Plus } from "lucide-react";
import { useRef, useState } from "react";
import { useLive } from "../api/live";
import { Page, useTab } from "../components/Page";
import { Dot } from "../components/ui";
import { useI18n } from "../i18n";
import { type FibreView, fibre } from "../lib/state";
import { ConnForm } from "./internet/ConnForm";
import { ConnList } from "./internet/ConnList";
import { type Conn, conns, KINDS } from "./internet/conns";
import { useServices } from "./internet/shared";

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

export function Internet() {
  const { t } = useI18n();
  const { data: live } = useLive();
  const list = conns(live);
  const { list: services, ready } = useServices();
  const fib = fibre(live);
  /* Until the OLT's services are read, they are taken as there. */
  const known = !ready || services.length > 0;
  const l2 = !ready || (fib.connected && known);
  const [kind, setKind] = useTab(KINDS);
  const [editKey, setEditKey] = useState<string | null>(null);
  const editing = list.find((c) => c.key === editKey) ?? null;
  /* The form on a narrow screen, shut until asked for. */
  const [open, setOpen] = useState(false);
  const form = useRef<HTMLDivElement>(null);

  const edit = (c: Conn) => {
    setEditKey(c.key);
    setOpen(true);
    requestAnimationFrame(() => form.current?.scrollIntoView({ block: "nearest" }));
  };
  const shown = open || editing != null;

  return (
    <Page title={t("internet.title")} hint={t("internet.hint")}>
      <div className="flex min-h-0 flex-1 flex-col gap-4">
        {!l2 && <NoServices fib={fib} />}
        <div className="grid min-h-0 flex-1 content-start gap-6 xl:grid-cols-[minmax(0,1fr)_25rem] xl:content-stretch xl:overflow-hidden">
          <div className="min-h-0">
            <ConnList
              list={list}
              services={services}
              l2={l2}
              known={known}
              editing={editKey}
              onEdit={edit}
            />
          </div>
          {/* On a narrow screen the form, or the button that opens it, comes first. */}
          <div ref={form} className="min-h-0 max-xl:order-first xl:flex xl:flex-col">
            {!shown && (
              <button
                type="button"
                className="btn btn-primary w-full xl:hidden"
                onClick={() => setOpen(true)}
              >
                <Plus className="size-4" />
                {t("internet.addConn")}
              </button>
            )}
            <div className={`xl:min-h-0 xl:flex-1 ${shown ? "" : "max-xl:hidden"}`}>
              <ConnForm
                list={list}
                kind={kind}
                onKind={setKind}
                editing={editing}
                onEditDone={() => {
                  setEditKey(null);
                  setOpen(false);
                }}
                onClose={() => setOpen(false)}
              />
            </div>
          </div>
        </div>
      </div>
    </Page>
  );
}
