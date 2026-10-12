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


def measure(ctx: Context, into: str, out_of: str, stick_in: str, stick_out: str,
            count: int = COUNT):
    """One burst of count frames into the bench port `into`, counted where it
    left the bench, at the bench port `out_of` and, when the image has
    ethtool, at the stick's ports on the way."""
    prime(ctx)
    dst = ctx.bench.text(f"cat /sys/class/net/{shlex.quote(out_of)}/address")
    cpu = conduit(ctx)

    def bench_counts():
        return (bench_counter(ctx, into, "tx_packets"), bench_counter(ctx, out_of, "rx_packets"))

    def counts(bench):
        return bench + (stick_counter(ctx, cpu, "rx_packets"), hardware(ctx, stick_in),
                        hardware(ctx, stick_out))

    before = counts(settle(bench_counts))
    seconds = burst(ctx, into, dst, count=count)
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
                       f"({arrived / sent:.2%}), carried at {rate:.0f} Mb/s; CPU saw {cpu}{hw}")


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
    # The burst is above the PON's rate, so the share that arrives is the PON's rate
    # over the burst's, and the rate is the PON's.
    return verdict(ok, f"{sent} frames of {FRAME} B left the bench, {arrived} arrived "
                       f"({arrived / sent:.2%} of a burst above the PON's rate), carried at "
                       f"{rate:.0f} Mb/s; CPU saw {cpu}; ONU O{state}{hw}")


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
           count: int = XLATE_COUNT, pcp: int = XLATE_PCP) -> dict[str, int]:
    """Send count frames tagged vid with priority pcp, untagged for vid 0,
    into the bench port into; return what the bench port out_of received from
    it, per tag."""
    command = sudo(ctx.settings, " ".join(shlex.quote(a) for a in (
        "python3", "-", into, out_of, str(vid), str(pcp), str(count))))
    reply = ctx.bench.run(command, stdin=TAGGED, timeout=60)
    try:
        return json.loads(reply.out.strip().splitlines()[-1])
    except (IndexError, ValueError):
        raise ValueError(f"the probe did not run: {(reply.err or reply.out).strip()!r}")


def tags(seen: dict[str, int]) -> str:
    return ", ".join(f"{n} as {tag}" for tag, n in sorted(seen.items())) or "nothing"


@case("data.vlan-translation", "Data path",
      "A C-VID translation in the switch rewrites the VLAN of frames both ways across the PON "
      "and keeps their priority", tools=("tc",))
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


# The priority case: a translation of the subscriber VLAN's frames of one
# priority only, which the OMCI daemon programs for an ME 171 rule whose
# filter names a priority.
PRI_MATCHED = 5
PRI_OTHER = 3


@case("data.vlan-priority", "Data path",
      "A translation of one priority rewrites the VLAN of that priority's frames alone",
      tools=("tc",))
def vlan_priority(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    prime(ctx)
    s = ctx.settings
    sub, net = XLATE_SUB_VID, XLATE_NET_VID
    added = clsact(ctx, ["sfp"])
    try:
        reply = ctx.stick.run(f"tc filter add dev sfp ingress pref {XLATE_PREF} "
                              f"protocol 802.1Q flower skip_sw vlan_id {sub} "
                              f"vlan_prio {PRI_MATCHED} action vlan modify id {net}")
        if not reply.ok:
            return failed(f"tc refused {sub} priority {PRI_MATCHED} -> {net}: "
                          f"{(reply.err or reply.out).strip()}")
        hit = tagged(ctx, s.subscriber_port, s.olt_port, sub, pcp=PRI_MATCHED)
        miss = tagged(ctx, s.subscriber_port, s.olt_port, sub, pcp=PRI_OTHER)
    finally:
        ctx.stick.run(f"tc filter del dev sfp ingress pref {XLATE_PREF} 2>/dev/null; " +
                      "".join(f"tc qdisc del dev {dev} clsact; " for dev in added) + "true")
    want = XLATE_COUNT * 0.99
    ok = (hit.get(f"{net}/{PRI_MATCHED}", 0) >= want and
          miss.get(f"{sub}/{PRI_OTHER}", 0) >= want)
    return verdict(ok, f"{XLATE_COUNT} frames of VLAN {sub}/{PRI_MATCHED}: {tags(hit)}; "
                       f"{XLATE_COUNT} of {sub}/{PRI_OTHER}: {tags(miss)}")


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
      "downstream, with the priorities it is given", tools=("tc",))
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


# The management VLAN of the boot script: no frame of it may cross the PON, so
# that the network behind the OLT neither sees the management link nor reaches
# it. Frames of another VLAN, which the lab OLT carries, cross both ways as the
# control.
MGMT_CONTROL_VID = 3005


def mgmt_vlan(ctx: Context) -> int | None:
    """The VID of the stick's management VLAN interface, mgmt, or None."""
    reply = ctx.stick.run("cat /proc/net/vlan/config")
    found = re.search(r"^mgmt\s*\|\s*(\d+)\s*\|", reply.out, re.M)
    return int(found.group(1)) if found else None


