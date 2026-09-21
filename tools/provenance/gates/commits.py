# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Commit-message lint and the one-component-per-commit rule.

The rules follow CONTRIBUTING.rst and the kernel's submitting-patches
conventions: a component prefix, an imperative summary, a subject of at most
75 characters, a blank line, a body wrapped at 75 columns, and a
``Signed-off-by:`` of the author. ``Co-developed-by:`` must be followed by
that person's own sign-off, ``Link:`` points to public URLs only and
``Fixes:`` to commits of this repository. The message text also goes through
the content gates, with issue numbers refused outright.
"""

from __future__ import annotations

import ipaddress
import re
from typing import Callable, Optional
from urllib.parse import urlsplit

from . import GATE_COMMIT_MESSAGE, GATE_ONE_COMPONENT
from .components import LINUX, component_of, describe, prefix_names_component
from .findings import KIND_COMMIT, KIND_MESSAGE, Finding

SUBJECT_MAX = 75
BODY_LINE_MAX = 75

_PREFIXES = re.compile(r"^((?:[^\s:]+: )+)")
_KERNEL_PREFIX = re.compile(r"^[A-Za-z0-9_][\w.,+/-]*$")
_TRAILER = re.compile(r"^([A-Za-z][A-Za-z0-9-]*):[ \t]*(\S.*)$")
_IDENT = re.compile(r"^(.*?)\s*<([^<>\s]+)>\s*$")
_FIXES = re.compile(r"^([0-9a-f]{12,64})\b")

# First words of a summary that are not imperatives, by shape.
_ED_OK = {"embed", "feed", "need", "seed", "speed", "shed", "bleed", "breed", "proceed", "exceed",
          "succeed", "shred", "bed", "wed", "red", "heed", "weed", "deed", "reseed", "unembed"}
_ING_OK = {"bring", "string", "ring", "sing", "spring", "sting", "swing", "wring", "ping",
           "fling", "sling", "cling", "unstring"}
_NOT_VERBS = {"is", "was", "were", "are", "has", "had", "does", "did", "the", "a", "an", "this",
              "these", "that", "we", "it", "initial", "various", "minor", "misc", "more", "some",
              "new", "wip", "tmp", "temp", "fixup", "squash"}
# Base forms whose third-person "-s" we recognise.
_VERBS = {
    "add", "adjust", "allow", "apply", "avoid", "bump", "build", "call", "change", "check",
    "clarify", "clean", "clear", "convert", "correct", "create", "define", "delete", "describe",
    "disable", "document", "drop", "enable", "ensure", "explain", "export", "extend", "fix",
    "generate", "handle", "hide", "implement", "import", "improve", "include", "initialise",
    "initialize", "introduce", "keep", "limit", "load", "make", "mark", "merge", "move", "name",
    "note", "parse", "pass", "pin", "prefer", "prevent", "print", "program", "protect", "provide",
    "read", "record", "reduce", "refactor", "refuse", "register", "reject", "release", "remove",
    "rename", "reorder", "replace", "report", "require", "reset", "restore", "return", "reuse",
    "revert", "rework", "rewrite", "run", "scan", "select", "separate", "set", "show", "simplify",
    "skip", "sort", "split", "start", "stop", "store", "support", "switch", "take", "teach",
    "test", "track", "treat", "trim", "turn", "update", "use", "validate", "verify", "wire",
    "write", "do", "go", "fetch", "catch", "match", "patch", "touch", "flush", "push", "reach",
}


def mood_problem(word: str) -> Optional[str]:
    """Why ``word`` does not start an imperative summary, or None."""
    w = re.sub(r"[^a-z]", "", word.lower())
    if not w:
        return None
    if w in _NOT_VERBS:
        return "starts with \"%s\", not with a verb in the imperative" % word
    if w.endswith("ed") and len(w) > 3 and w not in _ED_OK:
        return "\"%s\" is past tense; use the imperative" % word
    if w.endswith("ing") and len(w) > 4 and w not in _ING_OK:
        return "\"%s\" is a gerund; use the imperative" % word
    if w.endswith("s") and not w.endswith("ss"):
        stems = {w[:-1]}
        if w.endswith("es"):
            stems.add(w[:-2])
        if w.endswith("ies"):
            stems.add(w[:-3] + "y")
        if stems & _VERBS:
            return "\"%s\" is third person; use the imperative" % word
    return None


def _host_is_public(host: str) -> bool:
    host = host.strip("[]").lower()
    if not host or host == "localhost" or "." not in host and ":" not in host:
        return False
    if host.endswith((".local", ".lan", ".internal", ".home", ".corp", ".localdomain", ".intranet",
                      ".home.arpa")):
        return False
    try:
        addr = ipaddress.ip_address(host)
    except ValueError:
        return True
    return addr.is_global


def _trailers(lines: list[str]) -> list[tuple[str, str]]:
    """The trailer block: the last paragraph, if it is all ``Key: value`` lines."""
    block: list[str] = []
    for line in reversed(lines):
        if not line.strip():
            break
        block.append(line)
    block.reverse()
    trailers = []
    for line in block:
        m = _TRAILER.match(line)
        if m:
            trailers.append((m.group(1), m.group(2).strip()))
        elif line[:1].isspace() and trailers:
            continue  # folded continuation
        else:
            return []
    return trailers


def lint_message(message: str, *, component: Optional[str], paths: list[str],
                 author: Optional[str] = None, commit: Optional[str] = None,
                 commit_exists: Optional[Callable[[str], bool]] = None) -> list[Finding]:
    """Format rules for one commit message (content gates run separately)."""
    findings: list[Finding] = []

    def add(rule: str, text: str, detail: str, line: Optional[int] = None) -> None:
        findings.append(Finding(GATE_COMMIT_MESSAGE, rule, detail, text=text, line=line,
                                kind=KIND_MESSAGE, commit=commit))

    lines = message.rstrip("\n").split("\n")
    subject = lines[0]
    if not subject.strip():
        add("subject-prefix", "", "the subject line is empty", 1)
        return findings

    summary = subject
    if not subject.startswith('Revert "'):
        m = _PREFIXES.match(subject)
        if m is None:
            add("subject-prefix", subject,
                "no component prefix; %s" % _prefix_hint(component, paths), 1)
        else:
            prefixes = m.group(1).split(": ")[:-1]
            summary = subject[m.end():]
            if component == LINUX:
                bad = [p for p in prefixes if not _KERNEL_PREFIX.match(p)]
                if bad:
                    add("subject-prefix", subject, "malformed kernel subject prefix \"%s\"" % bad[0], 1)
            elif component is not None and not prefix_names_component(prefixes[0], component, paths):
                add("subject-prefix", subject, "prefix \"%s\" does not name what the commit touches; %s"
                    % (prefixes[0], _prefix_hint(component, paths)), 1)
        if not summary.strip():
            add("subject-summary", subject, "the summary after the prefix is empty", 1)
        else:
            problem = mood_problem(summary.split()[0])
            if problem:
                add("subject-imperative", subject, problem, 1)
    if len(subject) > SUBJECT_MAX:
        add("subject-length", subject, "subject is %d characters; at most %d"
            % (len(subject), SUBJECT_MAX), 1)
    if subject.rstrip().endswith(".") and not subject.rstrip().endswith("..."):
        add("subject-period", subject, "the subject ends with a full stop", 1)

    if len(lines) > 1 and lines[1].strip():
        add("blank-line", lines[1], "the second line must be blank", 2)
    trailers = _trailers(lines[1:])
    trailer_start = len(lines) - len(trailers) if trailers else len(lines)
    for number, line in enumerate(lines[2:], 3):
        if len(line) <= BODY_LINE_MAX or number > trailer_start:
            continue
        if line[:1].isspace() or "://" in line or " " not in line.strip():
            continue  # quoted output, URLs, one long token
        add("body-line-length", line, "body line is %d characters; wrap at %d"
            % (len(line), BODY_LINE_MAX), number)

    signoffs = [value for key, value in trailers if key.lower() == "signed-off-by"]
    if not signoffs:
        add("signed-off-by", "", "no Signed-off-by: trailer (Developer Certificate of Origin)")
    elif author:
        want = _identity(author)
        if want and all(_identity(s) != want for s in signoffs):
            add("signed-off-by", author, "no Signed-off-by: from the author %s" % author)
    for position, (key, value) in enumerate(trailers):
        low = key.lower()
        if low == "co-developed-by":
            following = trailers[position + 1] if position + 1 < len(trailers) else None
            if following is None or following[0].lower() != "signed-off-by" \
                    or _identity(following[1]) != _identity(value):
                add("co-developed-by", "%s: %s" % (key, value),
                    "Co-developed-by: must be followed by that person's own Signed-off-by: "
                    "(credit tools with Co-Authored-By:)")
        elif low in ("link", "closes"):
            url = urlsplit(value.split()[0])
            if url.scheme not in ("http", "https") or not _host_is_public(url.hostname or ""):
                add("link-public", "%s: %s" % (key, value), "%s: must point to a public URL" % key)
        elif low == "fixes":
            m = _FIXES.match(value)
            if m is None:
                add("fixes-local", "%s: %s" % (key, value),
                    "Fixes: takes an abbreviated commit id (12+ digits) and the subject")
            elif commit_exists is not None and not commit_exists(m.group(1)):
                add("fixes-local", "%s: %s" % (key, value),
                    "Fixes: must name a commit of this repository")
    return findings


def _identity(value: str) -> Optional[tuple[str, str]]:
    m = _IDENT.match(value.strip())
    if not m:
        return None
    return (" ".join(m.group(1).split()), m.group(2).lower())


def _prefix_hint(component: Optional[str], paths: list[str]) -> str:
    if component is None:
        return "use \"<component>: <imperative summary>\""
    if component == LINUX:
        return "kernel commits use \"subsystem: component: imperative summary\""
    common = _common_dir(paths)
    return "expected \"%s: ...\" for a commit to %s" % (common or component, describe(component))


def _common_dir(paths: list[str]) -> str:
    dirs = [p.split("/")[:-1] for p in paths]
    if not dirs:
        return ""
    common = dirs[0]
    for d in dirs[1:]:
        n = 0
        while n < min(len(common), len(d)) and common[n] == d[n]:
            n += 1
        common = common[:n]
    if common and common[0] == "br2-external":
        common = common[1:]  # Buildroot names paths inside the external tree
    return "/".join(common[:2])


def component_findings(paths: list[str], commit: Optional[str]) -> list[Finding]:
    """The one-component rule for the paths one commit touches."""
    by_component: dict[str, list[str]] = {}
    for path in paths:
        by_component.setdefault(component_of(path), []).append(path)
    if len(by_component) <= 1:
        return []
    parts = ["%s (%s)" % (describe(c), ", ".join(sorted(ps)[:2]) + (", ..." if len(ps) > 2 else ""))
             for c, ps in sorted(by_component.items())]
    return [Finding(GATE_ONE_COMPONENT, "one-component",
                    "touches %d components: %s" % (len(by_component), "; ".join(parts)),
                    kind=KIND_COMMIT, commit=commit)]


def commit_component(paths: list[str]) -> Optional[str]:
    """The component of a commit that touches exactly one."""
    components = {component_of(p) for p in paths}
    return components.pop() if len(components) == 1 else None
