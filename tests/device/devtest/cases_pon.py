# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The PON side: downstream lock, activation, the OMCI channel, what the OLT
provisioned, and the optics."""

from __future__ import annotations

import math
import re
import time

from .core import NO_DEBUGFS, Context, case, debugfs, failed, passed, skipped, verdict

def pon(ctx: Context) -> str:
    """What the pon tool reports of the PON device and its ONU."""
    return ctx.stick.text("pon")


def onu_state(text: str) -> int:
    match = re.search(r"\bO([1-7]) [a-z]", text)
    if not match:
        raise ValueError("no ONU state in the report")
    return int(match.group(1))


def field(text: str, pattern: str) -> int:
    match = re.search(pattern, text)
    if not match:
        raise ValueError(f"no {pattern!r} in the report")
    return int(match.group(1))


def alarms(text: str) -> str:
    match = re.search(r"\balarms ([A-Za-z0-9 x]+?)(?:  |$)", text, re.M)
    if not match:
        raise ValueError("no alarms in the report")
    return match.group(1).strip()


@case("gpon.downstream", "GPON",
      "Downstream is locked: bandwidth maps keep being accepted, none with a CRC error, "
      "and no alarm")
def downstream(ctx: Context):
    # The MAC counts the maps it accepts, not every frame's map: the rate
    # depends on the OLT, so the case asks that it is not zero.
    first = pon(ctx)
    start = time.monotonic()
    time.sleep(2)
    second = pon(ctx)
    maps = r"bandwidth maps\s+(\d+)"
    crc = r"bandwidth map CRC errors\s+(\d+)"
    rate = (field(second, maps) - field(first, maps)) / (time.monotonic() - start)
    errors = field(second, crc) - field(first, crc)
    now = alarms(second)
    return verdict(rate > 0 and errors == 0 and now == "none",
                   f"{rate:.0f} bandwidth maps/s, {errors} with a CRC error, alarms {now}")


@case("gpon.activation", "GPON", "The ONU is in O5, operation")
def activation(ctx: Context):
    text = pon(ctx)
    state = onu_state(text)
    onu_id = field(text, r"ONU-ID (\d+)")
    return verdict(state == 5, f"O{state}, ONU-ID {onu_id}")


@case("gpon.stable", "GPON", "The ONU stays in O5 for a minute")
def stable(ctx: Context):
    seen = []
    for _ in range(12):
        seen.append(onu_state(pon(ctx)))
        time.sleep(5)
    left = [s for s in seen if s != 5]
    return verdict(not left, f"{len(seen)} reads 5 s apart: "
                             + ("all O5" if not left else " ".join(f"O{s}" for s in seen)))


@case("omci.channel", "OMCI", "OMCI messages flow both ways and none is dropped")
def channel(ctx: Context):
    # rx_discarded counts the messages an OLT's ending of the OMCI channel
    # took with it, which is no fault of the stick; it is reported, not judged.
    base = "/sys/class/pon/pon0omci0/statistics"
    values = {}
    for name in ("rx_messages", "rx_dropped", "rx_discarded", "tx_messages", "tx_errors"):
        values[name] = int(ctx.stick.text(f"cat {base}/{name}"))
    ok = (values["rx_messages"] > 0 and values["tx_messages"] > 0
          and values["rx_dropped"] == 0 and values["tx_errors"] == 0)
    return verdict(ok, ", ".join(f"{k} {v}" for k, v in values.items()))


@case("omci.mib-upload", "OMCI", "omcid answered the OLT's MIB upload")
def mib_upload(ctx: Context):
    lines = ctx.stick.run("grep 'mib upload:' /run/omcid.log").out.strip().splitlines()
    if not lines:
        return failed("no MIB upload in /run/omcid.log")
    return passed(f"{len(lines)} upload(s), last: {lines[-1].split('mib upload: ')[-1]}")


@case("omci.gem-ports", "OMCI",
      "The GEM ports the OLT created are interfaces, bridged with the host link, or each the "
      "one a PPPoE session of the stick runs on")
def gem_ports(ctx: Context):
    gems = [line.split()[0] for line in ctx.stick.text("pon gem").splitlines() if line.split()]
    # What the stick made of the settings, told by onu-apply: the lines name=value.
    told = dict(line.split("=", 1) for line in ctx.stick.text("onu-apply state").splitlines()
                if "=" in line)
    if told.get("mode") == "passthrough":
        sessions = {key.split(".")[1]: value for key, value in told.items()
                    if re.fullmatch(r"session\.\d+\.gem", key)}
        lost = [n for n, gem in sessions.items() if gem not in gems]
        shown = ", ".join(f"session {n} on {gem}" for n, gem in sorted(sessions.items()))
        return verdict(bool(gems) and bool(sessions) and not lost,
                       f"{len(gems)} GEM interface(s); sessions: {shown or 'none'}"
                       + (f"; no interface for session {', '.join(lost)}" if lost else ""))
    ports = ctx.stick.run("ls /sys/class/net/omcibr0/brif").out.split()
    bridged = [g for g in gems if g in ports]
    ok = "sfp" in ports and bool(bridged)
    return verdict(ok, f"{len(gems)} GEM interface(s); omcibr0 holds {' '.join(ports) or 'nothing'}")


@case("omci.tcont-map", "OMCI",
      "Each T-CONT the OLT mapped to an Alloc-ID through OMCI holds that Alloc-ID")
def tcont_map(ctx: Context):
    if not debugfs(ctx, "rtl9602c/gpon"):
        return skipped(NO_DEBUGFS)
    line = ctx.stick.run("grep '^tcont' /sys/kernel/debug/rtl9602c/gpon").out
    tconts = re.findall(r"(\d+)=(\S+)/(\S+)", line)
    if not tconts:
        return failed("the gpon view shows no T-CONTs")
    mapped = [(n, bound, alloc) for n, bound, alloc in tconts if alloc != "-"]
    ok = bool(mapped) and all(bound == alloc for _, bound, alloc in mapped)
    shown = ", ".join(f"T-CONT {n} holds {bound}, mapped to {alloc}" for n, bound, alloc in mapped)
    return verdict(ok, shown or "no T-CONT mapped")


@case("omci.alarms", "OMCI",
      "omcid raises no alarm on the clean line, with the power level the kernel reports")
def omci_alarms(ctx: Context):
    level = re.search(r"power level (\w+)", ctx.stick.run("pon").out)
    lines = ctx.stick.run("grep 'alarm:' /run/omcid.log").out.splitlines()
    raised = [line.split("alarm: ", 1)[1] for line in lines
              if "declared" in line or "notifying" in line]
    shown = f"power level {level.group(1)}" if level else "no power level reported"
    shown += f"; {len(raised)} alarm(s), the last: {raised[-1]}" if raised else "; no alarm"
    return verdict(level is not None and not raised, shown)


def hwmon(ctx: Context, name: str) -> str:
    for path in ctx.stick.text("ls -d /sys/class/hwmon/hwmon*").split():
        if ctx.stick.run(f"cat {path}/name").out.strip() == name:
            return path
    raise ValueError(f"no hwmon device {name}")


def dbm(microwatts: int) -> str:
    return f"{10 * math.log10(microwatts / 1000):.1f} dBm" if microwatts > 0 else "dark"


@case("optics.diagnostics", "Optics",
      "Temperature, supply, bias and both optical powers read within the module's warning thresholds")
def diagnostics(ctx: Context):
    base = hwmon(ctx, "gn25l95")
    raw = ctx.stick.text(f"cd {base} && for f in *_input; do c=${{f%_input}}; "
                         f"echo $c $(cat $f) $(cat ${{c}}_min) $(cat ${{c}}_max) "
                         f"$(cat ${{c}}_label 2>/dev/null); done")
    values, low, high, labels = {}, {}, {}, {}
    for line in raw.splitlines():
        name, value, lo, hi, *label = line.split()
        values[name], low[name], high[name] = int(value), int(lo), int(hi)
        labels[name] = " ".join(label)
    outside = [name for name in values if not low[name] <= values[name] <= high[name]]
    temp = values["temp1"] / 1000
    vcc = values["in0"] / 1000
    bias = values["curr1"]
    tx, rx = values["power1"], values["power2"]
    shown = (f"{temp:.1f} °C, {vcc:.2f} V, bias {bias} mA, "
             f"{labels.get('power1') or 'power1'} {dbm(tx)}, "
             f"{labels.get('power2') or 'power2'} {dbm(rx)}")
    if outside:
        shown += "; outside the warning thresholds: " + ", ".join(
            f"{name} {values[name]} not in {low[name]}..{high[name]}" for name in outside)
    return verdict(len(values) == 5 and not outside and tx > 0 and rx > 0, shown)
