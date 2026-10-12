# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The stick's updates: an update into the other bank, started on trial and kept, and
the way back to the bank it came from."""

from __future__ import annotations

import time

from .access import RemoteError
from .core import Context, case, failed, skipped, verdict
from .cases_system import kv

# Seconds the stick may be gone after a restart: the boot loader counts down, starts the
# bank, the bank starts and the PON comes up.
BOOT_WAIT = 600
# Seconds a tried bank has to commit itself: onu-trial waits 300 s for the system to be up.
COMMIT_WAIT = 400
# Seconds the write of the update file into the bank may take, streamed over ssh.
WRITE_WAIT = 300


def banks(ctx: Context) -> dict[str, str]:
    """onu-update's report of the banks: RUNNING, COMMITTED, BANKn_STATE ..."""
    return kv(ctx.stick.text("onu-update status"))


def boot_id(ctx: Context) -> str:
    reply = ctx.stick.run("cat /proc/sys/kernel/random/boot_id", timeout=10)
    return reply.out.strip() if reply.ok else ""


def restart_into(ctx: Context, bank: str) -> tuple[bool, float, str]:
    """Restart the stick, which the loader starts in the bank it was told, and wait until the
    bank runs and has committed itself: whether it did, how long that took, and what the stick
    says if it did not."""
    before = boot_id(ctx)
    start = time.monotonic()
    ctx.stick.run("reboot", timeout=20)
    seen = False
    status: dict[str, str] = {}
    while time.monotonic() - start < BOOT_WAIT + COMMIT_WAIT:
        time.sleep(5)
        now = boot_id(ctx)
        if not now or now == before:
            continue
        seen = True
        try:
            status = banks(ctx)
        except RemoteError:
            continue
        if status.get("RUNNING") == bank and status.get("COMMITTED") == bank:
            return True, time.monotonic() - start, ""
        if status.get("RUNNING") not in (None, bank):
            return False, time.monotonic() - start, f"the stick came back in bank {status['RUNNING']}"
    return False, time.monotonic() - start, ("never saw the stick come back" if not seen else
                                              f"bank {bank} did not commit: {status.get('RUNNING')}/{status.get('COMMITTED')}")


@case("update.trial-and-back", "Updates",
      "An update written into the other bank starts there on trial and is kept, and the stick "
      "goes back to the bank it came from by the same way",
      destructive=True)
def trial_and_back(ctx: Context):
    s = ctx.settings
    if not s.update:
        return skipped("no update file: give --update a build's update.bin")
    with open(s.update, "rb") as handle:
        data = handle.read()
    status = banks(ctx)
    running = status.get("RUNNING", "-1")
    if running not in ("0", "1"):
        return skipped("the stick runs from memory: start it from the flash for this case")
    other = "1" if running == "0" else "0"
    old_state = status.get(f"BANK{other}_STATE", "")
    if old_state == "shipped" and not s.overwrite_shipped:
        return skipped(f"bank {other} holds the shipped firmware: --overwrite-shipped lets this "
                       "case write over it")
    if status.get("COMMITTED") != running:
        return failed(f"the stick runs bank {running} and bank {status.get('COMMITTED')} is committed")

    start = time.monotonic()
    ctx.stick.run(f"onu-update write - {other}", stdin=data, timeout=WRITE_WAIT, check=True)
    wrote = time.monotonic() - start
    written = banks(ctx)
    if written.get(f"BANK{other}_STATE") != "valid":
        return failed(f"bank {other} reads {written.get(f'BANK{other}_STATE')} after the write")
    ctx.stick.run(f"onu-update try {other}", check=True)
    ok, to_new, why = restart_into(ctx, other)
    if not ok:
        return failed(f"written into bank {other} in {wrote:.0f} s; on trial: {why}")
    ctx.stick.run(f"onu-update try {running}", check=True)
    ok, to_old, why = restart_into(ctx, running)
    if not ok:
        return failed(f"bank {other} kept after {to_new:.0f} s; back to bank {running}: {why}")
    now = banks(ctx)
    return verdict(now.get("RUNNING") == running and now.get("COMMITTED") == running,
                   f"written into bank {other} ({len(data)} B, was {old_state or 'unknown'}) in "
                   f"{wrote:.0f} s; on trial it ran and committed after {to_new:.0f} s "
                   f"(version {written.get(f'BANK{other}_VERSION', '?')}); back in bank {running}, "
                   f"committed, after {to_old:.0f} s")
