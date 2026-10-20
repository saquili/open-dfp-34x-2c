// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The session. pond keeps a few sessions, each an id in an HttpOnly cookie
 * that ends after a spell of idleness; the panel only asks who is signed in,
 * signs in and out, and changes the password. Every other read belongs to a
 * session: signing in or out forgets them, so that a page never shows what
 * was read, or refused, under the session before.
 */

import {
  mutationOptions,
  type QueryClient,
  queryOptions,
  useMutation,
  useQueryClient,
} from "@tanstack/react-query";
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

/** Forget every read but the session's: they were made, or refused, under another session. */
export function forgetReads(qc: QueryClient) {
  qc.removeQueries({ predicate: (q) => q.queryKey[0] !== sessionQuery.queryKey[0] });
}

export function signInOptions(qc: QueryClient) {
  return mutationOptions({
    mutationKey: ["auth", "login"],
    mutationFn: (v: { user: string; secret: string }) => request<Session>("/api/auth/login", v),
    onSuccess: (s) => {
      forgetReads(qc);
      qc.setQueryData(sessionQuery.queryKey, s);
    },
  });
}

export function signOutOptions(qc: QueryClient) {
  return mutationOptions({
    mutationKey: ["auth", "logout"],
    mutationFn: () => request<Record<string, never>>("/api/auth/logout", {}),
    onSettled: () => {
      qc.setQueryData(sessionQuery.queryKey, null);
      forgetReads(qc);
    },
  });
}

/* The new password ends the must_change of a session with the default one. */
export function passwordOptions(qc: QueryClient) {
  return mutationOptions({
    mutationKey: ["auth", "password"],
    mutationFn: (v: { current: string; next: string }) => request<Session>("/api/auth/password", v),
    onSuccess: (s) => qc.setQueryData(sessionQuery.queryKey, s),
  });
}

export const useSignIn = () => useMutation(signInOptions(useQueryClient()));
export const useSignOut = () => useMutation(signOutOptions(useQueryClient()));
export const useChangePassword = () => useMutation(passwordOptions(useQueryClient()));
