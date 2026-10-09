// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The connections: a table whose rows are cards, one a connection of
 * whatever kind, the same columns for all; a card opens on what is its kind's
 * alone. The columns line up from card to card (subgrid); in a narrow list a
 * card's cells stand one under another, each under its column's name.
 *
 * A connection works step by step, each step on the one before: light on
 * the fibre, the operator's services (OMCI), its VLAN among them, the
 * session or the lease or the gateway, and the address. Its state is the
 * first step that does not hold.
 */

import { ChevronRight, Pencil, Trash2 } from "lucide-react";
import { type ReactNode, useState } from "react";
import { useIpoeChange, useNetwork, useSessionChange } from "../../api/config";
import { useLive } from "../../api/live";
import { useChangeState } from "../../components/Field";
import { Dot } from "../../components/ui";
import { useI18n } from "../../i18n";
import { duration } from "../../lib/format";
import type { Service } from "../../lib/omci";
import {
  downVlan,
  type FibreView,
  fibre,
  gems,
  MAX_CONNS,
  sessions,
  type Tone,
} from "../../lib/state";
import { type Conn, serviceIn } from "./conns";
import { connName, FAMILY, Mono, MonoList, Sub, type T } from "./shared";

/*
 * The cells' places in a narrower list: the name and the buttons in a row,
 * and under them the VLAN, the state and the address, under the names of
 * their columns; in a list as narrow as a phone, the address in a row of
 * its own, and each cell under its column's name.
 */
const NARROW = {
  caret: "@max-3xl:col-start-1 @max-3xl:row-start-1",
  name: "@max-3xl:col-span-3 @max-3xl:col-start-2 @max-3xl:row-start-1 @max-lg:col-span-2",
  acts: "@max-3xl:col-start-5 @max-3xl:row-start-1 @max-lg:col-start-4",
  vlan: "@max-3xl:col-start-2 @max-3xl:row-start-2",
  state: "@max-3xl:col-start-3 @max-3xl:row-start-2",
  addr: "@max-3xl:col-start-4 @max-3xl:row-start-2 @max-lg:col-span-2 @max-lg:col-start-2 @max-lg:row-start-3",
  more: "@max-3xl:col-span-4 @max-3xl:col-start-2 @max-3xl:row-start-3 @max-lg:col-span-3 @max-lg:row-start-4",
};

/* The columns of a narrower list, the same in its head and in every card. */
const MEDIUM = "@max-3xl:grid-cols-[2rem_minmax(0,1fr)_minmax(0,1fr)_minmax(0,1.4fr)_3.75rem]";
const PHONE = "@max-lg:grid-cols-[2rem_minmax(0,1fr)_minmax(0,1fr)_auto]";

/* A cell; in a narrow list, under its column's name. */
function Cell({ head, at, children }: { head?: string; at: string; children: ReactNode }) {
  return (
    <div
      data-h={head}
      className={`grid min-w-0 content-start gap-1 ${at} ${
        head
          ? "@max-lg:before:text-xs @max-lg:before:text-base-content/70 @max-lg:before:content-[attr(data-h)]"
          : ""
      }`}
    >
      {children}
    </div>
  );
}

/** The connection's way through the steps it rests on, and the first that does not hold. */
function steps(t: T, c: Conn, fib: FibreView, known: boolean, svc: Service | undefined) {
  const link = c.x.up;
  const address =
    c.kind === "pppoe" ? !!(c.x.ipv4 || c.x.prefix || c.x.ipv6) : !!(c.x.ipv4 || c.x.ipv6);
  const holds = [
    fib.rxDbm != null && fib.quality !== "none" && fib.quality !== "tooWeak",
    fib.connected && known,
    !!svc,
    link,
    address,
  ];
  const label = [
    t("internet.stepLight"),
    t("internet.stepOmci"),
    `VLAN ${c.vlan}`,
    c.kind === "pppoe" ? "PPPoE" : c.kind === "dhcp" ? "DHCP" : t("internet.stepGateway"),
    t("internet.stepAddress"),
  ];
  const broken = holds.indexOf(false);
  /* A session that dials and a lease asked for are under way, not broken. */
  const under = broken === 3 && c.kind !== "static";
  return label.map((l, i) => ({
    label: l,
    tone: (broken < 0 || i < broken
      ? "good"
      : i > broken
        ? "neutral"
        : under
          ? "warn"
          : "bad") as Tone,
  }));
}

