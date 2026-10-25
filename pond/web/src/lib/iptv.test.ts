// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

import { describe, expect, it } from "vitest";
import type { Iptv } from "../api/types";
import {
  eventRows,
  knownRows,
  linkKind,
  linkLocal,
  refusedRows,
  shortAddr,
  withoutLinkLocal,
} from "./iptv";

describe("the addresses of the television page", () => {
  it("shortens an IPv6 address as it is written and leaves IPv4 alone", () => {
    expect(shortAddr("ff02:0000:0000:0000:0000:0000:0000:0002")).toBe("ff02::2");
    expect(shortAddr("ff02:0000:0000:0000:0000:0001:ffd0:609b")).toBe("ff02::1:ffd0:609b");
    expect(shortAddr("2001:0db8:0000:0000:0000:0000:00d0:609b")).toBe("2001:db8::d0:609b");
    expect(shortAddr("239.10.0.1")).toBe("239.10.0.1");
    expect(shortAddr("not an address:")).toBe("not an address:");
  });

  it("knows the groups that never leave their link", () => {
    expect(linkLocal("224.0.0.2")).toBe(true);
    expect(linkLocal("224.0.0.255")).toBe(true);
    expect(linkLocal("224.0.1.1")).toBe(false);
    expect(linkLocal("232.0.18.103")).toBe(false);
    expect(linkLocal("ff02:0000:0000:0000:0000:0000:0000:0002")).toBe(true);
    expect(linkLocal("ff02::1:ff00:0")).toBe(true);
    expect(linkLocal("ff01::1")).toBe(true);
    expect(linkLocal("ff05::1:3")).toBe(false);
    expect(linkLocal("ff3e::77:1")).toBe(false);
    expect(linkLocal("fe80::1")).toBe(false);
    expect(linkLocal("ff0::")).toBe(false);
  });
});

const refusal = (at_s: number, group: string, client: string | null, reason = "not_allowed") =>
  ({ at_s, group, vlan: 4094, client, reason }) as Iptv["rejected"][number];

describe("the refusals", () => {
  it("are one row for a group refused again and again, with who asked", () => {
    const rows = refusedRows([
      refusal(
        100,
        "ff02:0000:0000:0000:0000:0000:0000:0002",
        "2001:0db8:0000:0000:0000:0000:00d0:609b",
      ),
      refusal(103, "ff02::2", "2001:db8::d0:609b"),
      refusal(101, "ff02::2", null),
      refusal(90, "239.1.1.1", "192.0.2.2", "max_groups"),
    ]);
    expect(rows).toHaveLength(3);
    const same = rows.filter((r) => r.group.startsWith("ff02"));
    expect(same.map((r) => [r.count, r.lastAt])).toContainEqual([1, 100]);
    expect(rows[0]).toMatchObject({ group: "ff02::2", count: 2, lastAt: 103 });
    expect(rows[0]?.clients).toEqual(["2001:db8::d0:609b"]);
    expect(rows.at(-1)).toMatchObject({ group: "239.1.1.1", reason: "max_groups", count: 1 });
  });
});

const ev = (at_s: number, event: Iptv["log"][number]["event"], group: string | null, vlan = 839) =>
  ({ at_s, event, group, vlan }) as Iptv["log"][number];

describe("the events", () => {
  it("are one row for what came again, the latest first", () => {
    const rows = eventRows([
      ev(110, "join", "224.0.0.13"),
      ev(108, "query", null, 2600),
      ev(107, "join", "224.0.0.22"),
      ev(80, "join", "224.0.0.13"),
      ev(79, "join", "224.0.0.22"),
      ev(50, "join", "232.0.18.103"),
    ]);
    expect(rows.map((r) => [r.event, r.group, r.count, r.lastAt])).toEqual([
      ["join", "224.0.0.13", 2, 110],
      ["query", null, 1, 108],
      ["join", "224.0.0.22", 2, 107],
      ["join", "232.0.18.103", 1, 50],
    ]);
  });
});

