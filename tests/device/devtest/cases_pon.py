# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The PON side: downstream lock, activation, the OMCI channel, what the OLT
provisioned, and the optics."""

from __future__ import annotations

import math
import re
import time

from .core import Context, case, failed, passed, verdict

DEBUGFS = "/sys/kernel/debug"


def debugfs(ctx: Context, name: str) -> str:
    if not ctx.memo.get("debugfs"):
        ctx.stick.run(f"grep -q ' {DEBUGFS} ' /proc/mounts || "
                      f"mount -t debugfs debugfs {DEBUGFS}", check=True)
        ctx.memo["debugfs"] = True
    return ctx.stick.text(f"cat {DEBUGFS}/{name}")


def gpon_field(text: str, line: str, field: str) -> int:
    match = re.search(rf"^{line}\b.*?\b{field} (\d+)", text, re.M)
    if not match:
        raise ValueError(f"no {field} on the {line} line")
    return int(match.group(1))


def superframes(text: str) -> int:
    match = re.search(r"^frame (\d+)", text, re.M)
    if not match:
        raise ValueError("no frame line")
    return int(match.group(1))


@case("gpon.downstream", "GPON", "Downstream is locked: 8000 superframes a second, no LOS or LOF")
def downstream(ctx: Context):
    first = debugfs(ctx, "rtl9602c/gpon")
    start = time.monotonic()
    time.sleep(2)
    second = debugfs(ctx, "rtl9602c/gpon")
    rate = (superframes(second) - superframes(first)) / (time.monotonic() - start)
    los, lof = gpon_field(second, "ds", "los"), gpon_field(second, "ds", "lof")
    # The two reads are a round trip apart, so the rate is close to, not at, 8000.
    return verdict(7000 < rate < 9000 and not los and not lof,
                   f"{rate:.0f} superframes/s, los {los}, lof {lof}")


@case("gpon.activation", "GPON", "The ONU is in O5, operation")
def activation(ctx: Context):
    text = debugfs(ctx, "rtl9602c/gpon")
    state = gpon_field(text, "onu", "state")
    onu_id = gpon_field(text, "onu", "id")
    return verdict(state == 5, f"O{state}, ONU-ID {onu_id}")


@case("gpon.stable", "GPON", "The ONU stays in O5 for a minute")
def stable(ctx: Context):
    seen = []
    for _ in range(12):
        seen.append(gpon_field(debugfs(ctx, "rtl9602c/gpon"), "onu", "state"))
        time.sleep(5)
    left = [s for s in seen if s != 5]
    return verdict(not left, f"{len(seen)} reads 5 s apart: "
                             + ("all O5" if not left else " ".join(f"O{s}" for s in seen)))


@case("omci.channel", "OMCI", "OMCI messages flow both ways and none is dropped")
def channel(ctx: Context):
    base = "/sys/class/pon/pon0omci0/statistics"
    values = {}
    for name in ("rx_messages", "rx_dropped", "tx_messages", "tx_errors"):
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


@case("omci.gem-bridge", "OMCI",
      "The GEM ports the OLT created are interfaces, bridged with the host link")
def gem_bridge(ctx: Context):
    gems = [l.split(":")[1].strip().split("@")[0]
            for l in ctx.stick.text("ip -o link show type gem").splitlines()]
    ports = ctx.stick.run("ls /sys/class/net/omcibr0/brif").out.split()
    bridged = [g for g in gems if g in ports]
    ok = "sfp" in ports and bool(bridged)
    return verdict(ok, f"{len(gems)} GEM interface(s); omcibr0 holds {' '.join(ports) or 'nothing'}")


def hwmon(ctx: Context, name: str) -> str:
    for path in ctx.stick.text("ls -d /sys/class/hwmon/hwmon*").split():
        if ctx.stick.run(f"cat {path}/name").out.strip() == name:
            return path
    raise ValueError(f"no hwmon device {name}")


def dbm(microwatts: int) -> str:
    return f"{10 * math.log10(microwatts / 1000):.1f} dBm" if microwatts > 0 else "dark"


@case("optics.diagnostics", "Optics",
      "Temperature, supply, bias and both optical powers read within range")
def diagnostics(ctx: Context):
    base = hwmon(ctx, "rtl9602c_optics")
    raw = ctx.stick.text(f"cd {base} && for f in *_input; do echo $f $(cat $f) "
                         f"$(cat ${{f%_input}}_label 2>/dev/null); done")
    values, labels = {}, {}
    for line in raw.splitlines():
        name, value, *label = line.split()
        values[name] = int(value)
        labels[name] = " ".join(label)
    temp = values["temp1_input"] / 1000
    vcc = values["in0_input"] / 1000
    bias = values["curr1_input"]
    tx, rx = values["power1_input"], values["power2_input"]
    ok = -10 < temp < 85 and 3.0 < vcc < 3.6 and 0 < bias < 100 and tx > 0 and rx > 0
    shown = (f"{temp:.1f} °C, {vcc:.2f} V, bias {bias} mA, "
             f"{labels.get('power1_input') or 'power1'} {dbm(tx)}, "
             f"{labels.get('power2_input') or 'power2'} {dbm(rx)}")
    return verdict(ok, shown)
