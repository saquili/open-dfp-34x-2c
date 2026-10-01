# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The hosts the tool talks to besides the console: the image host and the board.

The **image host** carries the SFP cage that the board sits in and serves the
image over TFTP; it may be the machine the tool runs on. Three things must
be true on it at every run, and none of them survives a reboot of it: the
served file exists, the server runs, and the firewall lets UDP port 69 in on
the cage's interface. Each fails quietly, as ``T`` marks or a TFTP error far
from the cause, so the tool checks all three every time and says which one was
missing. The server's source is pushed at every run and the server restarted
from it, never trusted from an earlier copy.

The tool does only what that needs. It copies one file, replaces it by a
rename so that the loader never reads a half-written file, asks the sockets
(not the process list, whose pattern match finds its own command line) whether
anything listens on UDP 69, and adds, at most once, the one allowance rule
for that port with a comment that names it so it can be found and removed. It
never brings the cage's interface up or down, changes its address, restarts
its network service or touches any other firewall rule: on the bench's image
host that link is the only way in.

The **board** is reached over ssh for the one thing the loader's prompt
cannot do from a running kernel: restart it. Every host command goes through
a ``Runner``, so tests replace the processes with a recording double.
"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
from collections.abc import Sequence
from dataclasses import dataclass, field
from typing import Protocol

from .console import BenchError, Clock, SystemClock, ToolLog

