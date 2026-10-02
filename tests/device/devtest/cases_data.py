# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The data path, measured with traffic.

The bench host is both ends: its network-side port reaches the stick through
the OLT and the fibre, its subscriber-side port is the far end of the stick's
host link. The kernel's packet generator on the bench host sends a burst into
one side, and the counters say how much of it came out of the other, and how
much of it the stick's CPU saw: a frame that the switch forwards in hardware
never reaches the CPU.

The frames are UDP from and to address 0.0.0.0, to the receiving port's own
MAC address: the receiving end counts them and then drops them, as it drops
every packet with that source, so none of them is routed anywhere.
"""

from __future__ import annotations

import re
import shlex

from .access import sudo
from .core import Context, case, failed, skipped, verdict

FRAME = 1514           # bytes, without the FCS
THREADS = 2
COUNT = 200000         # frames per thread
PKTGEN = "/proc/net/pktgen"


def bench_counter(ctx: Context, port: str, name: str) -> int:
    return int(ctx.bench.text(f"cat /sys/class/net/{shlex.quote(port)}/statistics/{name}"))


def conduit(ctx: Context) -> str:
    """The stick's interface towards the switch's CPU port."""
    if "conduit" not in ctx.memo:
        ctx.memo["conduit"] = ctx.stick.text(
            "for i in /sys/class/net/*; do [ -e $i/dsa/tagging ] && echo ${i##*/}; done; true")
    if not ctx.memo["conduit"]:
        raise ValueError("no DSA conduit on the stick")
    return ctx.memo["conduit"]


def stick_counter(ctx: Context, port: str, name: str) -> int:
    return int(ctx.stick.text(f"cat /sys/class/net/{port}/statistics/{name}"))


def hardware(ctx: Context, port: str) -> dict[str, int]:
    """The switch's own counters of one port, if the image has ethtool."""
    reply = ctx.stick.run(f"ethtool -S {port}")
    if not reply.ok:
        return {}
    found = {}
    for line in reply.out.splitlines():
        name, _, value = line.strip().partition(":")
        if value.strip().isdigit():
            found[name.strip()] = int(value)
    return found


def burst(ctx: Context, port: str, dst_mac: str) -> float:
    """Send THREADS x COUNT frames out of port; return the slowest thread's
    run time in seconds."""
    script = [f"P={PKTGEN}"]
    for i in range(THREADS):
        dev = f"{port}@{i}"
        script += [f"echo rem_device_all > $P/kpktgend_{i}",
                   f"echo 'add_device {dev}' > $P/kpktgend_{i}"]
        for setting in (f"count {COUNT}", "clone_skb 0", f"pkt_size {FRAME + 4}", "delay 0",
                        f"dst_mac {dst_mac}", "udp_src_min 9", "udp_src_max 9",
                        "udp_dst_min 9", "udp_dst_max 9"):
            script.append(f"echo '{setting}' > $P/{dev}")
    script.append("echo start > $P/pgctrl")
    for i in range(THREADS):
        script += [f"grep -h -A1 '^Result' $P/{port}@{i}",
                   f"echo rem_device_all > $P/kpktgend_{i}"]
    out = ctx.bench.text(sudo(ctx.settings, "\n".join(script)), timeout=300)
    times = [int(t) / 1e6 for t in re.findall(r"Result: OK: (\d+)\(", out)]
    if len(times) != THREADS:
        raise ValueError(f"the packet generator did not report every thread: {out!r}")
    return max(times)


def needs(ctx: Context):
    s = ctx.settings
    if not (s.olt_port and s.subscriber_port):
        return skipped("the settings name no network-side and subscriber-side ports")
    if not ctx.bench.run(f"test -e {PKTGEN}/pgctrl").ok:
        return skipped("the bench host has no packet generator (modprobe pktgen)")
    return None


def measure(ctx: Context, into: str, out_of: str, stick_in: str, stick_out: str):
    """One burst into the bench port `into`, counted at the bench port `out_of`
    and, when the image has ethtool, at the stick's ports on the way."""
    dst = ctx.bench.text(f"cat /sys/class/net/{shlex.quote(out_of)}/address")
    cpu = conduit(ctx)
    before = (bench_counter(ctx, out_of, "rx_packets"), stick_counter(ctx, cpu, "rx_packets"),
              hardware(ctx, stick_in), hardware(ctx, stick_out))
    seconds = burst(ctx, into, dst)
    after = (bench_counter(ctx, out_of, "rx_packets"), stick_counter(ctx, cpu, "rx_packets"),
             hardware(ctx, stick_in), hardware(ctx, stick_out))
    sent = THREADS * COUNT
    arrived = after[0] - before[0]
    cpu_seen = after[1] - before[1]
    hw = ""
    if before[2] and before[3]:
        took = after[2]["rx_unicast"] - before[2]["rx_unicast"]
        gave = after[3]["tx_unicast"] - before[3]["tx_unicast"]
        hw = f"; switch: {stick_in} took {took}, {stick_out} sent {gave}"
    rate = arrived * FRAME * 8 / seconds / 1e6
    return sent, arrived, cpu_seen, seconds, rate, hw


@case("data.downstream", "Data path",
      "A burst from the network side crosses the OLT, the PON and the switch to the host side "
      "in hardware")
def downstream(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    sent, arrived, cpu, seconds, rate, hw = measure(ctx, ctx.settings.olt_port,
                                                    ctx.settings.subscriber_port, "pon", "sfp")
    ok = arrived >= sent * 0.999 and cpu < sent * 0.01
    return verdict(ok, f"{sent} frames of {FRAME} B sent, {arrived} arrived "
                       f"({arrived / sent:.2%}) at {rate:.0f} Mb/s; CPU saw {cpu}{hw}")


@case("data.upstream", "Data path",
      "A burst from the host side, above the upstream rate, leaves by the PON in hardware "
      "and the ONU stays in O5")
def upstream(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    from .cases_pon import onu_state, pon
    sent, arrived, cpu, seconds, rate, hw = measure(ctx, ctx.settings.subscriber_port,
                                                    ctx.settings.olt_port, "sfp", "pon")
    state = onu_state(pon(ctx))
    if arrived == 0:
        return failed(f"{sent} frames sent, none arrived; ONU O{state}")
    ok = cpu < sent * 0.01 and state == 5
    return verdict(ok, f"{sent} frames of {FRAME} B offered, {arrived} arrived "
                       f"({arrived / sent:.2%}) at {rate:.0f} Mb/s; CPU saw {cpu}; "
                       f"ONU O{state}{hw}")
