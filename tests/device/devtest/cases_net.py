# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The host side: the link, the management VLAN, what the stick listens on, the
ssh server and the status daemon, seen from the bench host as a subscriber's
machine sees them."""

from __future__ import annotations

import json
import re
import shlex
import subprocess
import time

from .access import ssh_base, sudo
from .core import Context, case, failed, skipped, verdict

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


# The host-link search: the bench host's subscriber-side port goes from
# 2500BaseX without negotiation to 1000BaseX with in-band negotiation and back,
# and each time the stick must find the setting, its management answering
# again with sfp at the new speed. A network manager that holds the port lets
# it go for the change and takes it back, which brings its own settings back.
# The port is first set back to the speed and negotiation it had: whenever the
# link goes, NetworkManager puts back the settings the port had when it took
# the port (a port taken back at 1000BaseX with negotiation was left with
# negotiation on after the stick's next reset, and the stick's next kernel
# found no link; measured). Before anything changes, the bench host arms a
# restore of its own, so that the port goes back even if the case dies
# halfway. The management path rides this link, so the case is destructive.
SEARCH_WAIT = 30.0     # seconds for the link and the stick to come back
SEARCH_RESTORE = 1800  # seconds after which the bench host restores its port
SEARCH_UNIT = "devtest-host-link"


def search_back(ctx: Context, port: str, want: str) -> tuple[float, str]:
    """Wait until the bench port has a link and the stick answers with sfp at
    the speed wanted; return how long that took and the speed that sfp last
    reported, or "none"."""
    start, speed = time.monotonic(), "none"
    while time.monotonic() - start < SEARCH_WAIT:
        link = ctx.bench.run(sudo(ctx.settings, f"ethtool {shlex.quote(port)}")).out
        if "Link detected: yes" in link:
            reply = ctx.stick.run("cat /sys/class/net/sfp/speed", timeout=10)
            if reply.ok and reply.out.strip():
                speed = reply.out.strip()
                if speed == want:
                    break
        time.sleep(1)
    return time.monotonic() - start, speed


@case("net.host-link-search", "Host side",
      "The host link finds the host's setting as the host moves from 2500BaseX to 1000BaseX "
      "with in-band negotiation and back", destructive=True)
def host_link_search(ctx: Context):
    s = ctx.settings
    if not s.subscriber_port:
        return skipped("the settings name no subscriber-side port")
    port = shlex.quote(s.subscriber_port)
    now = ctx.bench.run(sudo(s, f"ethtool {port}")).out
    speed = re.search(r"Speed: (\d+)Mb/s", now)
    neg = re.search(r"Auto-negotiation: (on|off)", now)
    if not speed or not neg:
        return skipped(f"{s.subscriber_port} reports no speed or no negotiation setting")
    managed = f"{s.subscriber_port}:connected" in ctx.bench.run(
        "nmcli -t -f DEVICE,STATE device 2>/dev/null").out
    back = f"ethtool -s {port} speed {speed[1]} duplex full autoneg {neg[1]}"
    if managed:
        back += f"; nmcli device set {port} managed yes; nmcli device connect {port}"
    ctx.bench.run(sudo(s, f"systemd-run --on-active={SEARCH_RESTORE} --unit={SEARCH_UNIT} "
                          f"/bin/sh -c {shlex.quote(back)}"))
    try:
        if managed:
            ctx.bench.run(sudo(s, f"nmcli device set {port} managed no"))
        ctx.bench.run(sudo(s, f"ethtool -s {port} speed 1000 duplex full autoneg on"))
        slow, slow_speed = search_back(ctx, s.subscriber_port, "1000")
    finally:
        ctx.bench.run(sudo(s, back + "; true"))
        fast, fast_speed = search_back(ctx, s.subscriber_port, "2500")
        ctx.bench.run(sudo(s, f"systemctl stop {SEARCH_UNIT}.timer 2>/dev/null; true"))
    ok = slow_speed == "1000" and fast_speed == "2500"
    return verdict(ok, f"1000BaseX with negotiation: back after {slow:.1f} s, sfp {slow_speed} Mb/s; "
                       f"2500BaseX: back after {fast:.1f} s, sfp {fast_speed} Mb/s")


@case("net.mgmt-vlan", "Host side", "The management address answers on its VLAN of the host link")
def mgmt_vlan(ctx: Context):
    line = ctx.stick.text("grep '^mgmt' /proc/net/vlan/config")
    name, vid, parent = (f.strip() for f in line.split("|"))
    ok = parent == "sfp"
    return verdict(ok, f"{name}: VLAN {vid} on {parent}; this run reaches the stick through it")


def listeners(text: str) -> set[tuple[str, int]]:
    """The (address, port) pairs that BusyBox's "netstat -ltn" shows in LISTEN."""
    found = set()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 6 and fields[0].startswith("tcp") and fields[-1] == "LISTEN":
            address, _, port = fields[3].rpartition(":")
            found.add((address, int(port)))
    return found


@case("net.listeners", "Host side",
      "The servers listen on the two host-side addresses and on no other, and the bench "
      "reaches ssh and the panel and is refused elsewhere")
