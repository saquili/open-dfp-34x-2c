// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The OMCI database in two tabs: what the OLT set up, entity by entity, and
 * the conversation in which it did, as it goes on. They point at each
 * other: an attribute says when it last changed and by whom, as far as the
 * conversation omcid keeps reaches back, and opens that message; a message
 * opened shows what it carried, its frames, and its entity in the database
 * with the attributes it touched.
 */

import { Link } from "@tanstack/react-router";
import { CircleAlert, Pause, Play, X } from "lucide-react";
import { type KeyboardEvent, type ReactNode, useEffect, useMemo, useRef, useState } from "react";
import { type Conversation, useConversation } from "../api/conversation";
import { Page, Tabs, useTab } from "../components/Page";
import { Filter } from "../components/Pager";
import { Reading } from "../components/Read";
import { Detail, Dot, Facts, TABLE } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { integer } from "../lib/format";
import type { Me } from "../lib/omci";
import { CLASSES } from "../lib/omci-classes";
import {
  type AttrValue,
  type Change,
  type Exchange,
  exchanges,
  type Frame,
  frameOf,
  isRead,
  lastChanges,
  MT,
} from "../lib/omci-frames";
import { type Shown, shown } from "../lib/omci-values";
import { reason } from "../lib/read";
import { FibreState, OmciState, useMib } from "./Fibre";

type T = (key: Key, vars?: Record<string, string | number>) => string;

/* A word of the dictionary under a path made at run time, or null when it has none. */
function word(t: T, path: string): string | null {
  const s = t(path as Key);
  return s === path ? null : s;
}

/** A class in a few words, or as G.988 names it. */
function className(t: T, cls: number): string {
  return word(t, `omci.cls.${cls}`) ?? CLASSES[cls]?.name ?? String(cls);
}

const hex4 = (n: number) => `0x${n.toString(16).padStart(4, "0")}`;
const hexBytes = (b: Uint8Array) => [...b].map((x) => x.toString(16).padStart(2, "0")).join(" ");

/** A value in words; a pointer to an entity of the MIB as a button that shows it. */
function valueText(t: T, s: Shown, go?: (cls: number, inst: number) => void): ReactNode {
  switch (s.kind) {
    case "hidden":
      return "••••";
    case "none":
      return t("omci.none");
    case "num":
      return String(s.n);
    case "hex":
      return `0x${s.n.toString(16).padStart(4, "0")}`;
    case "text":
      return `"${s.text}"`;
    case "word":
      return word(t, `omci.list.${s.list}.${s.n}`) ?? String(s.n);
    case "ref": {
      const label = `${className(t, s.cls)} ${hex4(s.inst)}`;
      return go ? (
        <button
          type="button"
          className="link link-primary text-left"
          onClick={() => go(s.cls, s.inst)}
        >
          {label}
        </button>
      ) : (
        label
      );
    }
    case "id":
      return hex4(s.inst);
    case "mac":
    case "ip":
      return s.text;
    case "rows":
      return t("omci.rows", { n: s.n });
    default:
      return null;
  }
}

/** When a change was, as a clock. */
function clock(at: number, lang: string): string {
  return new Date(at).toLocaleTimeString(lang === "pl" ? "pl-PL" : "en-GB", { hour12: false });
}

/* ------------------------------------------------------------- the classes */

