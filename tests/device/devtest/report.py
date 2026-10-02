# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The record of a run: a reStructuredText page with the table of results, the
log of every command each case ran, and the same results as JSON."""

from __future__ import annotations

import dataclasses
import json

from .core import FAIL, PASS, SKIP, Result

LOG_LINES = 400
SHOWN = {PASS: "**PASS**", FAIL: "**FAIL**", SKIP: "skip"}


def counts(results: list[Result]) -> str:
    n = {s: sum(r.status == s for r in results) for s in (PASS, FAIL, SKIP)}
    return f"{n[PASS]} passed, {n[FAIL]} failed, {n[SKIP]} skipped, of {len(results)}"


def cell(text: str) -> str:
    """One table cell: a single line, with nothing that reStructuredText would
    take for markup."""
    text = " ".join(text.split())
    for ch in ("\\", "*", "`", "|", "_"):
        text = text.replace(ch, "\\" + ch)
    return text or "-"


def page(title: str, header: dict[str, str], results: list[Result], copyright: str) -> str:
    out = [f".. SPDX-License-Identifier: GPL-2.0-only",
           f".. SPDX-FileCopyrightText: {copyright}", "",
           "=" * len(title), title, "=" * len(title), ""]
    for key, value in header.items():
        out.append(f":{key}: {cell(value)}")
    out.append(f":Result: {counts(results)}")
    out += ["", ".. list-table::", "   :header-rows: 1", "   :widths: 18 10 32 7 33", "",
            "   * - Case", "     - Area", "     - What is checked", "     - Result",
            "     - Evidence"]
    for r in results:
        out += [f"   * - ``{r.id}``", f"     - {cell(r.area)}", f"     - {cell(r.check)}",
                f"     - {SHOWN[r.status]}", f"     - {cell(r.evidence)}"]
    out.append("")
    return "\n".join(out)


def log(results: list[Result]) -> str:
    out = []
    for r in results:
        lines = r.log.splitlines()
        if len(lines) > LOG_LINES:
            lines = lines[:LOG_LINES] + [f"... {len(lines) - LOG_LINES} more lines"]
        out += [f"===== {r.id}: {r.status} in {r.seconds} s", f"evidence: {r.evidence}",
                *lines, ""]
    return "\n".join(out)


def as_json(header: dict[str, str], results: list[Result]) -> str:
    return json.dumps({"header": header,
                       "results": [{k: v for k, v in dataclasses.asdict(r).items() if k != "log"}
                                   for r in results]}, indent=2) + "\n"
