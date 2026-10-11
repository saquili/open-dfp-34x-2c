// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import type { Firmware, FirmwareBank } from "../api/types";
import {
  type BankView,
  bankViews,
  otherBank,
  overShipped,
  updateTarget,
  uploading,
} from "./firmware";

function firmware(over: Partial<Firmware> = {}): Firmware {
  return {
    running: 0,
    committed: 0,
    next: 0,
    trial: null,
    failed: null,
    banks: [
      {
        bank: 0,
        state: "valid",
        version: "0123456789ab",
        time: 1759000000,
        kernel_size_b: 1900544,
        rootfs_size_b: 2031616,
      },
      {
        bank: 1,
        state: "valid",
        version: "7c5fba548abc",
        time: 1759800000,
        kernel_size_b: 1900544,
        rootfs_size_b: 2031616,
      },
    ],
    update: {
      state: "idle",
      received_b: 0,
      expected_b: 0,
      bank: null,
      version: null,
      message: null,
    },
    ...over,
  };
}

/* The two views, A and B. */
function views(f: Firmware): [BankView, BankView] {
  const [a, b] = bankViews(f);
  if (!a || !b) throw new Error("two banks");
  return [a, b];
}

/* A bank of the status, to change for a test. */
function bank(f: Firmware, n: number): FirmwareBank {
  const b = f.banks[n];
  if (!b) throw new Error(`no bank ${n}`);
  return b;
}

describe("the banks", () => {
  it("names them A and B and says which runs and which starts", () => {
    const [a, b] = views(firmware());
    expect(a.letter).toBe("A");
    expect(a.tags).toEqual(["running", "default"]);
    expect(b.letter).toBe("B");
    expect(b.tags).toEqual([]);
    expect(b.usable).toBe(true);
  });

  it("marks a start armed for once, and a bank on trial", () => {
    expect(views(firmware({ next: 1 }))[1].tags).toEqual(["once"]);
    const [a, b] = views(firmware({ running: 1, trial: 1 }));
    expect(a.tags).toEqual(["default"]);
    expect(b.tags).toEqual(["running", "trial"]);
  });

  it("marks a trial that failed, and banks that hold no whole system", () => {
    const f = firmware({ failed: 1 });
    bank(f, 1).state = "incomplete";
    const b = views(f)[1];
    expect(b.tags).toEqual(["failed", "incomplete"]);
    expect(b.usable).toBe(false);
    expect(b.version).toBeNull();
  });

  it("names the shipped firmware, with its version, as no system to start", () => {
    const f = firmware();
    bank(f, 1).state = "shipped";
    bank(f, 1).version = "V1.0-220923";
    const b = views(f)[1];
    expect(b.tags).toEqual(["shipped"]);
    expect(b.usable).toBe(false);
    expect(b.version).toBe("V1.0-220923");
  });
});

describe("what can be done", () => {
  it("writes an update into the bank that does not run", () => {
    expect(updateTarget(firmware())).toBe(1);
    expect(updateTarget(firmware({ running: 1 }))).toBe(0);
    expect(updateTarget(firmware({ running: null }))).toBeNull();
  });

  it("knows when an update would write over the shipped firmware", () => {
    const f = firmware();
    expect(overShipped(f, updateTarget(f))).toBe(false);
    bank(f, 1).state = "shipped";
    expect(overShipped(f, updateTarget(f))).toBe(true);
    expect(overShipped(f, null)).toBe(false);
    expect(otherBank(f)).toBeNull();
  });

  it("starts the other bank only when it holds a system", () => {
    expect(otherBank(firmware())).toBe(1);
    const f = firmware();
    bank(f, 1).state = "empty";
    expect(otherBank(f)).toBeNull();
    expect(otherBank(firmware({ running: null }))).toBeNull();
  });

  it("tells an upload under way", () => {
    expect(uploading(firmware())).toBe(false);
    const f = firmware();
    f.update.state = "writing";
    expect(uploading(f)).toBe(true);
    expect(uploading(undefined)).toBe(false);
  });
});
