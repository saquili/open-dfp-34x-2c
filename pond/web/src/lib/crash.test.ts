// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import type { Crash } from "../api/types";
import { consoleRecord, crashLine, crashText, kernelRecord, lastLines } from "./crash";

const panic =
  "Panic#1 Part1\n<6>[   16.99] omcibr0: port 3(gem257) entered forwarding state\n" +
  "<6>[   23.09] sysrq: Trigger a crash\n<0>[   23.10] Kernel panic - not syncing: sysrq triggered crash\n" +
  "\nECC: No errors detected\n";

const oops =
  "Oops#1 Part1\n<1>[  120.50] CPU 0 Unable to handle kernel paging request at virtual address 00000004, epc == 802b1c40\n" +
  "<4>[  120.51] Call Trace:\n<0>[  120.60] Kernel panic - not syncing: Fatal exception\n";

const crash: Crash = {
  records: [
    { name: "console-ramoops-0", kind: "console", reason: null, time_s: null, text: "boot\n" },
    { name: "dmesg-ramoops-0", kind: "dmesg", reason: "oops", time_s: 1763419000, text: oops },
    { name: "dmesg-ramoops-1", kind: "dmesg", reason: "panic", time_s: 1763419001, text: panic },
  ],
};

describe("the kernel's last words", () => {
  it("finds the last record of an oops or a panic, and the console's", () => {
    expect(kernelRecord(crash)?.name).toBe("dmesg-ramoops-1");
    expect(consoleRecord(crash)?.name).toBe("console-ramoops-0");
    expect(kernelRecord({ records: crash.records.slice(0, 1) })).toBeUndefined();
    expect(kernelRecord(undefined)).toBeUndefined();
  });

  it("says why the kernel stopped: the panic's own message", () => {
    expect(crashLine(panic)).toBe("Kernel panic - not syncing: sysrq triggered crash");
  });

  it("names the oops rather than the panic that only follows it", () => {
    expect(crashLine(oops)).toBe(
      "CPU 0 Unable to handle kernel paging request at virtual address 00000004, epc == 802b1c40",
    );
  });

  it("falls back on the last line of a record that names no trouble", () => {
    expect(crashLine("Panic#1 Part1\n<6>[ 1.0] something\n\n")).toBe("something");
    expect(crashLine("Oops#1 Part1\n")).toBe("");
    expect(crashLine("")).toBe("");
  });

  it("shows the last lines that hold text, without their level or ramoops's note", () => {
    expect(lastLines(panic, 2)).toBe(
      "[   23.09] sysrq: Trigger a crash\n[   23.10] Kernel panic - not syncing: sysrq triggered crash",
    );
  });

  it("puts every record in one text, each under its name", () => {
    const text = crashText(crash);
    expect(text.startsWith("===== console-ramoops-0\nboot\n\n===== dmesg-ramoops-0\nOops#1")).toBe(
      true,
    );
    expect(text).toContain("===== dmesg-ramoops-1\nPanic#1 Part1\n");
  });
});
