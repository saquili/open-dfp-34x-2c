// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The frame around every page: the menu (a drawer on a phone), whether the
 * stick answers, the language and the user. When the stick stops answering,
 * the pages keep the last figures and a bar says since when. A change of the
 * settings on trial is confirmed here, whatever the page (ADR 0014).
 */

import { Link, Outlet, useNavigate, useRouterState } from "@tanstack/react-router";
import {
  Activity,
  BarChart3,
  Cable,
  EthernetPort,
  Globe,
  KeyRound,
  LayoutDashboard,
  LogOut,
  Menu,
  Network,
  Settings2,
  ShieldCheck,
  WifiOff,
} from "lucide-react";
import { type ReactNode, useEffect, useRef, useState } from "react";
import { useSignOut } from "../api/auth";
import { OfflineError } from "../api/client";
import { useSettingsChange } from "../api/config";
import { useLive } from "../api/live";
import { type Key, useI18n } from "../i18n";
import { useCountdown } from "../lib/countdown";
import { clock } from "../lib/format";
import { trialToConfirm } from "../lib/trial";

const NAV: { to: string; key: Key; icon: ReactNode }[] = [
  { to: "/", key: "nav.overview", icon: <LayoutDashboard className="size-4" /> },
  { to: "/fibre", key: "nav.fibre", icon: <Activity className="size-4" /> },
  { to: "/internet", key: "nav.internet", icon: <Globe className="size-4" /> },
  { to: "/sfp", key: "nav.sfp", icon: <EthernetPort className="size-4" /> },
  { to: "/switch", key: "nav.switch", icon: <Network className="size-4" /> },
  { to: "/stats", key: "nav.stats", icon: <BarChart3 className="size-4" /> },
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
 * A change of the settings on trial: confirmed as soon as a fresh reading
 * shows it applied, which the panel could only get by reaching the stick
 * again. While the stick does not answer, the bar says when it goes back.
 */
function TrialBar() {
  const { t } = useI18n();
  const live = useLive();
  const confirm = useSettingsChange();
  const sent = useRef<number | null>(null);
  const settings = live.data?.settings;
  const fresh = !live.error && !live.isFetching;
  const due = trialToConfirm(settings, fresh, sent.current);
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
  const left = useCountdown(settings?.trial?.until_s, live.data?.uptime?.up_s, live.dataUpdatedAt);
  if (!settings?.trial) return null;
  const waiting = live.error instanceof OfflineError;
  return (
    <div
      role="status"
      className={`alert alert-soft mb-4 ${waiting ? "alert-warning" : "alert-info"}`}
    >
      <ShieldCheck className="size-5" />
      <span>
        {waiting
          ? t("connection.trialWaiting", { left: left ?? "…" })
          : t("connection.trialConfirming")}
      </span>
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

  const nav = (
    <ul className="menu w-full gap-1 p-0">
      {NAV.map((n) => (
        <li key={n.to}>
          <Link
            to={n.to}
            className={path === n.to ? "menu-active" : ""}
            onClick={() => {
              const box = document.getElementById("nav-drawer") as HTMLInputElement | null;
              if (box) box.checked = false;
            }}
          >
            {n.icon}
            {t(n.key)}
          </Link>
        </li>
      ))}
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
