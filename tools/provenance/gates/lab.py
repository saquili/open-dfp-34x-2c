# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The lab's half of the gates: the deny-list and the lab's own paths.

Both are kept in the project's local lab, outside this repository, in a
directory that the environment variable ``OPEN_DFP_34X_2C_PROVENANCE_LAB``
(or the ``--lab`` option) names. It holds two files::

    denylist.sha256   the hashed deny-list (the format is in denylist.py)
    lab-paths         the patterns of the lab-path rule, one per line

``lab-paths`` holds one Python regular expression per line, matched
case-sensitively; blank lines and lines that start with ``#`` are left out,
and so is the white space around a pattern. The patterns are joined into one
alternation, in their order, so a pattern that needs a flag uses a scoped one
such as ``(?i:...)``.

With the directory, every gate runs. Without it, the deny-list gate and the
lab-path rule are skipped and the command says so in one line; every other
gate runs, as it does in continuous integration. A directory that is named
but missing, or that lacks one of the two files, is a configuration error,
so that a mistyped name cannot skip a gate in silence.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field
from typing import Mapping, Optional

from . import GATE_DENYLIST, GATE_FORBIDDEN
from .denylist import DenyList
from .rules import Rule, lab_path_rule

ENV = "OPEN_DFP_34X_2C_PROVENANCE_LAB"
DENYLIST_FILE = "denylist.sha256"
LAB_PATHS_FILE = "lab-paths"


class LabError(ValueError):
    """The lab directory or one of its files is missing or malformed."""


def parse_lab_paths(text: str, origin: str = "<lab-paths>") -> list[str]:
    """Read the patterns of a lab-paths file, checking that each compiles."""
    patterns = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        try:
            re.compile(line)
        except re.error as exc:
            raise LabError("%s:%d: bad pattern: %s" % (origin, lineno, exc)) from exc
        patterns.append(line)
    return patterns


@dataclass
class Lab:
    """What the lab directory gives the gates, and what could not run."""

    directory: Optional[str] = None
    denylist: Optional[DenyList] = None
    rules: tuple[Rule, ...] = ()
    skipped: list[str] = field(default_factory=list)

    def notice(self) -> Optional[str]:
        """The one line that says what was skipped, or None."""
        if not self.skipped:
            return None
        verb = "is" if len(self.skipped) == 1 else "are"
        return ("tools/provenance: %s is not set, so %s %s skipped here and run on the "
                "maintainer's machine before every push" % (ENV, " and ".join(self.skipped), verb))


def load(gates, directory: Optional[str] = None, denylist: Optional[str] = None,
         environ: Mapping[str, str] = os.environ) -> Lab:
    """Load what the selected gates need from the lab directory.

    ``directory`` is the ``--lab`` option; without it the environment
    variable is read. ``denylist`` is the ``--denylist`` option: a list given
    there replaces the lab's.
    """
    gates = set(gates)
    want_denylist = GATE_DENYLIST in gates
    want_paths = GATE_FORBIDDEN in gates
    directory = directory or environ.get(ENV) or None
    lab = Lab(directory=directory)
    if want_denylist and denylist is not None:
        lab.denylist = DenyList.load(denylist)
        want_denylist = False
    if not (want_denylist or want_paths):
        return lab
    if directory is None:
        if want_denylist:
            lab.skipped.append("the deny-list gate")
        if want_paths:
            lab.skipped.append("the lab-path rule")
        return lab
    if not os.path.isdir(directory):
        raise LabError("the lab directory %s (%s) does not exist" % (directory, ENV))
    if want_denylist:
        lab.denylist = DenyList.load(os.path.join(directory, DENYLIST_FILE))
    if want_paths:
        path = os.path.join(directory, LAB_PATHS_FILE)
        with open(path, encoding="utf-8") as handle:
            patterns = parse_lab_paths(handle.read(), origin=path)
        if patterns:
            lab.rules = (lab_path_rule(patterns),)
    return lab