function ClassList({
  classes,
  shownCls,
  q,
  setQ,
  pick,
}: {
  classes: number[];
  shownCls: number | null;
  q: string;
  setQ: (q: string) => void;
  pick: (cls: number) => void;
}) {
  const { t } = useI18n();
  const { mib } = useMib();
  return (
    <section className="card flex min-h-0 border border-base-300 bg-base-100">
      <div className="card-body min-h-0 gap-3 p-4">
        <h2 className="card-title text-base">{t("omci.classes")}</h2>
        <Filter value={q} onChange={setQ} placeholder={t("omci.findClass")} className="sm:w-full" />
        <ul className="grid min-h-0 flex-1 content-start gap-0.5 overflow-auto pr-1">
          {classes.map((c) => (
            <li key={c}>
              <button
                type="button"
                onClick={() => pick(c)}
                aria-current={c === shownCls ? "true" : undefined}
                title={CLASSES[c]?.name}
                className={`grid w-full grid-cols-[2.25rem_minmax(0,1fr)_auto] items-center gap-2 rounded-field px-2.5 py-1.5 text-left text-sm transition-colors ${c === shownCls ? "bg-primary/10 font-semibold text-primary" : "hover:bg-base-200"}`}
              >
                <span
                  className={`tabular text-xs ${c === shownCls ? "text-primary" : "text-base-content/70"}`}
                >
                  {c}
                </span>
                <span className="leading-snug">{className(t, c)}</span>
                <span className="tabular text-xs text-base-content/70">{mib.all(c).length}</span>
              </button>
            </li>
          ))}
        </ul>
      </div>
    </section>
  );
}

/* -------------------------------------------------------------- the entity */

function LastChange({
  change,
  current,
  kept,
  open,
}: {
  change: Change | undefined;
  current: boolean;
  kept: number;
  open: (seq: number) => void;
}) {
  const { t, lang } = useI18n();
  if (!change)
    return (
      <span className="text-base-content/70" title={t("omci.beforeHistory", { n: kept })}>
        –
      </span>
    );
  const who =
    change.mt === MT.create ? "omci.created" : change.byOlt ? "omci.setByOlt" : "omci.changedByOnu";
  return (
    <button
      type="button"
      className="grid text-left hover:underline"
      title={t("omci.openTalk")}
      onClick={() => open(change.seq)}
    >
      <span>{t(who)}</span>
      <Detail>
        {clock(change.at, lang)}
        {current && ` · ${t("omci.thisMessage")}`}
      </Detail>
    </button>
  );
}

