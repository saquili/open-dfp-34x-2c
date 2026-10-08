// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { noticesText, ownLicence, packageOf, readNotice, stylesheetPackages } from "./licences";

describe("the packages the bundle holds", () => {
  it("names the package of a module, scoped or not", () => {
    expect(packageOf("/w/node_modules/preact/dist/preact.module.js")).toBe("preact");
    expect(packageOf("/w/node_modules/@tanstack/react-query/build/x.js")).toBe(
      "@tanstack/react-query",
    );
    expect(packageOf("/w/node_modules/a/node_modules/b/index.js?v=1")).toBe("b");
  });

  it("names the bundler's own helpers and nothing for the panel's code", () => {
    expect(packageOf("\0rolldown/runtime.js")).toBe("rolldown");
    expect(packageOf("\0vite/modulepreload-polyfill.js")).toBe("vite");
    expect(packageOf("/w/src/main.tsx")).toBeNull();
    expect(packageOf("\0commonjs-helpers")).toBeNull();
  });

  it("finds the packages a stylesheet imports and loads as plugins", () => {
    const css = `@import "tailwindcss" source(".");\n@plugin "daisyui" {}\n@plugin "daisyui/theme" {}\n@import "./local.css";\n@plugin "@scope/kit/x";`;
    expect(stylesheetPackages(css).sort()).toEqual(["@scope/kit", "daisyui", "tailwindcss"]);
  });
});

describe("the notices", () => {
  let dirs: string[] = [];
  afterEach(() => {
    for (const d of dirs) rmSync(d, { recursive: true, force: true });
    dirs = [];
  });

  function pkg(json: object, files: Record<string, string>): string {
    const dir = mkdtempSync(join(tmpdir(), "licences-"));
    dirs.push(dir);
    mkdirSync(dir, { recursive: true });
    writeFileSync(join(dir, "package.json"), JSON.stringify(json));
    for (const [name, text] of Object.entries(files)) writeFileSync(join(dir, name), text);
    return dir;
  }

  it("reads a package's licence and its text", () => {
    const dir = pkg({ name: "a", version: "1.0.0", license: "ISC" }, { LICENSE: "ISC text\n" });
    expect(readNotice(dir)).toEqual({
      name: "a",
      version: "1.0.0",
      licence: "ISC",
      text: "ISC text\n",
    });
    const md = pkg({ name: "b", version: "2.0.0", license: "MIT" }, { "LICENSE.md": "MIT text" });
    expect(readNotice(md).text).toBe("MIT text");
  });

  it("keeps a build tool's own licence and leaves out those of its bundled packages", () => {
    const text =
      "# Vite core license\nMIT text\n\n# Licenses of bundled dependencies\n## a\nA text\n";
    expect(ownLicence(text)).toBe("# Vite core license\nMIT text\n\n");
    expect(ownLicence("MIT text\n")).toBe("MIT text\n");
  });

  it("stops at a package without a licence file", () => {
    const dir = pkg({ name: "bare", version: "1.0.0", license: "MIT" }, { "README.md": "x" });
    expect(() => readNotice(dir)).toThrow(/bare 1.0.0 has no licence file/);
  });

  it("stops at a licence a GPL-2.0-only program cannot take in", () => {
    for (const licence of ["Apache-2.0", "MPL-2.0", "GPL-3.0-only"]) {
      const dir = pkg({ name: "x", version: "1.0.0", license: licence }, { LICENSE: "t" });
      expect(() => readNotice(dir)).toThrow(/needs a decision/);
    }
    const none = pkg({ name: "y", version: "1.0.0" }, { LICENSE: "t" });
    expect(() => readNotice(none)).toThrow(/no licence/);
  });

  it("lists the packages, then their texts, in name order, the same every time", () => {
    const notices = [
      { name: "zod", version: "4.0.0", licence: "MIT", text: "zod licence\n" },
      { name: "@tanstack/store", version: "0.1.0", licence: "MIT", text: "store licence" },
    ];
    const text = noticesText(notices);
    expect(text.indexOf("@tanstack/store 0.1.0: MIT")).toBeLessThan(text.indexOf("zod 4.0.0: MIT"));
    expect(text.indexOf("store licence")).toBeLessThan(text.indexOf("zod licence"));
    expect(noticesText([...notices].reverse())).toBe(text);
  });
});
