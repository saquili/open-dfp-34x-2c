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
import { useI18n } from "./i18n";
import { Fibre } from "./pages/Fibre";
import { Internet } from "./pages/Internet";
import { Overview } from "./pages/Overview";
import { SfpPage } from "./pages/SfpPage";
import { ChangePassword, SignIn } from "./pages/SignIn";
import { Stats } from "./pages/Stats";
import { Switch } from "./pages/Switch";
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

const overview = createRoute({ getParentRoute: () => app, path: "/", component: Overview });
const fibre = createRoute({ getParentRoute: () => app, path: "/fibre", component: Fibre });
const internet = createRoute({ getParentRoute: () => app, path: "/internet", component: Internet });
const sfpPage = createRoute({
  getParentRoute: () => app,
  path: "/sfp",
  component: SfpPage,
});
const switchPage = createRoute({ getParentRoute: () => app, path: "/switch", component: Switch });
const system = createRoute({ getParentRoute: () => app, path: "/system", component: System });
const stats = createRoute({ getParentRoute: () => app, path: "/stats", component: Stats });

const tree = root.addChildren([
  signIn,
  forcedPassword,
  app.addChildren([overview, fibre, internet, sfpPage, switchPage, stats, system]),
]);

export function makeRouter(queryClient: QueryClient) {
  return createRouter({
    routeTree: tree,
    context: { queryClient },
    defaultPreload: "intent",
    scrollRestoration: true,
  });
}

declare module "@tanstack/react-router" {
  interface Register {
    router: ReturnType<typeof makeRouter>;
  }
}