def listening(ctx: Context):
    # The stick has no packet filter (docs/handbook/system.rst): what keeps its servers
    # off the PON side is the addresses they listen on.
    mgmt = ctx.stick.text("cat /run/onu-mgmt")
    mine = {re.sub(r"/\d+$", "", v) for k, v in
            (l.split("=", 1) for l in mgmt.splitlines() if "=" in l)
            if k in ("ADDRESS", "DATA_ADDRESS")}
    found = listeners(ctx.stick.text("netstat -ltn"))
    stray = sorted(f"{a}:{p}" for a, p in found if a not in mine)
    ports = sorted({p for a, p in found if a in mine})
    ssh, panel, closed = probe(ctx, 22), probe(ctx, 443), probe(ctx, 23)
    ok = (len(mine) == 2 and not stray and {22, 443} <= set(ports) and ssh[0] == "open" and
          panel[0] == "open" and closed[0] == "refused")
    shown = ", ".join(str(p) for p in ports) or "none"
    return verdict(ok, f"listening: {shown} on {len(mine)} host-side addresses"
                       + (f"; also {', '.join(stray)}" if stray else "; on no other")
                       + f"; bench: ssh {ssh[0]}, panel {panel[0]}, a closed port {closed[0]}")


def local_run(argv: list[str], timeout: float) -> tuple[int, str]:
    """Run a command on the machine that runs the tests: exit code, stderr."""
    try:
        done = subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                              stdin=subprocess.DEVNULL, errors="replace")
        return done.returncode, done.stderr
    except subprocess.TimeoutExpired:
        return 124, "timed out"


@case("net.ssh-login", "Host side",
      "The ssh server offers root's key and root's password, and nothing else")
def ssh_login(ctx: Context):
    # From the machine that runs the tests, through the bench host, without
    # offering a key: the refusal names the methods the server offers. A
    # connection of its own, as one shared with the cases before it is
    # already logged in.
    argv = ssh_base(ctx.settings) + ["-o", "ControlMaster=no", "-o", "ControlPath=none",
                                     "-o", "PubkeyAuthentication=no",
                                     "-o", "StrictHostKeyChecking=no",
                                     "-o", "UserKnownHostsFile=/dev/null",
                                     "-J", ctx.settings.bench, f"root@{ctx.settings.stick}", "true"]
    code, err = local_run(argv, 60)
    ctx.log.append(f"local$ {' '.join(argv)}\n{err}[exit {code}]\n")
    denied = [l for l in err.splitlines() if "Permission denied" in l]
    if not denied:
        return failed(f"exit {code}: {err.strip()}")
    methods = re.search(r"\(([a-z,-]+)\)\.?$", denied[0].strip())
    return verdict(bool(methods) and set(methods[1].split(",")) == {"publickey", "password"},
                   denied[0])


def http(ctx: Context, path: str, token: bool = False) -> tuple[int, str]:
    # The daemon's certificate is its own, which curl cannot check; the case
    # mgmt.pond-https checks it against the stick's.
    url = f"https://{ctx.settings.stick}{path}"
    command = f"curl -s -k -m 15 -w '\\n%{{http_code}}' {'-H @-' if token else ''} {shlex.quote(url)}"
    header = f"Authorization: Bearer {ctx.settings.pond_token}\n" if token else None
    reply = ctx.bench.run(command, timeout=40, stdin=header)
    body, _, code = reply.out.rpartition("\n")
    return int(code or 0), body


@case("mgmt.pond-health", "Management", "The status daemon answers on the host side")
def pond_health(ctx: Context):
    code, body = http(ctx, "/api/health")
    return verdict(code == 200 and json.loads(body) == {"ok": True}, f"HTTP {code} {body}")


@case("mgmt.pond-https", "Management",
      "The status daemon answers over HTTPS with the certificate the stick shows, and sends "
      "HTTP there")
def pond_https(ctx: Context):
    stick = ctx.settings.stick
    shown = ctx.stick.text(f"pond -k /run/pond/tls.key -F {shlex.quote(stick)} 2>/dev/null; true")
    words = shown.split()
    if len(words) < 3 or words[1] != "SHA-256":
        return failed(f"the stick shows no certificate for {stick}: {shown!r}")
    served = ctx.bench.text(
        f"echo | openssl s_client -connect {shlex.quote(stick)}:443 -tls1_3 2>/dev/null | "
        "openssl x509 -noout -fingerprint -sha256 2>/dev/null; true", timeout=40)
    served = served.strip().partition("=")[2]
    sent = ctx.bench.text(
        f"curl -s -m 15 -o /dev/null -w '%{{http_code}} %{{redirect_url}}' "
        f"{shlex.quote(f'http://{stick}/settings')}", timeout=40).strip()
    ok = served == words[2] and sent == f"308 https://{stick}/settings"
    return verdict(ok, f"served {'the same certificate' if served == words[2] else served or 'none'}; "
                       f"HTTP: {sent or 'no answer'}")


@case("mgmt.pond-page", "Management", "The status page is served on the host side")
def pond_page(ctx: Context):
    # The panel is stored gzip-compressed only, and handed as it is to a
    # client that accepts gzip, as every browser does.
    command = (f"curl -s -k -m 15 -D - -o /dev/null -H 'Accept-Encoding: gzip' "
               f"{shlex.quote(f'https://{ctx.settings.stick}/')}")
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
    kernel = values.get("kernel")
    missing = [] if isinstance(kernel, str) and kernel.startswith("Linux version") else ["kernel"]
    missing += [k for k in ("onu", "optics") if not isinstance(values.get(k), dict)]
    shown = ", ".join(f"{k}: {len(json.dumps(v))} B" for k, v in values.items())
    return verdict(not missing, f"HTTP {code}, {shown}" + (f"; missing {missing}" if missing else ""))
