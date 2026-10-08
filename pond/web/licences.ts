// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The licence notices of the panel: a build writes licenses.txt beside the
 * panel's files, with the licence text of every package whose code is in what
 * the stick serves. The packages are those of the modules the bundle holds,
 * not those package.json lists, and those that the panel's stylesheets take in
 * with @import and @plugin (Tailwind and daisyUI, whose CSS the build writes
 * into the stylesheet). A package without a licence file, or under a licence
 * that cannot go into a GPL-2.0-only program, stops the build.
 */

import { existsSync, readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import type { Plugin } from "vite";

/* The licences whose terms a GPL-2.0-only work can take in. */
export const COMPATIBLE = new Set(["MIT", "ISC", "BSD-2-Clause", "BSD-3-Clause", "0BSD"]);

/* The helpers that the bundler itself writes into the output, by their virtual module ids. */
const VIRTUAL: Record<string, string> = { "rolldown/": "rolldown", "vite/": "vite" };

/* The package a module id comes from, or null for the panel's own code. */
export function packageOf(id: string): string | null {
  const clean = id.replace(/^\0/, "").replace(/\?.*$/, "");
  const at = clean.lastIndexOf("/node_modules/");
  if (at < 0) {
    for (const [prefix, name] of Object.entries(VIRTUAL)) if (clean.startsWith(prefix)) return name;
    return null;
  }
  const parts = clean.slice(at + "/node_modules/".length).split("/");
  const [first, second] = parts;
  if (!first) return null;
  return first.startsWith("@") && second ? `${first}/${second}` : first;
}

/* The packages a stylesheet takes in: @import "pkg" and @plugin "pkg/...". */
export function stylesheetPackages(css: string): string[] {
  const found = new Set<string>();
  for (const m of css.matchAll(/@(?:import|plugin)\s+["']([^"']+)["']/g)) {
    const spec = m[1] ?? "";
    if (spec.startsWith(".") || spec.startsWith("/")) continue;
    const parts = spec.split("/");
    found.add(spec.startsWith("@") ? parts.slice(0, 2).join("/") : (parts[0] ?? spec));
  }
  return [...found];
}

export interface Notice {
  name: string;
  version: string;
  licence: string;
  text: string;
}

const LICENCE_FILE = /^(licen[cs]e|copying)(\.(md|txt))?$/i;

/*
 * A build tool's licence file can go on with the licences of the packages
 * bundled into the tool itself, under this heading (Vite's lists some fifty,
 * 100 KB of text). Those packages run on the build machine, not in the panel,
 * which holds only the tool's own helper; the part before the heading is the
 * tool's own licence.
 */
const BUNDLED_HEADING = /^#+ *Licenses of bundled dependencies/m;

export function ownLicence(text: string): string {
  const at = text.search(BUNDLED_HEADING);
  return at < 0 ? text : text.slice(0, at);
}

/* A package's name, version, licence and licence text, read from its directory. */
export function readNotice(dir: string): Notice {
  const pkg = JSON.parse(readFileSync(join(dir, "package.json"), "utf8")) as {
    name: string;
    version: string;
    license?: unknown;
  };
  const licence = typeof pkg.license === "string" ? pkg.license : "";
  const file = readdirSync(dir).find((n) => LICENCE_FILE.test(n));
  if (!file) throw new Error(`licences: ${pkg.name} ${pkg.version} has no licence file`);
  if (!COMPATIBLE.has(licence)) {
    throw new Error(
      `licences: ${pkg.name} ${pkg.version} is under "${licence || "no licence"}", ` +
        "which a GPL-2.0-only program cannot take in; it needs a decision",
    );
  }
  return {
    name: pkg.name,
    version: pkg.version,
    licence,
    text: ownLicence(readFileSync(join(dir, file), "utf8")),
  };
}

/* The notices as one text, in the order of the package names. */
export function noticesText(notices: Notice[]): string {
  const sorted = [...notices].sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  const head =
    "The panel of pond holds code of these packages, under these licences.\n" +
    "pond itself is under the GNU General Public License version 2.\n\n" +
    sorted.map((n) => `  ${n.name} ${n.version}: ${n.licence}`).join("\n");
  const bodies = sorted.map(
    (n) => `${"=".repeat(72)}\n${n.name} ${n.version}\n\n${n.text.trim()}\n`,
  );
  return `${head}\n\n${bodies.join("\n")}`;
}

/* The directory of a package, found from the panel's directory as Node would. */
function packageDir(root: string, name: string): string {
  for (let dir = root; ; dir = dirname(dir)) {
    const candidate = join(dir, "node_modules", name);
    if (existsSync(join(candidate, "package.json"))) return candidate;
    if (dirname(dir) === dir) throw new Error(`licences: ${name} is not installed`);
  }
}

export function licenceNotices(): Plugin {
  let root = ".";
  return {
    name: "pond-licence-notices",
    apply: "build",
    configResolved(c) {
      root = c.root;
    },
    generateBundle(_options, bundle) {
      const names = new Set<string>();
      for (const file of Object.values(bundle)) {
        if (file.type !== "chunk") continue;
        for (const id of Object.keys(file.modules)) {
          const name = packageOf(id);
          if (name) names.add(name);
        }
      }
      for (const id of this.getModuleIds()) {
        const path = id.replace(/\?.*$/, "");
        if (id.startsWith("\0") || !path.endsWith(".css") || packageOf(path)) continue;
        for (const name of stylesheetPackages(readFileSync(path, "utf8"))) names.add(name);
      }
      const notices = [...names].map((name) => readNotice(packageDir(root, name)));
      this.emitFile({ type: "asset", fileName: "licenses.txt", source: noticesText(notices) });
    },
  };
}
