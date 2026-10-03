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

import json
import re
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


def clsact(ctx: Context, devs: list[str]) -> list[str]:
    """Give each of devs on the stick a clsact qdisc; return those that had
    none, from which the case takes it away again."""
    return [dev for dev in devs
            if ctx.stick.run(f"tc qdisc add dev {dev} clsact 2>/dev/null").ok]


# The translation the VLAN case installs: a pair of VIDs that no line is
# expected to carry, so that its entries meet none of the OMCI daemon's, and a
# tc priority of its own, so that it removes only its own filters.
XLATE_SUB_VID = 3001   # the VLAN on the subscriber side
XLATE_NET_VID = 3002   # the VLAN on the network side
XLATE_PCP = 5
XLATE_COUNT = 200
XLATE_PREF = 49000

# Run by python3 on the bench host with SRC DST VID PCP COUNT: COUNT frames of
# EtherType 0x88b5 tagged VID and PCP, untagged for VID 0, from port SRC to port
# DST's address, a millisecond apart. It prints, as JSON, how many such frames
# DST received from SRC's address per tag, "vid/pcp" or "untagged", reading the
# tag from the frame or, where the NIC took it off, from PACKET_AUXDATA.
TAGGED = """\
import json, socket, struct, sys, threading, time
src, dst, vid, pcp, count = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
mac = lambda p: bytes.fromhex(open("/sys/class/net/%s/address" % p).read().strip().replace(":", ""))
SOL_PACKET, PACKET_AUXDATA, TP_STATUS_VLAN_VALID = 263, 8, 0x10
rx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
rx.bind((dst, 0))
rx.setsockopt(SOL_PACKET, PACKET_AUXDATA, 1)
rx.settimeout(0.5)
seen, stop = {}, []
def receive():
    while not stop:
        try:
            data, anc, _, _ = rx.recvmsg(2048, socket.CMSG_SPACE(32))
        except socket.timeout:
            continue
        if data[6:12] != mac(src):
            continue
        tag = None
        for level, kind, value in anc:
            if level == SOL_PACKET and kind == PACKET_AUXDATA:
                status, tci = struct.unpack("I", value[:4])[0], struct.unpack("H", value[16:18])[0]
                if status & TP_STATUS_VLAN_VALID:
                    tag = "%d/%d" % (tci & 0xfff, tci >> 13)
        if data[12:14] == b"\\x81\\x00":
            tci = struct.unpack(">H", data[14:16])[0]
            tag, data = "%d/%d" % (tci & 0xfff, tci >> 13), data[:12] + data[16:]
        if data[12:14] != b"\\x88\\xb5":
            continue
        key = tag or "untagged"
        seen[key] = seen.get(key, 0) + 1
thread = threading.Thread(target=receive)
thread.start()
tx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
tx.bind((src, 0))
tag = struct.pack(">HH", 0x8100, vid | pcp << 13) if vid else b""
frame = mac(dst) + mac(src) + tag + b"\\x88\\xb5" + bytes(46)
for _ in range(count):
    tx.send(frame)
    time.sleep(0.001)
time.sleep(1.5)
stop.append(1)
thread.join()
print(json.dumps(seen))
"""


def tagged(ctx: Context, into: str, out_of: str, vid: int,
           count: int = XLATE_COUNT) -> dict[str, int]:
    """Send count frames tagged vid, untagged for vid 0, into the bench port
    into; return what the bench port out_of received from it, per tag."""
    command = sudo(ctx.settings, " ".join(shlex.quote(a) for a in (
        "python3", "-", into, out_of, str(vid), str(XLATE_PCP), str(count))))
    reply = ctx.bench.run(command, stdin=TAGGED, timeout=60)
    try:
        return json.loads(reply.out.strip().splitlines()[-1])
    except (IndexError, ValueError):
        raise ValueError(f"the probe did not run: {(reply.err or reply.out).strip()!r}")


def tags(seen: dict[str, int]) -> str:
    return ", ".join(f"{n} as {tag}" for tag, n in sorted(seen.items())) or "nothing"


@case("data.vlan-translation", "Data path",
      "A C-VID translation in the switch rewrites the VLAN of frames both ways across the PON "
      "and keeps their priority")
