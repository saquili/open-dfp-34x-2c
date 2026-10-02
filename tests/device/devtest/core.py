# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Device test cases, their outcomes and the loop that runs them.

A case checks one function of the stick and says so in one sentence. It returns
an Outcome: passed, failed or skipped, with the evidence, the measured values
that the verdict rests on, in a line short enough for a table. A case that
raises has failed, and the exception is its evidence. A case is skipped only
when what it needs is missing from the bench (no traffic generator, no token),
never because the stick misbehaved.
"""

from __future__ import annotations

import dataclasses
import time
import traceback
from typing import Callable

from .access import Remote, RemoteError, Settings, redact

PASS, FAIL, SKIP = "pass", "fail", "skip"


@dataclasses.dataclass
class Outcome:
    status: str
    evidence: str


def passed(evidence: str) -> Outcome:
    return Outcome(PASS, evidence)


def failed(evidence: str) -> Outcome:
    return Outcome(FAIL, evidence)


def skipped(evidence: str) -> Outcome:
    return Outcome(SKIP, evidence)


def verdict(ok: bool, evidence: str) -> Outcome:
    return Outcome(PASS if ok else FAIL, evidence)


@dataclasses.dataclass
class Context:
    settings: Settings
    stick: Remote
    bench: Remote
    log: list[str]
    destructive: bool = False
    memo: dict = dataclasses.field(default_factory=dict)


@dataclasses.dataclass(frozen=True)
class Case:
    id: str
    area: str
    check: str
    run: Callable[[Context], Outcome]
    destructive: bool = False


CASES: list[Case] = []


def case(id: str, area: str, check: str, *, destructive: bool = False):
    def register(fn: Callable[[Context], Outcome]) -> Callable[[Context], Outcome]:
        if any(c.id == id for c in CASES):
            raise ValueError(f"two cases named {id}")
        CASES.append(Case(id, area, check, fn, destructive))
        return fn
    return register


@dataclasses.dataclass
class Result:
    id: str
    area: str
    check: str
    status: str
    evidence: str
    seconds: float
    log: str


def run_case(c: Case, ctx: Context) -> Result:
    mark = len(ctx.log)
    start = time.monotonic()
    if c.destructive and not ctx.destructive:
        outcome = skipped("destructive: run with --destructive")
    else:
        try:
            outcome = c.run(ctx)
        except (RemoteError, AssertionError, ValueError, KeyError, IndexError) as exc:
            outcome = failed(f"{type(exc).__name__}: {exc}")
        except Exception:  # a bug in the case itself is a failure too
            outcome = failed(traceback.format_exc().strip().splitlines()[-1])
    seconds = time.monotonic() - start
    log = "".join(ctx.log[mark:])
    return Result(c.id, c.area, c.check, outcome.status, redact(outcome.evidence, ctx.settings),
                  round(seconds, 2), redact(log, ctx.settings))


def select(ids: list[str] | None) -> list[Case]:
    if not ids:
        return list(CASES)
    chosen = [c for c in CASES if any(c.id == i or c.id.startswith(i + ".") for i in ids)]
    unknown = [i for i in ids if not any(c.id == i or c.id.startswith(i + ".") for c in CASES)]
    if unknown:
        raise SystemExit(f"devtest: no case named {', '.join(unknown)}")
    return chosen
