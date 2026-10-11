// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The one way the panel talks to pond. The session is an HttpOnly cookie the
 * browser sends by itself; a change carries the X-Pond header, which a page of
 * another site cannot add without pond's consent, so a forged form cannot
 * change anything.
 *
 * Three failures are told apart, because the panel answers each differently:
 * no session (back to the sign-in), no answer at all (the stick is away: keep
 * showing the last data), and a refusal with a reason (show the reason).
 */

/** The stick did not answer: the network is down or the stick restarts. */
export class OfflineError extends Error {
  constructor() {
    super("offline");
    this.name = "OfflineError";
  }
}

/** There is no session, or it has ended. */
export class AuthError extends Error {
  constructor(message = "unauthorized") {
    super(message);
    this.name = "AuthError";
  }
}

/** pond refused the request and said why. */
export class ApiError extends Error {
  readonly status: number;
  readonly detail?: string;
  readonly retryAfter?: number;
  constructor(status: number, message: string, detail?: string, retryAfter?: number) {
    super(message);
    this.name = "ApiError";
    this.status = status;
    this.detail = detail;
    this.retryAfter = retryAfter;
  }
}

const TIMEOUT_MS = 12_000;

type Body = string | Record<string, unknown>;

export async function request<T>(
  path: string,
  body?: Body,
  extra?: Record<string, string>,
): Promise<T> {
  const headers: Record<string, string> = { Accept: "application/json", ...extra };
  let payload: string | undefined;
  if (body !== undefined) {
    headers["X-Pond"] = "1";
    if (typeof body === "string") {
      headers["Content-Type"] = "text/plain";
      payload = body;
    } else {
      headers["Content-Type"] = "application/json";
      payload = JSON.stringify(body);
    }
  }

  let response: Response;
  try {
    response = await fetch(path, {
      method: body === undefined ? "GET" : "POST",
      headers,
      body: payload,
      credentials: "same-origin",
      cache: "no-store",
      signal: AbortSignal.timeout(TIMEOUT_MS),
    });
  } catch {
    throw new OfflineError();
  }

  /* A proxy in front of the stick answers 502 to 504 while the stick is away. */
  if (response.status >= 502 && response.status <= 504) throw new OfflineError();

  const reply = (await response.json().catch(() => null)) as
    | (Record<string, unknown> & { error?: string; detail?: string; retry_after?: number })
    | null;

  if (response.status === 401) throw new AuthError(reply?.error);
  if (!response.ok) {
    throw new ApiError(
      response.status,
      reply?.error ?? response.statusText,
      reply?.detail,
      reply?.retry_after,
    );
  }
  return reply as T;
}

/** KEY=VALUE lines, the form pond takes a change of settings in. */
export function lines(values: Record<string, string | number | boolean | undefined>): string {
  return `${Object.entries(values)
    .filter(([, v]) => v !== undefined)
    .map(([k, v]) => `${k}=${typeof v === "boolean" ? (v ? "on" : "off") : v}`)
    .join("\n")}\n`;
}
