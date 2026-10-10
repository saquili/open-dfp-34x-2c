// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { trialToConfirm } from "./trial";

describe("trialToConfirm", () => {
  const onTrial = { trial: { until_s: 1264 }, applying: false };

  it("confirms a trial that a fresh reading shows applied", () => {
    expect(trialToConfirm(onTrial, true, null)).toBe(1264);
  });

  it("waits while the change is being applied", () => {
    expect(trialToConfirm({ ...onTrial, applying: true }, true, null)).toBeNull();
  });

  it("confirms nothing from the last reading while the stick does not answer", () => {
    expect(trialToConfirm(onTrial, false, null)).toBeNull();
  });

  it("confirms each trial once, and a new one again", () => {
    expect(trialToConfirm(onTrial, true, 1264)).toBeNull();
    expect(trialToConfirm({ ...onTrial, trial: { until_s: 1400 } }, true, 1264)).toBe(1400);
  });

  it("has nothing to confirm without a trial, or on a stick of before", () => {
    expect(trialToConfirm({ trial: null, applying: false }, true, null)).toBeNull();
    expect(trialToConfirm({ applying: false }, true, null)).toBeNull();
    expect(trialToConfirm(undefined, true, null)).toBeNull();
  });
});
