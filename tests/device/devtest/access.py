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


def redact(text: str, settings: Settings | None = None) -> str:
    text = MAC.sub("<mac>", text)
    text = SERIAL.sub("<serial>", text)
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

    def run(self, command: str, *, timeout: float = 60.0, check: bool = False,
            stdin: str | None = None) -> Reply:
        argv = self.ssh + [command]
        start = time.monotonic()
        try:
            done = subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                                  errors="replace", input=stdin,
                                  stdin=None if stdin is not None else subprocess.DEVNULL)
            reply = Reply(argv, done.returncode, done.stdout, done.stderr,
                          time.monotonic() - start)
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