/** A connection's state: a dot, a word, and why when it is not working. */
function stateOf(t: T, lang: string, c: Conn, l2: boolean, svc: Service | undefined) {
  if (!l2) return { tone: "neutral" as Tone, word: t("internet.inactive"), why: null };
  if (!svc)
    return {
      tone: "neutral" as Tone,
      word: t("internet.inactive"),
      why: t("internet.whyNoVlan", { vid: c.vlan }),
    };
  if (c.x.up)
    return {
      tone: "good" as Tone,
      word: t("internet.connUp"),
      why:
        c.kind === "dhcp" && c.x.lease != null
          ? t("internet.lease", { time: duration(c.x.lease, lang as never) })
          : null,
    };
  if (c.kind === "pppoe")
    return { tone: "warn" as Tone, word: t("internet.connecting"), why: null };
  if (c.kind === "dhcp")
    return { tone: "warn" as Tone, word: t("internet.waitingAddress"), why: null };
  return { tone: "bad" as Tone, word: t("internet.noLink"), why: null };
}

/* A label over its value. */
function Pair({ k, children }: { k: string; children: ReactNode }) {
  return (
    <div className="min-w-0">
      <dt className="text-xs text-base-content/70">{k}</dt>
      <dd className="mt-1 break-words">{children}</dd>
    </div>
  );
}

/** What is a connection's kind's alone: its settings and what it was given. */
function Facts({ c }: { c: Conn }) {
  const { t, lang } = useI18n();
  const { data: live } = useLive();
  const net = useNetwork();
  const portId = gems(live).find((g) => g.gem.name === c.x.gemDev)?.gem.port_id;
  const channel =
    portId != null
      ? `GEM ${portId}`
      : c.x.gem === "auto"
        ? t("internet.autoGem")
        : `GEM ${c.x.gem}`;
  const dash = "–";
  if (c.kind === "pppoe") {
    const x = c.x;
    /* Sessions in one VLAN send from the MAC address of the first of them. */
    const first = sessions(live).find((y) => y.vlan === x.vlan && y.gem === x.gem);
    const shared = first && first.id !== x.id ? first : undefined;
    const macAddr = shared ? shared.mac : x.mac;
    /* The MTU the session came to: 1500 when the concentrator took RFC 4638. */
    const mtu = (x.up && net.data?.links.find((l) => l.name === x.ppp)?.mtu) || null;
    const down = downVlan(x);
    return (
      <>
        <Pair k={t("internet.user")}>
          <span className="break-all">{x.user}</span>
        </Pair>
        <Pair k={t("internet.family")}>{t(FAMILY[x.family])}</Pair>
        <Pair k={t("internet.factIface")}>
          {x.dev ? <Mono>{[x.dev, x.ppp].filter(Boolean).join(" → ")}</Mono> : dash}
          <Sub>{channel}</Sub>
        </Pair>
        <Pair k={t("internet.factMac")}>
          {macAddr ? <Mono>{macAddr}</Mono> : t("internet.macStick")}
          {shared && <Sub>{t("internet.macShared", { id: shared.id })}</Sub>}
        </Pair>
        <Pair k="MTU">
          {mtu ? t(mtu >= 1500 ? "internet.mtuRfc" : "internet.mtuPlain", { mtu }) : dash}
        </Pair>
        <Pair k="DNS">
          <MonoList items={x.dns} />
        </Pair>
        {x.pass && (
          <Pair k={t("internet.factDown")}>
            {down ? `VLAN ${down}` : t("switch.untagged")}
            {x.family !== "ipv6" && x.gateway && (
              <Sub>
                {t("internet.gatewayIs")} <Mono>{x.gateway}</Mono>
              </Sub>
            )}
            {x.family !== "ipv4" && x.prefix && !x.prefixHost && (
              <Sub>{t("internet.prefixWaiting")}</Sub>
            )}
          </Pair>
        )}
      </>
    );
  }
  const x = c.x;
  const fixed = c.kind === "static";
  return (
    <>
      <Pair k={t("internet.gateway4")}>
        {(fixed ? x.gateway : x.router) ? <Mono>{fixed ? x.gateway : x.router}</Mono> : dash}
      </Pair>
      <Pair k={t("internet.dnsServers")}>
        <MonoList items={fixed ? x.dnsSet.split(/\s+/).filter(Boolean) : x.dns} />
      </Pair>
      {x.family !== "ipv4" && (
        <>
          {fixed && (
            <Pair k={t("internet.gateway6")}>{x.gateway6 ? <Mono>{x.gateway6}</Mono> : dash}</Pair>
          )}
          <Pair k={t("internet.prefix6")}>
            {(fixed ? x.prefix6 : x.prefix) ? <Mono>{fixed ? x.prefix6 : x.prefix}</Mono> : dash}
          </Pair>
        </>
      )}
      {!fixed && (
        <Pair k={t("internet.factLease")}>{x.lease != null ? duration(x.lease, lang) : dash}</Pair>
      )}
      <Pair k={t("internet.factIface")}>
        {x.dev ? <Mono>{x.dev}</Mono> : dash}
        <Sub>{channel}</Sub>
      </Pair>
      <Pair k={t("internet.factMac")}>{x.mac ? <Mono>{x.mac}</Mono> : t("internet.macStick")}</Pair>
    </>
  );
}

