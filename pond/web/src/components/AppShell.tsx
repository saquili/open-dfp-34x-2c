// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The frame around every page: the menu (a drawer on a phone), whether the
 * stick answers, the language and the user. When the stick stops answering,
 * the pages keep the last figures and a bar says since when.
 */

import { Link, Outlet, useNavigate, useRouterState } from "@tanstack/react-router";
import {
  Activity,
  Cable,
  Globe,
  KeyRound,
  LayoutDashboard,
  LogOut,
  Menu,
  Router,
  Settings2,
  WifiOff,
} from "lucide-react";
import type { ReactNode } from "react";
import { useSignOut } from "../api/auth";
import { OfflineError } from "../api/client";
import { useLive } from "../api/live";
import { type Key, useI18n } from "../i18n";
import { clock } from "../lib/format";

const NAV: { to: string; key: Key; icon: ReactNode }[] = [
  { to: "/", key: "nav.overview", icon: <LayoutDashboard className="size-4" /> },
  { to: "/fibre", key: "nav.fibre", icon: <Activity className="size-4" /> },
  { to: "/internet", key: "nav.internet", icon: <Globe className="size-4" /> },
  { to: "/router", key: "nav.router", icon: <Router className="size-4" /> },
  { to: "/system", key: "nav.system", icon: <Settings2 className="size-4" /> },
];

function Connection() {
  const { t, lang } = useI18n();
  const live = useLive();
  const offline = live.error instanceof OfflineError;
  if (offline) {
    return (
      <span className="flex items-center gap-1.5 text-sm text-error">
        <WifiOff className="size-4" />
        <span className="hidden sm:inline">{t("connection.offline")}</span>
      </span>
    );
  }
  return (
    <span className="flex items-center gap-1.5 text-sm text-base-content/70">
      <span className={`size-2 rounded-full ${live.isSuccess ? "bg-success" : "bg-base-300"}`} />
      <span className="hidden sm:inline">{t("connection.live")}</span>
      {live.dataUpdatedAt > 0 && (
        <span className="tabular hidden md:inline">· {clock(live.dataUpdatedAt, lang, true)}</span>
      )}
    </span>
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

export function AppShell({ children }: { children?: ReactNode }) {
  const { t, lang, setLang } = useI18n();
  const signOut = useSignOut();
  const navigate = useNavigate();
  const path = useRouterState({ select: (s) => s.location.pathname });
  const live = useLive();
  const model = live.data?.model;

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
      <div className="drawer-content flex min-h-dvh flex-col">
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
              {model && <span className="text-sm text-base-content/60"> · {model}</span>}
            </div>
          </div>
          <Connection />
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
        <main className="mx-auto w-full max-w-6xl flex-1 p-4 sm:p-6">
          <OfflineBar />
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