function Entity({
  cls,
  inst,
  setInst,
  go,
  changes,
  touched,
  pickedSeq,
  kept,
  open,
}: {
  cls: number;
  inst: number;
  setInst: (i: number) => void;
  go: (cls: number, inst: number) => void;
  changes: Map<string, Map<number, Change>>;
  touched: Set<number>;
  pickedSeq: number | null;
  kept: number;
  open: (seq: number) => void;
}) {
  const { t } = useI18n();
  const { mib } = useMib();
  const all = mib.all(cls);
  const me: Me | undefined = mib.get(cls, inst) ?? all[0];
  const own = me ? changes.get(`${cls}/${me.id}`) : undefined;
  const rowsOf = (attr: number, b: Uint8Array | null) => {
    const size = CLASSES[cls]?.sizes[attr - 1];
    return b && size ? Math.floor(b.length / size) : 0;
  };
  return (
    <section className="card flex min-h-0 border border-base-300 bg-base-100">
      <div className="card-body min-h-0 gap-3 p-4">
        <div className="flex flex-wrap items-baseline gap-x-3 gap-y-1">
          <h2 className="card-title text-base">
            <span className="tabular text-base-content/70">{cls}</span>
            {className(t, cls)}
          </h2>
          <Detail>{CLASSES[cls]?.name}</Detail>
          <span className="ml-auto text-xs text-base-content/70">
            {all.length === 1 ? t("omci.instanceOne") : t("omci.instancesN", { n: all.length })}
          </span>
        </div>
        {all.length > 1 && (
          <div className="flex max-h-20 flex-wrap gap-1.5 overflow-auto">
            {all.map((m) => (
              <button
                key={m.id}
                type="button"
                onClick={() => setInst(m.id)}
                aria-current={m.id === me?.id ? "true" : undefined}
                className={`btn btn-xs font-mono ${m.id === me?.id ? "btn-primary btn-soft" : "btn-ghost border-base-300"}`}
              >
                {hex4(m.id)}
              </button>
            ))}
          </div>
        )}
        {me?.error ? (
          <p className="flex items-center gap-1.5 text-sm text-base-content/70">
            <CircleAlert className="size-4 shrink-0 text-warning" aria-hidden />
            {t("omci.tooBig")}
          </p>
        ) : (
          me && (
            <div className="min-h-0 flex-1 overflow-auto">
              <table className={`${TABLE} table-sm`}>
                <thead className="sticky top-0 bg-base-100">
                  <tr>
                    <th className="w-8">#</th>
                    <th className="w-[34%]">{t("omci.colAttr")}</th>
                    <th>{t("omci.colValue")}</th>
                    <th className="w-44">{t("omci.colLast")}</th>
                  </tr>
                </thead>
                <tbody>
                  {[...me.attrs.entries()].map(([n, b]) => {
                    const table = me.tables.has(n);
                    const s = shown(cls, n, b, {
                      other: (a) => me.attrs.get(a),
                      table,
                      rows: rowsOf(n, b),
                    });
                    const words = valueText(t, s, (c, i) => (mib.get(c, i) ? go(c, i) : undefined));
                    const change = own?.get(n) ?? own?.get(0);
                    const current = touched.has(n);
                    return (
                      <tr key={n} className={current ? "bg-primary/10" : undefined}>
                        <td className="tabular text-base-content/70">{n}</td>
                        <td className="break-words">{CLASSES[cls]?.attrs[n] ?? n}</td>
                        <td className="break-words">
                          <span className="grid gap-0.5">
                            {words != null && <span>{words}</span>}
                            {table && cls === 171 && (
                              <Link
                                to="/operator/services"
                                search={{ tab: "rules" } as never}
                                className="link link-primary text-xs"
                              >
                                {t("omci.toRules")}
                              </Link>
                            )}
                            {b &&
                              !table &&
                              b.length <= 32 &&
                              !(s.kind === "num" && b.length === 1) && (
                                <span className="font-mono text-xs break-all text-base-content/70">
                                  {hexBytes(b)}
                                </span>
                              )}
                          </span>
                        </td>
                        <td>
                          <LastChange
                            change={change}
                            current={current && pickedSeq != null && change?.seq === pickedSeq}
                            kept={kept}
                            open={open}
                          />
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          )
        )}
      </div>
    </section>
  );
}

/* -------------------------------------------------------- the conversation */

/** The attributes of an exchange, in words, one after another. */
function attrsText(t: T, e: Exchange): string {
  if (
    e.kind === "upload" ||
    e.kind === "alarms" ||
    e.kind === "download" ||
    e.kind === "tableRead"
  ) {
    if (e.from == null) return "";
    return e.of != null
      ? t("omci.steps", { from: e.from + 1, to: (e.to ?? e.from) + 1, of: e.of })
      : t("omci.stepsOpen", { from: e.from + 1, to: (e.to ?? e.from) + 1 });
  }
  const one = (a: AttrValue) => {
    const name = CLASSES[e.cls]?.attrs[a.attr] ?? `#${a.attr}`;
    if (!a.bytes) return name;
    const s = shown(e.cls, a.attr, a.bytes, {
      other: (n) => e.attrs.find((x) => x.attr === n)?.bytes,
      table: a.tableSize,
      rows: a.tableSize ? undefined : 0,
    });
    if (a.tableSize) return name;
    const v = valueText(t, s);
    return typeof v === "string" ? `${name} ${v}` : name;
  };
  return e.attrs.map(one).join(", ");
}

function Result({ e }: { e: Exchange }) {
  const { t } = useI18n();
  if (e.rejected)
    return (
      <span className="flex items-center gap-1.5 text-error">
        <Dot tone="bad" />
        {t("omci.rejected")}
      </span>
    );
  if (e.kind === "onu") return <span className="text-base-content/70">–</span>;
  if (e.waiting) return <Detail>{t("omci.waiting")}</Detail>;
  const ok = e.result == null || e.result === 0;
  return (
    <span className={`flex items-center gap-1.5 whitespace-nowrap ${ok ? "" : "text-error"}`}>
      <Dot tone={ok ? "good" : "bad"} />
      {ok ? t("omci.ok") : (word(t, `omci.result.${e.result}`) ?? String(e.result))}
      {e.took != null && <Detail>{t("omci.ms", { n: e.took })}</Detail>}
    </span>
  );
}

function whatText(t: T, e: Exchange): string {
  if (e.kind === "upload") return word(t, `omci.mt.${MT.mibUpload}`) ?? "";
  if (e.kind === "alarms") return word(t, `omci.mt.${MT.getAllAlarms}`) ?? "";
  return word(t, `omci.mt.${e.mt}`) ?? String(e.mt);
}

function Talk({
  talk,
  list,
  cls,
  picked,
  pick,
  narrow,
}: {
  talk: Conversation;
  list: Exchange[];
  cls: number | null;
  picked: number | null;
  pick: (e: Exchange) => void;
  /** A message is open beside the list: the attributes are left to it. */
  narrow: boolean;
}) {
  const { t, lang } = useI18n();
  const { omci, mib } = useMib();
  const [onlyClass, setOnlyClass] = useState(false);
  const [noReads, setNoReads] = useState(false);
  const [frozen, setFrozen] = useState<number | null>(null);

  const visible = (frozen == null ? list : list.filter((e) => e.seq < frozen))
    .filter((e) => !onlyClass || cls == null || e.cls === cls)
    .filter((e) => !noReads || !isRead(e))
    .reverse();
  const newer = frozen == null ? 0 : list.filter((e) => e.seq >= frozen).length;
  const state = talk.error != null ? "offline" : frozen != null ? "paused" : "live";
  const frames = omci.data?.frames;
  const onKey = (e: KeyboardEvent<HTMLTableRowElement>, x: Exchange) => {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      pick(x);
    }
  };
  const toggle = (on: boolean) =>
    `btn btn-xs ${on ? "btn-primary btn-soft" : "btn-ghost border-base-300"}`;

  /* A message opened from the database is brought into view. */
  const box = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (picked == null) return;
    box.current?.querySelector(`[data-seq="${picked}"]`)?.scrollIntoView({ block: "nearest" });
  }, [picked]);

  return (
    <section className="card flex min-h-0 border border-base-300 bg-base-100">
      <div className="card-body min-h-0 gap-2 p-4">
        <div className="flex flex-wrap items-center gap-x-3 gap-y-2">
          <span className="flex items-center gap-1.5 text-sm">
            <Dot tone={state === "live" ? "good" : state === "paused" ? "neutral" : "bad"} />
            {t(`omci.${state}`)}
            {talk.error != null && (
              <span className="text-xs text-error">{reason(t, talk.error)}</span>
            )}
          </span>
          <span className="ml-auto flex flex-wrap gap-1.5">
            {cls != null && (
              <button
                type="button"
                className={toggle(onlyClass)}
                aria-pressed={onlyClass}
                onClick={() => setOnlyClass(!onlyClass)}
              >
                {t("omci.onlyClass", { n: cls })}
              </button>
            )}
            <button
              type="button"
              className={toggle(noReads)}
              aria-pressed={noReads}
              onClick={() => setNoReads(!noReads)}
            >
              {t("omci.noReads")}
            </button>
            <button
              type="button"
              className={toggle(frozen != null)}
              onClick={() =>
                setFrozen(frozen == null ? (list[list.length - 1]?.seq ?? 0) + 1 : null)
              }
            >
              {frozen == null ? (
                <>
                  <Pause className="size-3.5" aria-hidden />
                  {t("omci.pause")}
                </>
              ) : (
                <>
                  <Play className="size-3.5" aria-hidden />
                  {t("omci.resumeN", { n: newer })}
                </>
              )}
            </button>
          </span>
        </div>
        <div ref={box} className="min-h-0 flex-1 overflow-auto">
          {talk.loading ? (
            <div className="py-4">
              <Reading />
            </div>
          ) : visible.length === 0 ? (
            <p className="py-4 text-sm text-base-content/70">
              {onlyClass && list.length ? t("omci.emptyFiltered") : t("omci.empty")}
            </p>
          ) : (
            <table className={`${TABLE} table-sm table-fixed`}>
              <thead className="sticky top-0 bg-base-100">
                <tr>
                  <th className="w-20">{t("omci.colTime")}</th>
                  <th className={narrow ? "w-20" : "w-24"}>{t("omci.colWho")}</th>
                  <th className={narrow ? "w-36" : "w-48"}>{t("omci.colWhat")}</th>
                  <th className={narrow ? "" : "w-64"}>{t("omci.colEntity")}</th>
                  {!narrow && <th>{t("omci.colAttrs")}</th>}
                  <th className={narrow ? "w-32" : "w-40"}>{t("omci.colResult")}</th>
                </tr>
              </thead>
              <tbody>
                {visible.map((e) => {
                  const inMib = !!mib.get(e.cls, e.inst);
                  const steps = e.kind !== "request" && e.kind !== "onu";
                  const said = [e.rejected && word(t, `omci.fate.${e.rejected}`), attrsText(t, e)]
                    .filter(Boolean)
                    .join(" · ");
                  return (
                    <tr
                      key={e.seq}
                      data-seq={e.seq}
                      tabIndex={0}
                      onClick={() => pick(e)}
                      onKeyDown={(k) => onKey(k, e)}
                      aria-selected={e.seq === picked}
                      className={`cursor-pointer ${e.seq === picked ? "bg-primary/10" : "hover:bg-base-200"}`}
                    >
                      <td className="tabular" title={new Date(e.at).toISOString()}>
                        {clock(e.at, lang)}
                      </td>
                      <td className="whitespace-nowrap">
                        {e.kind === "onu" ? t("omci.fromOnu") : t("omci.fromOlt")}
                      </td>
                      <td className="truncate">{whatText(t, e)}</td>
                      <td className="truncate">
                        {steps ? (
                          <span className="text-base-content/70">{narrow ? said || "–" : "–"}</span>
                        ) : (
                          <span className={inMib ? "" : "text-base-content/70"}>
                            {className(t, e.cls)}{" "}
                            <span className="font-mono text-xs">{hex4(e.inst)}</span>
                          </span>
                        )}
                      </td>
                      {!narrow && (
                        <td>
                          <span className="block truncate" title={said}>
                            {said}
                          </span>
                        </td>
                      )}
                      <td>
                        <Result e={e} />
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          )}
        </div>
        <p className="border-t border-base-200 pt-2 text-xs text-base-content/70">
          {frames &&
            `${t("omci.footer", {
              handled: integer(frames.handled, lang),
              rejected: integer(frames.rejected, lang),
              kept: integer(talk.frames.length, lang),
            })} · `}
          {t("omci.passwordHidden")}
        </p>
      </div>
    </section>
  );
}

/* A frame in hexadecimal, by its parts: the header, the contents and the trailer. */
function FrameBytes({ f }: { f: Frame }) {
  const { t, lang } = useI18n();
  const part = (from: number, to: number) => hexBytes(f.bytes.subarray(from, to));
  return (
    <div className="grid gap-1">
      <span className="text-xs text-base-content/70">
        {f.fromOlt ? t("omci.oltToStick") : t("omci.stickToOlt")} · {clock(f.at, lang)}.
        {String(f.at % 1000).padStart(3, "0")}
      </span>
      <dl className="grid grid-cols-[auto_minmax(0,1fr)] gap-x-3 font-mono text-xs">
        <dt className="font-sans text-base-content/70">{t("omci.frameHeader")}</dt>
        <dd className="break-all">{part(0, 8)}</dd>
        <dt className="font-sans text-base-content/70">{t("omci.frameContents")}</dt>
        <dd className="break-all">{part(8, 40)}</dd>
        <dt className="font-sans text-base-content/70">{t("omci.frameTrailer")}</dt>
        <dd className="break-all">{part(40, 48)}</dd>
      </dl>
    </div>
  );
}

/** The message opened: what it carried, its result, its frames, and its entity. */
function ExchangeCard({
  e,
  close,
  show,
}: {
  e: Exchange;
  close: () => void;
  show: (e: Exchange) => void;
}) {
  const { t, lang } = useI18n();
  const { mib } = useMib();
  const inMib = !!mib.get(e.cls, e.inst);
  const steps = e.kind !== "request" && e.kind !== "onu";
  const when = new Date(e.at);
  return (
    <section className="card flex min-h-0 border border-base-300 bg-base-100">
      <div className="card-body min-h-0 gap-3 overflow-auto p-4">
        <div className="flex items-start gap-2">
          <h2 className="card-title text-base">{whatText(t, e)}</h2>
          <button
            type="button"
            className="btn btn-ghost btn-xs btn-square ml-auto"
            aria-label={t("common.close")}
            onClick={close}
          >
            <X className="size-4" aria-hidden />
          </button>
        </div>
        <Facts
          rows={[
            [
              t("omci.colTime"),
              `${when.toLocaleDateString(lang === "pl" ? "pl-PL" : "en-GB")} ${clock(e.at, lang)}.${String(e.at % 1000).padStart(3, "0")}`,
            ],
            [t("omci.direction"), e.kind === "onu" ? t("omci.stickToOlt") : t("omci.oltToStick")],
            !steps && [
              t("omci.colEntity"),
              <span key="e" className="grid justify-items-start gap-0.5">
                <span>
                  {className(t, e.cls)} <span className="font-mono text-xs">{hex4(e.inst)}</span>
                </span>
                {inMib && (
                  <button
                    type="button"
                    className="link link-primary text-xs font-normal"
                    onClick={() => show(e)}
                  >
                    {t("omci.showInDb")}
                  </button>
                )}
              </span>,
            ],
            steps && [t("omci.colAttrs"), attrsText(t, e) || "–"],
            e.frames[0] && [t("omci.tci"), hex4(e.frames[0].tci)],
            [t("omci.colResult"), <Result key="r" e={e} />],
            e.rejected && ["", word(t, `omci.fate.${e.rejected}`)],
          ]}
        />
        {!steps && e.attrs.length > 0 && (
          <table className={`${TABLE} table-sm`}>
            <thead>
              <tr>
                <th className="w-8">#</th>
                <th>{t("omci.colAttr")}</th>
                <th>{t("omci.colValue")}</th>
              </tr>
            </thead>
            <tbody>
              {e.attrs.map((a) => {
                const s = a.bytes
                  ? shown(e.cls, a.attr, a.bytes, {
                      other: (n) => e.attrs.find((x) => x.attr === n)?.bytes,
                    })
                  : null;
                const words = s && !a.tableSize ? valueText(t, s) : null;
                return (
                  <tr key={a.attr}>
                    <td className="tabular text-base-content/70">{a.attr}</td>
                    <td className="break-words">{CLASSES[e.cls]?.attrs[a.attr] ?? a.attr}</td>
                    <td className="break-words">
                      <span className="grid gap-0.5">
                        {words != null && <span>{words}</span>}
                        {a.bytes && !(s?.kind === "num" && a.bytes.length === 1) && (
                          <span className="font-mono text-xs break-all text-base-content/70">
                            {hexBytes(a.bytes)}
                          </span>
                        )}
                        {!a.bytes && <span className="text-base-content/70">–</span>}
                      </span>
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        )}
        <div className="grid gap-2">
          <h3 className="text-sm font-semibold">{t("omci.frames")}</h3>
          {e.frames.map((f) => (
            <FrameBytes key={f.seq} f={f} />
          ))}
        </div>
      </div>
    </section>
  );
}

/* ---------------------------------------------------------------- the page */

const OMCI_TABS = ["db", "talk"] as const;
type OmciTab = (typeof OMCI_TABS)[number];

function OmciView({ tab, setTab }: { tab: OmciTab; setTab: (t: OmciTab) => void }) {
  const { t } = useI18n();
  const { mib } = useMib();
  const talk = useConversation();
  const list = useMemo(() => exchanges(talk.frames.map(frameOf)), [talk.frames]);
  const changes = useMemo(() => lastChanges(list), [list]);
  const [cls, setCls] = useState<number | null>(null);
  const [inst, setInst] = useState<number | null>(null);
  const [picked, setPicked] = useState<Exchange | null>(null);
  const [q, setQ] = useState("");

  const classes = [...mib.byClass.keys()].filter((c) => {
    if (!q) return true;
    const s = q.toLowerCase();
    return (
      String(c).includes(s) ||
      className(t, c).toLowerCase().includes(s) ||
      (CLASSES[c]?.name ?? "").toLowerCase().includes(s)
    );
  });
  /* The class picked while the filter keeps it, else the first the filter keeps. */
  const shownCls = cls != null && classes.includes(cls) ? cls : (classes[0] ?? null);
  const shownInst =
    shownCls != null && inst != null && mib.get(shownCls, inst)
      ? inst
      : (mib.all(shownCls ?? -1)[0]?.id ?? 0);

  const go = (c: number, i: number) => {
    setQ("");
    setCls(c);
    setInst(i);
  };
  /* A message in the database: its entity, with the attributes it touched. */
  const show = (e: Exchange) => {
    setPicked(e);
    go(e.cls, e.inst);
    setTab("db");
  };
  /* A change of an attribute in the conversation: its message, opened. */
  const open = (seq: number) => {
    const e = list.find((x) => x.seq === seq);
    if (!e) return;
    setPicked(e);
    setTab("talk");
  };
  const touched = new Set(
    picked && picked.cls === shownCls && picked.inst === shownInst
      ? picked.attrs.map((a) => a.attr)
      : [],
  );

  if (tab === "talk")
    return (
      <div
        className={`grid min-h-0 flex-1 gap-4 ${picked ? "grid-cols-[minmax(0,1fr)_26rem]" : "grid-cols-[minmax(0,1fr)]"}`}
      >
        <Talk
          talk={talk}
          list={list}
          cls={shownCls}
          picked={picked?.seq ?? null}
          pick={setPicked}
          narrow={picked != null}
        />
        {picked && <ExchangeCard e={picked} close={() => setPicked(null)} show={show} />}
      </div>
    );

  return (
    <div className="grid min-h-0 flex-1 grid-cols-[16rem_minmax(0,1fr)] gap-4">
      <ClassList
        classes={classes}
        shownCls={shownCls}
        q={q}
        setQ={setQ}
        pick={(c) => {
          setCls(c);
          setInst(null);
        }}
      />
      {shownCls != null && (
        <Entity
          key={shownCls}
          cls={shownCls}
          inst={shownInst}
          setInst={setInst}
          go={go}
          changes={changes}
          touched={touched}
          pickedSeq={picked?.seq ?? null}
          kept={talk.frames.length}
          open={open}
        />
      )}
    </div>
  );
}

export function OmciPage() {
  const { t } = useI18n();
  const [tab, setTab] = useTab(OMCI_TABS);
  return (
    <Page
      title={t("nav.omci")}
      hint={t(tab === "db" ? "omci.hintDb" : "omci.hintTalk")}
      tabs={
        <Tabs
          items={[
            { id: "db", label: t("omci.tabDb") },
            { id: "talk", label: t("omci.tabTalk") },
          ]}
          value={tab}
          onChange={setTab}
          label={t("nav.omci")}
        />
      }
      actions={<FibreState />}
    >
      <OmciState>
        <OmciView tab={tab} setTab={setTab} />
      </OmciState>
    </Page>
  );
}