describe("the groups of the link", () => {
  const tv = {
    now_s: 200,
    policy: null,
    groups: [
      {
        group: "232.0.18.103",
        source: null,
        vlan: 839,
        client: "198.51.100.1",
        since_s: 5,
        rate_bps: 1,
      },
      { group: "224.0.0.2", source: null, vlan: 839, client: null, since_s: 5, rate_bps: null },
    ],
    rejected: [refusal(10, "ff02::2", null), refusal(11, "239.9.9.9", null, "max_groups")],
    lost: [],
    counters: { joins: 1, leaves: 0, rejected: 2, queries: 0, reports: 0 },
    querier: null,
    priority: null,
    log: [ev(20, "join", "224.0.0.13"), ev(21, "join", "232.0.18.103")],
  } as unknown as Iptv;

  it("are hidden from what is watched and the events unless asked for, and counted", () => {
    const hidden = withoutLinkLocal(tv, false);
    expect(hidden.tv.groups.map((g) => g.group)).toEqual(["232.0.18.103"]);
    expect(hidden.tv.log.map((e) => e.group)).toEqual(["232.0.18.103"]);
    expect(hidden.hidden).toEqual({ events: 1, watched: 1 });
    expect(withoutLinkLocal(tv, true).tv).toEqual(tv);
    expect(withoutLinkLocal(tv, true).hidden).toEqual({ events: 0, watched: 0 });
  });

  it("stay among the refusals, where they are told apart by their kind", () => {
    expect(withoutLinkLocal(tv, false).tv.rejected).toEqual(tv.rejected);
  });
});

describe("the kinds of the groups of the link", () => {
  it("tell the neighbour search of IPv6 from the rest", () => {
    expect(linkKind("ff02::1:ffd0:609b")).toBe("neighbours");
    expect(linkKind("ff02:0000:0000:0000:0000:0001:ff00:0000")).toBe("neighbours");
    expect(linkKind("ff02::1:fe00:1")).toBe("other");
    expect(linkKind("ff01::1:ffd0:609b")).toBe("other");
  });

  it("know the groups that have a use of their own", () => {
    expect(linkKind("ff02::1")).toBe("all_nodes");
    expect(linkKind("ff01::1")).toBe("all_nodes");
    expect(linkKind("ff02::2")).toBe("all_routers");
    expect(linkKind("ff02::16")).toBe("reports");
    expect(linkKind("ff02::fb")).toBe("discovery");
    expect(linkKind("ff02::1:3")).toBe("discovery");
    expect(linkKind("ff02::1:2")).toBe("dhcp");
    expect(linkKind("ff02::5")).toBe("routing");
    expect(linkKind("224.0.0.1")).toBe("all_nodes");
    expect(linkKind("224.0.0.2")).toBe("all_routers");
    expect(linkKind("224.0.0.22")).toBe("reports");
    expect(linkKind("224.0.0.251")).toBe("discovery");
    expect(linkKind("224.0.0.13")).toBe("routing");
    expect(linkKind("224.0.0.99")).toBe("other");
  });

  it("are none for a channel", () => {
    expect(linkKind("232.0.18.103")).toBeNull();
    expect(linkKind("239.10.0.1")).toBeNull();
    expect(linkKind("ff3e::77:1")).toBeNull();
    expect(linkKind("not an address:")).toBeNull();
  });
});

describe("the known refusals", () => {
  it("are one row for a kind, with how many groups and devices asked, the latest first", () => {
    const rows = knownRows([
      refusal(100, "ff02::1:ffd0:609b", "2001:db8::d0:609b"),
      refusal(101, "ff02::1:ff00:0", "2001:db8::d0:609b"),
      refusal(102, "ff02::1:ffd0:609b", "fe80::1"),
      refusal(103, "ff02::1:ffd0:609b", null),
      refusal(90, "224.0.0.2", null),
      refusal(95, "239.9.9.9", null, "max_groups"),
    ]);
    expect(rows).toEqual([
      {
        kind: "neighbours",
        count: 4,
        groups: 2,
        clients: 2,
        lastAt: 103,
        example: "ff02::1:ffd0:609b",
      },
      { kind: "all_routers", count: 1, groups: 1, clients: 0, lastAt: 90, example: "224.0.0.2" },
    ]);
  });

  it("are none without a group of the link", () => {
    expect(knownRows([refusal(1, "239.9.9.9", null, "max_groups")])).toEqual([]);
  });
});
