// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { addresses, ipv4Cidr, ipv6, ipv6Cidr } from "./schemas";

const t = (k: string) => k;
const ok = (s: { safeParse: (v: string) => { success: boolean } }, v: string) =>
  s.safeParse(v).success;

describe("addresses with their lengths", () => {
  it("takes an IPv4 address with its mask's length, or nothing", () => {
    expect(ok(ipv4Cidr(t), "192.0.2.10/24")).toBe(true);
    expect(ok(ipv4Cidr(t), "")).toBe(true);
    expect(ok(ipv4Cidr(t), "192.0.2.10")).toBe(false);
    expect(ok(ipv4Cidr(t), "192.0.2.10/33")).toBe(false);
    expect(ok(ipv4Cidr(t), "192.0.2.256/24")).toBe(false);
  });

  it("takes IPv6 addresses whole or with a run of zeros left out", () => {
    expect(ok(ipv6(t), "2001:db8::1")).toBe(true);
    expect(ok(ipv6(t), "2001:db8:0:0:0:0:0:1")).toBe(true);
    expect(ok(ipv6(t), "::1")).toBe(true);
    expect(ok(ipv6(t), "2001:db8::1::2")).toBe(false);
    expect(ok(ipv6(t), "2001:db8:0:0:0:0:1")).toBe(false);
    expect(ok(ipv6(t), "2001:db8::12345")).toBe(false);
    expect(ok(ipv6Cidr(t), "2001:db8:1200::/56")).toBe(true);
    expect(ok(ipv6Cidr(t), "2001:db8:1200::/129")).toBe(false);
  });

  it("takes name servers apart by spaces or commas", () => {
    expect(ok(addresses(t), "198.51.100.53 198.51.100.54")).toBe(true);
    expect(ok(addresses(t), "198.51.100.53, 2001:db8::53")).toBe(true);
    expect(ok(addresses(t), "198.51.100.53 dns.example")).toBe(false);
  });
});
