# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The system: the kernel, its log, the boot, the file systems, the watchdog."""

from __future__ import annotations

import json
import os
import re
import subprocess
import time

from .access import RemoteError
from .core import Context, case, failed, passed, skipped, verdict

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__)))))
# What the kernel prints when something went wrong in it, and when a driver
# failed an object that the bridge handed it through switchdev.
TROUBLE = (r"(BUG:|Oops|WARNING:|Call Trace|Kernel panic|segfault|unhandled|soft lockup|hung_task"
           r"|Failed to (add|del) .* \(object id=)")
# Seconds past the watchdog's timeout in which the stick must have gone, and
# in which it must be back: the boot loader counts down for up to ten minutes.
RESET_MARGIN = 15
RESET_RETURN = 900
# Seconds the bench's reload command may take: a RAM boot, with its retries.
RELOAD_TIMEOUT = 600
# Seconds past the watchdog's timeout that the stick must have lived to show it fed.
WATCHDOG_MARGIN = 5


def reload_stick(command: str) -> tuple[int, str]:
    """Run the bench's reload command here: its exit code and its last line."""
    try:
        done = subprocess.run(command, shell=True, capture_output=True, text=True,
                              timeout=RELOAD_TIMEOUT, check=False)
    except subprocess.TimeoutExpired:
        return 124, f"no end within {RELOAD_TIMEOUT} s"
    lines = (done.stdout + done.stderr).strip().splitlines()
    return done.returncode, lines[-1] if lines else ""


def kernel_version() -> str:
    fields = {}
    with open(os.path.join(REPO, "linux", "Makefile"), encoding="utf-8") as handle:
        for line in handle:
            match = re.match(r"^(VERSION|PATCHLEVEL|SUBLEVEL) = (\d+)$", line)
            if match:
                fields[match.group(1)] = match.group(2)
            if len(fields) == 3:
                break
    return f"{fields['VERSION']}.{fields['PATCHLEVEL']}.{fields['SUBLEVEL']}"


@case("system.kernel", "System", "The stick runs the kernel of this tree")
def kernel(ctx: Context):
    release = ctx.stick.text("uname -r")
    build = ctx.stick.text("uname -v")
    want = kernel_version()
    return verdict(release == want, f"uname -r {release} (tree {want}), {build}")


@case("system.log", "System",
      "After the other cases, the kernel log holds no oops, warning, BUG or panic and no "
      "switchdev object that a driver failed", last=True)
def log(ctx: Context):
    lines = ctx.stick.text(f"dmesg | grep -E '{TROUBLE}' || true").splitlines()
    total = ctx.stick.text("dmesg | wc -l")
    if lines:
        return failed(f"{len(lines)} of {total} lines, first: {lines[0]}")
    return passed(f"0 of {total} lines")


@case("system.services", "System", "The boot started the ssh server, omcid and pond")
def services(ctx: Context):
    found = {}
    for name in ("dropbear", "omcid", "pond"):
        found[name] = ctx.stick.run(f"pidof {name}").out.strip()
    missing = [n for n, pid in found.items() if not pid]
    shown = ", ".join(f"{n} pid {p or '-'}" for n, p in found.items())
    return verdict(not missing, shown)


@case("system.filesystems", "System",
      "The root is the image's own, read-only from flash or the initramfs of a RAM boot; "
      "/tmp, /run and /var are in memory")
def filesystems(ctx: Context):
    mounts = {}
    for line in ctx.stick.text("cat /proc/mounts").splitlines():
        dev, point, kind, options = line.split()[:4]
        mounts[point] = (kind, options.split(","))
    kind, options = mounts.get("/", ("missing", []))
    if kind == "rootfs":
        # A RAM boot runs from its initramfs, which lives in memory only.
        root_ok, root = True, "/ the initramfs of a RAM boot"
    else:
        root_ok, root = "ro" in options, f"/ {kind} {'ro' if 'ro' in options else 'rw'}"
    memory = {p: mounts.get(p, ("missing", []))[0] for p in ("/tmp", "/run", "/var")}
    ok = root_ok and all(k == "tmpfs" for k in memory.values())
    return verdict(ok, f"{root}; " + ", ".join(f"{p} {k}" for p, k in memory.items()))


# Seconds between the two reads of the PTP clock's pulse.
PPS_WAIT = 4