def vlan_translation(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    prime(ctx)
    s = ctx.settings
    sub, net, pcp = XLATE_SUB_VID, XLATE_NET_VID, XLATE_PCP
    added = clsact(ctx, ["sfp", "pon"])
    try:
        for dev, match, assign in (("sfp", sub, net), ("pon", net, sub)):
            reply = ctx.stick.run(f"tc filter add dev {dev} ingress pref {XLATE_PREF} "
                                  f"protocol 802.1Q flower skip_sw vlan_id {match} "
                                  f"action vlan modify id {assign}")
            if not reply.ok:
                return failed(f"tc refused {match} -> {assign} on {dev}: "
                              f"{(reply.err or reply.out).strip()}")
        up = tagged(ctx, s.subscriber_port, s.olt_port, sub)
        down = tagged(ctx, s.olt_port, s.subscriber_port, net)
    finally:
        ctx.stick.run(f"tc filter del dev sfp ingress pref {XLATE_PREF} 2>/dev/null; "
                      f"tc filter del dev pon ingress pref {XLATE_PREF} 2>/dev/null; " +
                      "".join(f"tc qdisc del dev {dev} clsact; " for dev in added) + "true")
    want = XLATE_COUNT * 0.99
    ok = up.get(f"{net}/{pcp}", 0) >= want and down.get(f"{sub}/{pcp}", 0) >= want
    return verdict(ok, f"{XLATE_COUNT} frames of VLAN {sub}/{pcp} upstream: {tags(up)}; "
                       f"{XLATE_COUNT} of {net}/{pcp} downstream: {tags(down)}")


# The tag-operation case: a C-tag pushed onto untagged frames upstream and
# popped downstream, and an S-tag pushed in front of a C-tag upstream and
# popped downstream, at VIDs no line is expected to carry and a tc priority of
# its own.
PUSH_VID = 3003
PUSH_PCP = 5
QINQ_CVID = 3001
QINQ_SVID = 3004
QINQ_PCP = 3
OPS_COUNT = 200
OPS_PREF = 49040

# Run by python3 on the bench host with SRC DST STACK COUNT: COUNT frames of
# EtherType 0x88b5 from port SRC to port DST's address, with the tag stack
# STACK, "u" for none or tags outer first joined by "+", c<vid>p<pcp> for
# 802.1Q and s<vid>p<pcp> for 802.1ad, a millisecond apart. It prints, as JSON,
# how many such frames DST received from SRC's address per tag stack, reading
# a tag the NIC took off from PACKET_AUXDATA.
STACKS = r"""
import json, socket, struct, sys, threading, time
src, dst, spec, count = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
mac = lambda p: bytes.fromhex(open("/sys/class/net/%s/address" % p).read().strip().replace(":", ""))
TPID, NAME = {"c": 0x8100, "s": 0x88a8}, {0x8100: "c", 0x88a8: "s"}
def tags(spec):
    out = []
    for t in ([] if spec == "u" else spec.split("+")):
        vid, _, pcp = t[1:].partition("p")
        out.append(struct.pack(">HH", TPID[t[0]], int(vid) | int(pcp or 0) << 13))
    return b"".join(out)
name = lambda tpid, tci: "%s%dp%d" % (NAME.get(tpid, "x"), tci & 0xfff, tci >> 13)
rx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
rx.bind((dst, 0))
rx.setsockopt(263, 8, 1)
rx.settimeout(0.5)
seen, stop = {}, []
def receive():
    while not stop:
        try:
            data, anc, _, _ = rx.recvmsg(2048, socket.CMSG_SPACE(32))
        except socket.timeout:
            continue
        if data[6:12] != mac(src):
            continue
        stack = []
        for level, kind, value in anc:
            if level == 263 and kind == 8:
                st, _, _, _, _, tci, tpid = struct.unpack("IIIHHHH", value[:20])
                if st & 0x10:
                    stack.append(name(tpid if st & 0x40 else 0x8100, tci))
        off = 12
        while data[off:off + 2] in (b"\x81\x00", b"\x88\xa8"):
            tpid, tci = struct.unpack(">HH", data[off:off + 4])
            stack.append(name(tpid, tci))
            off += 4
        if data[off:off + 2] == b"\x88\xb5":
            key = "+".join(stack) or "u"
            seen[key] = seen.get(key, 0) + 1
thread = threading.Thread(target=receive)
thread.start()
tx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
tx.bind((src, 0))
frame = mac(dst) + mac(src) + tags(spec) + b"\x88\xb5" + bytes(46)
for _ in range(count):
    tx.send(frame)
    time.sleep(0.001)
time.sleep(1.5)
stop.append(1)
thread.join()
print(json.dumps(seen))
"""


def stacks(ctx: Context, into: str, out_of: str, spec: str,
           count: int = OPS_COUNT) -> dict[str, int]:
    """Send count frames with the tag stack spec into the bench port into;
    return what the bench port out_of received from it, per tag stack."""
    command = sudo(ctx.settings, " ".join(shlex.quote(a) for a in (
        "python3", "-", into, out_of, spec, str(count))))
    reply = ctx.bench.run(command, stdin=STACKS, timeout=60)
    try:
        return json.loads(reply.out.strip().splitlines()[-1])
    except (IndexError, ValueError):
        raise ValueError(f"the probe did not run: {(reply.err or reply.out).strip()!r}")


def stack_text(seen: dict[str, int]) -> str:
    return ", ".join(f"{n} as {k}" for k, n in sorted(seen.items())) or "nothing"


@case("data.vlan-push-pop", "Data path",
      "The switch pushes a C-tag onto untagged frames upstream and pops it from frames "
      "downstream, and pushes an S-tag in front of a C-tag upstream and pops it "
      "downstream, with the priorities it is given")
def vlan_push_pop(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    prime(ctx)
    s = ctx.settings
    rule = f"tc filter add dev {{}} ingress pref {OPS_PREF} {{}}"
    drop = "; ".join(f"tc filter del dev {dev} ingress pref {OPS_PREF} 2>/dev/null"
                     for dev in ("sfp", "pon")) + "; true"
    added = clsact(ctx, ["sfp", "pon"])
    try:
        for dev, text in (
                ("sfp", "protocol all flower skip_sw num_of_vlans 0 "
                        f"action vlan push id {PUSH_VID} priority {PUSH_PCP}"),
                ("pon", f"protocol 802.1Q flower skip_sw vlan_id {PUSH_VID} action vlan pop")):
            reply = ctx.stick.run(rule.format(dev, text))
            if not reply.ok:
                return failed(f"tc refused {text!r} on {dev}: "
                              f"{(reply.err or reply.out).strip()}")
        up = stacks(ctx, s.subscriber_port, s.olt_port, "u")
        down = stacks(ctx, s.olt_port, s.subscriber_port, f"c{PUSH_VID}p{PUSH_PCP}")
        ctx.stick.run(drop)
        for dev, text in (
                ("sfp", f"protocol 802.1Q flower skip_sw vlan_id {QINQ_CVID} action vlan push "
                        f"id {QINQ_SVID} priority {QINQ_PCP} protocol 802.1ad"),
                ("pon", f"protocol 802.1ad flower skip_sw vlan_id {QINQ_SVID} action vlan pop")):
            reply = ctx.stick.run(rule.format(dev, text))
            if not reply.ok:
                return failed(f"tc refused {text!r} on {dev}: "
                              f"{(reply.err or reply.out).strip()}")
        qinq = stacks(ctx, s.subscriber_port, s.olt_port, f"c{QINQ_CVID}p1")
        stacked = f"s{QINQ_SVID}p{QINQ_PCP}+c{QINQ_CVID}p1"
        unstacked = stacks(ctx, s.olt_port, s.subscriber_port, stacked)
    finally:
        ctx.stick.run(drop.removesuffix("; true") + "; " +
                      "".join(f"tc qdisc del dev {dev} clsact; " for dev in added) + "true")
    want = OPS_COUNT * 0.99
    pushed = f"c{PUSH_VID}p{PUSH_PCP}"
    ok = (up.get(pushed, 0) >= want and down.get("u", 0) >= want and
          qinq.get(stacked, 0) >= want and unstacked.get(f"c{QINQ_CVID}p1", 0) >= want)
    return verdict(ok, f"{OPS_COUNT} untagged frames upstream: {stack_text(up)}; "
                       f"{OPS_COUNT} of {pushed} downstream: {stack_text(down)}; "
                       f"{OPS_COUNT} of c{QINQ_CVID}p1 upstream: {stack_text(qinq)}; "
                       f"{OPS_COUNT} of {stacked} downstream: {stack_text(unstacked)}")


# The ACL case: frames of the probe's EtherType from the network side, against
# drop and trap rules on the PON port at two tc priorities of its own. The CPU
# gets a frame from the PON on the interface of its GEM port, so a software
# filter on each GEM interface counts the frames that reach it.
ACL_COUNT = 200
ACL_PREFS = (49010, 49020)
ACL_COUNTER = 49030


def acl_round(ctx: Context, gems: list[str],
              rules: list[tuple[int, str]]) -> tuple[int, int]:
    """Install rules, (priority, action) pairs in the order given, on the PON
    port's ingress for EtherType 0x88b5; send ACL_COUNT such frames from the
    network side to the subscriber side; remove the rules. Returns how many
    arrived on the subscriber side and how many reached the CPU."""
    s = ctx.settings
    rule = "tc filter {} dev {} ingress pref {} protocol 0x88b5 flower {}"
    for gem in gems:
        ctx.stick.run(rule.format("add", gem, ACL_COUNTER, "skip_hw action pass"))
    try:
        for pref, action in rules:
            reply = ctx.stick.run(rule.format("add", "pon", pref, f"skip_sw action {action}"))
            if not reply.ok:
                raise ValueError(f"tc refused {action} at {pref}: "
                                 f"{(reply.err or reply.out).strip()}")
        arrived = tagged(ctx, s.olt_port, s.subscriber_port, 0, ACL_COUNT).get("untagged", 0)
        shown = ctx.stick.text("; ".join(
            f"tc -s filter show dev {gem} ingress pref {ACL_COUNTER}" for gem in gems))
    finally:
        ctx.stick.run("; ".join(
            [f"tc filter del dev pon ingress pref {pref} 2>/dev/null" for pref, _ in rules] +
            [f"tc filter del dev {gem} ingress pref {ACL_COUNTER} 2>/dev/null" for gem in gems] +
            ["true"]))
    counts = re.findall(r"Sent \d+ bytes (\d+) pkt", shown)
    if len(counts) != len(gems):
        raise ValueError(f"no count from a software filter: {shown!r}")
    return arrived, sum(int(n) for n in counts)


@case("data.acl", "Data path",
      "ACL rules on the PON port drop frames from the network side or trap them to the CPU, "
      "the rule of the lower tc priority deciding whichever was added first")
def acl(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    prime(ctx)
    gems = ctx.stick.text("cd /sys/class/net && ls -d gem* 2>/dev/null; true").split()
    if not gems:
        return failed("no GEM interface on the stick")
    first, second = ACL_PREFS
    every, none = ACL_COUNT * 0.99, ACL_COUNT * 0.01
    forwarded = lambda a, t: a >= every and t <= none
    trapped = lambda a, t: a <= none and t >= every
    dropped = lambda a, t: a <= none and t <= none
    rounds = (
        ("no rule", [], forwarded),
        ("trap", [(first, "trap")], trapped),
        ("drop", [(first, "drop")], dropped),
        ("trap over drop, added last", [(second, "drop"), (first, "trap")], trapped),
        ("drop over trap, added last", [(second, "trap"), (first, "drop")], dropped),
    )
    added = clsact(ctx, ["pon", *gems])
    seen, ok = [], True
    try:
        for name, rules, good in rounds:
            arrived, took = acl_round(ctx, gems, rules)
            ok = ok and good(arrived, took)
            seen.append(f"{name}: {arrived} arrived, {took} to the CPU")
    finally:
        ctx.stick.run("; ".join([f"tc qdisc del dev {dev} clsact" for dev in added] + ["true"]))
    return verdict(ok, f"{ACL_COUNT} frames each; " + "; ".join(seen))


# The multicast case: groups of the organisation-local IPv4 scope (RFC 2365)
# and of the transient IPv6 range, which no line is expected to carry, and the
# mDNS group as a link-local one, which a snooping bridge floods unasked.
MC_COUNT = 200
MC_REPORTS = 3
MC_UNASKED = "239.255.77.1"
MC_JOINED = "239.255.77.2"
MC_LOCAL = "224.0.0.251"
MC6_UNASKED = "ff3e::77:1"
MC6_JOINED = "ff3e::77:2"
MC_REFRESH = 4         # seconds between a join and the reports that must restart its timer

# Run by python3 on the bench host with SRC DST KIND GROUP COUNT: COUNT frames
# of KIND for GROUP from port SRC, 2 ms apart, DST put in all-multicast mode
# meanwhile. It prints, as JSON, how many of them DST received unchanged. KIND
# is query (an IGMPv2 general query), join and leave (IGMPv2), data (UDP to the
# group, IPv4 or IPv6), mldquery (an MLDv2 general query), mldjoin2 (an MLDv2
# report, to ff02::16) or mldjoin1 (an MLDv1 report, to the group).
MCAST = r"""
import json, socket, struct, sys, threading, time
src, dst, kind, group, count = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
mac = lambda p: bytes.fromhex(open("/sys/class/net/%s/address" % p).read().strip().replace(":", ""))
SRC4, SRC6 = socket.inet_aton("192.0.2.10"), socket.inet_pton(socket.AF_INET6, "fe80::10")

def csum(b):
    b += b"\0" * (len(b) % 2)
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff

def mac4(g):
    a = socket.inet_aton(g)
    return b"\x01\x00\x5e" + bytes([a[1] & 0x7f]) + a[2:]

def mac6(g):
    return b"\x33\x33" + socket.inet_pton(socket.AF_INET6, g)[12:]

def ip4(to, proto, payload, ttl=1):
    ra = b"\x94\x04\x00\x00" if proto == 2 else b""
    h = struct.pack("!BBHHHBBH4s4s", 0x45 + len(ra) // 4, 0xc0 if ra else 0, 20 + len(ra) + len(payload),
                    0, 0x4000, ttl, proto, 0, SRC4, socket.inet_aton(to)) + ra
    return mac4(to) + mac(src) + b"\x08\x00" + h[:10] + struct.pack("!H", csum(h)) + h[12:] + payload

def ip6(to, nxt, payload):
    d = socket.inet_pton(socket.AF_INET6, to)
    payload = payload[:2 if nxt == 58 else 6] + struct.pack(
        "!H", csum(SRC6 + d + struct.pack("!IxxxB", len(payload), nxt) + payload)) + payload[4 if nxt == 58 else 8:]
    if nxt == 58:
        body, first, hops = struct.pack("!BB", 58, 0) + b"\x05\x02\x00\x00\x01\x00" + payload, 0, 1
    else:
        body, first, hops = payload, nxt, 8
    return mac6(to) + mac(src) + b"\x86\xdd" + struct.pack("!IHBB16s16s", 6 << 28, len(body), first, hops, SRC6, d) + body

def igmp(t, to, g, mrt=0):
    m = struct.pack("!BBH4s", t, mrt, 0, socket.inet_aton(g))
    return ip4(to, 2, m[:2] + struct.pack("!H", csum(m)) + m[4:])

g6 = socket.inet_pton(socket.AF_INET6, group) if ":" in group else b""
frame = {
    "query": lambda: igmp(0x11, "224.0.0.1", "0.0.0.0", 10),
    "join": lambda: igmp(0x16, group, group),
    "leave": lambda: igmp(0x17, "224.0.0.2", group),
    "data": lambda: (ip6(group, 17, struct.pack("!HHHH", 5000, 5001, 26, 0) + b"devtest multicast.")
                     if g6 else ip4(group, 17, struct.pack("!HHHH", 5000, 5001, 26, 0) + b"devtest multicast.", 8)),
    "mldquery": lambda: ip6("ff02::1", 58, struct.pack("!BBHHH16sBBH", 130, 0, 0, 1000, 0, bytes(16), 2, 125, 0)),
    "mldjoin2": lambda: ip6("ff02::16", 58, struct.pack("!BBHHHBBH16s", 143, 0, 0, 0, 1, 4, 0, 0, g6)),
    "mldjoin1": lambda: ip6(group, 58, struct.pack("!BBHHH16s", 131, 0, 0, 0, 0, g6)),
}[kind]()
rx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
rx.bind((dst, 0))
rx.setsockopt(263, 1, struct.pack("iHH8s", socket.if_nametoindex(dst), 2, 0, b""))
rx.settimeout(0.3)
got, stop = [0], []
def receive():
    while not stop:
        try:
            data = rx.recv(2048)
        except socket.timeout:
            continue
        if data[:len(frame)] == frame:
            got[0] += 1
thread = threading.Thread(target=receive)
thread.start()
time.sleep(0.2)
tx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
tx.bind((src, 0))
for _ in range(count):
    tx.send(frame)
    time.sleep(0.002)
time.sleep(1.5)
stop.append(1)
thread.join()
print(json.dumps({"sent": count, "got": got[0]}))
"""


def mcast(ctx: Context, into: str, out_of: str, kind: str, group: str, count: int) -> int:
    """Send count frames of kind for group into the bench port into; return how
    many the bench port out_of received unchanged."""
    command = sudo(ctx.settings, " ".join(shlex.quote(a) for a in (
        "python3", "-", into, out_of, kind, group, str(count))))
    reply = ctx.bench.run(command, stdin=MCAST, timeout=60)
    try:
        return int(json.loads(reply.out.strip().splitlines()[-1])["got"])
    except (IndexError, KeyError, ValueError):
        raise ValueError(f"the probe did not run: {(reply.err or reply.out).strip()!r}")


def group_timer(ctx: Context, group: str) -> float | None:
    """The bridge's timer of group on sfp, None while the bridge has no such
    group, and whether the group is offloaded, from bridge -s mdb show."""
    shown = ctx.stick.text("bridge -s mdb show dev omcibr0 2>/dev/null; true")
    for line in shown.splitlines():
        words = line.split()
        if "sfp" in words and group in words and "offload" in words:
            try:
                return float(words[-1])
            except ValueError:
                return None
    return None


@case("data.igmp", "Data path",
      "Multicast snooping: from the network side a group a host joined reaches it and one no "
      "host asked for does not, the link-local groups pass, and every report reaches the network")
def igmp(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    s = ctx.settings
    if ctx.stick.text("cat /sys/class/net/omcibr0/bridge/multicast_snooping 2>/dev/null; true") != "1":
        return failed("no bridge omcibr0 that snoops multicast")

    def down(kind, group, count=MC_COUNT):
        return mcast(ctx, s.olt_port, s.subscriber_port, kind, group, count)

    def up(kind, group, count=MC_REPORTS):
        return mcast(ctx, s.subscriber_port, s.olt_port, kind, group, count)

    try:
        query = down("query", "0.0.0.0", 1)
        unasked = down("data", MC_UNASKED)
        local = down("data", MC_LOCAL)
        join = up("join", MC_JOINED)
        first = group_timer(ctx, MC_JOINED)
        joined = down("data", MC_JOINED)
        time.sleep(MC_REFRESH)
        again = up("join", MC_JOINED)
        later = group_timer(ctx, MC_JOINED)
        mquery = down("mldquery", "ff02::1", 1)
        mjoin = up("mldjoin2", MC6_JOINED)
        unasked6 = down("data", MC6_UNASKED)
        joined6 = down("data", MC6_JOINED)
        first6 = group_timer(ctx, MC6_JOINED)
        time.sleep(MC_REFRESH)
        again6 = up("mldjoin1", MC6_JOINED)
        later6 = group_timer(ctx, MC6_JOINED)
        ctx.stick.run("bridge link set dev sfp fastleave on")
        leave = up("leave", MC_JOINED, 1)
        gone = group_timer(ctx, MC_JOINED) is None
        left = down("data", MC_JOINED)
    finally:
        # Snooping off and on again forgets the case's groups and routers.
        ctx.stick.run("bridge link set dev sfp fastleave off; "
                      "ip link set omcibr0 type bridge mcast_snooping 0; "
                      "ip link set omcibr0 type bridge mcast_snooping 1")
    # A timer the reports restarted reads about what it read after the join;
    # one they did not has run down by MC_REFRESH seconds and more.
    every, none = MC_COUNT * 0.99, MC_COUNT * 0.01
    refreshed = lambda a, b: a is not None and b is not None and b > a - MC_REFRESH / 2
    ok = (query == 1 and unasked <= none and local >= every and join == MC_REPORTS and
          first is not None and joined >= every and again == MC_REPORTS and refreshed(first, later) and
          mquery == 1 and mjoin == MC_REPORTS and unasked6 <= none and joined6 >= every and
          again6 == MC_REPORTS and refreshed(first6, later6) and leave == 1 and gone and left <= none)
    timer = lambda a, b: "restarted" if refreshed(a, b) else f"not restarted ({a} s, then {b} s)"
    return verdict(ok, f"IGMPv2 query {query}/1 to the host side; from the network side, unasked "
                       f"{unasked}/{MC_COUNT}, {MC_LOCAL} {local}/{MC_COUNT}; join {join}/{MC_REPORTS} "
                       f"to the network, group {'offloaded' if first is not None else 'missing'}, "
                       f"its data {joined}/{MC_COUNT}; {again}/{MC_REPORTS} reports again, timer "
                       f"{timer(first, later)}; MLD query {mquery}/1, MLDv2 join {mjoin}/{MC_REPORTS}, "
                       f"unasked {unasked6}/{MC_COUNT}, joined {joined6}/{MC_COUNT}, MLDv1 reports "
                       f"{again6}/{MC_REPORTS}, timer {timer(first6, later6)}; leave {leave}/1, group "
                       f"{'gone' if gone else 'kept'}, its data {left}/{MC_COUNT}")
