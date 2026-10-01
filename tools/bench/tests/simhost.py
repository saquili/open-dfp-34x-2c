# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""A command-runner double that behaves like an image host and records every command.

It answers the ``ss``, ``nft``, ``scp``, ``sudo``, ``mkdir``, ``mv``, ``stat``
and ``sh`` commands the tool sends, over ssh or locally, and keeps every
program line it was given, so a test can assert that nothing but the one
allowance rule was added and that no interface was touched.
"""

from __future__ import annotations

import re
import shlex
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field

from bench.host import RunResult


@dataclass
class Rule:
    handle: int
    interface: str
    port: int
    tag: str


class SimProcess:
    """A process the tool started and does not wait for."""

    def __init__(self, argv: Sequence[str], exit_after: float | None, clock) -> None:
        self.argv = list(argv)
        self.clock = clock
        self.started = clock.now() if clock is not None else 0.0
        self.exit_after = exit_after
        self.terminated = False

    def poll(self) -> int | None:
        if self.terminated:
            return -15
        if self.exit_after is not None and self.clock is not None \
                and self.clock.now() - self.started >= self.exit_after:
            return 255
        return None

    def terminate(self) -> None:
        self.terminated = True


@dataclass
class SimImageHost:
    """The state of an image host."""

    files: dict[str, bytes] = field(default_factory=dict)
    directories: set[str] = field(default_factory=set)
    listening: dict[int, str] = field(default_factory=dict)  # port -> "ours" or "foreign"
    rules: list[Rule] = field(default_factory=list)
    chain_exists: bool = True
    nft_add_fails: bool = False
    scp_fails: bool = False
    start_never_listens: bool = False
    next_handle: int = 4
    launched: list[str] = field(default_factory=list)


class SimRunner:
    """A ``Runner`` double; ``board`` is called for spawned ssh calls to the board."""

    def __init__(self, host: SimImageHost | None = None, clock=None) -> None:
        self.host = host or SimImageHost()
        self.clock = clock
        self.calls: list[list[str]] = []
        self.spawned: list[SimProcess] = []
        self.on_spawn: Callable[[Sequence[str]], float | None] | None = None
        self.board_run: Callable[[Sequence[str]], RunResult | None] | None = None

    # -- the Runner interface --------------------------------------------------

    def run(self, argv: Sequence[str], *, timeout: float = 30.0) -> RunResult:
        line = list(argv)
        self.calls.append(line)
        if line[0] == "ssh":
            if self.board_run is not None and line[-2].startswith("root@"):
                answer = self.board_run(line)
                if answer is not None:
                    return answer
            return self._remote(shlex.split(line[-1]))
        if line[0] == "scp":
            return self._scp(line)
        return self._remote(line)

    def spawn(self, argv: Sequence[str]) -> SimProcess:
        self.calls.append(list(argv))
        exit_after = self.on_spawn(argv) if self.on_spawn else None
        process = SimProcess(argv, exit_after, self.clock)
        self.spawned.append(process)
        return process

    # -- what the host does ----------------------------------------------------

    def _scp(self, line: list[str]) -> RunResult:
        if self.host.scp_fails:
            return RunResult(1, "", "scp: connection refused")
        local, remote = line[-2], line[-1].split(":", 1)[1]
        with open(local, "rb") as handle:
            self.host.files[remote] = handle.read()
        return RunResult(0)

    def _remote(self, words: list[str]) -> RunResult:
        host = self.host
        if words[:2] == ["sudo", "-n"]:
            words = words[2:]
        program = words[0]
        if program == "mkdir":
            host.directories.add(words[-1])
            return RunResult(0)
        if program == "mv":
            host.files[words[-1]] = host.files.pop(words[-2])
            return RunResult(0)
        if program == "stat":
            path = words[-1]
            if path in host.files:
                return RunResult(0, "%d\n" % len(host.files[path]))
            return RunResult(1, "", "stat: cannot statx '%s': No such file or directory" % path)
        if program == "ss":
            lines = ["State  Recv-Q Send-Q Local Address:Port Peer Address:Port Process"]
            lines += ["UNCONN 0      0      192.0.2.1:%d     0.0.0.0:*" % port
                      for port in sorted(host.listening)]
            lines.append("UNCONN 0      0      127.0.0.53:53   0.0.0.0:*")
            return RunResult(0, "\n".join(lines) + "\n")
        if program == "nft":
            return self._nft(words[1:])
        if program == "sh":
            return self._shell(words[2])
        return RunResult(127, "", "%s: command not found" % program)

    def _nft(self, args: list[str]) -> RunResult:
        host = self.host
        if args and args[0] == "-a":
            args = args[1:]
            with_handles = True
        else:
            with_handles = False
        if args[:4] == ["list", "chain", "inet", "filter"]:
            if not host.chain_exists:
                return RunResult(1, "", "Error: No such file or directory")
            body = ["table inet filter {", "\tchain input {", "\t\ttype filter hook input priority 0;"]
            for rule in host.rules:
                text = '\t\tiifname "%s" udp dport %d accept comment "%s"' % (
                    rule.interface, rule.port, rule.tag)
                if with_handles:
                    text += " # handle %d" % rule.handle
                body.append(text)
            body += ["\t}", "}"]
            return RunResult(0, "\n".join(body) + "\n")
        if args[:5] == ["add", "rule", "inet", "filter", "input"]:
            if host.nft_add_fails or not host.chain_exists:
                return RunResult(1, "", "Error: No such file or directory")
            match = re.fullmatch(r'iifname "([^"]+)" udp dport (\d+) accept comment "([^"]+)"',
                                 " ".join(args[5:]))
            assert match, "an unexpected firewall rule: %r" % args
            host.rules.append(Rule(host.next_handle, match.group(1), int(match.group(2)),
                                   match.group(3)))
            host.next_handle += 1
            return RunResult(0)
        if args[:5] == ["delete", "rule", "inet", "filter", "input"] and args[5] == "handle":
            host.rules = [r for r in host.rules if r.handle != int(args[6])]
            return RunResult(0)
        raise AssertionError("an unexpected nft command: %r" % args)

    def _shell(self, script: str) -> RunResult:
        host = self.host
        if "nohup" in script:
            host.launched.append(script)
            port = int(re.search(r"--port (\d+)", script).group(1))  # type: ignore[union-attr]
            if not host.start_never_listens:
                host.listening[port] = "ours"
            return RunResult(0)
        if "kill" in script:
            for port in [p for p, who in host.listening.items() if who == "ours"]:
                del host.listening[port]
            return RunResult(0)
        raise AssertionError("an unexpected shell script: %r" % script)

    # -- what a test asks ------------------------------------------------------

    def programs(self) -> list[str]:
        """The program of every call, with ``ssh`` replaced by what it ran remotely."""
        names = []
        for line in self.calls:
            if line[0] == "ssh":
                words = shlex.split(line[-1])
                if words[:2] == ["sudo", "-n"]:
                    words = words[2:]
                names.append(words[0])
            else:
                names.append(line[0])
        return names