/** A connection: its card, and what opens under it. */
function ConnCard({
  c,
  services,
  l2,
  known,
  open,
  onToggle,
  editing,
  onEdit,
}: {
  c: Conn;
  services: Service[];
  l2: boolean;
  known: boolean;
  open: boolean;
  onToggle: () => void;
  editing: boolean;
  onEdit: () => void;
}) {
  const { t, lang } = useI18n();
  const fib = fibre(useLive().data);
  const session = useSessionChange();
  const ipoe = useIpoeChange();
  const change = c.kind === "pppoe" ? session : ipoe;
  const state = useChangeState(change);
  const svc = serviceIn(services, c.vlan);
  const st = stateOf(t, lang, c, l2, svc);
  const more = `conn-${c.key.replace(":", "-")}`;
  const v4 = c.x.family !== "ipv6";
  const v6 = c.x.family !== "ipv4";

  const remove = () => {
    if (!window.confirm(t("internet.removeConfirm", { name: connName(t, c), vid: c.vlan }))) return;
    if (c.kind === "pppoe") session.mutate({ ID: String(c.x.id), USER: "" });
    else ipoe.mutate({ ID: String(c.x.id), KIND: "" });
  };

  /* The VID on the fibre, when the OLT translates the one the connection is named by. */
  const onFibre = svc?.vlan != null && String(svc.vlan) !== c.vlan ? svc.vlan : null;

  return (
    // biome-ignore lint/a11y/useKeyWithClickEvents lint/a11y/noStaticElementInteractions: the caret is its key
    <div
      className={`col-span-full grid cursor-pointer grid-cols-subgrid items-start gap-y-4 rounded-box border bg-base-100 px-4 py-3 ${MEDIUM} ${PHONE} @max-3xl:gap-x-4 @max-3xl:gap-y-3 @max-lg:gap-x-3 ${
        editing ? "border-primary ring-1 ring-primary" : "border-base-300"
      }`}
      onClick={(e) => {
        if ((e.target as Element).closest("button, a, input, [data-more]")) return;
        onToggle();
      }}
    >
      <div className={`-my-1.5 -ml-2 ${NARROW.caret}`}>
        <button
          type="button"
          className="btn btn-ghost btn-sm btn-square text-base-content/70"
          aria-expanded={open}
          aria-controls={more}
          aria-label={t("internet.details", { name: connName(t, c) })}
          onClick={onToggle}
        >
          <ChevronRight className={`size-4 transition-transform ${open ? "rotate-90" : ""}`} />
        </button>
      </div>
      <Cell head={t("internet.colVlan")} at={NARROW.vlan}>
        <span>{`VLAN ${c.vlan}`}</span>
        {onFibre != null && <Sub>{t("internet.onFibre", { vid: onFibre })}</Sub>}
      </Cell>
      <Cell at={NARROW.name}>
        <span className="font-semibold">{connName(t, c)}</span>
        <Sub>{t(FAMILY[c.x.family])}</Sub>
      </Cell>
      <Cell head={t("internet.colState")} at={NARROW.state}>
        <span className="flex items-center gap-2 whitespace-nowrap">
          <Dot tone={st.tone} />
          {st.word}
        </span>
        {st.why && <Sub>{st.why}</Sub>}
        {state.state === "bad" && <p className="text-xs text-error">{state.message}</p>}
      </Cell>
      <Cell head={t("internet.colAddress")} at={NARROW.addr}>
        {c.kind === "pppoe" ? (
          <>
            {v4 && <span>{c.x.ipv4 ? <Mono>{c.x.ipv4}</Mono> : "IPv4 –"}</span>}
            {v6 && (
              <span>
                {c.x.prefix ? (
                  <Mono>{c.x.prefix}</Mono>
                ) : c.x.ipv6 ? (
                  t("internet.prefixNone")
                ) : (
                  "IPv6 –"
                )}
              </span>
            )}
            <Sub>
              {c.x.pass
                ? downVlan(c.x)
                  ? t("internet.goesDown", { vid: downVlan(c.x) })
                  : t("internet.goesDownUntagged")
                : t("internet.staysRouter")}
            </Sub>
          </>
        ) : (
          <>
            {v4 && <span>{c.x.ipv4 ? <Mono>{c.x.ipv4}</Mono> : "IPv4 –"}</span>}
            {v6 && <span>{c.x.ipv6 ? <Mono>{c.x.ipv6}</Mono> : "IPv6 –"}</span>}
            <Sub>{t("internet.staysRouter")}</Sub>
          </>
        )}
      </Cell>
      <div className={`-my-1.5 -mr-2 flex items-center justify-end gap-1 ${NARROW.acts}`}>
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
      </div>
      {open && (
        <div
          id={more}
          data-more
          className={`col-span-5 col-start-2 grid cursor-auto gap-6 pb-1 ${NARROW.more}`}
        >
          <section>
            <h3 className="mb-2 text-sm font-semibold">{t("internet.steps")}</h3>
            <ol className="flex flex-wrap items-center gap-x-2 gap-y-1">
              {steps(t, c, fib, known, svc).map((s, i) => (
                <li key={s.label} className="flex items-center gap-2">
                  {i > 0 && (
                    <ChevronRight className="size-4 text-base-content/45" aria-hidden="true" />
                  )}
                  <Dot tone={s.tone} />
                  {s.label}
                </li>
              ))}
            </ol>
          </section>
          <dl className="grid grid-cols-2 gap-x-4 gap-y-3 @3xl:grid-cols-3 @max-md:grid-cols-1">
            <Facts c={c} />
          </dl>
        </div>
      )}
    </div>
  );
}

