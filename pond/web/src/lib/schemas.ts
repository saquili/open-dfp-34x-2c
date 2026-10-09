// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * What a value of a form must look like, checked in the browser before pond
 * checks it again. The messages come in the reader's language. zod/mini keeps
 * the checks small enough for the stick's flash.
 */

import * as z from "zod/mini";

type T = (
  key:
    | "form.vlan"
    | "form.mac"
    | "form.ipv4"
    | "form.ipv4Cidr"
    | "form.ipv6"
    | "form.ipv6Cidr"
    | "form.addresses"
    | "form.required",
) => string;

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

const isIpv4 = (v: string) =>
  /^(\d{1,3}\.){3}\d{1,3}$/.test(v) && v.split(".").every((o) => +o <= 255);

/* An IPv6 address: groups of hexadecimal digits, one run of them left out at most. */
const isIpv6 = (v: string) => {
  if (!/^[0-9a-f:]+$/i.test(v) || (v.match(/::/g) ?? []).length > 1) return false;
  const groups = v.split(":").filter((g, i, all) => g !== "" || (i > 0 && i < all.length - 1));
  return (
    groups.every((g) => g.length <= 4) &&
    (v.includes("::") ? groups.length < 8 : groups.length === 8)
  );
};

/* An address and the length of its network's prefix, up to @max. */
const withLength = (v: string, test: (a: string) => boolean, max: number) => {
  const m = /^([^/]+)\/(\d{1,3})$/.exec(v);
  return m != null && +(m[2] ?? "") <= max && test(m[1] ?? "");
};

export const ipv4Cidr = (t: T) => optional((v) => withLength(v, isIpv4, 32), t("form.ipv4Cidr"));
export const ipv6 = (t: T) => optional(isIpv6, t("form.ipv6"));
export const ipv6Cidr = (t: T) => optional((v) => withLength(v, isIpv6, 128), t("form.ipv6Cidr"));

/** Addresses, IPv4 or IPv6, apart by spaces or commas. */
export const addresses = (t: T) =>
  optional(
    (v) =>
      v
        .split(/[\s,]+/)
        .filter(Boolean)
        .every((a) => isIpv4(a) || isIpv6(a)),
    t("form.addresses"),
  );

/** A required string, trimmed. */
export const required = (message: string) => z.string().check(z.trim(), z.minLength(1, message));

/** A string of at most n characters, trimmed. */
export const upTo = (n: number) => z.string().check(z.trim(), z.maxLength(n));

export { z };
