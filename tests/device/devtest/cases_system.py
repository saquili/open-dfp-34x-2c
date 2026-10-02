# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The system: the kernel, its log, the boot, the file systems, the watchdog."""

from __future__ import annotations

import os
import re
import time

from .core import Context, case, failed, passed, verdict

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__)))))
# What the kernel prints when something went wrong in it.
TROUBLE = r"(BUG:|Oops|WARNING:|Call Trace|Kernel panic|segfault|unhandled|soft lockup|hung_task)"


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


@case("system.log", "System", "The kernel log holds no oops, warning, BUG or panic")
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
      "The root file system is read-only; /tmp, /run and /var are in memory")
def filesystems(ctx: Context):
    mounts = {}
    for line in ctx.stick.text("cat /proc/mounts").splitlines():
        dev, point, kind, options = line.split()[:4]
        mounts[point] = (kind, options.split(","))
    root = mounts.get("/", ("?", []))
    memory = {p: mounts.get(p, ("missing", []))[0] for p in ("/tmp", "/run", "/var")}
    ok = "ro" in root[1] and all(k == "tmpfs" for k in memory.values())
    return verdict(ok, f"/ {root[0]} {'ro' if 'ro' in root[1] else 'rw'}; "
                       + ", ".join(f"{p} {k}" for p, k in memory.items()))


@case("system.watchdog", "System", "The watchdog is there with its timeout")
def watchdog(ctx: Context):
    base = "/sys/class/watchdog/watchdog0"
    identity = ctx.stick.text(f"cat {base}/identity")
    timeout = int(ctx.stick.text(f"cat {base}/timeout"))
    state = ctx.stick.text(f"cat {base}/state")
    return verdict(timeout > 0, f"{identity}, timeout {timeout} s, {state}")


@case("system.watchdog-reset", "System",
      "An armed watchdog that nobody feeds resets the stick within its timeout",
      destructive=True)
def watchdog_reset(ctx: Context):
    timeout = int(ctx.stick.text("cat /sys/class/watchdog/watchdog0/timeout"))
    boot = ctx.stick.text("cat /proc/sys/kernel/random/boot_id")
    # Writing anything but the magic character and closing leaves it armed.
    ctx.stick.run("echo armed > /dev/watchdog", timeout=10)
    start = time.monotonic()
    while time.monotonic() - start < timeout + 120:
        time.sleep(5)
        reply = ctx.stick.run("cat /proc/sys/kernel/random/boot_id", timeout=10)
        if reply.ok and reply.out.strip() and reply.out.strip() != boot:
            return passed(f"timeout {timeout} s; back with a new boot after "
                          f"{time.monotonic() - start:.0f} s")
    return failed(f"no new boot within {timeout + 120} s of arming")