/** The connections, and how many of each the stick still has room for. */
export function ConnList({
  list,
  services,
  l2,
  known,
  editing,
  onEdit,
}: {
  list: Conn[];
  services: Service[];
  /** The fibre is up and the OLT set services: the connections can work. */
  l2: boolean;
  known: boolean;
  editing: string | null;
  onEdit: (c: Conn) => void;
}) {
  const { t } = useI18n();
  const [open, setOpen] = useState<string[]>([]);
  const toggle = (key: string) =>
    setOpen((o) => (o.includes(key) ? o.filter((k) => k !== key) : [...o, key]));
  return (
    <section aria-labelledby="conns" className="@container flex flex-col xl:h-full xl:min-h-0">
      {/* As high as the form's title, which sits within the form's border. */}
      <div className="flex min-h-14 shrink-0 items-center border-t border-transparent pt-4 pb-2">
        <h2 id="conns" className="text-base font-semibold">
          {t("internet.conns")}
        </h2>
      </div>
      {/* The cards scroll under the names of the columns, which stay, and above the count. */}
      <div className="min-h-0 flex-1 px-px xl:overflow-auto">
        {list.length === 0 ? (
          <div className="rounded-box border border-dashed border-base-300 px-6 py-8 text-center">
            <p className="text-base font-semibold">{t("internet.empty")}</p>
            <p className="mx-auto mt-1 max-w-prose text-base-content/70">
              {t("internet.emptyHint")}
            </p>
          </div>
        ) : (
          <div className="grid grid-cols-[2rem_auto_minmax(8rem,1fr)_auto_auto_auto] gap-x-4 gap-y-2 pb-2 @max-3xl:flex @max-3xl:flex-col @max-3xl:gap-3">
            <div
              className={`sticky top-0 z-10 col-span-full grid grid-cols-subgrid border-x border-b border-x-transparent border-b-base-300 bg-base-200 px-4 pt-1 pb-2 text-xs font-semibold text-base-content/70 ${MEDIUM} @max-3xl:gap-x-4 @max-lg:hidden`}
            >
              <span />
              <span>{t("internet.colVlan")}</span>
              <span className="@max-3xl:hidden">{t("internet.colConn")}</span>
              <span>{t("internet.colState")}</span>
              <span>{t("internet.colAddress")}</span>
              <span />
            </div>
            {list.map((c) => (
              <ConnCard
                key={c.key}
                c={c}
                services={services}
                l2={l2}
                known={known}
                open={open.includes(c.key)}
                onToggle={() => toggle(c.key)}
                editing={editing === c.key}
                onEdit={() => onEdit(c)}
              />
            ))}
          </div>
        )}
      </div>
      {/* As high as the form's buttons, so that the two lines over them meet. */}
      <p className="flex min-h-16 shrink-0 items-center border-t border-base-300 text-xs text-base-content/70">
        {t("internet.room", { n: list.length, max: MAX_CONNS })}
      </p>
    </section>
  );
}
