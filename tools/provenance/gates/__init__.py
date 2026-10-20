# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Provenance gates for open-dfp-34x-2c.

The gates enforce docs/policy/provenance.rst: a hashed deny-list over
identifier-like tokens, plain regular expressions for forbidden words and
unit identifiers, a binaries-and-size check, a commit-message lint and the
one-component-per-commit rule. Everything is Python standard library. The
deny-list and the patterns of the lab's own paths are kept in the project's
local lab and loaded from there (lab.py).
"""

import sys

MIN_PYTHON = (3, 11)

if sys.version_info < MIN_PYTHON:  # tomllib arrived in 3.11
    sys.exit("tools/provenance needs Python %d.%d or later" % MIN_PYTHON)

# Gate names, as they appear in reports and in the allowlist.
GATE_DENYLIST = "denylist"
GATE_FORBIDDEN = "forbidden-words"
GATE_UNIT_IDS = "unit-ids"
GATE_BINARIES = "binaries"
GATE_COMMIT_MESSAGE = "commit-message"
GATE_ONE_COMPONENT = "one-component"

GATES = (
    GATE_DENYLIST,
    GATE_FORBIDDEN,
    GATE_UNIT_IDS,
    GATE_BINARIES,
    GATE_COMMIT_MESSAGE,
    GATE_ONE_COMPONENT,
)
