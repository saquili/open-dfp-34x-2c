// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The session. pond keeps a few sessions, each an id in an HttpOnly cookie
 * that ends after a spell of idleness; the panel only asks who is signed in,
 * signs in and out, and changes the password.
 */

import { queryOptions, useMutation, useQueryClient } from "@tanstack/react-query";
import { AuthError, request } from "./client";
import type { Session } from "./types";

export const sessionQuery = queryOptions({
  queryKey: ["session"],
  queryFn: async (): Promise<Session | null> => {
    try {
      return await request<Session>("/api/auth/session");
    } catch (e) {
      if (e instanceof AuthError) return null;
      throw e;
    }
  },
  staleTime: 60_000,
  retry: false,
});

export function useSignIn() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (v: { user: string; secret: string }) => request<Session>("/api/auth/login", v),
    onSuccess: (s) => qc.setQueryData(sessionQuery.queryKey, s),
  });
}

export function useSignOut() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => request<Record<string, never>>("/api/auth/logout", {}),
    onSettled: () => {
      qc.setQueryData(sessionQuery.queryKey, null);
      qc.removeQueries({ predicate: (q) => q.queryKey[0] !== "session" });
    },
  });
}

export function useChangePassword() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (v: { current: string; next: string }) => request<Session>("/api/auth/password", v),
    onSuccess: (s) => qc.setQueryData(sessionQuery.queryKey, s),
  });
}