def pps_assert(text: str) -> tuple[float, int]:
    """The time and the sequence number of a PPS source's last assert event,
    from its sysfs line "<sec>.<nsec>#<sequence>"."""
    match = re.fullmatch(r"(\d+)\.(\d{9})#(\d+)", text.strip())
    if not match:
        raise ValueError(f"no PPS assert event in {text!r}")
    return int(match.group(1)) + int(match.group(2)) / 1e9, int(match.group(3))


@case("system.phc", "System",
      "The PTP hardware clock is the ports' timestamping clock, and its pulse comes once a "
      "second", tools=("ethtool",))
def phc(ctx: Context):
    info = ctx.stick.text("ethtool -T sfp")
    match = re.search(r"(?:Hardware timestamp provider index|PTP Hardware Clock):\s*(\d+)", info)
    if not match:
        return failed("ethtool -T sfp names no hardware clock")
    n = int(match.group(1))
    name = ctx.stick.text(f"cat /sys/class/ptp/ptp{n}/clock_name")
    caps = [c for c in ("hardware-receive", "hardware-raw-clock") if c in info]
    pps = ctx.stick.text(f"for d in /sys/class/pps/pps*; do [ \"$(cat $d/name)\" = ptp{n} ] "
                         "&& echo ${d##*/}; done; true")
    if not pps:
        return failed(f"ptp{n} {name}: no PPS source")
    ctx.stick.run(f"echo 1 > /sys/class/ptp/ptp{n}/pps_enable")
    try:
        # Both reads in one command, so that no new connection comes between
        # them to stretch the wait.
        reads = ctx.stick.text(f"sleep 2; cat /sys/class/pps/{pps}/assert; "
                               f"sleep {PPS_WAIT}; cat /sys/class/pps/{pps}/assert").split()
    finally:
        ctx.stick.run(f"echo 0 > /sys/class/ptp/ptp{n}/pps_enable")
    if len(reads) != 2:
        return failed(f"ptp{n} {name}: two PPS reads expected, got {reads!r}")
    first, last = pps_assert(reads[0]), pps_assert(reads[1])
    pulses, seconds = last[1] - first[1], last[0] - first[0]
    ok = (name == "rtl9602c-ptp" and len(caps) == 2 and abs(pulses - PPS_WAIT) <= 1 and
          pulses > 0 and abs(seconds - pulses) < 0.1)
    return verdict(ok, f"ptp{n} {name}, {', '.join(caps) or 'no hardware stamping'}; "
                       f"{pulses} pulses {seconds:.3f} s apart")


@case("system.watchdog", "System", "The watchdog runs, and procd, the first process, feeds it")
def watchdog(ctx: Context):
    base = "/sys/class/watchdog/watchdog0"
    identity = ctx.stick.text(f"cat {base}/identity")
    timeout = int(ctx.stick.text(f"cat {base}/timeout"))
    state = ctx.stick.text(f"cat {base}/state")
    first = ctx.stick.text("cat /proc/1/comm")
    held = ctx.stick.run("ls -l /proc/1/fd 2>/dev/null | grep -c /dev/watchdog").out.strip() == "1"
    # The watchdog has no count to read, so it is shown fed by the stick's age: an armed
    # watchdog that nobody fed would have reset it after its timeout. A stick younger
    # than that is waited for.
    up = float(ctx.stick.text("cut -d' ' -f1 /proc/uptime"))
    if up <= timeout + WATCHDOG_MARGIN:
        time.sleep(timeout + WATCHDOG_MARGIN - up)
        up = float(ctx.stick.text("cut -d' ' -f1 /proc/uptime"))
    ok = timeout > 0 and state == "active" and first == "procd" and held and up > timeout
    return verdict(ok, f"{identity}, timeout {timeout} s, {state}; "
                       f"{first} (pid 1) {'holds' if held else 'does not hold'} /dev/watchdog; "
                       f"up {up:.0f} s")


@case("system.watchdog-reset", "System",
      "An armed watchdog that nobody feeds resets the stick within its timeout",
      destructive=True, last=True, tools=("ubus",))
