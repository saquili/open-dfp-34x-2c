# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Helpers for the gates' tests: seeds, fixture repositories, running a check.

The seeds these tests plant -- forbidden words, unit identifiers, names on a
deny-list -- are assembled at run time from fragments or drawn at random, so
that no file of the repository contains one and the repository passes the
gates it tests.
"""

from __future__ import annotations

import atexit
import contextlib
import io
import json
import os
import random
import shutil
import subprocess
import tempfile
import unittest
from typing import Optional

from gates.content import ContentScanner
from gates.denylist import format_denylist
from gates.tokens import Hasher, normalize_ident

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMMITTED_DENYLIST = os.path.join(HERE, "denylist.sha256")
COMMITTED_ALLOWLIST = os.path.join(HERE, "allowlist.toml")

AUTHOR_NAME = "Ada Tester"
AUTHOR_EMAIL = "ada@example.org"
AUTHOR = "%s <%s>" % (AUTHOR_NAME, AUTHOR_EMAIL)
SIGNOFF = "Signed-off-by: " + AUTHOR


def word(*fragments: str) -> str:
    """Join fragments into one word at run time (see the module docstring)."""
    return "".join(fragments)


class Seeds:
    """Seeds drawn at random, reproducibly from one number."""

    def __init__(self, seed: int = 20260921) -> None:
        self.rng = random.Random(seed)

    def hexdigits(self, count: int) -> str:
        return "".join(self.rng.choice("0123456789abcdef") for _ in range(count))

    def identifier(self) -> str:
        """A fresh identifier of three components that no word list holds."""
        return "q%s_%s_%s" % (self.hexdigits(5), self.hexdigits(3), self.hexdigits(4))

    def gpon_serial(self) -> str:
        """Four capitals, at least one outside A-F, then eight hex digits."""
        letters = [self.rng.choice("GHJKLMNPQRSTUVWXYZ")]
        letters += [self.rng.choice("ABCDEFGHJKLMNPQRSTUVWXYZ") for _ in range(3)]
        self.rng.shuffle(letters)
        return "".join(letters) + self.hexdigits(8).upper()

    def private_ipv4(self) -> str:
        return "10.%d.%d.%d" % (self.rng.randrange(256), self.rng.randrange(256),
                                self.rng.randrange(1, 255))

    def mac(self) -> str:
        """A unicast, globally administered address outside the documentation block."""
        first = self.rng.randrange(0x10, 0xFF) & 0xFC
        octets = [first] + [self.rng.randrange(256) for _ in range(5)]
        return ":".join("%02x" % o for o in octets)

    def code_address(self) -> str:
        """An address in the kernel segment, as a function address is written."""
        return word("0x", "801") + self.hexdigits(5)

    def blob(self) -> bytes:
        return bytes(self.rng.randrange(256) for _ in range(64)) + b"\0" + b"\x01\x02"


def make_denylist(path: str, names: list[tuple[str, str, str]], salt: bytes = b"test-salt") -> None:
    """Write a deny-list of ``(name, match, category)`` entries to ``path``."""
    hasher = Hasher(salt)
    entries = [(hasher(normalize_ident(name)), match, category) for name, match, category in names]
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(format_denylist(salt, entries, ["a deny-list made by a test"]))


_EMPTY_DENYLIST: list[str] = []


def empty_denylist() -> str:
    """A deny-list without entries, so that a test does not depend on the committed one."""
    if not _EMPTY_DENYLIST:
        tmp = tempfile.mkdtemp()
        atexit.register(shutil.rmtree, tmp, ignore_errors=True)
        path = os.path.join(tmp, "empty.sha256")
        make_denylist(path, [])
        _EMPTY_DENYLIST.append(path)
    return _EMPTY_DENYLIST[0]


def git_env() -> dict[str, str]:
    """An environment for git that ignores the user's and the system's settings."""
    return dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1",
                GIT_AUTHOR_NAME=AUTHOR_NAME, GIT_AUTHOR_EMAIL=AUTHOR_EMAIL,
                GIT_COMMITTER_NAME=AUTHOR_NAME, GIT_COMMITTER_EMAIL=AUTHOR_EMAIL,
                GIT_AUTHOR_DATE="2026-01-01T00:00:00+0000",
                GIT_COMMITTER_DATE="2026-01-01T00:00:00+0000")


class Repo:
    """A throw-away git repository."""

    def __init__(self, path: str) -> None:
        self.path = path
        os.makedirs(path, exist_ok=True)
        self.git("init", "-q", "-b", "main")

    def git(self, *args: str, input_text: Optional[str] = None,
            env: Optional[dict[str, str]] = None) -> str:
        proc = subprocess.run(["git", "-c", "commit.gpgsign=false", "-C", self.path, *args],
                              input=input_text, capture_output=True, text=True,
                              env=env or git_env())
        if proc.returncode != 0:
            raise AssertionError("git %s failed: %s" % (" ".join(args), proc.stderr))
        return proc.stdout

    def write(self, rel: str, content) -> str:
        full = os.path.join(self.path, rel)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        mode = "wb" if isinstance(content, bytes) else "w"
        with open(full, mode, **({} if mode == "wb" else {"encoding": "utf-8"})) as handle:
            handle.write(content)
        return full

    def commit(self, subject: str, body: str = "", *, signoff: bool = True,
               paths: Optional[list[str]] = None) -> str:
        message = subject + "\n"
        if body:
            message += "\n" + body.rstrip("\n") + "\n"
        if signoff:
            message += "\n" + SIGNOFF + "\n"
        self.git("add", "-A", "--", *(paths or ["."]))
        self.git("commit", "-q", "-F", "-", input_text=message)
        return self.head()

    def head(self) -> str:
        return self.git("rev-parse", "HEAD").strip()

    def subtree_import(self, prefix: str, files: dict[str, str]) -> str:
        """Import ``files`` under ``prefix`` the way ``git subtree add --squash`` does.

        A parentless squash commit holds the upstream tree at its root; a merge
        commit brings it in under the prefix. Built from plumbing, so the tests
        do not depend on the subtree script being installed.
        """
        with tempfile.TemporaryDirectory() as tmp:
            for rel, content in files.items():
                full = os.path.join(tmp, "tree", rel)
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, "w", encoding="utf-8") as handle:
                    handle.write(content)
            env = dict(git_env(), GIT_INDEX_FILE=os.path.join(tmp, "index"))
            self.git("--work-tree", os.path.join(tmp, "tree"), "add", "-A", env=env)
            tree = self.git("write-tree", env=env).strip()
        split = "1" * 40
        squash = self.git("commit-tree", tree, input_text=(
            "Squashed '%s/' content from commit %s\n\ngit-subtree-dir: %s\n"
            "git-subtree-split: %s\n" % (prefix, split[:7], prefix, split))).strip()
        self.git("read-tree", "--prefix=%s/" % prefix, "-u", squash)
        merged = self.git("write-tree").strip()
        merge = self.git("commit-tree", merged, "-p", "HEAD", "-p", squash,
                         input_text="Merge commit '%s' as '%s'\n" % (squash, prefix)).strip()
        self.git("update-ref", "HEAD", merge)
        return squash


def run_check(*argv: str, repo: Optional[str] = None, denylist: Optional[str] = None,
              allowlist: Optional[str] = None) -> tuple[int, dict]:
    """Run tools/provenance/check in-process; return its exit status and JSON report."""
    from gates import cli  # imported here: the rule tests need no command line

    args = ["--format", "json", "--denylist", denylist or empty_denylist(),
            "--allowlist", allowlist or os.devnull]
    if repo:
        args += ["--repo", repo]
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = cli.main(args + list(argv))
    text = out.getvalue()
    report = json.loads(text) if text.strip().startswith("{") else {"text": text}
    report["stderr"] = err.getvalue()
    return code, report


def pairs(report: dict) -> set[tuple[str, str]]:
    """The (gate, rule) pairs of a report's findings."""
    return {(f["gate"], f["rule"]) for f in report.get("findings", [])}


def scan(text: str, path: str = "notes.txt", denylist=None) -> set[tuple[str, str]]:
    """(gate, rule) pairs the text gates find in one text."""
    scanner = ContentScanner(denylist)
    return {(f.gate, f.rule) for f in scanner.scan_text(path, text)}


class TempDirTestCase(unittest.TestCase):
    """A test case with a temporary directory in ``self.tmp``."""

    def setUp(self) -> None:
        handle = tempfile.TemporaryDirectory()
        self.addCleanup(handle.cleanup)
        self.tmp = handle.name
