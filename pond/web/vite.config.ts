// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The panel builds to static files that pond serves from the stick's flash.
 * Every file is stored gzip-compressed only: pond hands a browser that
 * accepts gzip the .gz file as it is, so the stick never compresses anything
 * and the flash holds one copy. licenses.txt, beside the panel's files, holds
 * the licences of the packages whose code the panel carries (licences.ts).
 *
 * "vite" proxies /api to a stick (POND_TARGET names it, and the server stops
 * without it; with POND_TOKEN it presents that bearer token, so the panel shows
 * the stick without a sign-in); "vite --mode mock" answers /api from the dev server itself with
 * the mock stick of src/mocks, for work without one. The mocks run in the
 * server, not in a service worker, so they work in any browser and for any
 * client, and nothing of them reaches the stick's build.
 *
 * "vite build --mode demo" is the other build: the panel with the mock stick
 * answering /api in the browser (src/mocks/browser.ts), for a static host.
 * POND_DEMO_BASE is the path it is served from ("/" without it); its files are
 * not compressed, since a static host serves them as they are, and 404.html,
 * a copy of index.html, lets the host answer a deep link with the panel.
 */

import {
  copyFileSync,
  readdirSync,
  readFileSync,
  statSync,
  unlinkSync,
  writeFileSync,
} from "node:fs";
import type { IncomingMessage } from "node:http";
import { join } from "node:path";
import { gzipSync } from "node:zlib";
import preact from "@preact/preset-vite";
import tailwindcss from "@tailwindcss/vite";
import { defineConfig, type Plugin } from "vite";
import { licenceNotices } from "./licences.ts";

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

/* The demo's answer to a path the static host does not have: the panel itself. */
function spaFallback(): Plugin {
  let outDir = "dist";
  return {
    name: "pond-demo-404",
    apply: "build",
    configResolved(c) {
      outDir = c.build.outDir;
    },
    writeBundle() {
      copyFileSync(join(outDir, "index.html"), join(outDir, "404.html"));
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
        const { getResponse } = await server.ssrLoadModule("/src/mocks/http.ts");
        const hasBody = req.method !== "GET" && req.method !== "HEAD";
        const request = new Request(new URL(req.url, "http://stick.invalid"), {
          method: req.method,
          headers: req.headers as Record<string, string>,
          body: hasBody ? new Uint8Array(await body(req)) : undefined,
        });
        const response: Response | undefined = await getResponse(handlers, request);
        if (!response) return next();
        if (response.type === "error") return req.socket.destroy();
        res.statusCode = response.status;
        response.headers.forEach((v, k) => {
          res.setHeader(k, v);
        });
        res.end(Buffer.from(await response.arrayBuffer()));
      });
    },
  };
}

export default defineConfig(({ command, mode }) => {
  const target = process.env.POND_TARGET;

  /* The dev server has nothing to talk to without a stick; a build and the tests need none. */
  if (command === "serve" && mode === "development" && !target) {
    throw new Error(
      "POND_TARGET names the stick, or the proxy in front of it, that npm run dev talks to, " +
        "for example https://192.0.2.1; npm run dev:mock needs none",
    );
  }

  const demo = mode === "demo";

  return {
    base: demo ? (process.env.POND_DEMO_BASE ?? "/") : "/",
    plugins: [
      preact(),
      tailwindcss(),
      licenceNotices(),
      ...(demo ? [spaFallback()] : [gzipOnly()]),
      ...(mode === "mock" ? [mockApi()] : []),
    ],
    build: {
      outDir: demo ? "dist-demo" : "dist",
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
                target,
                changeOrigin: true,
                secure: true,
                /* With POND_TOKEN, the proxy signs in with the stick's bearer token. */
                headers: process.env.POND_TOKEN
                  ? { Authorization: `Bearer ${process.env.POND_TOKEN}` }
                  : undefined,
              },
            },
    },
  };
});
