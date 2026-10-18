// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { MAX_MOVED, moved, ports, readPorts } from "./ports";

describe("readPorts", () => {
  it("reads one port or a range", () => {
    expect(readPorts("443")).toEqual({ from: 443, to: 443 });
    expect(readPorts(" 8000 - 8100 ")).toEqual({ from: 8000, to: 8100 });
    expect(readPorts("8000–8100")).toEqual({ from: 8000, to: 8100 });
  });
  it("refuses what is not a port or a range in order", () => {
    expect(readPorts("0")).toBeNull();
    expect(readPorts("65536")).toBeNull();
    expect(readPorts("8100-8000")).toBeNull();
    expect(readPorts("http")).toBeNull();
    expect(readPorts("")).toBeNull();
  });
  it("writes them as they are read", () => {
    expect(ports(443, 443)).toBe("443");
    expect(ports(8000, 8100)).toBe("8000–8100");
  });
});

describe("moved", () => {
  const f = { proto: "tcp" as const, ext_from: 8000, ext_to: 8010, int_port: 9000, on: true };
  it("counts the ports of a range moved to other ports, each protocol's apart", () => {
    expect(moved(f)).toBe(11);
    expect(moved({ ...f, proto: "both" })).toBe(22);
  });
  it("counts none for a port alone, a range kept, or a forward not in force", () => {
    expect(moved({ ...f, ext_to: 8000 })).toBe(0);
    expect(moved({ ...f, int_port: 8000 })).toBe(0);
    expect(moved({ ...f, on: false })).toBe(0);
  });
  it("keeps to the stick's bound", () => {
    expect(MAX_MOVED).toBe(1024);
  });
});
