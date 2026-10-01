# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""A virtual clock that the simulated board advances.

Every wait and every pause in the tool uses the clock it is given. In the
tests that clock is this one: ``sleep`` moves virtual time forward and runs
the events that fall due on the way, so a 255-second transfer or a 47-second
watchdog reset costs milliseconds of real time. A test that sleeps for real
is a bug.
"""

from __future__ import annotations

import heapq
import itertools
from collections.abc import Callable

from bench.console import Clock


class VirtualClock(Clock):
    """Time that only moves when the tool sleeps or a test advances it."""

    def __init__(self, start: float = 1000.0) -> None:
        self.time = start
        self._queue: list[tuple[float, int, Callable[[], None]]] = []
        self._order = itertools.count()
        self.slept = 0.0

    def now(self) -> float:
        return self.time

    def sleep(self, seconds: float) -> None:
        self.slept += max(seconds, 0.0)
        self.advance(seconds)

    def schedule(self, delay: float, event: Callable[[], None]) -> None:
        """Run ``event`` when ``delay`` seconds of virtual time have passed."""
        heapq.heappush(self._queue, (self.time + max(delay, 0.0), next(self._order), event))

    def advance(self, seconds: float) -> None:
        """Move time forward, running every event that falls due, in order."""
        target = self.time + max(seconds, 0.0)
        while self._queue and self._queue[0][0] <= target:
            when, _order, event = heapq.heappop(self._queue)
            self.time = max(self.time, when)
            event()
        self.time = target

    def pending(self) -> int:
        return len(self._queue)
