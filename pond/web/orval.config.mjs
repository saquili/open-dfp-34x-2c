// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The panel's types and requests of pond, made from ../openapi.json by Orval:
 * npm run gen:api writes src/api/gen, and npm run gen:check fails when what
 * is there is not what the description makes. A request goes through
 * pondFetch (src/api/fetcher.ts), the panel's request(), and gives the data
 * of its answer, a refusal being an error; the mock stick stays the panel's
 * own (src/mocks).
 */

import { defineConfig } from "orval";

export default defineConfig({
  pond: {
    input: { target: "../openapi.json" },
    output: {
      mode: "single",
      target: "src/api/gen/pond.ts",
      schemas: "src/api/gen/model",
      client: "react-query",
      httpClient: "fetch",
      clean: true,
      override: {
        mutator: { path: "src/api/fetcher.ts", name: "pondFetch" },
        fetch: { includeHttpResponseReturnType: false },
      },
    },
    hooks: { afterAllFilesWrite: "biome format --write" },
  },
});
