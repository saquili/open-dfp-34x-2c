# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The system: the kernel, its log, the boot, the file systems, the watchdog."""

from __future__ import annotations

import os
import re
import subprocess
import time

from .core import Context, case, failed, passed, verdict

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
      "second")
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


@case("system.watchdog", "System", "The watchdog runs, and the daemon of the boot feeds it")
def watchdog(ctx: Context):
    base = "/sys/class/watchdog/watchdog0"
    identity = ctx.stick.text(f"cat {base}/identity")
    timeout = int(ctx.stick.text(f"cat {base}/timeout"))
    state = ctx.stick.text(f"cat {base}/state")
    daemon = ctx.stick.run("pidof watchdog").out.strip()
    return verdict(timeout > 0 and state == "active" and bool(daemon),
                   f"{identity}, timeout {timeout} s, {state}, "
                   f"daemon {'pid ' + daemon if daemon else 'not running'}")


@case("system.watchdog-reset", "System",
      "An armed watchdog that nobody feeds resets the stick within its timeout",
      destructive=True, last=True)
def watchdog_reset(ctx: Context):
    timeout = int(ctx.stick.text("cat /sys/class/watchdog/watchdog0/timeout"))
    boot = ctx.stick.text("cat /proc/sys/kernel/random/boot_id")
    # The daemon stops feeding it; writing anything but the magic character
    # and closing leaves it armed.
    ctx.stick.run("killall watchdog; sleep 1; echo armed > /dev/watchdog", timeout=20)
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
