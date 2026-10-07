// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Reading and changing the stick's configuration. A change goes to pond as
 * KEY=VALUE lines; pond checks it, keeps it in the stick's store and applies
 * it, and the live figures show the result at the next poll.
 */

import { queryOptions, useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { lines, request } from "./client";
import type { Banks, Bridge, Identity, Network, TcRules } from "./types";

interface Value<T> {
  id: string;
  value: T;
}

export const banksQuery = queryOptions({
  queryKey: ["banks"],
  queryFn: async () => (await request<Value<Banks>>("/api/get/banks")).value,
  staleTime: 60_000,
});

export const identityQuery = queryOptions({
  queryKey: ["identity"],
  queryFn: async () => (await request<Value<Identity>>("/api/get/identity")).value,
  staleTime: 30_000,
});

export function useBanks() {
  return useQuery(banksQuery);
}

export function useIdentity() {
  return useQuery(identityQuery);
}

/** A change of one of pond's writable values, then a fresh read of what it touches. */
function useChange(id: string, refresh: string[][]) {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (values: Record<string, string | undefined>) =>
      request<{ id: string; ok: boolean }>(`/api/set/${id}`, lines(values)),
    onSuccess: () => {
      for (const key of refresh) qc.invalidateQueries({ queryKey: key });
    },
  });
}

export const useSessionChange = () => useChange("session", [["live"]]);
export const useLinkChange = () => useChange("link", [["live"]]);
export const useIdentityChange = () => useChange("identity", [["identity"], ["live"]]);

export function useRestart() {
  return useMutation({ mutationFn: () => request<{ ok: boolean }>("/api/system/restart", {}) });
}

/*
 * The switch view's values: read only while the view is open, and less
 * often than the live figures, since the stick is slow to gather them.
 */
const SWITCH_POLL_MS = 10_000;

const switchQuery = <T>(id: string) =>
  queryOptions({
    queryKey: ["switch", id],
    queryFn: async () => (await request<Value<T>>(`/api/get/${id}`)).value,
    refetchInterval: SWITCH_POLL_MS,
  });

export const useNetwork = () => useQuery(switchQuery<Network>("network"));
export const useBridge = () => useQuery(switchQuery<Bridge>("bridge"));
export const useTcRules = () => useQuery(switchQuery<TcRules>("tc_rules"));