@case("data.mgmt-isolation", "Data path",
      "No frame of the management VLAN crosses the PON in either direction, while the "
      "frames of another VLAN cross both ways")
def mgmt_isolation(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    vid = mgmt_vlan(ctx)
    if vid is None:
        return failed("the stick has no management VLAN interface mgmt")
    prime(ctx)
    s = ctx.settings
    mgmt, other = f"c{vid}", f"c{MGMT_CONTROL_VID}"
    up = stacks(ctx, s.subscriber_port, s.olt_port, mgmt)
    down = stacks(ctx, s.olt_port, s.subscriber_port, mgmt)
    other_up = stacks(ctx, s.subscriber_port, s.olt_port, other)
    other_down = stacks(ctx, s.olt_port, s.subscriber_port, other)
    want = OPS_COUNT * 0.99
    ok = (not up and not down and other_up.get(f"{other}p0", 0) >= want and
          other_down.get(f"{other}p0", 0) >= want)
    return verdict(ok, f"{OPS_COUNT} of VLAN {vid} upstream: {stack_text(up)}, downstream: "
                       f"{stack_text(down)}; {OPS_COUNT} of VLAN {MGMT_CONTROL_VID} upstream: "
                       f"{stack_text(other_up)}, downstream: {stack_text(other_down)}")


# The stick's silence towards the PON: it has no address there, so it answers
# nothing that comes from the network side, and nothing of its own leaves by
# the PON. The bench asks from the network side, untagged, for the management
# address by ARP and pings all IPv6 nodes of the link; software filters on the
# stick's GEM interfaces count the questions that reach its CPU, which a zero
# needs as its control. The same questions from the subscriber side, in the
# management VLAN, are answered: the control that the probe and the stick's
# answers work.
SILENCE_COUNT = 10
SILENCE_PREF = 49040

# Run by python3 on the bench host with PORT VID ADDRESS STICK COUNT: COUNT
# times, an ARP request for ADDRESS and an ICMPv6 echo request from the port's
# own link-local address to all nodes (ff02::1), tagged VID, untagged for VID
# 0, a fifth of a second apart. It joins its solicited-node group and answers
# a neighbour solicitation for its address, which the bench need not hold,
# so that an echo can be answered.
# It prints, as JSON, what came back to PORT from the MAC address STICK alone,
# whatever its tag (another ONU on the PON may answer too): ARP replies for
# ADDRESS, echo replies, and neighbour solicitations for its address.
SILENCE = """\
import json, socket, struct, sys, threading, time
port, vid, address, stick, count = (sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4],
                                    int(sys.argv[5]))
src = bytes.fromhex(open("/sys/class/net/%s/address" % port).read().strip().replace(":", ""))
peer = bytes.fromhex(stick.replace(":", ""))
own = b"\\xfe\\x80" + bytes(6) + bytes([src[0] ^ 2]) + src[1:3] + b"\\xff\\xfe" + src[3:6]
target = socket.inet_aton(address)
tag = struct.pack(">HH", 0x8100, vid) if vid else b""
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
s.bind((port, 0))
s.settimeout(0.3)
# The solicitations for its address go to its solicited-node group, which
# the port filters out unless the socket joins it.
s.setsockopt(263, 1, struct.pack("iHH8s", socket.if_nametoindex(port), 0, 6,
                                 b"\\x33\\x33\\xff" + own[13:16]))
seen = {"arp": 0, "echo6": 0, "ns": 0}
stop = []
def checksum(b):
    b += b"\\0" * (len(b) % 2)
    total = sum(struct.unpack(">%dH" % (len(b) // 2), b))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return ~total & 0xffff
def icmp6(dst_mac, dst, body):
    body = body[:2] + struct.pack(">H", checksum(own + dst + struct.pack(">I", len(body)) +
                                                b"\\0\\0\\0\\x3a" + body)) + body[4:]
    return (dst_mac + src + tag + b"\\x86\\xdd" +
            struct.pack(">IHBB", 0x60000000, len(body), 58, 255) + own + dst + body)
def receive():
    while not stop:
        try:
            data = s.recv(2048)
        except socket.timeout:
            continue
        if data[6:12] != peer:
            continue
        off = 12
        while data[off:off + 2] in (b"\\x81\\x00", b"\\x88\\xa8"):
            off += 4
        kind, body = data[off:off + 2], data[off + 2:]
        if kind == b"\\x08\\x06" and body[6:8] == b"\\x00\\x02" and body[14:18] == target:
            seen["arp"] += 1
        elif kind == b"\\x86\\xdd" and len(body) > 41 and body[6] == 58:
            if body[40] == 129:
                seen["echo6"] += 1
            elif body[40] == 135 and body[48:64] == own:
                seen["ns"] += 1
                advert = (struct.pack(">BBHI", 136, 0, 0, 0x60000000) + own +
                          b"\\x02\\x01" + src)
                s.send(icmp6(data[6:12], body[8:24], advert))
thread = threading.Thread(target=receive, daemon=True)
thread.start()
arp = (b"\\xff" * 6 + src + tag + b"\\x08\\x06" + struct.pack(">HHBBH", 1, 0x0800, 6, 4, 1) +
       src + socket.inet_aton("192.0.2.254") + bytes(6) + target)
arp += bytes(max(0, 60 - len(arp)))
nodes = bytes.fromhex("ff020000000000000000000000000001")
echo = icmp6(b"\\x33\\x33\\x00\\x00\\x00\\x01", nodes,
             struct.pack(">BBHHH", 128, 0, 0, 0x5eed, 1) + b"silence")
for _ in range(count):
    s.send(arp)
    s.send(echo)
    time.sleep(0.2)
time.sleep(1.5)
stop.append(1)
thread.join()
print(json.dumps(seen))
"""


def silence_probe(ctx: Context, port: str, vid: int, address: str,
                  stick: str) -> dict[str, int]:
    command = sudo(ctx.settings, " ".join(shlex.quote(a) for a in (
        "python3", "-", port, str(vid), address, stick, str(SILENCE_COUNT))))
    reply = ctx.bench.run(command, stdin=SILENCE, timeout=60)
    try:
        return json.loads(reply.out.strip().splitlines()[-1])
    except (IndexError, ValueError):
        raise ValueError(f"the probe did not run: {(reply.err or reply.out).strip()!r}")


@case("data.pon-silence", "Data path",
      "The stick keeps IPv6 off its PON side and answers neither ARP for its management "
      "address nor an IPv6 ping of all nodes from the network side, though the questions "
      "reach its CPU; from the subscriber side, in the management VLAN, it answers both",
      tools=("tc",))
def pon_silence(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    vid = mgmt_vlan(ctx)
    found = re.search(r"inet (\d+(?:\.\d+){3})/", ctx.stick.run("ip -4 -o addr show dev mgmt").out)
    if vid is None or not found:
        return failed("the stick has no management VLAN interface mgmt with an address")
    address = found.group(1)
    mac = ctx.stick.text("cat /sys/class/net/mgmt/address")
    gems = ctx.stick.text("cd /sys/class/net && ls -d gem* 2>/dev/null; true").split()
    if not gems:
        return failed("no GEM interface on the stick")
    pon_side = [dev for dev in ("pon", "cpu", "omcibr0", *gems)
                if ctx.stick.run(f"[ -e /sys/class/net/{dev} ]").ok]
    ipv6_on = [dev for dev in pon_side
               if ctx.stick.text(f"cat /proc/sys/net/ipv6/conf/{dev}/disable_ipv6") != "1"]
    with_address = sorted({line.split()[1] for line in
                           ctx.stick.text("ip -6 -o addr show; true").splitlines()
                           if len(line.split()) > 1 and line.split()[1] in pon_side})
    arp_ignore = ctx.stick.text("cat /proc/sys/net/ipv4/conf/all/arp_ignore")
    rule = "tc filter {} dev {} ingress pref {} {}"
    counters = (("protocol ipv6 flower skip_hw dst_ip ff02::1 ip_proto icmpv6 type 128 "
                 "action pass"),
                f"protocol arp flower skip_hw arp_tip {address} action pass")
    added = clsact(ctx, gems)
    try:
        for gem in gems:
            for k, text in enumerate(counters):
                reply = ctx.stick.run(rule.format("add", gem, SILENCE_PREF + k, text))
                if not reply.ok:
                    return failed(f"tc refused {text!r} on {gem}: "
                                  f"{(reply.err or reply.out).strip()}")
        network = silence_probe(ctx, ctx.settings.olt_port, 0, address, mac)
        shown = ctx.stick.text("; ".join(f"tc -s filter show dev {gem} ingress pref "
                                         f"{SILENCE_PREF + k}"
                                         for gem in gems for k in range(len(counters))))
    finally:
        ctx.stick.run("; ".join(
            [rule.format("del", gem, SILENCE_PREF + k, "2>/dev/null")
             for gem in gems for k in range(len(counters))] +
            [f"tc qdisc del dev {dev} clsact" for dev in added] + ["true"]))
    host = silence_probe(ctx, ctx.settings.subscriber_port, vid, address, mac)
    took = sum(int(n) for n in re.findall(r"Sent \d+ bytes (\d+) pkt", shown))
    ok = (not ipv6_on and not with_address and arp_ignore == "1" and
          not any(network.values()) and took >= SILENCE_COUNT and
          host["arp"] >= SILENCE_COUNT * 0.9 and host["echo6"] >= SILENCE_COUNT * 0.9)
    return verdict(ok, f"IPv6 on {', '.join(ipv6_on) or 'none'} of {', '.join(pon_side)}; "
                       f"IPv6 addresses on {', '.join(with_address) or 'none'}; arp_ignore "
                       f"{arp_ignore}; {SILENCE_COUNT} ARP requests and IPv6 pings from the "
                       f"network side: {took} reached the CPU, answered {network['arp']} ARP, "
                       f"{network['echo6']} echo, {network['ns']} solicitation; from the "
                       f"subscriber side in VLAN {vid}: {host['arp']} ARP, {host['echo6']} echo")


# The ACL case: frames of the probe's EtherType from the network side, against
# drop and trap rules on the PON port at two tc priorities of its own. The CPU
# gets a frame from the PON on the interface of its GEM port, so a software
# filter on each GEM interface counts the frames that reach it.
ACL_COUNT = 200
ACL_PREFS = (49010, 49020)
ACL_COUNTER = 49030


def acl_round(ctx: Context, gems: list[str],
              rules: list[tuple[int, str]]) -> tuple[int, int, dict[int, int]]:
    """Install rules, (priority, action) pairs in the order given, on the PON
    port's ingress for EtherType 0x88b5; send ACL_COUNT such frames from the
    network side to the subscriber side; remove the rules. Returns how many
    arrived on the subscriber side, how many reached the CPU and the frames
    each rule's hit count, as tc reports it, says it decided for."""
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
        hits = {}
        for pref, _ in rules:
            text = ctx.stick.text(f"tc -s filter show dev pon ingress pref {pref}")
            found = re.search(r"Sent \d+ bytes (\d+) pkt", text)
            hits[pref] = int(found.group(1)) if found else -1
    finally:
        ctx.stick.run("; ".join(
            [f"tc filter del dev pon ingress pref {pref} 2>/dev/null" for pref, _ in rules] +
            [f"tc filter del dev {gem} ingress pref {ACL_COUNTER} 2>/dev/null" for gem in gems] +
            ["true"]))
    counts = re.findall(r"Sent \d+ bytes (\d+) pkt", shown)
    if len(counts) != len(gems):
        raise ValueError(f"no count from a software filter: {shown!r}")
    return arrived, sum(int(n) for n in counts), hits


@case("data.acl", "Data path",
      "ACL rules on the PON port drop frames from the network side or trap them to the CPU, "
      "the rule of the lower tc priority deciding whichever was added first, and tc counts "
      "the frames for the rule that decided", tools=("tc",))
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
            arrived, took, hits = acl_round(ctx, gems, rules)
            counted = all((hits[pref] >= every) == (pref == first) and
                          (hits[pref] >= every or hits[pref] <= none) for pref, _ in rules)
            ok = ok and good(arrived, took) and counted
            shown = "".join(f", {hits[pref]} counted at {pref}" for pref, _ in rules)
            seen.append(f"{name}: {arrived} arrived, {took} to the CPU{shown}")
    finally:
        ctx.stick.run("; ".join([f"tc qdisc del dev {dev} clsact" for dev in added] + ["true"]))
    return verdict(ok, f"{ACL_COUNT} frames each; " + "; ".join(seen))


# The mirror case: what the PON port receives and what it sends, copied to the
# SFP port at a tc priority of the case's own. The frames from the network side
# go to the stick's own address, which the switch sends to the CPU alone, and
# those from the subscriber side to the network-side port's, which leave by the
# PON: without the mirror, none of them reaches the subscriber side.
MIRROR_COUNT = 2000
MIRROR_PREF = 49040

# Run by python3 on the bench host with SRC RECV DST COUNT: COUNT frames of
# EtherType 0x88b5 from port SRC to the address DST, 0.2 ms apart, below the
# upstream rate. It prints how many frames from SRC's address port RECV
# received meanwhile, in promiscuous mode, leaving out what RECV sent itself.
MIRRORED = """\
import socket, struct, sys, threading, time
src, recv, dst, count = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
mac = lambda text: bytes.fromhex(text.replace(":", ""))
own = mac(open("/sys/class/net/%s/address" % src).read().strip())
SOL_PACKET, PACKET_ADD_MEMBERSHIP, PACKET_MR_PROMISC = 263, 1, 1
rx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x88b5))
rx.bind((recv, 0))
rx.setsockopt(SOL_PACKET, PACKET_ADD_MEMBERSHIP,
              struct.pack("iHH8s", socket.if_nametoindex(recv), PACKET_MR_PROMISC, 0, bytes(8)))
rx.settimeout(0.5)
seen, stop = [0], []
def receive():
    while not stop:
        try:
            data, addr = rx.recvfrom(2048)
        except socket.timeout:
            continue
        if addr[2] != socket.PACKET_OUTGOING and data[6:12] == own:
            seen[0] += 1
thread = threading.Thread(target=receive)
thread.start()
tx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
tx.bind((src, 0))
frame = mac(dst) + own + b"\\x88\\xb5" + bytes(46)
for _ in range(count):
    tx.send(frame)
    time.sleep(0.0002)
time.sleep(1.5)
stop.append(1)
thread.join()
print(seen[0])
"""


def mirrored(ctx: Context, into: str, dst: str) -> int:
    """Send MIRROR_COUNT frames into the bench port into, to the address dst;
    return how many of them the subscriber-side port received."""
    command = sudo(ctx.settings, " ".join(shlex.quote(a) for a in (
        "python3", "-", into, ctx.settings.subscriber_port, dst, str(MIRROR_COUNT))))
    reply = ctx.bench.run(command, stdin=MIRRORED, timeout=60)
    try:
        return int(reply.out.strip().splitlines()[-1])
    except (IndexError, ValueError):
        raise ValueError(f"the probe did not run: {(reply.err or reply.out).strip()!r}")


@case("data.mirror", "Data path",
      "A matchall mirror copies to the host side what the PON port receives and what it "
      "sends, both at once, and nothing once it is removed", tools=("tc",))
def mirror(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    prime(ctx)
    s = ctx.settings
    own = ctx.stick.text("cat /sys/class/net/sfp/address")
    net = ctx.bench.text(f"cat /sys/class/net/{shlex.quote(s.olt_port)}/address")
    ways = ("ingress", "egress")
    added = clsact(ctx, ["pon"])
    try:
        for way in ways:
            reply = ctx.stick.run(f"tc filter add dev pon {way} pref {MIRROR_PREF} matchall "
                                  "skip_sw action mirred egress mirror dev sfp")
            if not reply.ok:
                raise ValueError(f"tc refused the {way} mirror: "
                                 f"{(reply.err or reply.out).strip()}")
        down, up = mirrored(ctx, s.olt_port, own), mirrored(ctx, s.subscriber_port, net)
    finally:
        ctx.stick.run("; ".join(
            [f"tc filter del dev pon {way} pref {MIRROR_PREF} 2>/dev/null" for way in ways] +
            [f"tc qdisc del dev {dev} clsact" for dev in added] + ["true"]))
    down_after, up_after = mirrored(ctx, s.olt_port, own), mirrored(ctx, s.subscriber_port, net)
    every, none = MIRROR_COUNT * 0.99, MIRROR_COUNT * 0.01
    ok = down >= every and up >= every and down_after <= none and up_after <= none
    return verdict(ok, f"{MIRROR_COUNT} frames each way; mirrored, {down} of those from the "
                       f"network side to this device and {up} of those from the host side "
                       f"reached the host side; after, {down_after} and {up_after}")


# The address-table case: an address of the documentation range (RFC 7042,
# 2.1.2) that no line carries, made static
# through the bridge.
FDB_STATIC = "00:00:5e:00:53:10"
FDB_WAIT = 3.0         # seconds the switch has to follow the bridge


def fdb_entry(ctx: Context, dev: str, mac: str, static: bool) -> str:
    """The switch's own entry for mac on dev, static or learned, as bridge fdb
    lists it, or ''."""
    for line in ctx.stick.text(f"bridge fdb show dev {dev}").splitlines():
        if (line.lower().startswith(mac.lower() + " ") and " self" in line and
                (" static" in line) == static):
            return line.strip()
    return ""


def within(check, limit: float = FDB_WAIT, pause: float = 0.25):
    """check() until it returns something true or limit seconds have passed;
    its last result."""
    waited = 0.0
    while not (result := check()) and waited < limit:
        time.sleep(pause)
        waited += pause
    return result


@case("data.fdb", "Data path",
      "The switch learns each bench port's address behind the port it reaches it by, and a "
      "static address given to the bridge enters its table and leaves it again", tools=("bridge",))
def fdb(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    prime(ctx)
    s = ctx.settings
    sub = ctx.bench.text(f"cat /sys/class/net/{shlex.quote(s.subscriber_port)}/address")
    net = ctx.bench.text(f"cat /sys/class/net/{shlex.quote(s.olt_port)}/address")
    on_sfp = fdb_entry(ctx, "sfp", sub, static=False)
    on_pon = fdb_entry(ctx, "pon", net, static=False)
    ctx.stick.run(f"bridge fdb add {FDB_STATIC} dev sfp master static")
    try:
        added = within(lambda: fdb_entry(ctx, "sfp", FDB_STATIC, static=True))
    finally:
        ctx.stick.run(f"bridge fdb del {FDB_STATIC} dev sfp master static")
    gone = within(lambda: not fdb_entry(ctx, "sfp", FDB_STATIC, static=True))
    ok = bool(on_sfp and on_pon and added and gone)
    return verdict(ok, f"host side on sfp: {on_sfp or 'not learned'}; network side on pon: "
                       f"{on_pon or 'not learned'}; static: {added or 'never there'}, "
                       f"{'then gone' if gone else 'still there'}")


# The shaper case: a tbf on the SFP port, whose egress the switch shapes, under
# the frames from the network side. The rate is taken over the time the sender
# ran, while the frames the switch still holds when it stops arrive after it:
# a longer burst makes them a smaller part of the count.
SHAPER_MBIT = 200
SHAPER_TOLERANCE = 0.05
SHAPER_COUNT = 3 * COUNT
FULL_MBIT = 900        # the downstream rate without a shaper is about 984 Mb/s


@case("data.shaper", "Data path",
      "A tbf on the SFP port holds the frames from the network side to its rate in hardware, "
      "and its removal gives the full rate back", tools=("tc",))
def shaper(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    s = ctx.settings
    reply = ctx.stick.run(f"tc qdisc replace dev sfp root handle 1: tbf rate {SHAPER_MBIT}mbit "
                          "burst 64k latency 50ms")
    if not reply.ok:
        return failed(f"tc refused the tbf: {(reply.err or reply.out).strip()}")
    try:
        sent, arrived, cpu, rate, _ = measure(ctx, s.olt_port, s.subscriber_port, "pon", "sfp",
                                              count=SHAPER_COUNT)
    finally:
        ctx.stick.run("tc qdisc del dev sfp root 2>/dev/null; true")
    sent2, arrived2, cpu2, rate2, _ = measure(ctx, s.olt_port, s.subscriber_port, "pon", "sfp")
    if min(sent, sent2) < COUNT // 100:
        return failed(f"the bench: {s.olt_port} sent {sent} and {sent2} of {COUNT} frames")
    ok = (abs(rate - SHAPER_MBIT) <= SHAPER_MBIT * SHAPER_TOLERANCE and rate2 >= FULL_MBIT and
          cpu < sent * 0.01 and cpu2 < sent2 * 0.01)
    return verdict(ok, f"tbf {SHAPER_MBIT} Mbit/s: {arrived} of {sent} frames arrived at "
                       f"{rate:.0f} Mb/s, CPU saw {cpu}; removed: {arrived2} of {sent2} at "
                       f"{rate2:.0f} Mb/s, CPU saw {cpu2}")


# The storm-control case: broadcasts from the host side towards the network
# side, STORM_COUNT at STORM_RATE frames a second, counted where they leave the
# PON. Under several police rules on the SFP port the one that tc applies
# first, the lowest priority value, must hold the class, through a replace of
# it too; a drop of the same frames is an ACL rule.
STORM_COUNT = 600
STORM_RATE = 300       # frames a second
STORM_SLACK = 40       # frames a meter lets through beyond its rate: its bucket

# Run by python3 on the bench host with SRC RECV DST COUNT RATE: COUNT frames
# of EtherType 0x88b5 to DST out of SRC at RATE frames a second, RECV put in
# promiscuous mode meanwhile. It prints how many of them RECV received.
STORMED = """\
import socket, struct, sys, threading, time
src, recv, dst, count, rate = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), float(sys.argv[5])
mac = lambda text: bytes.fromhex(text.replace(":", ""))
own = mac(open("/sys/class/net/%s/address" % src).read().strip())
SOL_PACKET, PACKET_ADD_MEMBERSHIP, PACKET_MR_PROMISC = 263, 1, 1
rx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x88b5))
rx.bind((recv, 0))
rx.setsockopt(SOL_PACKET, PACKET_ADD_MEMBERSHIP,
              struct.pack("iHH8s", socket.if_nametoindex(recv), PACKET_MR_PROMISC, 0, bytes(8)))
rx.settimeout(0.5)
seen, stop = [0], []
def receive():
    while not stop:
        try:
            data, addr = rx.recvfrom(2048)
        except socket.timeout:
            continue
        if addr[2] != socket.PACKET_OUTGOING and data[6:12] == own:
            seen[0] += 1
thread = threading.Thread(target=receive)
thread.start()
tx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
tx.bind((src, 0))
frame = mac(dst) + own + b"\\x88\\xb5" + bytes(46)
start = time.monotonic()
for i in range(count):
    tx.send(frame)
    wait = start + (i + 1) / rate - time.monotonic()
    if wait > 0:
        time.sleep(wait)
time.sleep(1.5)
stop.append(1)
thread.join()
print(seen[0])
"""


def storm_round(ctx: Context) -> int:
    """STORM_COUNT broadcasts from the host side; how many left the PON."""
    s = ctx.settings
    command = sudo(s, " ".join(shlex.quote(a) for a in (
        "python3", "-", s.subscriber_port, s.olt_port, "ff:ff:ff:ff:ff:ff", str(STORM_COUNT),
        str(STORM_RATE))))
    reply = ctx.bench.run(command, stdin=STORMED, timeout=60)
    try:
        return int(reply.out.strip().splitlines()[-1])
    except (IndexError, ValueError):
        raise ValueError(f"the probe did not run: {(reply.err or reply.out).strip()!r}")


@case("data.storm", "Data path",
      "Of several police rules on broadcast the one of the lowest priority value holds the "
      "class, through a replace of it too, and a drop of broadcast is an ACL rule that drops",
      tools=("tc",))
def storm(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    prime(ctx)
    rule = "tc filter {} dev sfp ingress pref {} handle 1 flower skip_sw dst_mac ff:ff:ff:ff:ff:ff "
    police = "action police pkts_rate {} pkts_burst 100 conform-exceed drop"

    def add(verb, pref, action):
        reply = ctx.stick.run(rule.format(verb, pref) + action)
        if not reply.ok:
            raise ValueError(f"tc refused {action} at {pref}: {(reply.err or reply.out).strip()}")

    seconds = STORM_COUNT / STORM_RATE
    added = clsact(ctx, ["sfp"])
    try:
        free = storm_round(ctx)
        add("add", 10, police.format(100))
        low = storm_round(ctx)
        add("add", 20, police.format(1000))
        kept = storm_round(ctx)
        add("replace", 10, police.format(200))
        replaced = storm_round(ctx)
        ctx.stick.run("tc filter del dev sfp ingress pref 10; tc filter del dev sfp ingress pref 20")
        add("add", 30, "action drop")
        dropped = storm_round(ctx)
    finally:
        ctx.stick.run("; ".join(
            [f"tc filter del dev sfp ingress pref {pref} 2>/dev/null" for pref in (10, 20, 30)] +
            [f"tc qdisc del dev {dev} clsact" for dev in added] + ["true"]))
    after = storm_round(ctx)

    def near(count, rate):
        return rate * seconds <= count <= rate * seconds + STORM_SLACK

    ok = (free >= STORM_COUNT * 0.99 and near(low, 100) and near(kept, 100) and
          near(replaced, 200) and dropped <= STORM_COUNT * 0.01 and after >= STORM_COUNT * 0.99)
    return verdict(ok, f"{STORM_COUNT} broadcasts at {STORM_RATE}/s: {free} passed with no rule, "
                       f"{low} under pref 10 at 100/s, {kept} with pref 20 at 1000/s added, "
                       f"{replaced} with pref 10 replaced at 200/s; dropped by the ACL, "
                       f"{dropped} passed; {after} after")

# The PCP rewrite case: frames tagged REWR_VID with PCP 0 from the host side,
# which the PON port forwards to the OLT. A rewrite entry on pon for
# priority 0 must give them PCP REWR_PCP, and only them: frames of PCP 3, which
# have no entry, keep their PCP, and with the entry gone PCP 0 stays 0.
REWR_VID = 100
REWR_PCP = 5
REWR_COUNT = 100


@case("data.pcp-rewrite", "Data path",
      "A DCB rewrite entry on pon gives the frames of its priority that it sends the PCP "
      "of the entry, leaves the PCP of other priorities as it was, and stops with the entry",
      tools=("dcb",))
def pcp_rewrite(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    prime(ctx)
    s = ctx.settings
    entry = f"prio-pcp 0:{REWR_PCP}nd"

    def up(pcp):
        return tagged(ctx, s.subscriber_port, s.olt_port, REWR_VID, REWR_COUNT, pcp)

    reply = ctx.stick.run(f"dcb rewr add dev pon {entry}")
    if not reply.ok:
        return failed(f"dcb refused the entry: {(reply.err or reply.out).strip()}")
    try:
        rewritten, other = up(0), up(3)
    finally:
        ctx.stick.run(f"dcb rewr del dev pon {entry}")
    after = up(0)
    want = REWR_COUNT * 0.99
    ok = (rewritten.get(f"{REWR_VID}/{REWR_PCP}", 0) >= want and
          other.get(f"{REWR_VID}/3", 0) >= want and after.get(f"{REWR_VID}/0", 0) >= want)
    return verdict(ok, f"{REWR_COUNT} frames tagged {REWR_VID} each; under {entry}, PCP 0: "
                       f"{tags(rewritten)}, PCP 3: {tags(other)}; without it, PCP 0: "
                       f"{tags(after)}")

# The CPU-limit case: a flood from the host side to the stick's own address,
# which the switch hands the CPU, first under the limit that devlink's
# cpu_rx_pps holds and then, for a shorter time, with the limit lifted, which
# shows that the limit is what holds the flood. A shell loop on the stick times
# what the flood leaves of the CPU. It starts before the flood, in the
# background, because the flood also fills the bench host's queue towards the
# stick, and a command sent during it would arrive only after it.
CPU_FLOOD = 10         # seconds of flood under the limit
CPU_LIFTED = 3         # seconds of flood with the limit lifted
CPU_SLACK = 0.1        # how far the CPU's share may stray from the limit
CPU_SHARE = 0.5        # the share of its time the CPU must keep
CPU_PROBE = "/tmp/devtest-cpu-loop"
CPU_TIMED = ('t0=$(cut -d" " -f1 /proc/uptime); i=0; while [ $i -lt 20000 ]; do '
             'i=$((i+1)); done; t1=$(cut -d" " -f1 /proc/uptime); echo "$t0 $t1"')

# Run by python3 on the bench host with PORT DST SECONDS: minimal frames to DST
# out of PORT, as fast as one socket sends them, for SECONDS. It prints how
# many it sent.
FLOODER = """\
import socket, sys, time
port, dst, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
src = open("/sys/class/net/%s/address" % port).read().strip()
mac = lambda text: bytes.fromhex(text.replace(":", ""))
frame = mac(dst) + mac(src) + b"\\x88\\xb5" + bytes(46)
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
s.bind((port, 0))
sent, end = 0, time.monotonic() + seconds
while time.monotonic() < end:
    for _ in range(256):
        try:
            s.send(frame)
            sent += 1
        except OSError:
            pass
print(sent)
"""


def flood(ctx: Context, dst: str, seconds: float) -> int:
    """Flood dst from the subscriber-side port for seconds; return how many
    frames were sent."""
    command = sudo(ctx.settings, " ".join(shlex.quote(a) for a in (
        "python3", "-", ctx.settings.subscriber_port, dst, str(seconds))))
    reply = ctx.bench.run(command, stdin=FLOODER, timeout=seconds + 60)
    try:
        return int(reply.out.strip().splitlines()[-1])
    except (IndexError, ValueError):
        raise ValueError(f"the flooder did not run: {(reply.err or reply.out).strip()!r}")


def loop_seconds(text: str) -> float:
    """The seconds between the two uptimes that CPU_TIMED prints."""
    start, end = map(float, text.split())
    return end - start


def omci_counts(ctx: Context) -> tuple[int, int]:
    """The OMCI messages that the PON MAC received and those that reached the
    OMCI port, so far."""
    mac, port = ctx.stick.text(
        "mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug; "
        "sed -n 's/.*omci rx *\\([0-9]*\\) pkts.*/\\1/p' /sys/kernel/debug/rtl9602c/ponmac; "
        "cat /sys/class/pon/pon0omci0/statistics/rx_messages").split()
    return int(mac), int(port)


@case("data.cpu-limit", "Data path",
      "A flood to the stick's own address reaches its CPU no faster than cpu_rx_pps says, "
      "leaves the CPU at least half its time and the OMCI messages whole, and goes through "
      "once the limit is lifted", tools=("devlink",))
def cpu_limit(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    prime(ctx)
    param = "devlink dev param {} " + ctx.stick.text("devlink dev show | head -n 1") + \
        " name cpu_rx_pps"
    limit = int(re.search(r"value (\d+)", ctx.stick.text(param.format("show"))).group(1))
    if not limit:
        return failed("cpu_rx_pps is 0: nothing limits the frames handed to the CPU")
    own = ctx.stick.text("cat /sys/class/net/sfp/address")
    cpu = conduit(ctx)
    rest = loop_seconds(ctx.stick.text(CPU_TIMED))
    ctx.stick.run(f"rm -f {CPU_PROBE}; (sleep 3; {CPU_TIMED} > {CPU_PROBE}) "
                  "</dev/null >/dev/null 2>&1 &")
    omci = omci_counts(ctx)
    before = stick_counter(ctx, cpu, "rx_packets")
    sent = flood(ctx, own, CPU_FLOOD)
    took = stick_counter(ctx, cpu, "rx_packets") - before
    omci = tuple(b - a for a, b in zip(omci, omci_counts(ctx)))
    probe = within(lambda: ctx.stick.run(f"cat {CPU_PROBE} 2>/dev/null").out.strip(), limit=30)
    if not probe:
        return failed("the loop on the stick did not finish")
    loaded = loop_seconds(probe)
    try:
        ctx.stick.text(param.format("set") + " value 0 cmode runtime")
        before = stick_counter(ctx, cpu, "rx_packets")
        flood(ctx, own, CPU_LIFTED)
        lifted = stick_counter(ctx, cpu, "rx_packets") - before
    finally:
        ctx.stick.run(param.format("set") + f" value {limit} cmode runtime")
    if sent < 4 * limit * CPU_FLOOD:
        return failed(f"the bench: {sent} frames in {CPU_FLOOD} s, too few to test a limit "
                      f"of {limit}/s")
    allowed = limit * CPU_FLOOD
    ok = (abs(took - allowed) <= allowed * CPU_SLACK and loaded * CPU_SHARE <= rest and
          omci[1] == omci[0] and lifted >= 2 * limit * CPU_LIFTED)
    return verdict(ok, f"cpu_rx_pps {limit}: of {sent} frames in {CPU_FLOOD} s the CPU took "
                       f"{took}; a loop of {rest:.2f} s took {loaded:.2f} s; OMCI {omci[1]} of "
                       f"{omci[0]} delivered; lifted: {lifted / CPU_LIFTED:.0f} frames/s")

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
      "host asked for does not, the link-local groups pass, and every report reaches the network",
      tools=("bridge",))
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
        snooping = "/sys/class/net/omcibr0/bridge/multicast_snooping"
        ctx.stick.run(f"bridge link set dev sfp fastleave off; echo 0 > {snooping}; "
                      f"echo 1 > {snooping}")
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
