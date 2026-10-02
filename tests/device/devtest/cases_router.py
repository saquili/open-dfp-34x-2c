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
neighbour's address for a while, finds the same hosts on every run. The far
end answers UDP itself, inside its namespace, so the bench host's own packet
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
# writes how many it took and from which sources when its time is up.
RESPONDER = """\
import json, socket, sys, time
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
    s.sendto(data, peer)
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


def needs(ctx: Context):
    s = ctx.settings
    if not s.router:
        return skipped("router mode not asked for (--router)")
    if not (s.olt_port and s.subscriber_port and s.pond_token):
        return skipped("the settings name no ports or no token")
    return None


def set_mode(ctx: Context, lines: str, *, body_from: str = "") -> tuple[int, str]:
    """POST settings to the daemon from the bench host; body_from is a command
    on the bench whose output goes first in the body."""
    url = shlex.quote(f"http://{ctx.settings.stick}:8080/api/set/settings")
    head = shlex.quote(f"Authorization: Bearer {ctx.settings.pond_token}")
    first = f"{body_from}; " if body_from else ""
    command = (f"{{ {first}printf '%s' {shlex.quote(lines)}; }} | "
               f"curl -s -m 15 -w '\\n%{{http_code}}' -H {head} --data-binary @- {url}")
    reply = ctx.bench.run(command if not body_from else sudo(ctx.settings, command),
                          timeout=40)
    body, _, code = reply.out.rpartition("\n")
    return int(code or 0), body


def status(ctx: Context) -> str:
    return ctx.stick.run("onu-mode status").out


def wait_for(ctx: Context, test, limit: float) -> float | None:
    """Seconds until test(status) holds, or None after limit seconds."""
    start = time.monotonic()
    while time.monotonic() - start < limit:
        if test(status(ctx)):
            return time.monotonic() - start
        time.sleep(2)
    return None


def namespaces_up(ctx: Context) -> None:
    s = ctx.settings
    script = "; ".join([
        "set -e",
        "ip netns del devtest-lan 2>/dev/null || true",
        "ip netns del devtest-wan 2>/dev/null || true",
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
    reply = bench(ctx, script)
    if not reply.ok:
        raise RuntimeError(f"the bench's namespaces: {reply.err.strip() or reply.out.strip()}")
    ctx.memo["namespaces"] = True


def namespaces_down(ctx: Context) -> None:
    bench(ctx, "ip netns del devtest-lan 2>/dev/null; ip netns del devtest-wan 2>/dev/null; true")
    ctx.memo.pop("namespaces", None)


@case("router.static", "Router mode",
      "With a static WAN, a LAN host's UDP flow leaves translated to the WAN address and "
      "the switch carries it")
def static(ctx: Context):
    if (skip := needs(ctx)):
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


@case("router.pppoe", "Router mode",
      "With PPPoE, the stick opens a session with the concentrator and gets an address")
def pppoe(ctx: Context):
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
    return passed(f"session up in {took:.0f} s; ppp0 {address}; name servers {dns}")


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
