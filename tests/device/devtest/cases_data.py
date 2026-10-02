# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The data path, measured with traffic.

The bench host is both ends: its network-side port reaches the stick through
the OLT and the fibre, its subscriber-side port is the far end of the stick's
host link. A raw socket on the bench host sends a burst into one side, and the
counters say how much of it left the bench, how much came out of the other
side, and how much of it the stick's CPU saw: a frame that the switch forwards
in hardware never reaches the CPU.

The frames carry the local experimental EtherType 0x88b5 and are addressed to
the receiving port's own MAC address: the receiving end counts them and drops
them, since nothing on it takes that EtherType, so none of them goes further.
What left the bench is read from the sending port's own transmit count, not
from the sender: a frame the bench host could not send is not one the stick
lost, and a burst that never left the bench fails as the bench's fault. Some
drivers refresh those counts from the hardware only every few seconds, so
they are read until they have stood still for longer than that.

The OLT and the stick's switch are learning bridges: until one has seen a
frame from an address, it floods the frames for that address, to the stick's
CPU too, or drops them. So before the first measurement a few frames go each
way, slowly, and every bridge on the way learns both ends.
"""

from __future__ import annotations

import shlex
import time

from .access import sudo
from .core import Context, case, failed, skipped, verdict

FRAME = 1514           # bytes, without the FCS
COUNT = 400000         # frames offered per burst
PRIME = 100            # frames each way before the first measurement
PRIME_GAP = 0.001      # seconds between them
STILL = 200            # frames a counter may move in a pause and still be still

# The sender, run by python3 on the bench host with its arguments: the port,
# the destination MAC address, the frame size, the count and the gap in
# seconds. It prints how long it ran.
SENDER = """\
import socket, sys, time
port, dst, size, count, gap = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5])
src = open("/sys/class/net/%s/address" % port).read().strip()
mac = lambda text: bytes.fromhex(text.replace(":", ""))
frame = mac(dst) + mac(src) + b"\\x88\\xb5" + bytes(size - 14)
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
s.bind((port, 0))
s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
start = time.monotonic()
for _ in range(count):
    try:
        s.send(frame)
    except OSError:
        pass
    if gap:
        time.sleep(gap)
print("%.6f" % (time.monotonic() - start))
"""


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


def burst(ctx: Context, port: str, dst_mac: str, *, count: int = COUNT,
          gap: float = 0.0) -> float:
    """Offer count frames out of port, gap seconds apart; return how long the
    sender ran, in seconds."""
    command = sudo(ctx.settings, " ".join(shlex.quote(a) for a in (
        "python3", "-", port, dst_mac, str(FRAME), str(count), str(gap))))
    reply = ctx.bench.run(command, stdin=SENDER, timeout=300)
    try:
        return float(reply.out.strip().splitlines()[-1])
    except (IndexError, ValueError):
        raise ValueError(f"the sender did not run: {(reply.err or reply.out).strip()!r}")


def settle(read, *, quiet: float = 4.0, pause: float = 0.5,
           limit: float = 30.0) -> tuple[int, ...]:
    """Read counters until none of them has moved by more than STILL for
    quiet seconds, longer than a driver that refreshes its counts every few
    seconds leaves them standing, or until limit seconds have passed."""
    last = read()
    still = waited = 0.0
    while waited < limit and still < quiet:
        time.sleep(pause)
        waited += pause
        now = read()
        if all(abs(b - a) <= STILL for a, b in zip(last, now)):
            still += pause
        else:
            still = 0.0
            last = now
    return last


def prime(ctx: Context) -> None:
    """A few frames each way, upstream first, so that the OLT and the stick's
    switch have learned both ends before anything is measured."""
    if ctx.memo.get("primed"):
        return
    s = ctx.settings
    for into, out_of in ((s.subscriber_port, s.olt_port), (s.olt_port, s.subscriber_port)):
        dst = ctx.bench.text(f"cat /sys/class/net/{shlex.quote(out_of)}/address")
        burst(ctx, into, dst, count=PRIME, gap=PRIME_GAP)
    ctx.memo["primed"] = True


def needs(ctx: Context):
    s = ctx.settings
    if not (s.olt_port and s.subscriber_port):
        return skipped("the settings name no network-side and subscriber-side ports")
    if not ctx.bench.run("command -v python3").ok:
        return skipped("the bench host has no python3")
    return None


def measure(ctx: Context, into: str, out_of: str, stick_in: str, stick_out: str):
    """One burst into the bench port `into`, counted where it left the bench,
    at the bench port `out_of` and, when the image has ethtool, at the stick's
    ports on the way."""
    prime(ctx)
    dst = ctx.bench.text(f"cat /sys/class/net/{shlex.quote(out_of)}/address")
    cpu = conduit(ctx)

    def bench_counts():
        return (bench_counter(ctx, into, "tx_packets"), bench_counter(ctx, out_of, "rx_packets"))

    def counts(bench):
        return bench + (stick_counter(ctx, cpu, "rx_packets"), hardware(ctx, stick_in),
                        hardware(ctx, stick_out))

    before = counts(settle(bench_counts))
    seconds = burst(ctx, into, dst)
    after = counts(settle(bench_counts))
    sent = after[0] - before[0]
    arrived = after[1] - before[1]
    cpu_seen = after[2] - before[2]
    hw = ""
    if before[3] and before[4]:
        took = after[3]["rx_unicast"] - before[3]["rx_unicast"]
        gave = after[4]["tx_unicast"] - before[4]["tx_unicast"]
        hw = f"; switch: {stick_in} took {took}, {stick_out} sent {gave}"
    rate = arrived * FRAME * 8 / seconds / 1e6
    return sent, arrived, cpu_seen, rate, hw


@case("data.downstream", "Data path",
      "A burst from the network side crosses the OLT, the PON and the switch to the host side "
      "in hardware")
def downstream(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    sent, arrived, cpu, rate, hw = measure(ctx, ctx.settings.olt_port,
                                           ctx.settings.subscriber_port, "pon", "sfp")
    if sent < COUNT // 100:
        return failed(f"the bench: {ctx.settings.olt_port} sent {sent} of {COUNT} frames")
    ok = arrived >= sent * 0.999 and cpu < sent * 0.01
    return verdict(ok, f"{sent} frames of {FRAME} B left the bench, {arrived} arrived "
                       f"({arrived / sent:.2%}) at {rate:.0f} Mb/s; CPU saw {cpu}{hw}")


@case("data.upstream", "Data path",
      "A burst from the host side, above the upstream rate, leaves by the PON in hardware "
      "and the ONU stays in O5")
def upstream(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    from .cases_pon import onu_state, pon
    sent, arrived, cpu, rate, hw = measure(ctx, ctx.settings.subscriber_port,
                                           ctx.settings.olt_port, "sfp", "pon")
    state = onu_state(pon(ctx))
    if sent < COUNT // 100:
        return failed(f"the bench: {ctx.settings.subscriber_port} sent {sent} of {COUNT} frames")
    if arrived == 0:
        return failed(f"{sent} frames sent, none arrived; ONU O{state}")
    ok = cpu < sent * 0.01 and state == 5
    return verdict(ok, f"{sent} frames of {FRAME} B left the bench, {arrived} arrived "
                       f"({arrived / sent:.2%}) at {rate:.0f} Mb/s; CPU saw {cpu}; "
                       f"ONU O{state}{hw}")
