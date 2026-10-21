// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The mock stick answers as the API's description says (pond/openapi.json):
 * each value it reads, and the sign-in's, the sessions' and the token's
 * answers, against their schemas. The description and pond's sources are
 * checked against each other by pond's tests (tests/test_openapi.py); here
 * the panel's picture of the answers is checked against the description.
 * The check knows the parts of JSON Schema the description uses.
 */

import { readFileSync } from "node:fs";
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { handlers } from "./handlers";
import { getResponse } from "./http";

type Schema = {
  $ref?: string;
  type?: string | string[];
  const?: unknown;
  enum?: unknown[];
  properties?: Record<string, Schema>;
  required?: string[];
  additionalProperties?: boolean | Schema;
  items?: Schema;
  prefixItems?: Schema[];
  anyOf?: Schema[];
  allOf?: Schema[];
};

const spec = JSON.parse(
  readFileSync(new URL("../../../openapi.json", import.meta.url), "utf8"),
) as {
  paths: Record<
    string,
    { get?: { responses: Record<string, { content?: Record<string, { schema: Schema }> }> } }
  >;
  components: { schemas: Record<string, Schema> };
};

const kind = (v: unknown) =>
  v === null ? "null" : Array.isArray(v) ? "array" : typeof v === "number" ? "number" : typeof v;

/** What in @v the schema @s does not take, by the path to it; none when it takes all. */
function misfits(s: Schema, v: unknown, at: string): string[] {
  if (s.$ref) {
    const name = s.$ref.replace("#/components/schemas/", "");
    const target = spec.components.schemas[name];
    return target ? misfits(target, v, at) : [`${at}: no schema ${name}`];
  }
  if (s.allOf) return s.allOf.flatMap((p) => misfits(p, v, at));
  if (s.anyOf)
    return s.anyOf.some((p) => misfits(p, v, at).length === 0)
      ? []
      : [`${at}: none of anyOf takes ${JSON.stringify(v)}`];
  if (s.const !== undefined && v !== s.const)
    return [`${at}: ${JSON.stringify(v)} is not ${JSON.stringify(s.const)}`];
  if (s.enum && !s.enum.includes(v))
    return [`${at}: ${JSON.stringify(v)} is not one of ${JSON.stringify(s.enum)}`];
  if (s.type) {
    const types = ([] as string[]).concat(s.type);
    const k = kind(v);
    if (!types.includes(k) && !(k === "number" && types.includes("integer") && Number.isInteger(v)))
      return [`${at}: a ${k}, not ${types.join(" or ")}`];
  }
  if (kind(v) === "object") {
    const o = v as Record<string, unknown>;
    const out = (s.required ?? []).filter((k) => !(k in o)).map((k) => `${at}.${k}: missing`);
    for (const [k, x] of Object.entries(o)) {
      const p = s.properties?.[k];
      if (p) out.push(...misfits(p, x, `${at}.${k}`));
      else if (s.additionalProperties === false) out.push(`${at}.${k}: not described`);
      else if (typeof s.additionalProperties === "object")
        out.push(...misfits(s.additionalProperties, x, `${at}.${k}`));
    }
    return out;
  }
  if (Array.isArray(v)) {
    if (s.prefixItems) return s.prefixItems.flatMap((p, i) => misfits(p, v[i], `${at}[${i}]`));
    if (s.items) return v.flatMap((x, i) => misfits(s.items as Schema, x, `${at}[${i}]`));
  }
  return [];
}

const ask = (path: string, method = "GET", body?: string) =>
  getResponse(handlers, new Request(`http://stick.invalid${path}`, { method, body }));

/** The schema of the answer 200 of GET @path. */
const answer = (path: string) =>
  spec.paths[path]?.get?.responses["200"]?.content?.["application/json"]?.schema as Schema;

beforeAll(async () => {
  await ask("/api/auth/login", "POST", JSON.stringify({ user: "root", secret: "admin" }));
});
afterAll(async () => {
  await ask("/api/auth/logout", "POST");
});

/*
 * The values the mock stick does not serve, and why: the kernel's log, which
 * only the report of the device page reads; the leases, of a stick whose
 * software has no networks, which the mock is unless a scenario says.
 */
const MOCK_LACKS = ["/api/get/log", "/api/get/leases"];

describe("the mock stick's answers, against pond/openapi.json", () => {
  const reads = Object.keys(spec.paths).filter(
    (p) => p.startsWith("/api/get/") && !MOCK_LACKS.includes(p),
  );

  it.each(MOCK_LACKS)("%s is left out as the mock serves none", async (path) => {
    const r = await ask(path);
    const id = path.replace("/api/get/", "");
    const live = await ask(`/api/getm?ids=${id}`);
    const value = live && ((await live.json()) as Record<string, unknown>)[id];
    expect(r === undefined || r.status === 404).toBe(true);
    expect(value).toBeUndefined();
  });

  it("describes every value it reads", () => {
    expect(reads.length + MOCK_LACKS.length).toBeGreaterThan(30);
  });

  /*
   * A value the panel reads with the live figures, the mock serves through
   * /api/getm alone: it is taken from there, in the envelope of /api/get.
   */
  it.each(reads)("%s", async (path) => {
    const id = path.replace("/api/get/", "");
    let r = await ask(path);
    let reply: unknown;
    if (r === undefined) {
      r = await ask(`/api/getm?ids=${id}`);
      const all = (await r?.json()) as Record<string, unknown> | undefined;
      reply = { id, value: all?.[id] };
    } else reply = await r.json();
    expect(r?.status, `${path} answers`).toBe(200);
    expect(misfits(answer(path), reply, id)).toEqual([]);
  });

  it.each(["/api/auth/session", "/api/auth/sessions", "/api/token"])("%s", async (path) => {
    const r = await ask(path);
    expect(r?.status).toBe(200);
    expect(misfits(answer(path), await r?.json(), path)).toEqual([]);
  });
});
