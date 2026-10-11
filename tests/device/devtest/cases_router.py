# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Router mode: the stick's own WAN and LAN, set through the status daemon.

The cases put the stick in router mode through the daemon's settings, as a
person does on its page, measure, and put it back in bridge mode. They run
only when asked for (--router), since they change the stick's mode and add two
network namespaces to the bench host for as long as they run:

    devtest-lan  a macvlan on the subscriber-side port: a LAN host
    devtest-wan  a macvlan on the network-side port: the far end of the WAN

Both use the documentation ranges, so they take nothing from the bench's own
addressing, and fixed MAC addresses, so that the stick, which keeps a
neighbour's address for a while, finds the same hosts on every run. Before any
traffic, each host must reach the stick: the far end by an ARP request of its
own, which the OLT brings to the stick by the multicast GEM port. The far end
answers UDP itself, inside its namespace, so the bench host's own packet
filter is not involved.

PPPoE needs a concentrator on the network side; the bench names the file of its
secrets (--pppoe-secrets), and the case hands the file's first entry to the
daemon from the bench host itself, so that the secret is never on the command
line or in the record.
"""

from __future__ import annotations

import json
import shlex
import time

from .access import sudo
from .core import Context, case, failed, passed, skipped, verdict

LAN = "198.51.100.1/24"          # the stick's LAN address
LAN_HOST = "198.51.100.50/24"    # the bench's LAN host, outside the DHCP range
LAN_POOL = ("198.51.100.100", "198.51.100.199")
WAN = "203.0.113.2/24"           # the stick's static WAN address
WAN_PEER = "203.0.113.1"         # the far end of the WAN
LAN_MAC = "00:00:5e:00:53:01"    # the LAN host's MAC address
WAN_MAC = "00:00:5e:00:53:02"    # the far end's
PORT = 9                         # UDP discard, answered by the far end
SOURCE_PORT = 40009              # the LAN host's, the same in both bursts
WARM = 1000                      # datagrams that start the flow
FLOW = 20000                     # datagrams in the measured burst of it
RATE = 4000                      # of them a second

# The far end: a UDP responder in devtest-wan that answers every datagram and
# writes how many it took and from which sources when its time is up. It is
# started in the background by a sudo that may run commands on a terminal of
# their own, whose hangup at the command's end would kill it: it ignores the
# hangup, since it ends by itself. A datagram it cannot answer, from a source
# it has no route back to, is still counted.
RESPONDER = """\
import json, signal, socket, sys, time
signal.signal(signal.SIGHUP, signal.SIG_IGN)
port, seconds, out = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", port))
s.settimeout(0.2)
end = time.monotonic() + seconds
got, sources = 0, set()
while time.monotonic() < end:
    try:
        data, peer = s.recvfrom(2048)
    except socket.timeout:
        continue
    got += 1
    sources.add(peer[0])
    try:
        s.sendto(data, peer)
    except OSError:
        pass
json.dump({"received": got, "sources": sorted(sources)}, open(out, "w"))
"""

# The LAN host: datagrams of one flow at a steady rate, counting the answers.
SENDER = """\
import json, socket, sys, time
peer, port, count, rate = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", int(sys.argv[5])))
s.setblocking(False)
answers = 0
start = time.monotonic()
for i in range(count):
    while time.monotonic() < start + i / rate:
        try:
            s.recv(2048)
            answers += 1
        except BlockingIOError:
            pass
    s.sendto(i.to_bytes(4, "big") + bytes(60), (peer, port))
end = time.monotonic() + 2
while time.monotonic() < end:
    try:
        s.recv(2048)
        answers += 1
    except BlockingIOError:
        time.sleep(0.01)
