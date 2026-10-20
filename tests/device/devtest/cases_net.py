# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The host side: the link, the management VLAN, what the stick listens on, the
ssh server and the status daemon, seen from the bench host as a subscriber's
machine sees them."""

from __future__ import annotations

import ipaddress
import json
import re
import shlex
import subprocess
import time

from .access import jump, ssh_base, sudo
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
    # The servers listen on the management addresses alone, and the stick's guard
    # drops what comes to them by any other interface (docs/handbook/system.rst),
    # which net.guard checks.
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


# The guard's probe, as root on the bench: python3 - PORT VID MAC DST SRC
# PORTS ARP. It sends, from the bench's port PORT, tagged VID (untagged for 0),
# to the stick's MAC address MAC, a TCP SYN to the address DST from the
# address SRC, to each of PORTS ("22,443"), three times, and prints as JSON
# whether each port answered, with a SYN-ACK or a reset from DST to SRC, on
# whichever interface of the bench the answer comes back by. With ARP 1 it
# answers the stick's ARP requests for SRC in that VLAN, so that an answer to
# an address of a network of the stick's comes back to the bench.
GUARD_PROBE = """\
import json, random, socket, struct, sys, time
port, vid, mac, dst, src = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
ports, arp = [int(p) for p in sys.argv[6].split(",")], sys.argv[7] == "1"
own = bytes.fromhex(open("/sys/class/net/%s/address" % port).read().strip().replace(":", ""))
peer = bytes.fromhex(mac.replace(":", ""))
d, s = socket.inet_aton(dst), socket.inet_aton(src)
tag = struct.pack(">HH", 0x8100, vid) if vid else b""
def checksum(b):
    b += b"\\0" * (len(b) % 2)
    total = sum(struct.unpack(">%dH" % (len(b) // 2), b))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return ~total & 0xffff
def frame(kind, body):
    out = peer + own + tag + kind + body
    return out + bytes(max(0, 60 - len(out)))
def syn(sport, dport):
    tcp = struct.pack(">HHIIBBHHH", sport, dport, random.getrandbits(32), 0, 5 << 4, 2,
                      65535, 0, 0)
    tcp = tcp[:16] + struct.pack(">H", checksum(s + d + struct.pack(">BBH", 0, 6, len(tcp)) +
                                                tcp)) + tcp[18:]
    ip = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(tcp), 0, 0x4000, 64, 6, 0, s, d)
    return frame(b"\\x08\\x00", ip[:10] + struct.pack(">H", checksum(ip)) + ip[12:] + tcp)
rx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
rx.settimeout(0.2)
tx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
tx.bind((port, 0))
base = random.randint(40000, 60000)
sport = {base + i: p for i, p in enumerate(ports)}
answered = {p: False for p in ports}
for _ in range(3):
    for sp, dp in sport.items():
        tx.send(syn(sp, dp))
    end = time.time() + 0.7
    while time.time() < end:
        try:
            data, addr = rx.recvfrom(2048)
        except socket.timeout:
            continue
        if addr[2] == socket.PACKET_OUTGOING:
            continue
        off = 12
        while data[off:off + 2] in (b"\\x81\\x00", b"\\x88\\xa8"):
            off += 4
        kind, body = data[off:off + 2], data[off + 2:]
        if (arp and kind == b"\\x08\\x06" and data[6:12] == peer and body[6:8] == b"\\x00\\x01"
                and body[24:28] == s):
            tx.send(frame(b"\\x08\\x06", body[:6] + b"\\x00\\x02" + own + s + body[8:18]))
            continue
        if kind != b"\\x08\\x00":
            continue
        h = (body[0] & 15) * 4
        if len(body) < h + 14 or body[9] != 6 or body[12:16] != d or body[16:20] != s:
            continue
        sp, dp, flags = struct.unpack(">HH", body[h:h + 4]) + (body[h + 13],)
        if dp in sport and sport[dp] == sp and flags & 0x16:
            answered[sp] = True
print(json.dumps({str(p): a for p, a in answered.items()}))
"""


def guard_probe(ctx: Context, vid: int, mac: str, dst: str, src: str,
                arp: bool = False) -> dict[int, bool]:
    """Whether a SYN to ssh and to the panel at @dst, sent from the bench's
    subscriber-side port in VLAN @vid (0 for none) to the stick's MAC address
    @mac from the address @src, is answered; with @arp the bench answers the
    stick's ARP for @src."""
    command = sudo(ctx.settings, " ".join(shlex.quote(a) for a in (
        "python3", "-", ctx.settings.subscriber_port, str(vid), mac, dst, src, "22,443",
        "1" if arp else "0")))
    reply = ctx.bench.run(command, stdin=GUARD_PROBE, timeout=60)
    try:
        return {int(p): a for p, a in json.loads(reply.out.strip().splitlines()[-1]).items()}
    except (IndexError, ValueError):
        raise ValueError(f"the probe did not run: {(reply.err or reply.out).strip()!r}")


def vlans_on_sfp(text: str) -> dict[str, int]:
    """The VLAN interfaces on sfp and their VIDs, of /proc/net/vlan/config."""
    found = {}
    for line in text.splitlines()[2:]:
        fields = [f.strip() for f in line.split("|")]
        if len(fields) == 3 and fields[2] == "sfp" and fields[1].isdigit():
            found[fields[0]] = int(fields[1])
    return found


def addresses(text: str) -> dict[str, list[ipaddress.IPv4Interface]]:
    """The IPv4 addresses of each interface, of "ip -4 -o addr show"."""
    found: dict[str, list[ipaddress.IPv4Interface]] = {}
    for line in text.splitlines():
        m = re.match(r"\d+:\s+([^\s@]+)(?:@\S+)?\s+inet (\d+(?:\.\d+){3}/\d+)", line)
        if m:
            found.setdefault(m[1], []).append(ipaddress.IPv4Interface(m[2]))
    return found


def neighbour(own: ipaddress.IPv4Interface) -> str | None:
    """An address of the network of @own other than it, the last but one or two
    of the network, for a probe to come from; None for a network of fewer than
    four addresses."""
    net = own.network
    if net.prefixlen > 30:
        return None
    for host in (net.broadcast_address - 1, net.broadcast_address - 2):
        if host != own.ip:
            return str(host)
    return None


@case("net.guard", "Host side",
      "ssh and the panel answer on the management VLAN, and a SYN to them by the host link "
      "itself or by another VLAN of it is dropped, not answered")
def guarded(ctx: Context):
    # The stick's own programs answer on its management VLAN alone, in every mode
    # (onu-fw's guard, docs/handbook/system.rst): a packet for the management
    # address that comes in by another interface is dropped before a server sees
    # it, so not even a reset comes back. The same SYN in the management VLAN is
    # answered, which shows that the probe reaches the stick; a frame to the
    # stick's own MAC address goes to its CPU whatever its VLAN (I). By an
    # interface with a network of the stick's, a local network's, the SYN comes
    # from an address of that network, as a device of it would send it, which the
    # strict reverse-path check of such an interface takes; by one without, from
    # the bench's own address on the management network, and the interface's
    # reverse-path check is in the record.
    if not ctx.settings.subscriber_port:
        return skipped("the settings name no subscriber-side port")
    mgmt = dict(line.split("=", 1) for line in ctx.stick.text("cat /run/onu-mgmt").splitlines()
                if "=" in line)
    dst, vid = mgmt.get("ADDRESS", "").partition("/")[0], int(mgmt.get("VLAN", "0") or 0)
    if not dst or not vid:
        return failed("the stick names no management VLAN and address in /run/onu-mgmt")
    route = ctx.bench.text(f"ip -4 -o route get {shlex.quote(dst)}")
    found = re.search(r"\bsrc (\d+(?:\.\d+){3})", route)
    if not found:
        return failed(f"the bench has no address towards {dst}: {route!r}")
    src = found.group(1)
    mac = ctx.stick.text("cat /sys/class/net/sfp/address")
    own = addresses(ctx.stick.text("ip -4 -o addr show"))
    ways = {"sfp": 0}
    ways.update({name: v for name, v in
                 sorted(vlans_on_sfp(ctx.stick.text("cat /proc/net/vlan/config")).items())
                 if not name.startswith("mgmt")})
    ssh, panel = probe(ctx, 22), probe(ctx, 443)
    control = guard_probe(ctx, vid, mac, dst, src)
    shown, reached = [], []
    for name, v in ways.items():
        nets = [a for a in own.get(name, []) if neighbour(a)]
        if nets:
            frm = neighbour(nets[0])
            how = f"from {frm} of {nets[0].network}"
        else:
            rp = ctx.stick.text(f"cat /proc/sys/net/ipv4/conf/{name}/rp_filter; true")
            frm, how = src, f"from the bench's address, rp_filter {rp or '?'}"
        got = guard_probe(ctx, v, mac, dst, frm, arp=bool(nets))
        answered = [str(p) for p, a in sorted(got.items()) if a]
        way = f"{name} ({f'VLAN {v}' if v else 'untagged'}, {how})"
        shown.append(f"{way}: " + (f"answered on {', '.join(answered)}" if answered
                                   else "no answer"))
        if answered:
            reached.append(way)
    ok = ssh[0] == "open" and panel[0] == "open" and all(control.values()) and not reached
    held = ", ".join(str(p) for p, a in sorted(control.items()) if a) or "no port"
    return verdict(ok, f"on the management address: ssh {ssh[0]}, panel {panel[0]}; a SYN in "
                       f"VLAN {vid} answered on {held}; " + "; ".join(shown))


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
                                     *jump(ctx.settings), f"root@{ctx.settings.stick}", "true"]
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


@case("net.sessions", "Host side",
      "The stick holds its PPPoE sessions: each on a GEM interface and up under netifd, the "
      "address of the IPv4 one handed to the host and the delegated IPv6 prefix routed to it",
      tools=("ubus",))
def sessions(ctx: Context):
    from .cases_system import kv
    told = kv(ctx.stick.text("onu-apply state"))
    if told.get("mode") != "passthrough":
        return skipped("the stick holds no PPPoE account (mode " + (told.get("mode") or "unknown") + ")")
    numbers = sorted({k.split(".")[1] for k in told if re.fullmatch(r"session\.\d+\.dev", k)})
    if not numbers:
        return failed("passthrough, and no session in the state")
    problems, shown = [], []
    for n in numbers:
        gem, hands = told.get(f"session.{n}.gem", ""), told.get(f"session.{n}.hands", "")
        status = json.loads(ctx.stick.text(f"ubus call network.interface.ses{n} status"))
        if not gem or not status.get("up"):
            problems.append(f"session {n} {'has no GEM port' if not gem else 'is not up'}")
        if hands == "ipv4":
            if not told.get("pass.address") or not told.get("pass.peer_gateway"):
                problems.append(f"session {n}: no address handed to the host")
            shown.append(f"session {n} on {gem}, its address handed to the host")
        elif hands == "ipv6":
            dhcp = ctx.stick.run(f"ubus call dhcp6d.ses{n} status")
            state = json.loads(dhcp.out) if dhcp.ok else {}
            if not (state.get("prefix") and state.get("bound") and state.get("routed")):
                problems.append(f"session {n}: its prefix is not delegated and routed "
                                f"({dhcp.out.strip() or 'dhcp6d.ses%s is not on ubus' % n})")
            shown.append(f"session {n} on {gem}, prefix {state.get('prefix', 'none')} "
                         f"{'routed to the host' if state.get('routed') else 'not routed'}")
        else:
            shown.append(f"session {n} on {gem}, its address kept on the stick")
    return verdict(not problems, "; ".join(shown) + (" | " + "; ".join(problems) if problems else ""))


# Seconds the panel's reply to a sign-in may take.
LOGIN_WAIT = 30


@case("mgmt.pond-no-store", "Management",
      "When the stick cannot read its store the panel refuses every sign-in with 423 and "
      "does not fall back to the default password")
def pond_no_store(ctx: Context):
    # The state onu-config keeps in /run/onu-auth when it cannot read the store is a "!";
    # put it there, with the file's own mode and owner kept, and put the file back whatever
    # happens. The sign-in is tried with the published default password.
    ctx.stick.run("cp -p /run/onu-auth /run/onu-auth.devtest 2>/dev/null; true")
    saved = ctx.stick.run("[ -e /run/onu-auth.devtest ]").ok
    try:
        ctx.stick.run("echo '!' > /run/onu-auth", check=True)
        command = (f"curl -s -k -m {LOGIN_WAIT} -o /dev/null -w '%{{http_code}}' "
                   "-H 'Content-Type: application/json' -H 'X-Pond: 1' "
                   "-d '{\"user\":\"root\",\"secret\":\"admin\"}' "
                   f"{shlex.quote(f'https://{ctx.settings.stick}/api/auth/login')}")
        code = ctx.bench.text(command, timeout=LOGIN_WAIT + 15).strip()
    finally:
        if saved:
            ctx.stick.run("mv /run/onu-auth.devtest /run/onu-auth")
        else:
            ctx.stick.run("rm -f /run/onu-auth")
    return verdict(code == "423", f"a sign-in with the published default answered HTTP {code}")

