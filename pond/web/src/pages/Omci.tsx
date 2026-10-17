// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The OMCI database: what the OLT set up, entity by entity, and under it the
 * conversation in which it did, as it goes on. The two are one view: a
 * message picked in the conversation shows its entity above with the
 * attributes it touched, and each attribute says when it last changed and
 * by whom, as far as the conversation omcid keeps reaches back.
 */

import { Link } from "@tanstack/react-router";
import { CircleAlert, Pause, Play } from "lucide-react";
import { type KeyboardEvent, type ReactNode, useMemo, useState } from "react";
import { type Conversation, useConversation } from "../api/conversation";
import { Page } from "../components/Page";
import { Filter } from "../components/Pager";
import { Detail, Dot, TABLE } from "../components/ui";
import { type Key, useI18n } from "../i18n";
import { integer } from "../lib/format";
import type { Me } from "../lib/omci";
import { CLASSES } from "../lib/omci-classes";
import {
  type AttrValue,
  type Change,
  type Exchange,
  exchanges,
  frameOf,
  isRead,
  lastChanges,
  MT,
} from "../lib/omci-frames";
import { type Shown, shown } from "../lib/omci-values";
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
}: {
  change: Change | undefined;
  current: boolean;
  kept: number;
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
    <span className="grid">
      <span>{t(who)}</span>
      <Detail>
        {clock(change.at, lang)}
        {current && ` · ${t("omci.thisMessage")}`}
      </Detail>
    </span>
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
}: {
  cls: number;
  inst: number;
  setInst: (i: number) => void;
  go: (cls: number, inst: number) => void;
  changes: Map<string, Map<number, Change>>;
  touched: Set<number>;
  pickedSeq: number | null;
  kept: number;
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
}: {
  talk: Conversation;
  list: Exchange[];
  cls: number | null;
  picked: number | null;
  pick: (e: Exchange) => void;
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
  const state = talk.error ? "offline" : frozen != null ? "paused" : "live";
  const frames = omci.data?.frames;
  const onKey = (e: KeyboardEvent<HTMLTableRowElement>, x: Exchange) => {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      pick(x);
    }
  };
  const toggle = (on: boolean) =>
    `btn btn-xs ${on ? "btn-primary btn-soft" : "btn-ghost border-base-300"}`;

  return (
    <section className="card flex min-h-0 border border-base-300 bg-base-100">
      <div className="card-body min-h-0 gap-2 p-4">
        <div className="flex flex-wrap items-center gap-x-3 gap-y-2">
          <h2 className="card-title text-base">{t("omci.talk")}</h2>
          <span className="flex items-center gap-1.5 text-xs text-base-content/70">
            <Dot tone={state === "live" ? "good" : state === "paused" ? "neutral" : "bad"} />
            {t(`omci.${state}`)}
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
        <div className="min-h-0 flex-1 overflow-auto">
          {visible.length === 0 ? (
            <p className="py-4 text-sm text-base-content/70">
              {talk.loading
                ? ""
                : onlyClass && list.length
                  ? t("omci.emptyFiltered")
                  : t("omci.empty")}
            </p>
          ) : (
            <table className={`${TABLE} table-sm table-fixed`}>
              <thead className="sticky top-0 bg-base-100">
                <tr>
                  <th className="w-20">{t("omci.colTime")}</th>
                  <th className="w-24">{t("omci.colWho")}</th>
                  <th className="w-48">{t("omci.colWhat")}</th>
                  <th className="w-64">{t("omci.colEntity")}</th>
                  <th>{t("omci.colAttrs")}</th>
                  <th className="w-40">{t("omci.colResult")}</th>
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
                      <td>{whatText(t, e)}</td>
                      <td>
                        {steps ? (
                          <span className="text-base-content/70">–</span>
                        ) : (
                          <span className={inMib ? "" : "text-base-content/70"}>
                            {className(t, e.cls)}{" "}
                            <span className="font-mono text-xs">{hex4(e.inst)}</span>
                          </span>
                        )}
                      </td>
                      <td>
                        <span className="block truncate" title={said}>
                          {said}
                        </span>
                      </td>
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

/* ---------------------------------------------------------------- the page */

function OmciView() {
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
  const pickExchange = (e: Exchange) => {
    setPicked(e);
    if (mib.get(e.cls, e.inst)) go(e.cls, e.inst);
  };
  const touched = new Set(
    picked && picked.cls === shownCls && picked.inst === shownInst
      ? picked.attrs.map((a) => a.attr)
      : [],
  );

  return (
    <div className="grid min-h-0 flex-1 grid-rows-[minmax(0,1fr)_minmax(15rem,40%)] gap-4">
      <div className="grid min-h-0 grid-cols-[16rem_minmax(0,1fr)] gap-4">
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
          />
        )}
      </div>
      <Talk
        talk={talk}
        list={list}
        cls={shownCls}
        picked={picked?.seq ?? null}
        pick={pickExchange}
      />
    </div>
  );
}

export function OmciPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.omci")} hint={t("omci.hint")} actions={<FibreState />}>
      <OmciState>
        <OmciView />
      </OmciState>
    </Page>
  );
}
