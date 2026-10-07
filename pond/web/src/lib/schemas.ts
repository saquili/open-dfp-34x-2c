// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What a value of a form must look like, checked in the browser before pond
 * checks it again. The messages come in the reader's language. zod/mini keeps
 * the checks small enough for the stick's flash.
 */

import * as z from "zod/mini";

type T = (key: "form.vlan" | "form.mac" | "form.ipv4" | "form.required") => string;

/** A string, trimmed, that passes a test or is empty. */
const optional = (test: (v: string) => boolean, message: string) =>
  z.string().check(
    z.trim(),
    z.refine((v) => v === "" || test(v), message),
  );

export const vlan = (t: T) =>
  optional((v) => /^\d+$/.test(v) && +v >= 1 && +v <= 4094, t("form.vlan"));

export const mac = (t: T) =>
  optional((v) => /^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(v), t("form.mac"));

export const ipv4 = (t: T) =>
  optional(
    (v) => /^(\d{1,3}\.){3}\d{1,3}$/.test(v) && v.split(".").every((o) => +o <= 255),
    t("form.ipv4"),
  );

/** A required string, trimmed. */
export const required = (message: string) => z.string().check(z.trim(), z.minLength(1, message));

/** A string of at most n characters, trimmed. */
export const upTo = (n: number) => z.string().check(z.trim(), z.maxLength(n));

export { z };
