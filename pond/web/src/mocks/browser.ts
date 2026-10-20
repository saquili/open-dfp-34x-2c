// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The demo's stick: the build of "vite build --mode demo" answers /api in the
 * browser itself, from the handlers that the dev server's mock uses. fetch is
 * wrapped, not a service worker installed, so the demo works from any static
 * host and in any browser; requests for anything else go out as they would.
 */

import { handlers } from "./handlers";
import { getResponse } from "./http";

const network = window.fetch.bind(window);

window.fetch = async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
  const request = new Request(input, init);
  if (!new URL(request.url).pathname.startsWith("/api/")) return network(input, init);
  const response = await getResponse(handlers, request);
  if (!response) return new Response(null, { status: 404 });
  /* A mocked network error is what a dropped connection gives fetch. */
  if (response.type === "error") throw new TypeError("Failed to fetch");
  return response;
};
