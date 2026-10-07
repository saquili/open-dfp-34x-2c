// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { MutationCache, QueryCache, QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { RouterProvider } from "@tanstack/react-router";
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { sessionQuery } from "./api/auth";
import { AuthError, OfflineError } from "./api/client";
import { I18nProvider } from "./i18n";
import { makeRouter } from "./router";
import "./styles.css";

/* A request that finds the session gone sends the reader back to the sign-in. */
function lostSession(error: unknown) {
  if (!(error instanceof AuthError)) return;
  queryClient.setQueryData(sessionQuery.queryKey, null);
  if (router.state.location.pathname !== "/login") router.navigate({ to: "/login" });
}

const queryClient = new QueryClient({
  queryCache: new QueryCache({ onError: lostSession }),
  mutationCache: new MutationCache({ onError: lostSession }),
  defaultOptions: {
    queries: {
      /* Keep showing the last figures while the stick is away. */
      retry: (n, e) => e instanceof OfflineError && n < 1,
      refetchOnWindowFocus: true,
    },
  },
});
const router = makeRouter(queryClient);

document.documentElement.lang = navigator.language.toLowerCase().startsWith("pl") ? "pl" : "en";

const mount = document.getElementById("root");
if (!mount) throw new Error("no #root to mount the panel in");

createRoot(mount).render(
  <StrictMode>
    <QueryClientProvider client={queryClient}>
      <I18nProvider>
        <RouterProvider router={router} />
      </I18nProvider>
    </QueryClientProvider>
  </StrictMode>,
);