def watchdog_reset(ctx: Context):
    timeout = int(ctx.stick.text("cat /sys/class/watchdog/watchdog0/timeout"))
    boot = ctx.stick.text("cat /proc/sys/kernel/random/boot_id")
    # procd stops feeding it and lets go of the device without the magic character,
    # which leaves it armed (procd's "system watchdog" method, with "stop").
    ctx.stick.run("ubus call system watchdog '{\"stop\": true}'", timeout=20, check=True)
    start = time.monotonic()
    gone = None
    how = ""
    # Gone within the timeout and a margin, then back with a new boot: the
    # boot loader may count down for minutes before it starts a kernel, unless
    # the bench's reload command brings the stick back as soon as it is gone.
    while time.monotonic() - start < timeout + RESET_RETURN:
        time.sleep(5)
        reply = ctx.stick.run("cat /proc/sys/kernel/random/boot_id", timeout=10)
        now = time.monotonic() - start
        answer = reply.out.strip() if reply.ok else ""
        if not answer:
            if gone is None:
                gone = now
                if ctx.settings.reload:
                    code, last = reload_stick(ctx.settings.reload)
                    ctx.log.append(f"local$ the reload command\n{last}\n[exit {code}]\n")
                    how = (", brought back by the reload command" if code == 0 else
                           f", by the loader's countdown: the reload command failed "
                           f"(exit {code})")
        elif answer == boot:
            if now > timeout + RESET_MARGIN:
                return failed(f"timeout {timeout} s; still answering {now:.0f} s after arming")
        elif gone is None:
            return failed(f"timeout {timeout} s; a new boot after {now:.0f} s, "
                          "without having been seen gone")
        else:
            return passed(f"timeout {timeout} s; gone after {gone:.0f} s, back with a new "
                          f"boot after {now:.0f} s{how}")
    return failed(f"timeout {timeout} s; gone after {gone:.0f} s, no new boot within "
                  f"{timeout + RESET_RETURN} s")


def kv(text: str) -> dict[str, str]:
    """The lines KEY=VALUE of a program's report."""
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


@case("system.loader-listener", "System",
      "The boot loader is told not to listen on the host link for an image sent to it by "
      "multicast, which it would write over this system from anyone")
def loader_listener(ctx: Context):
    reply = ctx.stick.run("onu-update env mupgrade_en")
    value = kv(reply.out).get("mupgrade_en", "")
    return verdict(reply.ok and value == "0",
                   f"mupgrade_en={value}" if value else "mupgrade_en is not set")


# What "ls" of the partition config shows of this system: its one file. Everything
# else there is the shipped firmware's.
CONFIG_FILE = "open-dfp-34x-2c.conf"
CONFIG_LIMIT = 64 * 1024


@case("system.config-store", "System",
      "The partition config is the shipped firmware's JFFS2 with this system's one file beside "
      "its files, under its limit, and a save leaves the shipped firmware's files as they were")
def config_store(ctx: Context):
    mount = next((l.split() for l in ctx.stick.text("cat /proc/mounts").splitlines()
                  if len(l.split()) > 2 and l.split()[1] == "/config"), None)
    if not mount:
        return failed("/config is not mounted")
    # The shipped firmware names a file after its kernel and builder, with brackets and an
    # at sign, so the files are found by find, not named. Each is told by the digest of its
    # contents and of its name, which compare before and after a save and put no name in
    # the record.
    others = f"find /config -type f ! -name {CONFIG_FILE}"
    digest = ("sh -c 'echo \"$(sha256sum < \"$1\" | cut -c1-64) "
              "$(printf %s \"$1\" | sha256sum | cut -c1-8)\"' sh {} \\;")
    sums = f"{others} -exec {digest} | sort"
    shipped = int(ctx.stick.text(f"{others} | wc -l"))
    if not shipped:
        return failed("/config holds no file of the shipped firmware")
    ours = ctx.stick.run(f"[ -f /config/{CONFIG_FILE} ]").ok
    before = ctx.stick.text(sums)
    size = int(ctx.stick.run(f"wc -c < /config/{CONFIG_FILE}").out.strip() or 0) if ours else 0
    ctx.stick.run("onu-config save", timeout=60, check=True)
    after = ctx.stick.text(sums)
    ok = mount[2] == "jffs2" and ours and 0 < size <= CONFIG_LIMIT and before == after
    moved = len(set(before.splitlines()) ^ set(after.splitlines()))
    same = "as they were" if before == after else f"CHANGED ({moved} digests differ)"
    return verdict(ok, f"{mount[2]} ({mount[3].split(',')[0]}); the shipped firmware's {shipped} "
                       f"files {same} after a save; this system's file {size} B of {CONFIG_LIMIT}")


