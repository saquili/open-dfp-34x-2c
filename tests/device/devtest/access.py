# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The two machines a device test talks to, and what may be written down.

The stick is reached over ssh through the bench host, which also holds the far
ends of both of the stick's sides: the subscriber side, where the stick's host
link ends, and the network side, behind the OLT. Its host key is made again at
every boot, so it is not checked. Commands run with the shell of the remote end
and come back as a Reply.

Whatever a test keeps, its evidence and its log, passes through redact(), which
takes out what names one unit or one bench: MAC addresses, GPON serial numbers,
and the names and addresses that the settings give for the bench host and the
stick.
"""

from __future__ import annotations

import dataclasses
import ipaddress
import re
import shlex
import subprocess
import time
from typing import Sequence

MAC = re.compile(r"\b[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}\b")
# A GPON serial number: the four letters of the vendor ID, then eight hex
# digits. A run of twelve hex digits alone is too common to be told apart.
SERIAL = re.compile(r"\b[A-Z]{4}[0-9A-Fa-f]{8}\b")


@dataclasses.dataclass(frozen=True)
class Settings:
    bench: str
    stick: str
    bench_sudo: bool = True
    olt_port: str = ""
    subscriber_port: str = ""
    pond_token: str = ""
    ssh_options: tuple[str, ...] = ()
    reload: str = ""
    # A build's diag directory, which has the tools that a case needs and the
    # stick lacks (tools.py).
    tools: str = ""


@dataclasses.dataclass
class Reply:
    argv: list[str]
    code: int
    out: str
    err: str
    seconds: float

    @property
    def ok(self) -> bool:
        return self.code == 0


# An IPv4 address; the documentation ranges, masks and the special addresses
# name no bench and stay.
ADDRESS = re.compile(r"(?<![\w.])\d{1,3}(?:\.\d{1,3}){3}(?![\w]|\.\d)")
KEPT = [ipaddress.ip_network(n) for n in ("192.0.2.0/24", "198.51.100.0/24",
                                          "203.0.113.0/24", "0.0.0.0/8", "127.0.0.0/8",
                                          "224.0.0.0/4", "255.255.255.255/32")]


def address(match: re.Match) -> str:
    try:
        ip = ipaddress.IPv4Address(match.group(0))
    except ValueError:
        return match.group(0)
    mask = int(ip)
    if any(ip in net for net in KEPT) or (mask and (mask | (mask - 1)) == 0xffffffff):
        return match.group(0)
    return "<address>"


# An IPv6 address; a link-local one holds its interface's MAC address. The
# documentation prefix, multicast, the loopback and the unspecified address
# name no unit and stay.
ADDRESS6 = re.compile(r"(?<![\w:.])(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4}(?![\w:.])")
KEPT6 = [ipaddress.ip_network(n) for n in ("2001:db8::/32", "ff00::/8")]


def address6(match: re.Match) -> str:
    try:
        ip = ipaddress.IPv6Address(match.group(0))
    except ValueError:
        return match.group(0)
    if ip.is_loopback or ip.is_unspecified or any(ip in net for net in KEPT6):
        return match.group(0)
    return "<address6>"


def redact(text: str, settings: Settings | None = None) -> str:
    text = MAC.sub("<mac>", text)
    text = SERIAL.sub("<serial>", text)
    if settings and settings.stick:
        text = text.replace(settings.stick, "<stick>")
    text = ADDRESS.sub(address, text)
    text = ADDRESS6.sub(address6, text)
    if settings:
        for value, name in ((settings.pond_token, "<token>"), (settings.bench, "<bench>"),
                            (settings.stick, "<stick>")):
            if value:
                text = text.replace(value, name)
    return text


class Remote:
    """One end that runs shell commands: the stick or the bench host."""

    def __init__(self, name: str, ssh: Sequence[str], log: list[str]):
        self.name = name
        self.ssh = list(ssh)
        self.log = log
        # Run before each command, not recorded with it: the path to tools
        # that were put on the stick for the run (tools.py).
        self.prefix = ""

    def run(self, command: str, *, timeout: float = 60.0, check: bool = False,
            stdin: str | bytes | None = None) -> Reply:
        argv = self.ssh + [self.prefix + command]
        start = time.monotonic()
        binary = isinstance(stdin, bytes)
        try:
            done = subprocess.run(argv, capture_output=True, timeout=timeout, input=stdin,
                                  stdin=None if stdin is not None else subprocess.DEVNULL,
                                  **({} if binary else {"text": True, "errors": "replace"}))
            out, err = (done.stdout, done.stderr) if not binary else (
                done.stdout.decode(errors="replace"), done.stderr.decode(errors="replace"))
            reply = Reply(argv, done.returncode, out, err, time.monotonic() - start)
        except subprocess.TimeoutExpired as exc:
            out = exc.stdout.decode(errors="replace") if isinstance(exc.stdout, bytes) else ""
            reply = Reply(argv, 124, out, "timed out", time.monotonic() - start)
        self.log.append(f"{self.name}$ {command}\n{reply.out}{reply.err}"
                        f"[exit {reply.code}, {reply.seconds:.2f} s]\n")
        if check and not reply.ok:
            raise RemoteError(f"{self.name}: {command!r} exited {reply.code}: "
                              f"{reply.err.strip() or reply.out.strip()}")
        return reply

    def text(self, command: str, *, timeout: float = 60.0) -> str:
        return self.run(command, timeout=timeout, check=True).out.strip()


class RemoteError(Exception):
    pass


def ssh_base(settings: Settings) -> list[str]:
    return ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", "-o", "LogLevel=ERROR",
            *settings.ssh_options]


def bench_remote(settings: Settings, log: list[str]) -> Remote:
    argv = ssh_base(settings) + ["-o", "RequestTTY=no", "-o", "RemoteCommand=none",
                                 settings.bench]
    return Remote("bench", argv, log)


def stick_remote(settings: Settings, log: list[str]) -> Remote:
    argv = ssh_base(settings) + ["-o", "StrictHostKeyChecking=no",
                                 "-o", "UserKnownHostsFile=/dev/null",
                                 "-J", settings.bench, f"root@{settings.stick}"]
    return Remote("stick", argv, log)


def sudo(settings: Settings, command: str) -> str:
    if not settings.bench_sudo:
        return command
    return "sudo -n sh -c " + shlex.quote(command)
