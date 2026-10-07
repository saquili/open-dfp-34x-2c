// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The panel builds to static files that pond serves from the stick's flash.
 * Every file is stored gzip-compressed only: pond hands a browser that
 * accepts gzip the .gz file as it is, so the stick never compresses anything
 * and the flash holds one copy.
 *
 * "vite" proxies /api to a stick (POND_TARGET, the router's proxy by
 * default); "vite --mode mock" answers /api from the dev server itself with
 * the mock stick of src/mocks, for work without one. The mocks run in the
 * server, not in a service worker, so they work in any browser and for any
 * client, and nothing of them reaches a build.
 */

import { readdirSync, readFileSync, statSync, unlinkSync, writeFileSync } from "node:fs";
import type { IncomingMessage } from "node:http";
import { join } from "node:path";
import { gzipSync } from "node:zlib";
import tailwindcss from "@tailwindcss/vite";
import preact from "@preact/preset-vite";
import { defineConfig, type Plugin } from "vite";

/* Replace every file of the output with its gzip-compressed copy. */
function gzipOnly(): Plugin {
  let outDir = "dist";
  return {
    name: "pond-gzip-only",
    apply: "build",
    configResolved(c) {
      outDir = c.build.outDir;
    },
    closeBundle() {
      const walk = (dir: string): string[] =>
        readdirSync(dir).flatMap((n) => {
          const p = join(dir, n);
          return statSync(p).isDirectory() ? walk(p) : [p];
        });
      for (const file of walk(outDir)) {
        if (file.endsWith(".gz")) continue;
        writeFileSync(`${file}.gz`, gzipSync(readFileSync(file), { level: 9 }));
        unlinkSync(file);
      }
    },
  };
}

function body(req: IncomingMessage): Promise<Buffer> {
  return new Promise((done, fail) => {
    const chunks: Buffer[] = [];
    req.on("data", (c: Buffer) => chunks.push(c));
    req.on("end", () => done(Buffer.concat(chunks)));
    req.on("error", fail);
  });
}

/* Answer /api with the mock stick's handlers; a mocked network error drops the connection. */
function mockApi(): Plugin {
  return {
    name: "pond-mock-api",
    apply: "serve",
    configureServer(server) {
      server.middlewares.use(async (req, res, next) => {
        if (!req.url?.startsWith("/api/")) return next();
        const { handlers } = await server.ssrLoadModule("/src/mocks/handlers.ts");
        const { getResponse } = await import("msw");
        const hasBody = req.method !== "GET" && req.method !== "HEAD";
        const request = new Request(new URL(req.url, "http://stick.invalid"), {
          method: req.method,
          headers: req.headers as Record<string, string>,
          body: hasBody ? new Uint8Array(await body(req)) : undefined,
        });
        const response = await getResponse(handlers, request);
        if (!response) return next();
        if (response.type === "error") return req.socket.destroy();
        res.statusCode = response.status;
        response.headers.forEach((v, k) => res.setHeader(k, v));
        res.end(Buffer.from(await response.arrayBuffer()));
      });
    },
  };
}

export default defineConfig(({ mode }) => ({
  plugins: [preact(), tailwindcss(), gzipOnly(), ...(mode === "mock" ? [mockApi()] : [])],
  build: {
    target: "es2022",
    assetsInlineLimit: 0,
    reportCompressedSize: true,
  },
  server: {
    proxy:
      mode === "mock"
        ? undefined
        : {
            "/api": {
              target: process.env.POND_TARGET ?? "https://stick.invalid",
              changeOrigin: true,
              secure: true,
            },
          },
  },
}));
