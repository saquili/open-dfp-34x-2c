// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The connections: a table whose rows are cards, one a connection of
 * whatever kind, the same columns for all; a card opens on what is its kind's
 * alone. The columns line up from card to card (subgrid); in a narrow list a
 * card's cells stand one under another, each under its column's name.
 */

import { Pencil, Trash2 } from "lucide-react";
import { type ReactNode, useState } from "react";
import { useIpoeChange, useNetwork, useSessionChange } from "../../api/config";
import { useLive } from "../../api/live";
import { Acts, AT, CARD, Caret, Cell, HEAD, opens, ROWS } from "../../components/CardTable";
import { useChangeState } from "../../components/Field";
import { Dot } from "../../components/ui";
import { useI18n } from "../../i18n";
import { duration } from "../../lib/format";
import type { Service } from "../../lib/omci";
import { downVlan, gems, MAX_CONNS, sessions, type Tone } from "../../lib/state";
import { type Conn, serviceIn } from "./conns";
import { connName, FAMILY, Mono, MonoList, Sub, type T } from "./shared";

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
      {x.family !== "ipv6" && (
        <Pair k={t("internet.gateway4")}>
          {(fixed ? x.gateway : x.router) ? <Mono>{fixed ? x.gateway : x.router}</Mono> : dash}
        </Pair>
      )}
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
  open,
  onToggle,
  editing,
  onEdit,
}: {
  c: Conn;
  services: Service[];
  l2: boolean;
  open: boolean;
  onToggle: () => void;
  editing: boolean;
  onEdit: () => void;
}) {
  const { t, lang } = useI18n();
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
      className={`${CARD} cursor-pointer ${editing ? "border-primary ring-1 ring-primary" : "border-base-300"}`}
      onClick={opens(onToggle)}
    >
      <Caret
        open={open}
        controls={more}
        label={t("internet.details", { name: connName(t, c) })}
        onToggle={onToggle}
      />
      <Cell head={t("internet.colVlan")} at={AT.first}>
        <span>{`VLAN ${c.vlan}`}</span>
        {onFibre != null && <Sub>{t("internet.onFibre", { vid: onFibre })}</Sub>}
      </Cell>
      <Cell at={AT.name}>
        <span className="font-semibold">{connName(t, c)}</span>
        <Sub>{t(FAMILY[c.x.family])}</Sub>
      </Cell>
      <Cell head={t("internet.colState")} at={AT.second}>
        <span className="flex items-center gap-2 whitespace-nowrap">
          <Dot tone={st.tone} />
          {st.word}
        </span>
        {st.why && <Sub>{st.why}</Sub>}
        {state.state === "bad" && <p className="text-xs text-error">{state.message}</p>}
      </Cell>
      <Cell head={t("internet.colAddress")} at={AT.third}>
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
  editing,
  onEdit,
}: {
  list: Conn[];
  services: Service[];
  /** The fibre is up and the OLT set services: the connections can work. */
  l2: boolean;
  editing: string | null;
  onEdit: (c: Conn) => void;
}) {
  const { t } = useI18n();
  const [open, setOpen] = useState<string[]>([]);
  const toggle = (key: string) =>
    setOpen((o) => (o.includes(key) ? o.filter((k) => k !== key) : [...o, key]));
  return (
    <section aria-labelledby="conns" className="@container flex flex-col xl:h-full xl:min-h-0">
      {/*
       * The title and the names of the columns as high as the form's title
       * and tabs, which sit within the form's border, so that the lines
       * under them meet.
       */}
      <div className="mt-px flex min-h-14 shrink-0 items-center pt-4 pb-2">
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
          <div className={`grid grid-cols-[2rem_auto_minmax(8rem,1fr)_auto_auto_auto] ${ROWS}`}>
            <div className={HEAD}>
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
      <p className="mb-px flex min-h-16 shrink-0 items-center border-t border-base-300 text-xs text-base-content/70">
        {t("internet.room", { n: list.length, max: MAX_CONNS })}
      </p>
    </section>
  );
}
