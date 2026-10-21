// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The confirmation's run of an action (components/Confirm.tsx): nothing is
 * sent without root's password where it needs one, and every refusal is
 * said in the dialog, a wrong password, a stop after too many of them and a
 * stick without a password under the password's field, which is emptied.
 */

import { describe, expect, it, vi } from "vitest";
import { ApiError, AuthError, OfflineError } from "../api/client";
import { en } from "../i18n/en";
import { attempt, LOCK_S, refusal } from "./confirm";

/* The English words, as the panel's t() gives them. */
function t(key: string, vars?: Record<string, string | number>): string {
  let node: unknown = en;
  for (const part of key.split(".")) node = (node as Record<string, unknown>)[part];
  let text = typeof node === "string" ? node : key;
  for (const [k, v] of Object.entries(vars ?? {})) text = text.replace(`{${k}}`, String(v));
  return text;
}

describe("what the confirmation says of a refusal", () => {
  it("says a wrong password under the field", () => {
    expect(refusal(t, new ApiError(403, "wrong password", "root's password is needed"))).toEqual({
      text: en.confirm.wrong,
      password: true,
    });
  });

  it("says how long wrong passwords stop it, as the stick says, or half a minute", () => {
    const locked = refusal(t, new ApiError(429, "too many attempts", "wait", 17));
    expect(locked.password).toBe(true);
    expect(locked.text).toContain("17");
    expect(refusal(t, new ApiError(429, "too many attempts")).text).toContain(String(LOCK_S));
  });

  it("says that the stick knows no password", () => {
    expect(refusal(t, new ApiError(423, "no store"))).toEqual({
      text: en.confirm.noStore,
      password: true,
    });
  });

  it("says any other refusal with the stick's reason, apart from the field", () => {
    const other = refusal(t, new ApiError(400, "refused", "the key is kept already"));
    expect(other).toEqual({ text: "Not done: the key is kept already", password: false });
    expect(refusal(t, new ApiError(403, "must_change", "set a password")).password).toBe(false);
    expect(refusal(t, new OfflineError())).toEqual({
      text: en.connection.offline,
      password: false,
    });
    expect(refusal(t, new AuthError()).password).toBe(false);
  });
});

describe("the confirmation's run of an action", () => {
  it("sends nothing without root's password where it needs it", async () => {
    const run = vi.fn(async () => undefined);
    expect(await attempt(t, run, true, "")).toEqual({
      done: false,
      text: en.confirm.empty,
      password: true,
    });
    expect(run).not.toHaveBeenCalled();
  });

  it("runs the action with the password typed, and is done when it is", async () => {
    const run = vi.fn(async (_secret: string) => undefined);
    expect(await attempt(t, run, true, "zażółć 1")).toEqual({ done: true });
    expect(run).toHaveBeenCalledWith("zażółć 1");
  });

  it("runs one that needs no password without one", async () => {
    const run = vi.fn(async () => undefined);
    expect(await attempt(t, run, false, "")).toEqual({ done: true });
    expect(run).toHaveBeenCalledOnce();
  });

  it("is not done when the stick refuses it, and says why", async () => {
    const wrong = await attempt(
      t,
      async () => {
        throw new ApiError(403, "wrong password");
      },
      true,
      "nope",
    );
    expect(wrong).toEqual({ done: false, text: en.confirm.wrong, password: true });
    const refused = await attempt(
      t,
      async () => {
        throw new Error("8 keys at most");
      },
      true,
      "right",
    );
    expect(refused).toEqual({ done: false, text: "Not done: 8 keys at most", password: false });
  });
});