# The most a file system in memory may be, by its mount point, in KiB; "size=" in the
# options of /proc/mounts is what keeps a runaway log from filling the stick's memory.
MEMORY_LIMITS = {"/tmp": 2048, "/run": 4096, "/var": 1024}
# The system log rotates past this many KiB (the boot script's syslogd -s), and the OMCI
# daemon cuts its own log past 256 KiB (omcid/src/sys/log.c).
MESSAGES_KIB = 64
OMCID_LOG_KIB = 256
# Lines of about 100 bytes sent to the system log to pass its limit three times over.
LOG_LINES = 2000


@case("system.log-limits", "System",
      "The file systems in memory have limits, the system log rotates past its own, and the "
      "OMCI daemon's log stays under its")
def log_limits(ctx: Context):
    sizes = {}
    for line in ctx.stick.text("cat /proc/mounts").splitlines():
        fields = line.split()
        if len(fields) > 3 and fields[1] in MEMORY_LIMITS:
            match = re.search(r"size=(\d+)k", fields[3])
            sizes[fields[1]] = int(match[1]) if match else None
    unbounded = [p for p in MEMORY_LIMITS if sizes.get(p) is None]
    over = [p for p, limit in MEMORY_LIMITS.items() if (sizes.get(p) or 0) > limit]
    ctx.stick.run("awk 'BEGIN { for (i = 0; i < %d; i++) print \"line \" i \" \" "
                  "\"0123456789012345678901234567890123456789012345678901234567890123456789"
                  "01234567890123456789\" }' | logger -t devtest" % LOG_LINES, timeout=120)
    time.sleep(2)
    current = int(ctx.stick.text("wc -c < /run/messages")) // 1024
    older = ctx.stick.run("wc -c < /run/messages.0")
    rotated = int(older.out.strip() or 0) // 1024 if older.ok else None
    omcid = int(ctx.stick.run("wc -c < /run/omcid.log").out.strip() or 0) // 1024
    ok = (not unbounded and not over and current <= MESSAGES_KIB + 4 and rotated is not None
          and rotated >= MESSAGES_KIB - 4 and omcid <= OMCID_LOG_KIB)
    shown = ", ".join(f"{p} {sizes[p]} KiB" if sizes.get(p) else f"{p} unbounded"
                      for p in MEMORY_LIMITS)
    return verdict(ok, f"{shown}; after {LOG_LINES} lines the log is {current} KiB and "
                       f"{'a rotated copy ' + str(rotated) + ' KiB' if rotated is not None else 'not rotated'}"
                       f" (limit {MESSAGES_KIB}); OMCI log {omcid} KiB (cut at {OMCID_LOG_KIB})")


# Seconds a program has to be running again after it is killed: procd waits two to start
# it again and the report of its state comes over ssh.
RESTART_WAIT = 40
# Seconds the stick has to be at O5 and omcid's and dhcp6d's objects back on ubus: they try
# again every ten seconds. A session that was cut off without a goodbye has longer: the
# concentrator holds the old one until its keepalive runs out.
RECOVER_WAIT = 120
SESSIONS_WAIT = 300
# The order the programs are killed in: the small ones first, the ones that the others hang
# from last, ubusd the very last.
KILL_ORDER = ("syslogd", "klogd", "act", "ntp", "trials", "udhcpd", "dhcp6d", "pond", "omcid",
              "ssh", "pppoed", "netifd", "ubus")


def procd_instances(ctx: Context) -> dict[str, tuple[bool, int]] | None:
    """procd's instances, "service/instance": running, and the pid. None when procd, or ubusd
    that it is asked through, does not answer."""
    reply = ctx.stick.run("ubus call service list", timeout=30)
    if not reply.ok:
        return None
    try:
        data = json.loads(reply.out)
    except ValueError:
        return None
    return {f"{svc}/{name}": (bool(inst.get("running")), int(inst.get("pid", 0)))
            for svc, body in data.items() for name, inst in body.get("instances", {}).items()}


