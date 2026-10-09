// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { ip6, ip6s, prefix6, subnet64, subnets64 } from "./ipv6";

describe("ip6", () => {
  it("reads an address, short or whole", () => {
    expect(ip6("::1")).toBe(1n);
    expect(ip6("2001:db8::")).toBe(0x20010db8n << 96n);
    expect(ip6("2001:0db8:0:0:0:0:0:1")).toBe((0x20010db8n << 96n) | 1n);
  });
  it("refuses what is not one", () => {
    expect(ip6("2001:db8")).toBeNull();
    expect(ip6("1::2::3")).toBeNull();
    expect(ip6("2001:db8::g")).toBeNull();
    expect(ip6("1:2:3:4:5:6:7:8:9")).toBeNull();
    expect(ip6("")).toBeNull();
  });
  it("writes the longest run of zeros as ::, and a single zero as 0", () => {
    expect(ip6s(ip6("2001:db8:0:0:1:0:0:1") as bigint)).toBe("2001:db8::1:0:0:1");
    expect(ip6s(ip6("2001:db8:0:1:1:1:1:1") as bigint)).toBe("2001:db8:0:1:1:1:1:1");
    expect(ip6s(0n)).toBe("::");
  });
});

describe("prefix6", () => {
  it("takes a prefix's first address", () => {
    expect(prefix6("2001:db8:5100:ff::/56")).toEqual({ addr: ip6("2001:db8:5100::"), len: 56 });
    expect(prefix6("2001:db8::/129")).toBeNull();
    expect(prefix6("2001:db8::")).toBeNull();
  });
});

describe("subnet64", () => {
  it("cuts a /64 out of a delegated prefix", () => {
    expect(subnets64("2001:db8:5100::/56")).toBe(256);
    expect(subnet64("2001:db8:5100::/56", 0)).toBe("2001:db8:5100::/64");
    expect(subnet64("2001:db8:5100::/56", 1)).toBe("2001:db8:5100:1::/64");
    expect(subnet64("2001:db8:5100::/56", 255)).toBe("2001:db8:5100:ff::/64");
    expect(subnet64("2001:db8:5100::/56", 256)).toBeNull();
  });
  it("gives a /64 itself as its only network, and none for a longer prefix", () => {
    expect(subnets64("2001:db8:5:100::/64")).toBe(1);
    expect(subnet64("2001:db8:5:100::/64", 0)).toBe("2001:db8:5:100::/64");
    expect(subnets64("2001:db8::/80")).toBe(0);
    expect(subnet64("2001:db8::/80", 0)).toBeNull();
  });
});
