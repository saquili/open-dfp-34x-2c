// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The few pieces of a request mock that the mock stick needs: handlers that
 * answer one method on one path of any host ("*" + "/api/..."), the responses
 * they give, and the lookup of the answer to a request. The dev server
 * (vite.config.ts) and the demo build (browser.ts) both answer /api with them.
 * The first handler of the request's method and path that gives a response
 * answers; one that gives none leaves the request to the next.
 */

type Answer = Response | null | undefined;

export interface Handler {
  method: string;
  path: string;
  resolve: (info: { request: Request }) => Answer | Promise<Answer>;
}

function on(method: string) {
  return (path: string, resolve: Handler["resolve"]): Handler => {
    if (!path.startsWith("*/")) throw new Error(`mock: ${path} is not a path of any host`);
    return { method, path: path.slice(1), resolve };
  };
}

export const http = { get: on("GET"), post: on("POST") };

export class HttpResponse extends Response {
  static override json(body: unknown, init?: ResponseInit): Response {
    return Response.json(body, init);
  }

  static override error(): Response {
    return Response.error();
  }
}

export function delay(ms: number): Promise<void> {
  return new Promise((done) => setTimeout(done, ms));
}

export async function getResponse(
  handlers: Handler[],
  request: Request,
): Promise<Response | undefined> {
  const path = new URL(request.url).pathname;
  for (const h of handlers) {
    if (h.method !== request.method || h.path !== path) continue;
    const answer = await h.resolve({ request: request.clone() });
    if (answer) return answer;
  }
  return undefined;
}
