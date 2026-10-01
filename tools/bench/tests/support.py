# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Shared helpers of the tests: temporary directories and a simulated console."""

from __future__ import annotations

import os
import tempfile
import unittest

from bench.console import Console, ConsoleConfig, ToolLog

from .faketmux import FakeTmux, ScriptedPane, Transcript
from .simclock import VirtualClock


class TempDirTestCase(unittest.TestCase):
    """A test case with a temporary directory in ``self.tmp``."""

    def setUp(self) -> None:
        handle = tempfile.TemporaryDirectory()
        self.addCleanup(handle.cleanup)
        self.tmp = handle.name


class ConsoleCase(TempDirTestCase):
    """A console over a fake tmux session and a scripted pane, on a virtual clock."""

    SESSION = "uart-wifi-1"

    def setUp(self) -> None:
        super().setUp()
        self.clock = VirtualClock()
        self.transcript = Transcript(os.path.join(self.tmp, self.SESSION + ".log"))
        self.pane = ScriptedPane(self.transcript)
        self.tmux = FakeTmux(self.transcript, self.pane, session=self.SESSION, clock=self.clock)
        self.config = ConsoleConfig(session=self.SESSION, transcript=self.transcript.path)
        self.logged: list[str] = []
        self.console = Console(self.config, self.tmux, self.clock, ToolLog(self.logged.append))
