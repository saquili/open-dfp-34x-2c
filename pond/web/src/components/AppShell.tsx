// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The frame around every page: the menu (a drawer on a phone), with the
 * pages of the internet, of the switch, of the operator and of the system
 * under an entry each, the internet's with how many things each holds;
 * whether the stick
 * answers, the panel's own settings (its theme and language, kept in the
 * browser) and the user. When the stick stops answering,
 * the pages keep the last figures and a bar says since when. A change of the
 * settings on trial is confirmed here, whatever the page (ADR 0011).
 */

import { Link, Outlet, useNavigate, useRouterState } from "@tanstack/react-router";
import {
  Cable,
  CircleAlert,
  Globe,
  KeyRound,
  LayoutDashboard,
  LogOut,
  Menu,
  Monitor,
  Moon,
  Network,
  RadioTower,
  Settings2,
  ShieldCheck,
  Sun,
  WifiOff,
} from "lucide-react";
import { type ReactNode, useEffect, useRef, useState } from "react";
import { useSignOut } from "../api/auth";
import { AuthError, OfflineError } from "../api/client";
import { useChanges } from "../api/config";
import { useLive } from "../api/live";
import { type Key, useI18n } from "../i18n";
import { useCountdown } from "../lib/countdown";
import { clock } from "../lib/format";
import { useProductName } from "../lib/product";
import { reason } from "../lib/read";
import { THEMES, type Theme, useTheme } from "../lib/theme";
import { confirmable, trialsToConfirm } from "../lib/trial";
import { INTERNET_PAGES, useInternetCounts } from "../pages/Internet";
import { Busy, Reading, Retry, useKept } from "./Read";

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
    { to: "/operator/phone", key: "nav.phone" },
    { to: "/operator/tv", key: "nav.tv" },
    { to: "/operator/omci", key: "nav.omci" },
  ],
};

/* The stick itself: what it has, what it runs, what it is, and every way into it. */
const SYSTEM: Section = {
  key: "nav.system",
  icon: <Settings2 className="size-4" />,
  pages: [
    { to: "/system/resources", key: "nav.resources" },
    { to: "/system/software", key: "nav.software" },
    { to: "/system/device", key: "nav.device" },
    { to: "/system/panel", key: "nav.panelAccess" },
    { to: "/system/ssh", key: "nav.ssh" },
    { to: "/system/https", key: "nav.https" },
  ],
};

