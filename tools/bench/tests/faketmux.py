# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""A tmux double wired to a board and to a transcript file.

The double answers the few tmux commands the console uses. Keys typed with
``send-keys`` become bytes for the board; what the board prints is appended
to the transcript file, which the console under test reads by byte offset. A
tmux command the tool must never run against a console session -- the screen
capture, killing the session or the server, toggling the pipe -- fails the
test at once.
"""

from __future__ import annotations

import os
from collections.abc import Sequence

from bench.console import TmuxResult

FORBIDDEN = ("capture-pane", "kill-session", "kill-server", "kill-pane", "kill-window",
             "new-session", "attach-session", "show-buffer", "save-buffer", "pipe-pane -o")
KEY_BYTES = {"Enter": b"\r", "C-c": b"\x03"}


class Transcript:
    """The append-only file that the pane's output is mirrored into."""

    def __init__(self, path: str) -> None:
        self.path = path
        with open(path, "wb"):
            pass

    def emit(self, data: bytes | str) -> None:
        if isinstance(data, str):
            data = data.encode("latin-1")
        with open(self.path, "ab") as handle:
            handle.write(data)

    def size(self) -> int:
        return os.path.getsize(self.path)

    def text(self) -> str:
        with open(self.path, "rb") as handle:
            return handle.read().decode("latin-1")


class FakeTmux:
    """A session with one pane, in the state tmux would report."""

    def __init__(self, transcript: Transcript, board=None, session: str = "uart-wifi-1",
                 start_command: str = "bridge-client 192.0.2.1 2001", clock=None) -> None:
        self.transcript = transcript
        self.clock = clock
        self.board = board
        self.session = session
        self.start_command = start_command
        self.original_command = start_command
        self.session_exists = True
        self.pane_dead = False
        self.pane_pipe = True
        self.history_limit = 200000
        self.calls: list[list[str]] = []
        self.typed: list[bytes] = []
        self.typed_at: list[float] = []  # the clock's time of each entry of ``typed``
        self.respawns: list[str] = []
        self.on_respawn = None  # called with the new command

    # -- the runner ------------------------------------------------------------

    def __call__(self, args: Sequence[str]) -> TmuxResult:
        words = list(args)
        self.calls.append(words)
        if words[:1] == ["-L"]:
            raise AssertionError("a named tmux socket would hide the session from a person")
        command = words[0]
        if command in FORBIDDEN or (command == "pipe-pane" and "-o" in words):
            raise AssertionError("the tool must never run: tmux %s" % " ".join(words))
        handler = getattr(self, "_" + command.replace("-", "_"), None)
        if handler is None:
            raise AssertionError("tmux %s is not a command the console may use" % command)
        return handler(words[1:])

    def _target_ok(self, words: list[str]) -> bool:
        return "-t" in words and words[words.index("-t") + 1] == self.session

    def _has_session(self, words: list[str]) -> TmuxResult:
        if self._target_ok(words) and self.session_exists:
            return TmuxResult(0)
        return TmuxResult(1, "", "can't find session")

    def _display_message(self, words: list[str]) -> TmuxResult:
        if not (self._target_ok(words) and self.session_exists):
            return TmuxResult(1, "", "can't find session")
        text = words[-1]
        for name, value in (("pane_dead", int(self.pane_dead)), ("pane_pipe", int(self.pane_pipe)),
                            ("pane_start_command", self.start_command),
                            ("history_limit", self.history_limit)):
            text = text.replace("#{%s}" % name, str(value))
        return TmuxResult(0, text + "\n")

    def _send_keys(self, words: list[str]) -> TmuxResult:
        if not (self._target_ok(words) and self.session_exists):
            return TmuxResult(1, "", "can't find session")
        if self.pane_dead:
            return TmuxResult(1, "", "target pane has exited")
        rest = words[words.index("-t") + 2:]
        data = b""
        if rest[:2] == ["-l", "--"]:
            text = rest[2]
            if text.endswith(";"):
                text = text[:-1]  # tmux reads a trailing semicolon as a separator
            data = text.encode("latin-1")
        else:
            for key in rest:
                if key not in KEY_BYTES:
                    raise AssertionError("an unexpected key name: %r" % key)
                data += KEY_BYTES[key]
        self.typed.append(data)
        self.typed_at.append(self.clock.now() if self.clock is not None else 0.0)
        if self.board is not None:
            self.board.receive(data)
        return TmuxResult(0)

    def _pipe_pane(self, words: list[str]) -> TmuxResult:
        if self.pane_dead:
            return TmuxResult(1, "", "target pane has exited")
        self.pane_pipe = True
        return TmuxResult(0)

    def _respawn_pane(self, words: list[str]) -> TmuxResult:
        self.start_command = words[-1]
        self.respawns.append(words[-1])
        self.pane_dead = False
        self.pane_pipe = False  # a respawned pane is no longer piped until pipe-pane runs
        if self.on_respawn is not None:
            self.on_respawn(words[-1])
        return TmuxResult(0)

    def _set_option(self, words: list[str]) -> TmuxResult:
        return TmuxResult(0)

    # -- what a test asks ------------------------------------------------------

    def subcommands(self) -> list[str]:
        return [call[0] for call in self.calls]

    def all_typed(self) -> bytes:
        return b"".join(self.typed)


class ScriptedPane:
    """The smallest board: it echoes typed characters and answers Enter by its mode.

    ``prompt`` prints a loader prompt, ``login`` a login prompt, ``newline`` the
    end of the line alone (a command that is still running) and ``silent``
    nothing at all. ``echo`` maps each typed character to what the board
    echoes, so a test can drop, double or substitute one.
    """

    PROMPT = "9601D# "

    def __init__(self, transcript: Transcript, mode: str = "prompt") -> None:
        self.transcript = transcript
        self.mode = mode
        self.echo = lambda ch: ch
        self.received = bytearray()

    def receive(self, data: bytes) -> None:
        for byte in data:
            self.received.append(byte)
            if byte == 0x0D:
                self._enter()
            elif byte == 0x03:
                if self.mode == "prompt":
                    self.transcript.emit("<INTERRUPT>\r\r\n" + self.PROMPT)
            elif self.mode != "silent":
                self.transcript.emit(self.echo(chr(byte)))

    def _enter(self) -> None:
        if self.mode == "prompt":
            self.transcript.emit("\r\r\n" + self.PROMPT)
        elif self.mode == "login":
            self.transcript.emit("\r\nlogin: ")
        elif self.mode == "newline":
            self.transcript.emit("\r\r\n")
