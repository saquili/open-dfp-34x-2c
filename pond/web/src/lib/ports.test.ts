// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { ports, readPorts } from "./ports";

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