/* The menu; "internet" stands for the internet's section, whose pages show their counts. */
const NAV: ((Entry & { icon: ReactNode }) | Section | "internet")[] = [
  { to: "/", key: "nav.overview", icon: <LayoutDashboard className="size-4" /> },
  "internet",
  SWITCH,
  OPERATOR,
  SYSTEM,
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
 * does not answer, or answers without its state.
 */
function LastAnswer({ now }: { now: number }) {
  const { t, lang } = useI18n();
  const live = useLive();
  const error = useKept(live)[0]?.error;
  const offline = error instanceof OfflineError;
  const failed = error != null && !offline && !(error instanceof AuthError);
  const at = live.dataUpdatedAt;
  const age = at > 0 ? Math.max(0, Math.round((now - at) / 1000)) : null;
  const ago =
    age == null
      ? null
      : new Intl.RelativeTimeFormat(lang, { numeric: "always", style: "narrow" }).format(
          -age,
          "second",
        );
  const tone =
    offline || failed
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
      : failed
        ? t("connection.noState")
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

const THEME_ICON: Record<Theme, ReactNode> = {
  system: <Monitor className="size-4" />,
  light: <Sun className="size-4" />,
  dark: <Moon className="size-4" />,
};

/*
 * The panel's theme: the system's, light or dark, in a menu beside the
 * language's, since both are settings of this browser and not of the stick.
 * The menu opens as its button takes the focus and shuts as a choice, or
 * Escape, takes it away.
 */
function ThemeMenu() {
  const { t } = useI18n();
  const [theme, setTheme] = useTheme();
  const label = t("theme.label", { name: t(`theme.${theme}`) });
  const shut = () => (document.activeElement as HTMLElement | null)?.blur();
  return (
    // biome-ignore lint/a11y/noStaticElementInteractions: Escape from within the menu's buttons
    <div
      className="dropdown dropdown-end"
      onKeyDown={(e) => {
        if (e.key === "Escape") shut();
      }}
    >
      <button
        type="button"
        className="btn btn-ghost btn-sm btn-square"
        aria-label={label}
        title={label}
      >
        {THEME_ICON[theme]}
      </button>
      <ul className="menu dropdown-content z-30 mt-2 w-44 rounded-box border border-base-300 bg-base-100 p-1">
        {THEMES.map((x) => (
          <li key={x}>
            <button
              type="button"
              aria-pressed={x === theme}
              className={x === theme ? "menu-active" : ""}
              onClick={() => {
                setTheme(x);
                shut();
              }}
            >
              {THEME_ICON[x]}
              {t(`theme.${x}`)}
            </button>
          </li>
        ))}
      </ul>
    </div>
  );
}

/*
 * A section's pages in the menu, each with how many things it holds where
 * @counts says. The section of the page open stands unfolded and the others
 * folded, so that the menu fits a laptop's screen whole; a click on a
 * section's name folds or unfolds it, until another page is opened.
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
  const here = section.pages.some((p) => path === p.to);
  const [open, setOpen] = useState(here);
  useEffect(() => setOpen(here), [here]);
  return (
    <li>
      <details open={open} onToggle={(e) => setOpen(e.currentTarget.open)}>
        <summary>
          {section.icon}
          {t(section.key)}
        </summary>
        <ul className="mt-1 flex flex-col gap-1">
          {section.pages.map((p) => (
            <li key={p.to}>
              <Link
                to={p.to}
                className={`flex ${path === p.to ? "menu-active" : ""}`}
                onClick={onGo}
              >
                {t(p.key)}
                {counts[p.to] != null && (
                  <span className="tabular ml-auto text-xs text-base-content/70">
                    {counts[p.to]}
                  </span>
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

/*
 * The live figures' failure, over every page: the stick does not answer,
 * since when, or it answered without its state, and why. The pages keep the
 * last figures; the poll goes on, and the button reads at once. A session
 * that ended sends the reader to the sign-in instead (main.tsx).
 */
function LiveBar() {
  const { t, lang } = useI18n();
  const live = useLive();
  const e = useKept(live)[0]?.error;
  if (!e || e instanceof AuthError) return null;
  const since = live.dataUpdatedAt ? clock(live.dataUpdatedAt, lang, true) : null;
  const offline = e instanceof OfflineError;
  const text = offline
    ? since
      ? t("connection.offlineSince", { time: since })
      : t("connection.offline")
    : since
      ? t("connection.failedSince", { reason: reason(t, e), time: since })
      : t("connection.failed", { reason: reason(t, e) });
  return (
    <div role="alert" className="alert alert-error alert-soft mb-4">
      {offline ? <WifiOff className="size-5" /> : <CircleAlert className="size-5" />}
      <span>{text}</span>
      <Retry reads={live} />
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
      className={`alert alert-soft mb-4 text-base-content ${offline ? "alert-warning" : "alert-info"}`}
    >
      <ShieldCheck className={`size-5 ${offline ? "text-warning" : "text-info"}`} />
      <span className="grid gap-1">
        <span>{text}</span>
        {/* A confirmation refused: why; it is sent again at the next fresh reading. */}
        {confirm.isError && !offline && (
          <span className="text-error">
            {t("connection.trialFailed", { reason: reason(t, confirm.error) })}
          </span>
        )}
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
  const productName = useProductName(t, live.data?.product);
  const product = t("app.product", { name: productName });
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
              <span className="font-semibold">{product}</span>
              {name && <span className="text-sm text-base-content/70"> · {name}</span>}
            </div>
          </div>
          <Busy />
          <LastAnswer now={now} />
          <Clock now={now} />
          <ThemeMenu />
          <div className="dropdown dropdown-end">
            <button
              type="button"
              className="btn btn-ghost btn-sm gap-1"
              aria-label={t("nav.language")}
            >
              <Globe className="size-4" />
              <span className="uppercase">{lang}</span>
            </button>
            <ul className="menu dropdown-content z-30 mt-2 w-36 rounded-box border border-base-300 bg-base-100 p-1">
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
            <ul className="menu dropdown-content z-30 mt-2 w-52 rounded-box border border-base-300 bg-base-100 p-1">
              <li>
                <Link to="/system/panel">
                  <KeyRound className="size-4" />
                  {t("nav.changePassword")}
                </Link>
              </li>
              <li>
                <button
                  type="button"
                  disabled={signOut.isPending}
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
          <LiveBar />
          <TrialBar />
          {/*
           * Every page shows the live figures: until their first answer, or
           * their first failure, which the bar above says, that they are read.
           */}
          {live.data === undefined && live.errorUpdatedAt === 0 ? (
            <Reading />
          ) : (
            (children ?? <Outlet />)
          )}
        </main>
      </div>
      <div className="drawer-side z-30">
        <label htmlFor="nav-drawer" aria-label="close" className="drawer-overlay" />
        <aside className="flex min-h-full w-60 flex-col gap-4 border-r border-base-300 bg-base-100 p-3">
          <div className="flex items-center gap-2 px-2 pt-1">
            <Cable className="size-5 shrink-0 text-primary" />
            <span className="truncate font-semibold" title={product}>
              {product}
            </span>
          </div>
          {nav}
        </aside>
      </div>
    </div>
  );
}
