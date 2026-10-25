// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { afterEach, describe, expect, it } from "vitest";
import { LIVE } from "../mocks/fixture";
import { readTraffic, record, resetTraffic } from "./live";
import type { Live } from "./types";

/* The stick when its processor has counted @busy and @idle ticks of 10 ms, the rest as it was. */
function ticks(busy: number, idle: number): Live {
  const live = structuredClone(LIVE);
  if (live.cpu)
    live.cpu.ticks = {
      user: busy,
      nice: 0,
      system: 0,
      idle,
      iowait: 0,
      irq: 0,
      softirq: 0,
      steal: 0,
    };
  return live;
}

afterEach(resetTraffic);

describe("the history of the stick's rates", () => {
  it("has the share the processor was busy between two reads five seconds apart", () => {
    record(ticks(0, 0), 1_000);
    record(ticks(30, 470), 6_000);
    expect(readTraffic()).toHaveLength(1);
    expect(readTraffic()[0]?.cpuBusy).toBeCloseTo(0.06, 5);
  });

  it("takes two reads close together for one moment, whose busy share is the answering of them", () => {
    record(ticks(0, 0), 1_000);
    /* The stick spent 90 of the 100 ticks of the next second answering the reads. */
    record(ticks(9, 1), 1_100);
    expect(readTraffic()).toHaveLength(0);
    /* The next read is measured from the first, as if the close one had not been. */
    record(ticks(30, 470), 6_000);
    expect(readTraffic()).toHaveLength(1);
    expect(readTraffic()[0]?.cpuBusy).toBeCloseTo(0.06, 5);
  });
});
