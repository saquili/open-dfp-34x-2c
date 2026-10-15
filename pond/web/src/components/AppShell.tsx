// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The frame around every page: the menu (a drawer on a phone), with the
 * pages of the internet, of the switch and of the operator under an entry
 * each, the internet's with how many things each holds; whether the stick
 * answers, the language and the user. When the stick stops answering,
 * the pages keep the last figures and a bar says since when. A change of the
 * settings on trial is confirmed here, whatever the page (ADR 0014).
 */

import { Link, Outlet, useNavigate, useRouterState } from "@tanstack/react-router";
import {
  Cable,
  Globe,
  KeyRound,
  LayoutDashboard,
  LogOut,
  Menu,
  Network,
  RadioTower,
  Settings2,
  ShieldCheck,
  WifiOff,
} from "lucide-react";
import { type ReactNode, useEffect, useRef, useState } from "react";
import { useSignOut } from "../api/auth";
import { OfflineError } from "../api/client";
import { useChanges } from "../api/config";
import { useLive } from "../api/live";
import { type Key, useI18n } from "../i18n";
import { useCountdown } from "../lib/countdown";
import { clock } from "../lib/format";
import { confirmable, trialsToConfirm } from "../lib/trial";
import { INTERNET_PAGES, useInternetCounts } from "../pages/Internet";

/** A page of the menu. */
interface Entry {
  to: string;
  key: Key;
}

/** A section of the menu: an entry that folds its pages. */
interface Section {
  key: Key;
  icon: ReactNode;
  pages: Entry[];
}

/* The switch with its ports, and what the operator set up on the fibre. */
const SWITCH: Section = {
  key: "nav.switch",
  icon: <Network className="size-4" />,
  pages: [
    { to: "/switch/sfp", key: "nav.sfp" },
    { to: "/switch/fibre", key: "nav.fibre" },
    { to: "/switch/vlans", key: "nav.vlans" },
    { to: "/switch/mac", key: "nav.mac" },
    { to: "/switch/stats", key: "nav.stats" },
  ],
};
const OPERATOR: Section = {
  key: "nav.operator",
  icon: <RadioTower className="size-4" />,
  pages: [
    { to: "/operator/services", key: "nav.services" },
    { to: "/operator/channels", key: "nav.channels" },
    { to: "/operator/identity", key: "nav.identity" },
    { to: "/operator/omci", key: "nav.omci" },
  ],
};

/* The menu; "internet" stands for the internet's section, whose pages show their counts. */
const NAV: ((Entry & { icon: ReactNode }) | Section | "internet")[] = [
  { to: "/", key: "nav.overview", icon: <LayoutDashboard className="size-4" /> },
  "internet",
  SWITCH,
  OPERATOR,
  { to: "/system", key: "nav.system", icon: <Settings2 className="size-4" /> },
];

/** The time now, again every @ms milliseconds. */
export function useNow(ms = 1000): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), ms);
    return () => clearInterval(id);
  }, [ms]);
  return now;
}

/*
 * How long ago the stick last answered: green while the answers come as
 * often as they are asked for, amber when one is late, red when the stick
 * does not answer.
 */
function LastAnswer({ now }: { now: number }) {
  const { t, lang } = useI18n();
  const live = useLive();
  const offline = live.error instanceof OfflineError;
  const at = live.dataUpdatedAt;
  const age = at > 0 ? Math.max(0, Math.round((now - at) / 1000)) : null;
  const ago =
    age == null
      ? null
      : new Intl.RelativeTimeFormat(lang, { numeric: "always", style: "narrow" }).format(
          -age,
          "second",
        );
  const tone = offline
    ? "bg-error"
    : age == null
      ? "bg-base-300"
      : age > 12
        ? "bg-warning"
        : "bg-success";
  const text = offline
    ? ago
      ? t("connection.silentSince", { ago })
      : t("connection.offline")
    : ago
      ? t("connection.answered", { ago })
      : t("connection.connecting");
  return (
    <span
      className={`flex items-center gap-1.5 text-sm ${offline ? "text-error" : "text-base-content/70"}`}
      title={at > 0 ? clock(at, lang, true) : undefined}
    >
      {offline ? (
        <WifiOff className="size-4" />
      ) : (
        <span className={`size-2 rounded-full ${tone}`} />
      )}
      <span className="tabular hidden sm:inline">{text}</span>
    </span>
  );
}

