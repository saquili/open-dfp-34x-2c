// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Reading and changing the stick's configuration. A change goes to pond as
 * KEY=VALUE lines; pond checks it, keeps it in the stick's store and applies
 * it, and the live figures show the result at the next poll.
 */

import {
  mutationOptions,
  type QueryClient,
  type QueryKey,
  queryOptions,
  useMutation,
  useQuery,
  useQueryClient,
} from "@tanstack/react-query";
import { ApiError, lines, OfflineError, request } from "./client";
import type {
  Banks,
  Bridge,
  Clock,
  Firmware,
  Identity,
  LeaseInfo,
  Mgmt,
  Network,
  OmciValue,
  Sessions,
  SfpLane,
  Ssh,
  SshKey,
  Tls,
  TokenState,
} from "./types";

interface Value<T> {
  id: string;
  value: T;
}

export const banksQuery = queryOptions({
  queryKey: ["banks"],
  queryFn: async () => (await request<Value<Banks>>("/api/get/banks")).value,
  staleTime: 60_000,
});

/* The panel's certificates: they change only with the stick's addresses. */
export const tlsQuery = queryOptions({
  queryKey: ["tls"],
  queryFn: async () => (await request<Value<Tls>>("/api/get/tls")).value,
  staleTime: 300_000,
});

