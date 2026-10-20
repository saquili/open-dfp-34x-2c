// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The pages and who may see them. Every page but the sign-in needs a
 * session; a session with the default password sees only the password
 * change until it is done.
 */

import type { QueryClient } from "@tanstack/react-query";
import {
  createRootRouteWithContext,
  createRoute,
  createRouter,
  Outlet,
  redirect,
  useRouter,
} from "@tanstack/react-router";
import { WifiOff } from "lucide-react";
import { useEffect } from "react";
import { sessionQuery } from "./api/auth";
import { OfflineError } from "./api/client";
import { AppShell } from "./components/AppShell";
import { Reading } from "./components/Read";
import { useI18n } from "./i18n";
import { ChannelsPage, FibrePage, IdentityPage, ServicesPage } from "./pages/Fibre";
import {
  ClientsPage,
  FirewallPage,
  INTERNET_PAGES,
  LanPage,
  NatPage,
  WanPage,
} from "./pages/Internet";
import { OmciPage } from "./pages/Omci";
import { Overview } from "./pages/Overview";
import { SfpPage } from "./pages/SfpPage";
import { ChangePassword, SignIn } from "./pages/SignIn";
import { Stats } from "./pages/Stats";
import { MacPage, RoutesPage, VlansPage } from "./pages/Switch";
import { System } from "./pages/System";

interface Context {
  queryClient: QueryClient;
}

/* The stick did not answer before anything was shown: try again by itself. */
function Unreachable({ error }: { error: unknown }) {
  const { t } = useI18n();
  const router = useRouter();
  useEffect(() => {
    const id = setInterval(() => router.invalidate(), 5_000);
    return () => clearInterval(id);
  }, [router]);
  return (
    <div className="grid min-h-dvh place-items-center p-6 text-center">
      <div className="max-w-sm">
        <WifiOff className="mx-auto mb-3 size-10 text-error" />
        <p className="mb-4">{error instanceof OfflineError ? t("auth.offline") : String(error)}</p>
        <button type="button" className="btn btn-primary" onClick={() => router.invalidate()}>
          {t("common.retry")}
        </button>
      </div>
    </div>
  );
}

/* Who is signed in is asked before any page: while the answer is slow to come, that it is asked. */
function Asking() {
  return (
    <div className="grid min-h-dvh place-items-center p-6">
      <Reading />
    </div>
  );
}

const root = createRootRouteWithContext<Context>()({
  component: Outlet,
  errorComponent: ({ error }: { error: unknown }) => <Unreachable error={error} />,
});

const signIn = createRoute({
  getParentRoute: () => root,
  path: "/login",
  component: SignIn,
  beforeLoad: async ({ context }) => {
    const s = await context.queryClient.ensureQueryData(sessionQuery);
    if (s) throw redirect({ to: s.must_change ? "/password" : "/" });
  },
});

/* The default password must be changed before anything else. */
const forcedPassword = createRoute({
  getParentRoute: () => root,
  path: "/password",
  component: () => <ChangePasswordRoute />,
  beforeLoad: async ({ context }) => {
    const s = await context.queryClient.ensureQueryData(sessionQuery);
    if (!s) throw redirect({ to: "/login" });
  },
});

function ChangePasswordRoute() {
  const s = forcedPassword.useRouteContext({
    select: (c) => c.queryClient.getQueryData(sessionQuery.queryKey),
  });
  return s?.must_change ? (
    <ChangePassword forced />
  ) : (
    <AppShellWith>
      <ChangePassword forced={false} />
    </AppShellWith>
  );
}

function AppShellWith({ children }: { children: React.ReactNode }) {
  return <AppShell>{children}</AppShell>;
}

const app = createRoute({
  getParentRoute: () => root,
  id: "app",
  component: () => <AppShell />,
  beforeLoad: async ({ context }) => {
    const s = await context.queryClient.ensureQueryData(sessionQuery);
    if (!s) throw redirect({ to: "/login" });
    if (s.must_change) throw redirect({ to: "/password" });
  },
});

/* A page at @path. */
const page = <P extends string>(path: P, component: () => React.ReactNode) =>
  createRoute({ getParentRoute: () => app, path, component });

/* An address of before: it opens the page @to names for the tab it had, if any. */
const moved = <P extends string>(path: P, to: (tab: string) => string) =>
  createRoute({
    getParentRoute: () => app,
    path,
    beforeLoad: ({ search }) => {
      const tab = (search as { tab?: unknown }).tab;
      throw redirect({ to: to(typeof tab === "string" ? tab : "") });
    },
  });

/*
 * The menu's sections, each a few pages: the internet, the switch with its
 * ports, and what the operator set up. They were pages of a few tabs, and
 * the local network a page of its own before that: an address of before
 * opens the page its tab named.
 */
const pages = [
  page("/", Overview),
  page("/internet/wan", WanPage),
  page("/internet/lan", LanPage),
  page("/internet/clients", ClientsPage),
  page("/internet/nat", NatPage),
  page("/internet/fw", FirewallPage),
  page("/internet/routes", RoutesPage),
  page("/switch/sfp", SfpPage),
  page("/switch/fibre", FibrePage),
  page("/switch/vlans", VlansPage),
  page("/switch/mac", MacPage),
  page("/switch/stats", Stats),
  page("/operator/services", ServicesPage),
  page("/operator/channels", ChannelsPage),
  page("/operator/identity", IdentityPage),
  page("/operator/omci", OmciPage),
  page("/system", System),
  moved("/internet", (tab) => `/internet/${INTERNET_PAGES.find((p) => p === tab) ?? "wan"}`),
  moved("/lan", (tab) => (tab === "devices" ? "/internet/clients" : "/internet/lan")),
  moved("/sfp", () => "/switch/sfp"),
  moved(
    "/fibre",
    (tab) =>
      ({
        services: "/operator/services",
        channels: "/operator/channels",
        identity: "/operator/identity",
        omci: "/operator/omci",
      })[tab] ?? "/switch/fibre",
  ),
  moved(
    "/switch",
    (tab) =>
      ({
        vlans: "/switch/vlans",
        mac: "/switch/mac",
        routing: "/internet/routes",
        rules: "/operator/services",
      })[tab] ?? "/switch/sfp",
  ),
  moved("/stats", (tab) => (tab ? `/switch/stats?tab=${tab}` : "/switch/stats")),
  moved("/operator", () => "/operator/services"),
];

const tree = root.addChildren([signIn, forcedPassword, app.addChildren(pages)]);

export function makeRouter(queryClient: QueryClient) {
  return createRouter({
    routeTree: tree,
    /* "/" on the stick; the demo lives below its host's path. */
    basepath: import.meta.env.BASE_URL,
    context: { queryClient },
    defaultPreload: "intent",
    defaultPendingComponent: Asking,
    scrollRestoration: true,
  });
}

declare module "@tanstack/react-router" {
  interface Register {
    router: ReturnType<typeof makeRouter>;
  }
}