function Clock({ now }: { now: number }) {
  const { lang } = useI18n();
  return (
    <time
      dateTime={new Date(now).toISOString()}
      title={new Intl.DateTimeFormat(lang, { dateStyle: "full" }).format(now)}
      className="tabular hidden text-sm font-medium md:inline"
    >
      {clock(now, lang, true)}
    </time>
  );
}

/*
 * A section's pages in the menu, open from the start, each with how many
 * things it holds where @counts says; a click on the section's name folds
 * them.
 */
function MenuSection({
  section,
  counts = {},
  path,
  onGo,
}: {
  section: Section;
  counts?: Record<string, number | null>;
  path: string;
  onGo: () => void;
}) {
  const { t } = useI18n();
  return (
    <li>
      <details open>
        <summary>
          {section.icon}
          {t(section.key)}
        </summary>
        <ul className="mt-1 flex flex-col gap-0.5">
          {section.pages.map((p) => (
            <li key={p.to}>
              <Link
                to={p.to}
                className={`flex ${path === p.to ? "menu-active" : ""}`}
                onClick={onGo}
              >
                {t(p.key)}
                {counts[p.to] != null && (
                  <span className="tabular ml-auto text-xs opacity-60">{counts[p.to]}</span>
                )}
              </Link>
            </li>
          ))}
        </ul>
      </details>
    </li>
  );
}

/* The internet's section, with the counts of its pages. */
function InternetSection({ path, onGo }: { path: string; onGo: () => void }) {
  const counts = useInternetCounts();
  const section: Section = {
    key: "nav.internet",
    icon: <Globe className="size-4" />,
    pages: INTERNET_PAGES.map((p) => ({ to: `/internet/${p}`, key: `nav.${p}` })),
  };
  return (
    <MenuSection
      section={section}
      counts={Object.fromEntries(INTERNET_PAGES.map((p) => [`/internet/${p}`, counts[p]]))}
      path={path}
      onGo={onGo}
    />
  );
}

function OfflineBar() {
  const { t, lang } = useI18n();
  const live = useLive();
  if (!(live.error instanceof OfflineError)) return null;
  const since = live.dataUpdatedAt ? clock(live.dataUpdatedAt, lang, true) : null;
  return (
    <div role="alert" className="alert alert-error alert-soft mb-4">
      <WifiOff className="size-5" />
      <span>{since ? t("connection.offlineSince", { time: since }) : t("connection.offline")}</span>
      <span
        role="status"
        className="loading loading-dots loading-sm"
        aria-label={t("connection.retrying")}
      />
    </div>
  );
}

/*
 * The changes on trial: confirmed as soon as a fresh reading shows them
 * applied, which the panel could only get by reaching the stick again; a new
 * management address only by a panel opened at it. While the stick does not
 * answer, or a new address waits for its own panel, the bar says when the
 * stick goes back.
 */
function TrialBar() {
  const { t } = useI18n();
  const live = useLive();
  const confirm = useChanges();
  const sent = useRef<string | null>(null);
  const settings = live.data?.settings;
  const here = window.location.hostname;
  const fresh = !live.error && !live.isFetching;
  const due = trialsToConfirm(settings, fresh, sent.current, here);
  useEffect(() => {
    if (due == null) return;
    sent.current = due;
    confirm.mutate(
      { CONFIRM: "1" },
      {
        onError: () => {
          sent.current = null;
        },
      },
    );
  }, [due, confirm.mutate]);
  const trials = settings?.trials ?? [];
  const first = trials.length ? Math.min(...trials.map((x) => x.until_s)) : null;
  const left = useCountdown(first, live.data?.uptime?.up_s, live.dataUpdatedAt);
  if (!trials.length) return null;
  const offline = live.error instanceof OfflineError;
  /* Only a new management address is left, which its own panel confirms. */
  const elsewhere = trials.some((x) => confirmable(x, here))
    ? undefined
    : trials.find((x) => x.address)?.address?.split("/")[0];
  const text = offline
    ? t("connection.trialWaiting", { left: left ?? "…" })
    : elsewhere
      ? t("connection.trialElsewhere", { address: elsewhere })
      : t("connection.trialConfirming");
  return (
    <div
      role="status"
      className={`alert alert-soft mb-4 ${offline ? "alert-warning" : "alert-info"}`}
    >
      <ShieldCheck className="size-5" />
      <span>{text}</span>
    </div>
  );
}