def pppoed_pids(ctx: Context) -> dict[str, int]:
    """The pppoed of each session netifd runs: session number and pid."""
    found = {}
    for line in ctx.stick.run("ps").out.splitlines():
        match = re.match(r"\s*(\d+)\s.*pppoed up .* -f /run/onu-pppoe(\d)\b", line)
        if match:
            found[match[2]] = int(match[1])
    return found


def sessions_state(ctx: Context) -> tuple[int, int]:
    """How many of the stick's PPPoE sessions netifd has up, and how many it holds."""
    numbers = sorted({k.split(".")[1] for k in kv(ctx.stick.run("onu-apply state").out)
                      if re.fullmatch(r"session\.\d+\.dev", k)})
    up = 0
    for n in numbers:
        reply = ctx.stick.run(f"ubus call network.interface.ses{n} status", timeout=30)
        try:
            up += bool(reply.ok and json.loads(reply.out).get("up"))
        except ValueError:
            pass
    return up, len(numbers)


def objects_on_ubus(ctx: Context) -> set[str]:
    return {l for l in ctx.stick.run("ubus list", timeout=30).out.split()
            if l == "omcid" or l.startswith("dhcp6d.")}


def wait_for(ctx: Context, seconds: float, ready) -> float | None:
    """Seconds until ready() holds, or None when it does not in time."""
    start = time.monotonic()
    while time.monotonic() - start < seconds:
        try:
            if ready():
                return time.monotonic() - start
        except (ValueError, KeyError, RemoteError):
            pass
        time.sleep(2)
    return None


@case("system.restart", "System",
      "Every program that procd keeps, and each session's pppoed under netifd, is running again "
      "after kill -9, and the stick is back at O5 with its sessions and its objects on ubus",
      destructive=True, tools=("ubus",))
def restart(ctx: Context):
    from .cases_pon import onu_state, pon
    before = procd_instances(ctx)
    if not before:
        return failed("procd's list of services cannot be read")
    sessions = sessions_state(ctx)[0]
    objects = objects_on_ubus(ctx)
    pppoed = pppoed_pids(ctx)

    def program(key: str, kind: str) -> str:
        """The name that KILL_ORDER knows a target by."""
        if kind == "pppoed":
            return "pppoed"
        service, _, instance = key.partition("/")
        return re.sub(r"(-.*|\d+)$", "", instance if service == "onu" else service)

    def rank(target) -> tuple[int, str]:
        word = program(target[0], target[1])
        return (KILL_ORDER.index(word) if word in KILL_ORDER else len(KILL_ORDER), target[0])

    targets = [(key, "service", pid) for key, (running, pid) in before.items() if running]
    targets += [(f"pppoed {n}", "pppoed", pid) for n, pid in pppoed.items()]
    targets.sort(key=rank)
    back, stuck = {}, []
    for key, kind, pid in targets:
        ctx.stick.run(f"kill -9 {pid}")
        if kind == "service":
            def running_again(key=key, pid=pid):
                now = procd_instances(ctx)
                return bool(now) and now.get(key, (False, 0))[0] and now[key][1] != pid
        else:
            n = key.split()[1]

            def running_again(n=n, pid=pid):
                return pppoed_pids(ctx).get(n, pid) != pid
        took = wait_for(ctx, RESTART_WAIT, running_again)
        if took is None:
            stuck.append(key)
        else:
            back[key] = took
    o5 = wait_for(ctx, RECOVER_WAIT, lambda: onu_state(pon(ctx)) == 5)
    again = wait_for(ctx, SESSIONS_WAIT, lambda: sessions_state(ctx)[0] >= sessions)
    published = wait_for(ctx, RECOVER_WAIT, lambda: objects <= objects_on_ubus(ctx))
    ok = not stuck and o5 is not None and again is not None and published is not None
    slowest = max(back.items(), key=lambda item: item[1], default=("-", 0.0))
    parts = [f"{len(back)} of {len(targets)} killed programs running again, the slowest "
             f"{slowest[0]} after {slowest[1]:.0f} s"]
    if stuck:
        parts.append("not back: " + ", ".join(stuck))
    parts.append("O5 again" if o5 is not None else "NOT at O5")
    parts.append(f"{sessions} session(s) up again after {again:.0f} s" if again is not None
                 else "sessions NOT up again")
    if objects:
        parts.append(f"{', '.join(sorted(objects))} on ubus after {published:.0f} s"
                     if published is not None else "objects on ubus NOT back")
    return verdict(ok, "; ".join(parts))