/* The clock and its source: timed writes its state every 10 s. */
export const clockQuery = queryOptions({
  queryKey: ["clock"],
  queryFn: async () => (await request<Value<Clock>>("/api/get/clock")).value,
  staleTime: 5_000,
  refetchInterval: 10_000,
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

/*
 * The devices with an address from the stick's DHCP servers: thousands of
 * them in a big house, which change as leases end, an hour or a day apart.
 * Only the pages that show them read them: when such a page opens, unless
 * a read of the last 10 s is there, and every 30 s while it is open; the
 * settings, which the panel polls every 5 s, do not carry them.
 */
export const LEASES_POLL_MS = 30_000;

export const leasesQuery = queryOptions({
  queryKey: ["leases"],
  queryFn: async () => (await request<Value<LeaseInfo[]>>("/api/get/leases")).value,
  staleTime: 10_000,
  refetchInterval: LEASES_POLL_MS,
  retry: false,
});

/** The leases, read where they are shown. */
export function useLeases() {
  return useQuery(leasesQuery);
}

/**
 * The leases as the last read left them, without a read of its own: for
 * what counts them beside every page, the menu; undefined before a page
 * that shows them was opened.
 */
export function useLeasesRead() {
  return useQuery({ ...leasesQuery, enabled: false });
}

export function useBanks() {
  return useQuery(banksQuery);
}

/* The ssh server's keys: they change only when a person changes them. */
export const sshQuery = queryOptions({
  queryKey: ["ssh"],
  queryFn: async () => (await request<Value<Ssh>>("/api/get/ssh")).value,
  staleTime: 60_000,
});

export function useSsh() {
  return useQuery(sshQuery);
}

/*
 * The sessions open: another browser signs in or idles out on its own, so
 * the list is read again every half a minute while it is shown.
 */
export const sessionsQuery = queryOptions({
  queryKey: ["sessions"],
  queryFn: () => request<Sessions>("/api/auth/sessions"),
  staleTime: 10_000,
  refetchInterval: 30_000,
});

export function useSessions() {
  return useQuery(sessionsQuery);
}

/* Whether a token of scripts is in force: it changes only when a person changes it. */
export const tokenQuery = queryOptions({
  queryKey: ["token"],
  queryFn: () => request<TokenState>("/api/token"),
  staleTime: 60_000,
});

export function useToken() {
  return useQuery(tokenQuery);
}

export function useTls() {
  return useQuery(tlsQuery);
}

export function useClock() {
  return useQuery(clockQuery);
}

export function useIdentity() {
  return useQuery(identityQuery);
}

/*
 * What each change makes stale: the reads the panel does again once pond has
 * taken it, and no others. The data path's settings are part of the live
 * figures, so every change of them refreshes those, its trial with them. A
 * connection or a network adds or takes away interfaces, addresses and
 * VLANs, which the switch's network and bridge show, and a connection or a
 * network removed takes its devices' leases along. A new management pair is
 * a trial and an address beside the old one, with a certificate of its own.
 * An update, a start of the other bank and the way back change what the
 * banks hold and what starts next. A restart changes nothing the panel
 * reads that the stick's coming back does not show by itself. A new
 * password and the end of the other sessions change the sessions open.
 */
export const REFRESH = {
  session: [["live"], ["leases"], ["switch"]],
  ipoe: [["live"], ["leases"], ["switch"]],
  lan: [["live"], ["leases"], ["switch"]],
  /* CONFIRM=1 keeps the changes on trial, REVERT=1 takes back all of them. */
  changes: [["live"], ["sfp_lane"], ["mgmt"], ["leases"], ["switch"]],
  fixed: [["live"]],
  forward: [["live"]],
  dmz: [["live"]],
  rule: [["live"]],
  fw: [["live"]],
  sfp_lane: [["sfp_lane"], ["live"]],
  mgmt: [["mgmt"], ["live"], ["switch"], ["tls"]],
  hostname: [["live"]],
  /* The name is in the bar over every page and in the tab's title, which the live figures carry. */
  product: [["live"]],
  identity: [["identity"], ["live"]],
  upload: [["firmware"], ["banks"]],
  try: [["firmware"], ["banks"]],
  return: [["firmware"], ["banks"]],
  restart: [],
  /* The ssh server's host key, its keys and whether it takes them alone. */
  ssh_host_key: [["ssh"]],
  ssh_key: [["ssh"]],
  ssh_keys_only: [["ssh"]],
  /* The panel's key and an own certificate: the certificates it serves. */
  tls_key: [["tls"]],
  tls_certificate: [["tls"]],
  tls_self_signed: [["tls"]],
  /* A new password ends the other sessions, and so does their end. */
  password: [["sessions"]],
  sessions_end: [["sessions"]],
  /* A new token of scripts, or none. */
  token: [["token"]],
} as const satisfies Record<string, readonly QueryKey[]>;

export type Change = keyof typeof REFRESH;

/*
 * Read again what @change made stale: the reads on screen at once, the
 * others when they are shown. A change is done when pond has taken it, not
 * when the reads are back, so that a slow read does not hold its form.
 */
export function refresh(qc: QueryClient, change: Change) {
  return Promise.all(
    REFRESH[change].map((queryKey) => qc.invalidateQueries({ queryKey: [...queryKey] })),
  );
}

/** A change of pond's writable value @id: KEY=VALUE lines, then a fresh read of what it touches. */
export function changeOptions(qc: QueryClient, id: Change) {
  return mutationOptions({
    mutationKey: ["set", id],
    mutationFn: (values: Record<string, string | undefined>) =>
      request<{ id: string; ok: boolean }>(`/api/set/${id}`, lines(values)),
    onSuccess: () => void refresh(qc, id),
  });
}

const useChange = (id: Change) => useMutation(changeOptions(useQueryClient(), id));

/* A session, a connection or a network removed takes its networks' devices with it. */
export const useSessionChange = () => useChange("session");
/** CONFIRM=1 keeps the changes on trial, REVERT=1 takes them all back. */
export const useChanges = () => useChange("changes");
export const useIpoeChange = () => useChange("ipoe");
export const useLanChange = () => useChange("lan");
export const useFixedChange = () => useChange("fixed");
export const useForwardChange = () => useChange("forward");
export const useDmzChange = () => useChange("dmz");
export const useRuleChange = () => useChange("rule");
export const useFwChange = () => useChange("fw");

/* The SFP lane and the management address: read often while a change waits. */
const waiting = (q: { state: { data?: { trial?: unknown; pending?: unknown } } }) =>
  q.state.data?.trial || q.state.data?.pending ? 2_000 : 15_000;

export const useSfpLane = () =>
  useQuery({
    queryKey: ["sfp_lane"],
    queryFn: async () => (await request<Value<SfpLane>>("/api/get/sfp_lane")).value,
    refetchInterval: waiting,
    retry: false,
  });
export const useMgmt = () =>
  useQuery({
    queryKey: ["mgmt"],
    queryFn: async () => (await request<Value<Mgmt>>("/api/get/mgmt")).value,
    refetchInterval: waiting,
    retry: false,
  });
export const useLaneChange = () => useChange("sfp_lane");
export const useMgmtChange = () => useChange("mgmt");
export const useHostnameChange = () => useChange("hostname");
export const useProductChange = () => useChange("product");
export const useIdentityChange = () => useChange("identity");

export function restartOptions(qc: QueryClient) {
  return mutationOptions({
    mutationKey: ["system", "restart"],
    mutationFn: () => request<{ ok: boolean }>("/api/system/restart", {}),
    onSuccess: () => void refresh(qc, "restart"),
  });
}

export const useRestart = () => useMutation(restartOptions(useQueryClient()));

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

/*
 * Root's password, which writing an update, starting the other bank, going
 * back to the shipped firmware and copying the flash need besides the
 * session: percent-encoded, since a header carries Latin-1 only.
 */
const PASSWORD_HEADER = "X-Pond-Password";
const passwordHeader = (secret: string) => ({ [PASSWORD_HEADER]: encodeURIComponent(secret) });

/** Start a bank once, the other one without @bank: pond arms it and restarts. */
export function tryOptions(qc: QueryClient) {
  return mutationOptions({
    mutationKey: ["firmware", "try"],
    mutationFn: ({ bank, secret }: { bank?: number; secret: string }) =>
      request<{ ok: boolean }>(
        "/api/firmware/try",
        bank === undefined ? "" : String(bank),
        passwordHeader(secret),
      ),
    onSuccess: () => void refresh(qc, "try"),
  });
}

export const useTryBank = () => useMutation(tryOptions(useQueryClient()));

/** Go back to the shipped firmware of @bank for good: pond keeps it and restarts. */
export function returnOptions(qc: QueryClient) {
  return mutationOptions({
    mutationKey: ["firmware", "return"],
    mutationFn: ({ bank, secret }: { bank: number; secret: string }) =>
      request<{ ok: boolean }>("/api/firmware/return", String(bank), passwordHeader(secret)),
    onSuccess: () => void refresh(qc, "return"),
  });
}

export const useReturnBank = () => useMutation(returnOptions(useQueryClient()));

/*
 * The keys of the stick's two servers, each change with root's password
 * besides the session (pond's routes of /api/ssh and /api/tls). After a new
 * key or certificate of the panel, the connections the browser opens from
 * then on meet the new certificate, which it warns about once unless it
 * trusts its authority; the answer carries the new value "tls", which the
 * panel shows at once, with the fingerprints to compare.
 */
type Keyed<T> = T & { secret: string };

export function sshHostKeyOptions(qc: QueryClient) {
  return mutationOptions({
    mutationKey: ["ssh", "host-key"],
    mutationFn: ({ secret }: Keyed<object>) =>
      request<{ ok: boolean; host_key?: { type: string; fingerprint: string } }>(
        "/api/ssh/host-key",
        "",
        passwordHeader(secret),
      ),
    onSuccess: () => void refresh(qc, "ssh_host_key"),
  });
}

export function sshKeyAddOptions(qc: QueryClient) {
  return mutationOptions({
    mutationKey: ["ssh", "key-add"],
    mutationFn: ({ key, secret }: Keyed<{ key: string }>) =>
      request<{ ok: boolean; key?: SshKey }>(
        "/api/ssh/keys",
        `${key.trim()}\n`,
        passwordHeader(secret),
      ),
    onSuccess: () => void refresh(qc, "ssh_key"),
  });
}

export function sshKeyRemoveOptions(qc: QueryClient) {
  return mutationOptions({
    mutationKey: ["ssh", "key-remove"],
    mutationFn: ({ fingerprint, secret }: Keyed<{ fingerprint: string }>) =>
      request<{ ok: boolean }>("/api/ssh/keys/remove", fingerprint, passwordHeader(secret)),
    onSuccess: () => void refresh(qc, "ssh_key"),
  });
}

export function sshKeysOnlyOptions(qc: QueryClient) {
  return mutationOptions({
    mutationKey: ["ssh", "keys-only"],
    mutationFn: ({ on, secret }: Keyed<{ on: boolean }>) =>
      request<{ ok: boolean }>("/api/ssh/keys-only", on ? "on" : "off", passwordHeader(secret)),
    onSuccess: () => void refresh(qc, "ssh_keys_only"),
  });
}

type TlsAnswer = { ok: boolean; tls?: Tls };

/* A change of the panel's certificates: its answer is the value tls, at once. */
function tlsOptions<V>(qc: QueryClient, change: Change, send: (v: V) => Promise<TlsAnswer>) {
  return mutationOptions({
    mutationKey: ["tls", change],
    mutationFn: send,
    onSuccess: (reply: TlsAnswer) => {
      if (reply.tls) qc.setQueryData(tlsQuery.queryKey, reply.tls);
      void refresh(qc, change);
    },
  });
}

export const tlsKeyOptions = (qc: QueryClient) =>
  tlsOptions(qc, "tls_key", ({ secret }: Keyed<object>) =>
    request<TlsAnswer>("/api/tls/key", "", passwordHeader(secret)),
  );

export const tlsCertificateOptions = (qc: QueryClient) =>
  tlsOptions(
    qc,
    "tls_certificate",
    ({ certificate, key, secret }: Keyed<{ certificate: string; key: string }>) =>
      request<TlsAnswer>("/api/tls/certificate", { certificate, key }, passwordHeader(secret)),
  );

export const tlsSelfSignedOptions = (qc: QueryClient) =>
  tlsOptions(qc, "tls_self_signed", ({ secret }: Keyed<object>) =>
    request<TlsAnswer>("/api/tls/self-signed", "", passwordHeader(secret)),
  );

/** End every session but this browser's, with root's password: how many ended. */
export function endOthersOptions(qc: QueryClient) {
  return mutationOptions({
    mutationKey: ["sessions", "others"],
    mutationFn: ({ secret }: Keyed<object>) =>
      request<{ ok: boolean; ended: number }>(
        "/api/auth/sessions/others",
        "",
        passwordHeader(secret),
      ),
    onSuccess: () => void refresh(qc, "sessions_end"),
  });
}

/**
 * A new token of scripts in place of the one in force, which the answer
 * carries once: the panel shows it until the page is left, and the stick
 * never again.
 */
export function tokenNewOptions(qc: QueryClient) {
  return mutationOptions({
    mutationKey: ["token", "new"],
    mutationFn: ({ secret }: Keyed<object>) =>
      request<{ ok: boolean; token?: string }>("/api/token/new", "", passwordHeader(secret)),
    onSuccess: () => void refresh(qc, "token"),
  });
}

/** No token of scripts. */
export function tokenOffOptions(qc: QueryClient) {
  return mutationOptions({
    mutationKey: ["token", "off"],
    mutationFn: ({ secret }: Keyed<object>) =>
      request<{ ok: boolean }>("/api/token/off", "", passwordHeader(secret)),
    onSuccess: () => void refresh(qc, "token"),
  });
}

export const useEndOthers = () => useMutation(endOthersOptions(useQueryClient()));
export const useTokenNew = () => useMutation(tokenNewOptions(useQueryClient()));
export const useTokenOff = () => useMutation(tokenOffOptions(useQueryClient()));
export const useSshHostKey = () => useMutation(sshHostKeyOptions(useQueryClient()));
export const useSshKeyAdd = () => useMutation(sshKeyAddOptions(useQueryClient()));
export const useSshKeyRemove = () => useMutation(sshKeyRemoveOptions(useQueryClient()));
export const useSshKeysOnly = () => useMutation(sshKeysOnlyOptions(useQueryClient()));
export const useTlsKey = () => useMutation(tlsKeyOptions(useQueryClient()));
export const useTlsCertificate = () => useMutation(tlsCertificateOptions(useQueryClient()));
export const useTlsSelfSigned = () => useMutation(tlsSelfSignedOptions(useQueryClient()));

/*
 * A copy of the whole flash, as a file the browser saves. It needs root's
 * password, which a link cannot send, so the copy comes through fetch, some
 * 8 MiB held in the page until it is saved; no time limit, as the stick reads
 * its flash as it sends it.
 */
export async function downloadFlash(secret: string): Promise<void> {
  let response: Response;
  try {
    response = await fetch("/api/firmware/backup", {
      headers: { "X-Pond": "1", ...passwordHeader(secret) },
      credentials: "same-origin",
      cache: "no-store",
    });
  } catch {
    throw new OfflineError();
  }
  if (!response.ok) {
    const reply = (await response.json().catch(() => null)) as {
      error?: string;
      detail?: string;
      retry_after?: number;
    } | null;
    throw new ApiError(
      response.status,
      reply?.error ?? response.statusText,
      reply?.detail,
      reply?.retry_after,
    );
  }
  const blob = await response.blob();
  const name =
    /filename="([^"]+)"/.exec(response.headers.get("Content-Disposition") ?? "")?.[1] ??
    "flash.bin";
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  URL.revokeObjectURL(url);
}

/*
 * Send an update file. The body goes up as it is, with no time limit: the
 * stick writes it into the flash as it comes, then checks it, some 40 s in
 * all; the progress counts what has left the browser.
 */
export function uploadFirmware(
  file: Blob,
  secret: string,
  onProgress: (sent: number, total: number) => void,
): Promise<string> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/firmware/upload");
    xhr.setRequestHeader("X-Pond", "1");
    for (const [k, v] of Object.entries(passwordHeader(secret))) xhr.setRequestHeader(k, v);
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    xhr.upload.onprogress = (e) => onProgress(e.loaded, e.total || file.size);
    xhr.onload = () => {
      let reply: { ok?: boolean; error?: string; detail?: string; retry_after?: number } = {};
      try {
        reply = JSON.parse(xhr.responseText);
      } catch {
        /* an answer that is not JSON is told by its status */
      }
      if (xhr.status === 200 && reply.ok) resolve(reply.detail ?? "");
      else
        reject(
          new ApiError(xhr.status, reply.error ?? xhr.statusText, reply.detail, reply.retry_after),
        );
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
