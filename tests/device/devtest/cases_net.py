# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The host side: the link, the management VLAN, the packet filter, the ssh
server and the status daemon, seen from the bench host as a subscriber's
machine sees them."""

from __future__ import annotations

import json
import shlex

from .core import Context, case, failed, passed, skipped, verdict

# A TCP connection attempt from the bench host: "refused", "timeout" or
# "open", and how long it took.
PROBE = """
import socket, sys, time
s = socket.socket(socket.AF_INET6 if ":" in sys.argv[1] else socket.AF_INET)
s.settimeout(float(sys.argv[3]))
t = time.monotonic()
try:
    s.connect((sys.argv[1], int(sys.argv[2])))
    r = "open"
except ConnectionRefusedError:
    r = "refused"
except socket.timeout:
    r = "timeout"
print(r, round(time.monotonic() - t, 3))
"""


def probe(ctx: Context, port: int, wait: float = 3.0) -> tuple[str, float]:
    command = (f"python3 -c {shlex.quote(PROBE)} {shlex.quote(ctx.settings.stick)} "
               f"{port} {wait}")
    result, seconds = ctx.bench.text(command, timeout=wait + 30).split()
    return result, float(seconds)


@case("net.host-link", "Host side", "The host link sfp is up at its speed")
def host_link(ctx: Context):
    state = ctx.stick.text("cat /sys/class/net/sfp/operstate")
    speed = ctx.stick.run("cat /sys/class/net/sfp/speed").out.strip() or "?"
    return verdict(state == "up", f"sfp {state}, {speed} Mb/s")


@case("net.mgmt-vlan", "Host side", "The management address answers on its VLAN of the host link")
def mgmt_vlan(ctx: Context):
    line = ctx.stick.text("grep '^mgmt' /proc/net/vlan/config")
    name, vid, parent = (f.strip() for f in line.split("|"))
    ok = parent == "sfp"
    return verdict(ok, f"{name}: VLAN {vid} on {parent}; this run reaches the stick through it")


@case("net.filter-loaded", "Host side", "The packet filter is loaded, dropping by default")
def filter_loaded(ctx: Context):
    rules = ctx.stick.text("nft list ruleset")
    drops = rules.count("policy drop;")
    return verdict(drops == 2, f"{drops} chains with policy drop (input, forward), "
                               f"{len(rules.splitlines())} lines")


@case("net.filter-drops", "Host side",
      "A port nobody listens on is dropped with the filter and refused without it")
def filter_drops(ctx: Context):
    with_filter = probe(ctx, 23)
    try:
        ctx.stick.run("nft flush ruleset", check=True)
        without = probe(ctx, 23)
    finally:
        ctx.stick.run("nft -f /etc/nftables.conf", check=True)
    back = ctx.stick.text("nft list ruleset").count("policy drop;")
    ok = with_filter[0] == "timeout" and without[0] == "refused" and back == 2
    return verdict(ok, f"filtered: {with_filter[0]} after {with_filter[1]:.2f} s; "
                       f"unfiltered: {without[0]} in {without[1]:.3f} s; filter restored")


@case("net.ssh-keys-only", "Host side", "The ssh server offers keys only, no passwords")
def ssh_keys_only(ctx: Context):
    command = ("ssh -o BatchMode=yes -o PubkeyAuthentication=no -o ConnectTimeout=10 "
               "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "
               f"root@{shlex.quote(ctx.settings.stick)} true")
    reply = ctx.bench.run(command, timeout=40)
    denied = [l for l in reply.err.splitlines() if "Permission denied" in l]
    if not denied:
        return failed(f"exit {reply.code}: {reply.err.strip() or reply.out.strip()}")
    return verdict(denied[0].rstrip(".").endswith("(publickey)"), denied[0])


def http(ctx: Context, path: str, token: bool = False) -> tuple[int, str]:
    url = f"http://{ctx.settings.stick}:8080{path}"
    command = f"curl -s -m 15 -w '\\n%{{http_code}}' {'-H @-' if token else ''} {shlex.quote(url)}"
    header = f"Authorization: Bearer {ctx.settings.pond_token}\n" if token else None
    reply = ctx.bench.run(command, timeout=40, stdin=header)
    body, _, code = reply.out.rpartition("\n")
    return int(code or 0), body


@case("mgmt.pond-health", "Management", "The status daemon answers on the host side")
def pond_health(ctx: Context):
    code, body = http(ctx, "/api/health")
    return verdict(code == 200 and json.loads(body) == {"ok": True}, f"HTTP {code} {body}")


@case("mgmt.pond-page", "Management", "The status page is served on the host side")
def pond_page(ctx: Context):
    command = (f"curl -s -m 15 -D - -o /dev/null "
               f"{shlex.quote(f'http://{ctx.settings.stick}:8080/')}")
    head = ctx.bench.text(command, timeout=40).lower()
    status = head.splitlines()[0] if head else "no answer"
    ok = " 200" in status and "content-type: text/html" in head
    return verdict(ok, f"{status.strip()}, "
                       f"{'html' if 'text/html' in head else 'not html'}, "
                       f"{'with' if 'content-security-policy' in head else 'without'} a CSP")


@case("mgmt.pond-locked", "Management", "Without the token the daemon's API stays locked")
def pond_locked(ctx: Context):
    code, body = http(ctx, "/api/schema")
    return verdict(code == 401, f"HTTP {code} {json.loads(body).get('error', '')}")


@case("mgmt.pond-values", "Management",
      "With the token the daemon reports the kernel, the ONU and the optics")
def pond_values(ctx: Context):
    if not ctx.settings.pond_token:
        return skipped("no token in the settings")
    code, body = http(ctx, "/api/getm?ids=kernel,onu,optics", token=True)
    if code != 200:
        return failed(f"HTTP {code} {body[:200]}")
    values = json.loads(body)
    missing = [k for k in ("kernel", "onu", "optics") if not isinstance(values.get(k), dict)]
    shown = ", ".join(f"{k}: {len(json.dumps(v))} B" for k, v in values.items())
    return verdict(not missing, f"HTTP {code}, {shown}" + (f"; missing {missing}" if missing else ""))