print(json.dumps({"sent": count, "answers": answers}))
"""


def bench(ctx: Context, command: str, *, stdin: str | None = None, timeout: float = 60):
    return ctx.bench.run(sudo(ctx.settings, command), stdin=stdin, timeout=timeout)


# Three cases measure router mode as it was, with NAT: masquerade, the MSS
# clamp and the flows that the switch carried for it. The stick translates no
# address any more and passes the router behind it the WAN's own address
# instead, which router.passthrough measures; the NAT cases are held.
HELD = ("router mode translates no address now; this case measures NAT, and "
        "router.passthrough the address handed on")


def needs(ctx: Context, *, nat: bool = False):
    s = ctx.settings
    if nat and HELD:
        return skipped(HELD)
    if not s.router:
        return skipped("router mode not asked for (--router)")
    if not (s.olt_port and s.subscriber_port and s.pond_token):
        return skipped("the settings name no ports or no token")
    return None


def set_mode(ctx: Context, lines: str, *, body_from: str = "") -> tuple[int, str]:
    """POST settings to the daemon from the bench host; body_from is a command
    on the bench whose output goes first in the body."""
    url = shlex.quote(f"https://{ctx.settings.stick}/api/set/settings")
    head = shlex.quote(f"Authorization: Bearer {ctx.settings.pond_token}")
    first = f"{body_from}; " if body_from else ""
    command = (f"{{ {first}printf '%s' {shlex.quote(lines)}; }} | "
               f"curl -s -k -m 15 -w '\\n%{{http_code}}' -H {head} --data-binary @- {url}")
    reply = ctx.bench.run(command if not body_from else sudo(ctx.settings, command),
                          timeout=40)
    body, _, code = reply.out.rpartition("\n")
    return int(code or 0), body


def status(ctx: Context) -> str:
    return ctx.stick.run("onu-apply status").out


def wait_for(ctx: Context, test, limit: float) -> float | None:
    """Seconds until test(status) holds, or None after limit seconds."""
    start = time.monotonic()
    while time.monotonic() - start < limit:
        if test(status(ctx)):
            return time.monotonic() - start
        time.sleep(2)
    return None


def namespaces_up(ctx: Context) -> None:
    """The two namespaces, new. A namespace that is deleted goes away a moment
    later, its macvlan with it, and a new macvlan with the same address is
    refused until then: the creation is tried again, up to five times."""
    s = ctx.settings
    script = "; ".join([
        "set -e",
        "ip netns add devtest-lan",
        "ip netns add devtest-wan",
        f"ip link add devtest-lan0 link {shlex.quote(s.subscriber_port)} address {LAN_MAC} "
        "type macvlan mode bridge",
        f"ip link add devtest-wan0 link {shlex.quote(s.olt_port)} address {WAN_MAC} "
        "type macvlan mode bridge",
        "ip link set devtest-lan0 netns devtest-lan",
        "ip link set devtest-wan0 netns devtest-wan",
        f"ip -n devtest-lan address add {LAN_HOST} dev devtest-lan0",
        f"ip -n devtest-wan address add {WAN_PEER}/24 dev devtest-wan0",
        "ip -n devtest-lan link set devtest-lan0 up",
        "ip -n devtest-wan link set devtest-wan0 up",
        f"ip -n devtest-lan route add default via {LAN.split('/')[0]}",
    ])
    for attempt in range(5):
        namespaces_down(ctx)
        if attempt:
            time.sleep(1)
        reply = bench(ctx, script)
        if reply.ok:
            break
    else:
        raise RuntimeError(f"the bench's namespaces: {reply.err.strip() or reply.out.strip()}")
    ctx.memo["namespaces"] = True
    # Each host must reach the stick, and the stick each host, before any
    # traffic: a new macvlan takes a moment, and the far end's ping starts
    # with an ARP request that comes to the stick by the multicast GEM port.
    wan, lan = WAN.split("/")[0], LAN.split("/")[0]
    ready = bench(ctx, "for i in 1 2 3 4 5 6 7 8 9 10; do "
                       f"ip netns exec devtest-wan ping -c 1 -W 1 {wan} >/dev/null && "
                       f"ip netns exec devtest-lan ping -c 1 -W 1 {lan} >/dev/null && "
                       "exit 0; sleep 1; done; exit 1", timeout=40)
    if not ready.ok:
        raise RuntimeError("the bench's namespaces do not reach the stick")


def namespaces_down(ctx: Context) -> None:
    """The namespaces gone, and an interface that a failed creation left
    behind outside them."""
    bench(ctx, "ip netns del devtest-lan 2>/dev/null; ip netns del devtest-wan 2>/dev/null; "
               "ip link del devtest-lan0 2>/dev/null; ip link del devtest-wan0 2>/dev/null; "
               f"ip link del {PASS_HOST} 2>/dev/null; true")
    ctx.memo.pop("namespaces", None)


@case("router.static", "Router mode",
      "With a static WAN, a LAN host's UDP flow leaves translated to the WAN address and "
      "the switch carries it")
def static(ctx: Context):
    if (skip := needs(ctx, nat=True)):
        return skip
    code, body = set_mode(ctx, "MODE=router\nWAN_PROTO=static\nWAN_VLAN=\n"
                               f"WAN_ADDRESS={WAN}\nWAN_GATEWAY={WAN_PEER}\n"
                               f"LAN_ADDRESS={LAN}\nLAN_DHCP=on\n"
                               f"LAN_DHCP_START={LAN_POOL[0]}\nLAN_DHCP_END={LAN_POOL[1]}\n")
    if code != 200:
        return failed(f"the daemon refused the settings: HTTP {code} {body}")
    took = wait_for(ctx, lambda st: "mode router" in st and "out by gem" in st, 90)
    if took is None:
        return failed(f"no WAN within 90 s: {status(ctx).strip()!r}")
    namespaces_up(ctx)
    cpu = ctx.stick.text("for i in /sys/class/net/*; do [ -e $i/dsa/tagging ] && "
                         "echo ${i##*/}; done; true")
    seconds = (WARM + FLOW) / RATE + 20
    bench(ctx, "cat > /tmp/devtest-far.py; "
               f"ip netns exec devtest-wan python3 /tmp/devtest-far.py {PORT} {seconds} "
               "/tmp/devtest-far.json > /dev/null 2>&1 & sleep 1", stdin=RESPONDER)
    started = time.monotonic()

    def send(count):
        before = int(ctx.stick.text(f"cat /sys/class/net/{cpu}/statistics/rx_packets"))
        reply = bench(ctx, f"ip netns exec devtest-lan python3 - {WAN_PEER} {PORT} {count} "
                           f"{RATE} {SOURCE_PORT}", stdin=SENDER, timeout=seconds + 30)
        after = int(ctx.stick.text(f"cat /sys/class/net/{cpu}/statistics/rx_packets"))
        return json.loads(reply.out.strip().splitlines()[-1]), after - before

    # The first datagrams cross the CPU until the flow is in the switch; the
    # measured burst is the same flow afterwards.
    _, warm_seen = send(WARM)
    time.sleep(2)
    lan, seen = send(FLOW)
    time.sleep(max(0.0, seconds - (time.monotonic() - started)) + 1)
    reply = bench(ctx, "cat /tmp/devtest-far.json; rm -f /tmp/devtest-far.json /tmp/devtest-far.py")
    far = json.loads(reply.out.strip() or '{"received": 0, "sources": []}')
    offload = ctx.stick.run("grep -c HW_OFFLOAD /proc/net/nf_conntrack").out.strip() or "?"
    translated = far["sources"] == [WAN.split("/")[0]]
    total = WARM + FLOW
    ok = (far["received"] >= total * 0.99 and lan["answers"] >= FLOW * 0.99 and translated
          and seen < FLOW * 0.01)
    return verdict(ok, f"WAN up in {took:.0f} s; {WARM} datagrams to start the flow, "
                       f"{warm_seen} through the CPU; then {lan['sent']} sent and "
                       f"{lan['answers']} answered, {seen} through the CPU; "
                       f"{far['received']} of {total} arrived from "
                       f"{', '.join(far['sources']) or 'nobody'}; "
                       f"{offload} flow(s) in the switch")


# The TCP case: a connection from the LAN host to the far end of the WAN that
# carries TCP_BYTES, which the far end reads to the end and counts.
TCP_PORT = 5001
TCP_BYTES = 100 * 1000 * 1000

# The far end of the TCP case: one connection, read to its end, with how many
# bytes came and from which source written when it closes or its time is up.
TCP_SINK = """\
import json, signal, socket, sys, time
signal.signal(signal.SIGHUP, signal.SIG_IGN)
port, seconds, out = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", port))
s.listen(1)
s.settimeout(seconds)
got, source = 0, ""
try:
    conn, peer = s.accept()
    source = peer[0]
    conn.settimeout(seconds)
    while True:
        data = conn.recv(1 << 16)
        if not data:
            break
        got += len(data)
