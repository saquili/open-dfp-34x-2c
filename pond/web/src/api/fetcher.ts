// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The requests Orval makes from pond/openapi.json (src/api/gen) go through
 * here, into the panel's one way to pond, request() (client.ts): its session,
 * X-Pond header and errors. A change of settings is described as an object
 * of its keys sent as text/plain, which pond takes as KEY=VALUE lines; a
 * POST without a body is sent empty, as a change still.
 */

import { lines, request } from "./client";

/** What Orval gives a request: the method, the headers and the body it made. */
export interface PondInit {
  method?: string;
  headers?: HeadersInit;
  body?: unknown;
  signal?: AbortSignal | null;
}

/*
 * The headers a request adds of its own, root's password for one, as they
 * were named; the type of the body is request()'s to say.
 */
function extra(headers?: HeadersInit): Record<string, string> {
  const pairs = headers instanceof Headers ? [...headers.entries()] : Object.entries(headers ?? {});
  return Object.fromEntries(pairs.filter(([k]) => k.toLowerCase() !== "content-type"));
}

export async function pondFetch<T>(url: string, init: PondInit = {}): Promise<T> {
  const text = new Headers(init.headers).get("content-type") === "text/plain";
  const { body } = init;
  if (init.method !== "POST") return request<T>(url, undefined, extra(init.headers));
  if (body === undefined || body === null) return request<T>(url, "", extra(init.headers));
  if (typeof body === "string") return request<T>(url, body, extra(init.headers));
  const values = body as Record<string, string | number | boolean | undefined>;
  return request<T>(url, text ? lines(values) : values, extra(init.headers));
}

export default pondFetch;
