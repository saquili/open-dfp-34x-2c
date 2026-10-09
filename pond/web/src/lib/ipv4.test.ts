// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { ip4, ip4s, net4 } from "./ipv4";

describe("ip4", () => {
  it("reads an address", () => {
    expect(ip4("192.0.2.1")).toBe(0xc0000201);
    expect(ip4s(0xc0000201)).toBe("192.0.2.1");
  });
  it("refuses what is not one", () => {
    expect(ip4("192.0.2.256")).toBeNull();
    expect(ip4("192.0.2")).toBeNull();
    expect(ip4("")).toBeNull();
  });
});

describe("net4", () => {
  it("finds the network of an address", () => {
    const n = net4("192.0.2.1/24");
    expect(n?.text).toBe("192.0.2.0/24");
    expect(n && ip4s(n.last)).toBe("192.0.2.255");
  });
  it("takes a whole space and a single address", () => {
    expect(net4("127.1.2.3/8")?.text).toBe("127.0.0.0/8");
    expect(net4("198.51.100.7/32")?.text).toBe("198.51.100.7/32");
  });
  it("refuses a length out of range", () => {
    expect(net4("198.51.100.7/33")).toBeNull();
  });
});