except OSError:
    pass
json.dump({"received": got, "source": source}, open(out, "w"))
"""

# The LAN host of the TCP case: TCP_BYTES to the far end in two halves, the
# connection held idle for TCP_HOLD seconds between them, so that the stick's
# connection table can be read while the connection is up; a closed connection
# leaves the switch at once. It writes how many bytes it sent and how long the
# halves took, without the hold. Started in the background as the far end is.
TCP_HOLD = 4
TCP_SOURCE = """\
import json, signal, socket, sys, time
signal.signal(signal.SIGHUP, signal.SIG_IGN)
peer, port, total, hold, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
result = {"sent": 0, "seconds": 0.0}
try:
    s = socket.create_connection((peer, port), timeout=30)
    chunk = bytes(1 << 16)
    for half in (total // 2, total - total // 2):
        start, left = time.monotonic(), half
        while left:
            left -= s.send(chunk[:min(len(chunk), left)])
        result["sent"] += half
        result["seconds"] += time.monotonic() - start
        if half == total // 2:
            time.sleep(hold)
    s.shutdown(socket.SHUT_WR)
    s.recv(1)
except OSError as exc:
    result["error"] = str(exc)
json.dump(result, open(out, "w"))
"""


@case("router.tcp", "Router mode",
      "With a static WAN, a LAN host's TCP connection leaves translated to the WAN address "
      "and the switch carries it")
def tcp(ctx: Context):
    if (skip := needs(ctx, nat=True)):
        return skip
    if "mode router" not in status(ctx) or not ctx.memo.get("namespaces"):
        return failed("router mode with a static WAN is not up; router.static runs first")
    cpu = ctx.stick.text("for i in /sys/class/net/*; do [ -e $i/dsa/tagging ] && "
                         "echo ${i##*/}; done; true")
    seconds = 60
    bench(ctx, "cat > /tmp/devtest-sink.py; "
               f"ip netns exec devtest-wan python3 /tmp/devtest-sink.py {TCP_PORT} {seconds} "
               "/tmp/devtest-sink.json > /dev/null 2>&1 & sleep 1", stdin=TCP_SINK)
    before = int(ctx.stick.text(f"cat /sys/class/net/{cpu}/statistics/rx_packets"))
    bench(ctx, "cat > /tmp/devtest-source.py; "
               f"ip netns exec devtest-lan python3 /tmp/devtest-source.py {WAN_PEER} {TCP_PORT} "
               f"{TCP_BYTES} {TCP_HOLD} /tmp/devtest-source.json > /dev/null 2>&1 &",
          stdin=TCP_SOURCE)
    # Read the connection table during the hold, while the connection is up.
    time.sleep(TCP_HOLD / 2 + 1)
    offload = ctx.stick.run("grep 'dport=%d' /proc/net/nf_conntrack | grep -c HW_OFFLOAD"
                            % TCP_PORT).out.strip() or "0"
    lan = far = ""
    for _ in range(seconds):
        lan = bench(ctx, "cat /tmp/devtest-source.json 2>/dev/null").out.strip()
        far = bench(ctx, "cat /tmp/devtest-sink.json 2>/dev/null").out.strip()
        if lan and far:
            break
        time.sleep(1)
    after = int(ctx.stick.text(f"cat /sys/class/net/{cpu}/statistics/rx_packets"))
    bench(ctx, "rm -f /tmp/devtest-sink.json /tmp/devtest-sink.py "
               "/tmp/devtest-source.json /tmp/devtest-source.py")
    if not lan:
        return failed("the LAN host's connection did not end")
    lan = json.loads(lan)
    if "error" in lan:
        return failed(f"the LAN host's connection failed: {lan['error']}")
    far = json.loads(far or '{"received": 0, "source": ""}')
    seen = after - before
    segments = TCP_BYTES // 1448
    translated = far["source"] == WAN.split("/")[0]
    ok = (far["received"] == TCP_BYTES and translated and seen < segments * 0.01
          and offload != "0")
    rate = far["received"] * 8 / lan["seconds"] / 1e6 if lan["seconds"] else 0
    return verdict(ok, f"{lan['sent']} bytes sent, {far['received']} received from "
                       f"{far['source'] or 'nobody'} at {rate:.0f} Mb/s; {seen} frames "
                       f"through the CPU against some {segments} segments; "
                       f"{offload} flow(s) in the switch")


# The MSS probe of the PPPoE case, run by python3 on the bench host with the
# bench's PPP interface of the session and a port: it watches that interface
# for TCP SYNs to the port while the LAN host opens a connection to the
# interface's own address, and prints the MSS option of every SYN as JSON. A
# PPP interface carries IP packets without a link header.
MSS_PROBE = """\
import json, socket, struct, subprocess, sys, threading, time
iface, port = sys.argv[1], int(sys.argv[2])
addr = subprocess.run(["ip", "-4", "-o", "addr", "show", "dev", iface], capture_output=True,
                      text=True).stdout.split()[3].split("/")[0]
rx = socket.socket(socket.AF_PACKET, socket.SOCK_DGRAM, socket.htons(0x0800))
rx.bind((iface, 0))
rx.settimeout(0.5)
seen, stop = [], []
def watch():
    while not stop:
        try:
            d = rx.recv(2048)
        except socket.timeout:
            continue
        ihl = (d[0] & 0xf) * 4
        if d[9] != 6:
            continue
        tcp = d[ihl:]
        if struct.unpack(">H", tcp[2:4])[0] != port or not tcp[13] & 0x02:
            continue
        opts, mss, i = tcp[20:(tcp[12] >> 4) * 4], None, 0
        while i < len(opts) and opts[i]:
            if opts[i] == 1:
                i += 1
                continue
            if opts[i] == 2:
                mss = struct.unpack(">H", opts[i + 2:i + 4])[0]
            i += opts[i + 1]
        seen.append(mss)
thread = threading.Thread(target=watch)
thread.start()
time.sleep(0.5)
subprocess.run(["ip", "netns", "exec", "devtest-lan", "python3", "-c",
                "import socket\\ns = socket.socket()\\ns.settimeout(3)\\n"
                "try:\\n    s.connect((%r, %d))\\nexcept OSError:\\n    pass" % (addr, port)])
time.sleep(1)
stop.append(1)
thread.join()
print(json.dumps(seen))
"""
MSS_PORT = 5555
PPPOE_MTU = 1492       # a session's MTU unless the concentrator agrees to more
PPP_WAIT = 10          # seconds to wait for the bench's end of the session


def pppoe_mss(ctx: Context, address: str, mtu: int = PPPOE_MTU) -> tuple[bool, str]:
    """Whether the SYNs that a LAN host sends through the session carry an MSS
    that fits the session's MTU, and what was seen; address is the stick's end
    of the session."""
    # The concentrator gives its end of the session the peer address a
    # moment after the stick's end is up, so the interface is waited for.
    for _ in range(PPP_WAIT):
        iface = bench(ctx, "ip -4 -o addr | awk '$6 == \"%s/32\" {print $2}'"
                      % address).out.strip()
        if iface:
            break
        time.sleep(1)
    else:
        return False, f"no PPP interface of the session on the bench after {PPP_WAIT} s"
    reply = bench(ctx, f"python3 - {shlex.quote(iface)} {MSS_PORT}", stdin=MSS_PROBE, timeout=30)
    try:
        seen = json.loads(reply.out.strip().splitlines()[-1])
    except (IndexError, ValueError):
        return False, f"the MSS probe did not run: {(reply.err or reply.out).strip()!r}"
    fits = bool(seen) and all(m is not None and m <= mtu - 40 for m in seen)
    return fits, f"SYN MSS {', '.join(str(m) for m in seen) or 'none seen'}"


@case("router.pppoe", "Router mode",
      "With PPPoE, the stick opens a session with the concentrator, gets an address, and "
      "clamps a LAN host's TCP MSS to fit the session")
def pppoe(ctx: Context):
    if (skip := needs(ctx, nat=True)):
        return skip
    s = ctx.settings
    if not s.pppoe_secrets:
        return skipped("the settings name no file of PPPoE secrets on the bench (--pppoe-secrets)")
    if s.bng_service and not bench(ctx, f"systemctl is-active --quiet {s.bng_service}").ok:
        if not bench(ctx, f"systemctl start {s.bng_service}").ok:
            return failed(f"the bench could not start {s.bng_service}")
        ctx.memo["bng_started"] = True
    secrets = shlex.quote(s.pppoe_secrets)
    first = ("awk '$1 !~ /^#/ && NF >= 3 { gsub(/\"/, \"\"); "
             "print \"PPPOE_USER=\" $1; print \"PPPOE_SECRET=\" $3; exit }' " + secrets)
    code, body = set_mode(ctx, "MODE=router\nWAN_PROTO=pppoe\nWAN_VLAN=\n", body_from=first)
    if code != 200:
        return failed(f"the daemon refused the settings: HTTP {code} {body}")
    # The WAN is named before the session is up: the session is up when its
    # interface has an address.
    start = time.monotonic()
    address = ""
    while time.monotonic() - start < 90 and not address:
        address = ctx.stick.run("ip -4 -o address show dev ppp0 2>/dev/null | "
                                "awk '{print $4}'").out.strip()
        if not address:
            time.sleep(2)
    took = time.monotonic() - start
    if not address:
        log = ctx.stick.run("tail -5 /run/pppoed.log").out.strip()
        return failed(f"no session within 90 s: {status(ctx).strip()!r}; pppoed: {log!r}")
    dns = ctx.stick.run("cat /run/onu-dns 2>/dev/null").out.strip() or "none"
    # 1492, or up to 1500 when the concentrator took PPP-Max-Payload (RFC 4638).
    reply = ctx.stick.run("cat /sys/class/net/ppp0/mtu")
    mtu = int(reply.out) if reply.ok and reply.out.strip().isdigit() else PPPOE_MTU
    if not ctx.memo.get("namespaces"):
        return passed(f"session up in {took:.0f} s; ppp0 {address}, MTU {mtu}; "
                      f"name servers {dns}; MSS not checked without the LAN host of "
                      "router.static")
    fits, mss = pppoe_mss(ctx, address, mtu)
    return verdict(fits, f"session up in {took:.0f} s; ppp0 {address}, MTU {mtu}; "
                         f"name servers {dns}; {mss} through it")


# Passthrough: the stick terminates the PPPoE session itself and leases the
# session's address to the host behind it, on a VLAN of the host link, and the
# switch routes that host's frames both ways, translating nothing and keeping
# no state per flow. The host is a VLAN device of the bench in devtest-lan;
# the far end is the bench's end of the session, and a second source is the
# bench's own address towards the stick, which the host never sends to.
PASS_LAN_VLAN = 37                   # the stick's LAN link, a VLAN on the host link
PASS_WAN_VLAN = 410                   # a VLAN the bench's concentrator listens in
PASS_WAN_MAC = "00:00:5e:00:53:03"   # the MAC address the session runs from
PASS_GATEWAY = "169.254.0.1"         # the stick's address on the LAN link
PASS_HOST = "devtest-pt0"            # the host's VLAN device
PASS_COUNT = 2000                    # datagrams in each burst
PASS_RATE = 1500                     # of them a second, under the CPU port's limit

# A burst of datagrams from one address to another at a steady rate.
BURST = """\
import socket, sys, time
src, dst, port, count, rate = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind((src, 0))
start = time.monotonic()
for i in range(count):
    while time.monotonic() < start + i / rate:
        time.sleep(0.0002)
    s.sendto(bytes(512), (dst, port))
"""


@case("router.passthrough", "Router mode",
      "In passthrough mode the stick opens a PPPoE session and leases its address to the host "
      "behind it, and the switch routes the host's datagrams both ways, also those from a "
      "source the host never sent to")
def passthrough(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    s = ctx.settings
    if not s.pppoe_secrets:
        return skipped("the settings name no file of PPPoE secrets on the bench (--pppoe-secrets)")
    if s.bng_service and not bench(ctx, f"systemctl is-active --quiet {s.bng_service}").ok:
        if not bench(ctx, f"systemctl start {s.bng_service}").ok:
            return failed(f"the bench could not start {s.bng_service}")
        ctx.memo["bng_started"] = True
    secrets = shlex.quote(s.pppoe_secrets)
    first = ("awk '$1 !~ /^#/ && NF >= 3 { gsub(/\"/, \"\"); "
             "print \"PPPOE_USER=\" $1; print \"PPPOE_SECRET=\" $3; exit }' " + secrets)
    code, body = set_mode(ctx, f"MODE=passthrough\nWAN_PROTO=pppoe\nWAN_VLAN={PASS_WAN_VLAN}\n"
                               f"WAN_MAC={PASS_WAN_MAC}\nLAN_VLAN={PASS_LAN_VLAN}\n"
                               f"PASS_GATEWAY={PASS_GATEWAY}\n", body_from=first)
    if code != 200:
        return failed(f"the daemon refused the settings: HTTP {code} {body}")
    # The session is up when its address is handed on.
    start = time.monotonic()
    address = ""
    while time.monotonic() - start < 90 and not address:
        address = ctx.stick.run("cat /run/onu-pass 2>/dev/null").out.strip()
        if not address:
            time.sleep(2)
    took = time.monotonic() - start
    if not address:
        log = ctx.stick.run("tail -5 /run/pppoed.log").out.strip()
        return failed(f"no session within 90 s: {status(ctx).strip()!r}; pppoed: {log!r}")

    # The host takes the address as its lease would give it: the /32, the
    # stick's address on-link, the default route through it.
    namespaces_down(ctx)
    host = bench(ctx, "; ".join([
        "set -e",
        "ip netns add devtest-lan",
        f"ip link add link {shlex.quote(s.subscriber_port)} name {PASS_HOST} type vlan "
        f"id {PASS_LAN_VLAN}",
        f"ip link set {PASS_HOST} netns devtest-lan",
        f"ip -n devtest-lan address add {address}/32 dev {PASS_HOST}",
        f"ip -n devtest-lan link set {PASS_HOST} up",
        f"ip -n devtest-lan route add {PASS_GATEWAY} dev {PASS_HOST} scope link",
        f"ip -n devtest-lan route add default via {PASS_GATEWAY} dev {PASS_HOST}",
        "for i in 1 2 3 4 5 6 7 8 9 10; do "
        f"ip netns exec devtest-lan ping -c 1 -W 1 {PASS_GATEWAY} >/dev/null && exit 0; "
        "sleep 1; done; exit 1"]), timeout=60)
    ctx.memo["namespaces"] = True
    if not host.ok:
        return failed(f"the host behind the stick does not reach it: {host.err.strip()!r}")
    # The bench's end of the session: the PPP interface whose peer is the
    # address, and its own address.
    end = []
    for _ in range(PPP_WAIT):
        end = bench(ctx, "ip -4 -o addr | awk '$6 == \"%s/32\" {print $2, $4}'"
                    % address).out.split()
        if len(end) == 2:
            break
        time.sleep(1)
    else:
        return failed(f"no PPP interface of the session on the bench after {PPP_WAIT} s")
    iface, far = end
    other = bench(ctx, f"ip -4 -o route get {shlex.quote(s.stick)} | "
                       "sed -n 's/.* src \\([0-9.]*\\).*/\\1/p'").out.strip()
    cpu = ctx.stick.text("for i in /sys/class/net/*; do [ -e $i/dsa/tagging ] && "
                         "echo ${i##*/}; done; true")

    def burst(src: str, dst: str, netns: str, counter: str) -> tuple[int, int]:
        """Datagrams arrived at the counter, and frames that reached the CPU."""
        before = int(ctx.stick.text(f"cat /sys/class/net/{cpu}/statistics/rx_packets"))
        got = int(bench(ctx, counter).out.strip() or 0)
        run = f"ip netns exec {netns} python3" if netns else "python3"
        bench(ctx, f"{run} - {src} {dst} {PORT} {PASS_COUNT} {PASS_RATE}", stdin=BURST,
              timeout=PASS_COUNT / PASS_RATE + 30)
        time.sleep(1)
        got = int(bench(ctx, counter).out.strip() or 0) - got
        after = int(ctx.stick.text(f"cat /sys/class/net/{cpu}/statistics/rx_packets"))
        return got, after - before

    at_host = f"ip netns exec devtest-lan cat /sys/class/net/{PASS_HOST}/statistics/rx_packets"
    at_far = f"cat /sys/class/net/{shlex.quote(iface)}/statistics/rx_packets"
    down, down_cpu = burst(far, address, "", at_host)
    up, up_cpu = burst(address, far, "devtest-lan", at_far)
    alien, alien_cpu = burst(other, address, "", at_host) if other else (0, 0)
    # Every datagram must arrive, but for the losses of the path through the
    # OLT, and the CPU port count only its background and the frames of the
    # case's own ssh commands, a few dozen; the CPU's path would count every
    # datagram.
    whole = PASS_COUNT * 0.99
    quiet = PASS_COUNT * 0.05
    ok = (down >= whole and up >= whole and alien >= whole and bool(other)
          and max(down_cpu, up_cpu, alien_cpu) < quiet)
    return verdict(ok, f"session up in {took:.0f} s, {address} handed on; of {PASS_COUNT} "
                       f"datagrams at {PASS_RATE}/s: to the host {down}, CPU {down_cpu}; "
                       f"from it {up}, CPU {up_cpu}; to it from {other or 'no other source'} "
                       f"{alien}, CPU {alien_cpu}")


@case("router.bridge", "Router mode",
      "Back in bridge mode, the GEM ports are bridged with the host link again")
def bridge_again(ctx: Context):
    if (skip := needs(ctx)):
        return skip
    namespaces_down(ctx)
    code, body = set_mode(ctx, "MODE=bridge\n")
    if ctx.memo.pop("bng_started", False):
        bench(ctx, f"systemctl stop {ctx.settings.bng_service}")
    if code != 200:
        return failed(f"the daemon refused the settings: HTTP {code} {body}")
    took = wait_for(ctx, lambda st: "mode bridge" in st, 60)
    bridged = ""
    start = time.monotonic()
    while time.monotonic() - start < 300:
        bridged = ctx.stick.run("ls /sys/class/net/omcibr0/brif 2>/dev/null").out.split()
        if "sfp" in bridged and any(p.startswith("gem") for p in bridged):
            break
        time.sleep(5)
    ok = took is not None and "sfp" in bridged and any(p.startswith("gem") for p in bridged)
    waited = time.monotonic() - start
    return verdict(ok, f"bridge mode after {took or 0:.0f} s; omcibr0 holds "
                       f"{' '.join(bridged) or 'nothing'} after {waited:.0f} s more")
