// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import { BRIDGE, LIVE, NETWORK } from "../mocks/fixture";
import { vlanMatrix } from "./vlans";

describe("the VLANs on the ports", () => {
  const m = vlanMatrix(NETWORK, BRIDGE, LIVE, [
    { host: 420, fibre: 200 },
    { host: 421, fibre: 300 },
    { host: 410, fibre: 100 },
  ]);
  const row = (vid: number) => m.rows.find((r) => r.vid === vid);

  it("has the SFP port, the processor, then the channels by Port-ID", () => {
    expect(m.columns.map((c) => c.id)).toEqual([
      "sfp",
      "cpu",
      "gem278",
      "gem279",
      "gem280",
      "gem281",
      "gem2046",
      "gem2047",
    ]);
  });

  it("ends the stick's own VLANs on the processor, with what each is for", () => {
    expect(row(4094)).toMatchObject({
      role: "management",
      cells: { sfp: "tagged", cpu: "tagged" },
    });
    expect(row(36)).toMatchObject({ role: "handover", cells: { sfp: "tagged", cpu: "tagged" } });
    expect(row(100)).toMatchObject({ role: "session", cells: { gem278: "tagged", cpu: "tagged" } });
  });

  it("carries a bridged service on the SFP port and its channels, with its translation", () => {
    expect(row(420)?.cells).toEqual({
      sfp: "tagged",
      gem2047: "tagged",
      gem2046: "tagged",
      gem280: "tagged",
    });
    expect(row(420)?.peer).toBe(200);
    expect(row(200)?.peer).toBe(420);
    expect(row(420)?.role).toBe("bridged");
  });
});