export function AppShell({ children }: { children?: ReactNode }) {
  const { t, lang, setLang } = useI18n();
  const signOut = useSignOut();
  const navigate = useNavigate();
  const path = useRouterState({ select: (s) => s.location.pathname });
  const live = useLive();
  const name = live.data?.hostname || live.data?.model;
  const now = useNow();

  /* On a phone the menu is a drawer, which a chosen page closes. */
  const shut = () => {
    const box = document.getElementById("nav-drawer") as HTMLInputElement | null;
    if (box) box.checked = false;
  };
  const nav = (
    <ul className="menu w-full gap-1 p-0">
      {NAV.map((n) =>
        n === "internet" ? (
          <InternetSection key={n} path={path} onGo={shut} />
        ) : "pages" in n ? (
          <MenuSection key={n.key} section={n} path={path} onGo={shut} />
        ) : (
          <li key={n.to}>
            <Link to={n.to} className={path === n.to ? "menu-active" : ""} onClick={shut}>
              {n.icon}
              {t(n.key)}
            </Link>
          </li>
        ),
      )}
    </ul>
  );

  return (
    <div className="drawer lg:drawer-open">
      <input id="nav-drawer" type="checkbox" className="drawer-toggle" />
      <div className="drawer-content flex min-h-dvh flex-col lg:h-dvh lg:min-h-0">
        <header className="navbar sticky top-0 z-20 gap-2 border-b border-base-300 bg-base-100 px-3">
          <label
            htmlFor="nav-drawer"
            className="btn btn-ghost btn-square btn-sm lg:hidden"
            aria-label="menu"
          >
            <Menu className="size-5" />
          </label>
          <div className="min-w-0 flex-1">
            <div className="truncate">
              <span className="font-semibold">{t("app.product")}</span>
              {name && <span className="text-sm text-base-content/60"> · {name}</span>}
            </div>
          </div>
          <LastAnswer now={now} />
          <Clock now={now} />
          <div className="dropdown dropdown-end">
            <button
              type="button"
              className="btn btn-ghost btn-sm gap-1"
              aria-label={t("nav.language")}
            >
              <Globe className="size-4" />
              <span className="uppercase">{lang}</span>
            </button>
            <ul className="menu dropdown-content z-30 mt-2 w-36 rounded-box border border-base-300 bg-base-100 p-1 shadow">
              <li>
                <button type="button" onClick={() => setLang("pl")}>
                  Polski
                </button>
              </li>
              <li>
                <button type="button" onClick={() => setLang("en")}>
                  English
                </button>
              </li>
            </ul>
          </div>
          <div className="dropdown dropdown-end">
            <button type="button" className="btn btn-ghost btn-sm btn-square" aria-label="user">
              <KeyRound className="size-4" />
            </button>
            <ul className="menu dropdown-content z-30 mt-2 w-52 rounded-box border border-base-300 bg-base-100 p-1 shadow">
              <li>
                <Link to="/password">
                  <KeyRound className="size-4" />
                  {t("nav.changePassword")}
                </Link>
              </li>
              <li>
                <button
                  type="button"
                  onClick={() =>
                    signOut.mutate(undefined, { onSettled: () => navigate({ to: "/login" }) })
                  }
                >
                  <LogOut className="size-4" />
                  {t("nav.signOut")}
                </button>
              </li>
            </ul>
          </div>
        </header>
        <main className="mx-auto flex w-full max-w-7xl flex-1 flex-col p-4 sm:p-6 lg:min-h-0 lg:overflow-auto">
          <OfflineBar />
          <TrialBar />
          {children ?? <Outlet />}
        </main>
      </div>
      <div className="drawer-side z-30">
        <label htmlFor="nav-drawer" aria-label="close" className="drawer-overlay" />
        <aside className="flex min-h-full w-60 flex-col gap-4 border-r border-base-300 bg-base-100 p-3">
          <div className="flex items-center gap-2 px-2 pt-1">
            <Cable className="size-5 text-primary" />
            <span className="font-semibold">{t("app.product")}</span>
          </div>
          {nav}
        </aside>
      </div>
    </div>
  );
}
