// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Reading and changing the stick's configuration. A change goes to pond as
 * KEY=VALUE lines; pond checks it, keeps it in the stick's store and applies
 * it, and the live figures show the result at the next poll.
 */

import { queryOptions, useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { ApiError, lines, OfflineError, request } from "./client";
import type { Banks, Bridge, Firmware, Identity, Network, OmciValue, TcRules } from "./types";

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

/* What the OLT set up: slow to read, and it changes only when the OLT says so. */
export const omciQuery = queryOptions({
  queryKey: ["omci"],
  queryFn: async () => (await request<Value<OmciValue>>("/api/get/omci")).value,
  staleTime: 20_000,
  refetchInterval: 30_000,
});

export function useOmci() {
  return useQuery(omciQuery);
}

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

/* The banks: read often while an upload runs, rarely otherwise. */
export const firmwareQuery = queryOptions({
  queryKey: ["firmware"],
  queryFn: async () => (await request<Value<Firmware>>("/api/get/firmware")).value,
  refetchInterval: (query) => {
    const state = query.state.data?.update.state;
    return state === "receiving" || state === "writing" ? 2_000 : 30_000;
  },
});

export function useFirmware() {
  return useQuery(firmwareQuery);
}

/** Start a bank once, the other one without @bank: pond arms it and restarts. */
export function useTryBank() {
  return useMutation({
    mutationFn: (bank?: number) =>
      request<{ ok: boolean }>("/api/firmware/try", bank === undefined ? "" : String(bank)),
  });
}

/*
 * Send an update file. The body goes up as it is, with no time limit: the
 * stick writes it into the flash as it comes, then checks it, some 40 s in
 * all; the progress counts what has left the browser.
 */
export function uploadFirmware(
  file: Blob,
  onProgress: (sent: number, total: number) => void,
): Promise<string> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/firmware/upload");
    xhr.setRequestHeader("X-Pond", "1");
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    xhr.upload.onprogress = (e) => onProgress(e.loaded, e.total || file.size);
    xhr.onload = () => {
      let reply: { ok?: boolean; error?: string; detail?: string } = {};
      try {
        reply = JSON.parse(xhr.responseText);
      } catch {
        /* an answer that is not JSON is told by its status */
      }
      if (xhr.status === 200 && reply.ok) resolve(reply.detail ?? "");
      else reject(new ApiError(xhr.status, reply.error ?? xhr.statusText, reply.detail));
    };
    /* A connection cut short: what became of the upload is in the firmware value. */
    xhr.onerror = () => reject(new OfflineError());
    xhr.send(file);
  });
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
