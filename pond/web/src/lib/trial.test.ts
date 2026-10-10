// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import type { Trial } from "../api/types";
import { trialsToConfirm } from "./trial";

const settings: Trial = { name: "settings", until_s: 1264 };
const lane: Trial = { name: "lane", until_s: 1270 };
const mgmt: Trial = { name: "mgmt", until_s: 1800, address: "192.0.2.10/30" };
const here = "198.51.100.2";

describe("trialsToConfirm", () => {
  it("confirms the changes that a fresh reading shows applied", () => {
    expect(trialsToConfirm({ trials: [settings, lane], applying: false }, true, null, here)).toBe(
      "settings:1264,lane:1270",
    );
  });

  it("waits while a change is being applied", () => {
    expect(trialsToConfirm({ trials: [settings], applying: true }, true, null, here)).toBeNull();
  });

  it("confirms nothing from the last reading while the stick does not answer", () => {
    expect(trialsToConfirm({ trials: [settings], applying: false }, false, null, here)).toBeNull();
  });

  it("confirms the same changes once, and new ones again", () => {
    const s = { trials: [settings], applying: false };
    expect(trialsToConfirm(s, true, "settings:1264", here)).toBeNull();
    expect(
      trialsToConfirm(
        { ...s, trials: [{ ...settings, until_s: 1400 }] },
        true,
        "settings:1264",
        here,
      ),
    ).toBe("settings:1400");
  });

  it("confirms a new management address only from a panel opened at it", () => {
    expect(trialsToConfirm({ trials: [mgmt], applying: false }, true, null, here)).toBeNull();
    expect(trialsToConfirm({ trials: [mgmt, lane], applying: false }, true, null, here)).toBe(
      "lane:1270",
    );
    expect(trialsToConfirm({ trials: [mgmt], applying: false }, true, null, "192.0.2.10")).toBe(
      "mgmt:1800",
    );
  });

  it("has nothing to confirm without changes on trial, or on a stick of before", () => {
    expect(trialsToConfirm({ trials: [], applying: false }, true, null, here)).toBeNull();
    expect(trialsToConfirm({ applying: false }, true, null, here)).toBeNull();
    expect(trialsToConfirm(undefined, true, null, here)).toBeNull();
  });
});