SSH_REMOTE_OPTIONS = ("-o", "RemoteCommand=none", "-o", "RequestTTY=no")
# Options of the restart call: it never ends from the far end, because the
# board dies under the session, so every wait is bounded. The board's host key
# changes at every RAM boot.
SSH_BOARD_OPTIONS = ("-o", "ConnectTimeout=6", "-o", "BatchMode=yes",
                     "-o", "ServerAliveInterval=3", "-o", "ServerAliveCountMax=2",
                     "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                     "-o", "LogLevel=ERROR")

_SAFE_PATH = re.compile(r"/[A-Za-z0-9._/-]+")
_SAFE_WORD = re.compile(r"[A-Za-z0-9._-]{1,64}")
_ADDRESS = re.compile(r"\d{1,3}(?:\.\d{1,3}){3}")


class HostError(BenchError):
    """The image host is not in the state a run needs."""


@dataclass(frozen=True)
class RunResult:
    returncode: int
    stdout: str = ""
    stderr: str = ""


class Spawned(Protocol):
    """A started process that the tool does not wait for."""

    def poll(self) -> int | None: ...

    def terminate(self) -> None: ...


class Runner(Protocol):
    """How the tool starts host programs."""

    def run(self, argv: Sequence[str], *, timeout: float = 30.0) -> RunResult: ...

    def spawn(self, argv: Sequence[str]) -> Spawned: ...


class SubprocessRunner:
    """Runs programs on this machine."""

    def run(self, argv: Sequence[str], *, timeout: float = 30.0) -> RunResult:
        try:
            proc = subprocess.run(list(argv), capture_output=True, text=True, timeout=timeout,
                                  stdin=subprocess.DEVNULL, check=False)
        except FileNotFoundError:
            return RunResult(127, "", "%s: not found" % argv[0])
        except subprocess.TimeoutExpired:
            return RunResult(124, "", "%s: no answer within %.0f s" % (argv[0], timeout))
        return RunResult(proc.returncode, proc.stdout, proc.stderr)

    def spawn(self, argv: Sequence[str]) -> Spawned:
        return subprocess.Popen(list(argv), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)


@dataclass
class HostConfig:
    """Where the image host is and where its things live."""

    ssh: str = "local"  # an ssh destination, or ``local`` for this machine
    ssh_options: tuple[str, ...] = ()  # for example a jump host
    served_dir: str = "/tmp/bench/images"  # holds only the images
    work_dir: str = "/tmp/bench"  # the server's source, pid file and log
    interface: str = "eth1"  # the cage's interface
    bind: str = "192.0.2.1"  # the cage interface's address; the server binds only this
    port: int = 69
    rule_tag: str = "bench-tftp"  # the comment that names the allowance rule
    python: str = "python3"
    sudo: bool = True  # port 69 and the firewall need root

    def validate(self) -> None:
        for label, path in (("served_dir", self.served_dir), ("work_dir", self.work_dir)):
            if _SAFE_PATH.fullmatch(path) is None or ".." in path.split("/"):
                raise HostError("%s %r must be an absolute path of plain characters"
                                % (label, path))
        for label, word in (("interface", self.interface), ("rule_tag", self.rule_tag)):
            if _SAFE_WORD.fullmatch(word) is None:
                raise HostError("%s %r holds characters other than letters, digits . _ -"
                                % (label, word))
        if _ADDRESS.fullmatch(self.bind) is None:
            raise HostError("bind %r is not an IPv4 address" % self.bind)


def served_name_ok(name: str) -> bool:
    """Is this a file name the server will serve?"""
    return _SAFE_WORD.fullmatch(name) is not None and not name.startswith(".")


@dataclass(frozen=True)
class HostStatus:
    """What ``ImageHost.check`` found, each of the three things on its own."""

    file_size: int | None
    listening: bool
    rule: str  # "present", "absent" or "unknown"

    def problems(self, name: str) -> list[str]:
        found = []
        if self.file_size is None:
            found.append("the served file %s does not exist on the image host" % name)
        if not self.listening:
            found.append("nothing listens on the TFTP port of the image host")
        if self.rule == "absent":
            found.append("the firewall allowance for UDP 69 is absent")
        return found


class ImageHost:
    """The image host, local or over ssh."""

    def __init__(self, config: HostConfig, runner: Runner, log: ToolLog | None = None,
                 clock: Clock | None = None) -> None:
        config.validate()
        self.config = config
        self.runner = runner
        self.log = log or ToolLog()
        self.clock = clock or SystemClock()
        self.is_local = config.ssh == "local"

    # -- running things --------------------------------------------------------

    def command(self, argv: Sequence[str], *, privileged: bool = False) -> list[str]:
        """The program line that runs ``argv`` on the image host."""
        full = (["sudo", "-n"] if privileged and self.config.sudo else []) + list(argv)
        if self.is_local:
            return full
        text = " ".join(shlex.quote(word) for word in full)
        return ["ssh", *SSH_REMOTE_OPTIONS, *self.config.ssh_options, self.config.ssh, text]

    def run(self, argv: Sequence[str], *, privileged: bool = False,
            timeout: float = 30.0) -> RunResult:
        line = self.command(argv, privileged=privileged)
        self.log.event("host", " ".join(argv))
        return self.runner.run(line, timeout=timeout)

    def _must(self, argv: Sequence[str], what: str, *, privileged: bool = False) -> RunResult:
        result = self.run(argv, privileged=privileged)
        if result.returncode != 0:
            raise HostError("%s failed on the image host: %s"
                            % (what, result.stderr.strip() or "exit status %d" % result.returncode))
        return result

    # -- the served file -------------------------------------------------------

    def push(self, local_path: str, remote_dir: str, name: str) -> int:
        """Copy a file into a directory of the image host and return its size there.

        The copy is made to ``<name>.part`` and renamed, so that a transfer
        that starts meanwhile never reads half a file; a file of the same name
        is replaced. The size on the host is compared with the local size.
        """
        if not served_name_ok(name):
            raise HostError("%r is not a plain file name" % name)
        local_size = os.path.getsize(local_path)
        part, final = "%s/%s.part" % (remote_dir, name), "%s/%s" % (remote_dir, name)
        if self.is_local:
            os.makedirs(remote_dir, exist_ok=True)
            shutil.copyfile(local_path, part)
            os.replace(part, final)
            size = os.path.getsize(final)
        else:
            self._must(["mkdir", "-p", remote_dir], "creating %s" % remote_dir)
            copy = self.runner.run(["scp", "-q", *SSH_REMOTE_OPTIONS, *self.config.ssh_options,
                                    local_path, "%s:%s" % (self.config.ssh, part)], timeout=120.0)
            self.log.event("host", "scp %s to %s" % (os.path.basename(local_path), part))
            if copy.returncode != 0:
                raise HostError("copying %s to the image host failed: %s"
                                % (os.path.basename(local_path), copy.stderr.strip()))
            self._must(["mv", "-f", part, final], "renaming %s" % name)
            size = self.file_size(final) or -1
        if size != local_size:
            raise HostError("%s is %d bytes on the image host, %d here"
                            % (name, size, local_size))
        return size

    def file_size(self, path: str) -> int | None:
        """The size of a regular file on the image host, or None if there is none."""
        if self.is_local:
            return os.path.getsize(path) if os.path.isfile(path) else None
        result = self.run(["stat", "-c", "%s", path])
        if result.returncode != 0:
            return None
        text = result.stdout.strip()
        return int(text) if text.isdigit() else None

    def push_image(self, local_path: str, name: str) -> int:
        """Make an image available under ``name`` in the served directory."""
        return self.push(local_path, self.config.served_dir, name)

    # -- the server ------------------------------------------------------------

    def listening(self) -> bool:
        """Does anything listen on the TFTP port? The sockets are asked, not the process list."""
        result = self.run(["ss", "-lun"])
        if result.returncode != 0:
            raise HostError("ss -lun failed on the image host: %s" % result.stderr.strip())
        pattern = re.compile(r":%d\s" % self.config.port)
        return any(pattern.search(line) for line in result.stdout.splitlines())

    def _pidfile(self) -> str:
        return "%s/tftp.pid" % self.config.work_dir

    def stop_script(self) -> str:
        """The shell text that stops the server named by the pid file.

        A pid file can outlive its process, and the number can then belong to
        something else that runs as root. The process is signalled only when
        its command line names the server's own script.
        """
        pidfile = shlex.quote(self._pidfile())
        marker = shlex.quote("%s/tftp.py" % self.config.work_dir)
        return ("if [ -f %(p)s ]; then pid=$(cat %(p)s); "
                "line=$({ tr '\\000' ' ' < /proc/$pid/cmdline; } 2>/dev/null "
                "|| ps -o args= -p \"$pid\" 2>/dev/null); "
                "case \"$line\" in *%(m)s*) kill \"$pid\" 2>/dev/null;; esac; "
                "rm -f %(p)s; fi; true") % {"p": pidfile, "m": marker}

    def stop_server(self) -> None:
        """Stop the server this tool started, if its pid file is there and it still runs."""
        self.run(["sh", "-c", self.stop_script()], privileged=True)

    def start_server(self, server_source: str) -> None:
        """Push the server's source and start it, detached from the session.

        A server that this tool started earlier is stopped first; something
        else listening on the port is an error, not something to kill. The
        server runs as root (the port is privileged), under ``nohup``, with
        its output in a log file, because the ssh session ends.
        """
        cfg = self.config
        script_path = "%s/tftp.py" % cfg.work_dir
        self.push(server_source, cfg.work_dir, "tftp.py")
        self._must(["mkdir", "-p", cfg.served_dir], "creating %s" % cfg.served_dir)
        self.stop_server()
        deadline = self.clock.now() + 2.0
        while self.listening():  # a server that was just stopped needs a moment to let go
            if self.clock.now() >= deadline:
                raise HostError("something else already listens on UDP port %d of the image "
                                "host; the tool will not stop it" % cfg.port)
            self.clock.sleep(0.2)
        launch = ("nohup %s %s --dir %s --bind %s --port %d --pidfile %s >> %s/tftp.log 2>&1 "
                  "< /dev/null &" % (cfg.python, script_path, cfg.served_dir, cfg.bind, cfg.port,
                                     self._pidfile(), cfg.work_dir))
        self._must(["sh", "-c", launch], "starting the TFTP server", privileged=True)
        deadline = self.clock.now() + 5.0
        while not self.listening():
            if self.clock.now() >= deadline:
                raise HostError("the TFTP server did not start listening on UDP port %d; see "
                                "%s/tftp.log on the image host" % (cfg.port, cfg.work_dir))
            self.clock.sleep(0.3)

    # -- the firewall ----------------------------------------------------------

    def _chain(self, *extra: str) -> list[str]:
        return ["nft", *extra, "list", "chain", "inet", "filter", "input"]

    def rule_present(self) -> str:
        """``present`` when the allowance's comment is in the input chain, else ``absent``;
        ``unknown`` when the chain cannot be listed."""
        result = self.run(self._chain(), privileged=True)
        if result.returncode != 0:
            return "unknown"
        return "present" if 'comment "%s"' % self.config.rule_tag in result.stdout else "absent"

    def ensure_rule(self) -> str:
        """Add the one allowance rule unless it is there; report, never raise.

        Returns ``present``, ``added`` or ``failed: <reason>``. A failure is
        not fatal: the chain belongs to the host's distribution and may not
        exist, and the transfer decides whether the packets get through.
        """
        if self.rule_present() == "present":
            return "present"
        cfg = self.config
        result = self.run(["nft", "add", "rule", "inet", "filter", "input",
                           "iifname", '"%s"' % cfg.interface, "udp", "dport", str(cfg.port),
                           "accept", "comment", '"%s"' % cfg.rule_tag], privileged=True)
        if result.returncode != 0:
            return "failed: %s" % (result.stderr.strip() or "exit status %d" % result.returncode)
        return "added"

    def remove_rule(self) -> int:
        """Remove the allowance rule by its comment; return how many rules went."""
        result = self.run(self._chain("-a"), privileged=True)
        if result.returncode != 0:
            return 0
        handles = re.findall(r'comment "%s".*# handle (\d+)' % re.escape(self.config.rule_tag),
                             result.stdout)
        for handle in handles:
            self.run(["nft", "delete", "rule", "inet", "filter", "input", "handle", handle],
                     privileged=True)
        return len(handles)

    def plan(self, server_source: str, image_path: str | None = None,
             name: str | None = None) -> list[str]:
        """The program lines a run would send to the image host, for a dry run."""
        cfg = self.config
        lines = []
        if image_path is not None and name is not None:
            part, final = "%s/%s.part" % (cfg.served_dir, name), "%s/%s" % (cfg.served_dir, name)
            if self.is_local:
                lines.append("copy %s to %s, then rename it to %s" % (image_path, part, final))
            else:
                lines.append(" ".join(self.command(["mkdir", "-p", cfg.served_dir])))
                lines.append(" ".join(["scp", "-q", *SSH_REMOTE_OPTIONS, *cfg.ssh_options,
                                       image_path, "%s:%s" % (cfg.ssh, part)]))
                lines.append(" ".join(self.command(["mv", "-f", part, final])))
        lines.append("copy %s to %s/tftp.py on the image host" % (server_source, cfg.work_dir))
        lines.append(" ".join(self.command(["ss", "-lun"])))
        lines.append(" ".join(self.command(
            ["sh", "-c", "nohup %s %s/tftp.py --dir %s --bind %s --port %d --pidfile %s/tftp.pid "
             ">> %s/tftp.log 2>&1 < /dev/null &" % (cfg.python, cfg.work_dir, cfg.served_dir,
                                                     cfg.bind, cfg.port, cfg.work_dir,
                                                     cfg.work_dir)], privileged=True)))
        lines.append(" ".join(self.command(
            ["nft", "add", "rule", "inet", "filter", "input", "iifname", '"%s"' % cfg.interface,
             "udp", "dport", str(cfg.port), "accept", "comment", '"%s"' % cfg.rule_tag],
            privileged=True)) + "   (only when the rule is not there yet)")
        return lines

    # -- all three, together ---------------------------------------------------

    def check(self, name: str) -> HostStatus:
        """Look at the served file, the server and the allowance."""
        return HostStatus(self.file_size("%s/%s" % (self.config.served_dir, name)),
                          self.listening(), self.rule_present())

    def prepare(self, image_path: str, name: str, server_source: str) -> list[str]:
        """Make a run's three conditions true; return notes for the person.

        The image is pushed under ``name``, the server restarted from the
        current source, and the allowance added. The rule is the one step
        whose failure is only reported.
        """
        self.push_image(image_path, name)
        self.start_server(server_source)
        notes = []
        outcome = self.ensure_rule()
        if outcome.startswith("failed"):
            notes.append("the firewall allowance could not be added (%s); the transfer will "
                         "show whether it was needed" % outcome[8:])
        status = self.check(name)
        problems = [p for p in status.problems(name) if "allowance" not in p]
        if problems:
            raise HostError("; ".join(problems))
        return notes


# --------------------------------------------------------------------------
# The board over ssh
# --------------------------------------------------------------------------

@dataclass
class BoardAccess:
    """How the tool reaches the running board."""

    control_address: str = "198.51.100.2"  # answers whether or not the daemon built a bridge
    data_address: str = "192.0.2.2"  # stops answering once it has
    user: str = "root"
    jump: str = ""  # an ssh destination to reach the board through (often the image host)
    jump_options: tuple[str, ...] = field(default_factory=tuple)

    def addresses(self) -> list[str]:
        """Control address first, then the data address."""
        return [self.control_address, self.data_address]


def board_command(access: BoardAccess, address: str, remote: str, *,
                  options: Sequence[str] = SSH_BOARD_OPTIONS) -> list[str]:
    """The ssh line that runs ``remote`` on the board at ``address``.

    Through a jump host, both hops get ``RemoteCommand=none`` and
    ``RequestTTY=no`` and an explicit proxy command that forwards with ``-W``:
    a ``RemoteCommand`` in the jump host's own ssh configuration would
    otherwise run instead of forwarding, which looks like a dead board.
    """
    argv = ["ssh", *SSH_REMOTE_OPTIONS, *options]
    if access.jump:
        proxy = " ".join(["ssh", *SSH_REMOTE_OPTIONS, *access.jump_options, "-W", "%h:%p",
                          access.jump])
        argv += ["-o", "ProxyCommand=" + proxy]
    argv += ["%s@%s" % (access.user, address), remote]
    return argv


SYSRQ_RESTART = "echo b > /proc/sysrq-trigger"
