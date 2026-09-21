# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Walk a tree, a commit range, a change or a message file through the gates."""

from __future__ import annotations

import os
import re
import tempfile
from typing import Iterable, Optional

from . import gitio
from .commits import commit_component, component_findings, lint_message
from .components import UPSTREAM_SUBTREES
from .content import ContentScanner
from .findings import KIND_MESSAGE, Report

_HUNK = re.compile(rb"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", re.M)


def _hunk_lines(diff: bytes) -> set[int]:
    lines: set[int] = set()
    for m in _HUNK.finditer(diff):
        start = int(m.group(1))
        count = 1 if m.group(2) is None else int(m.group(2))
        lines.update(range(start, start + count))
    return lines


def _upstream_dir(path: str) -> Optional[str]:
    for sub in UPSTREAM_SUBTREES:
        if path.startswith(sub + "/"):
            return sub
    return None


class RepoScanner:
    """Runs the gates over one git repository (or a plain directory)."""

    def __init__(self, repo: str, content: ContentScanner, report: Report) -> None:
        self.repo = repo
        self.content = content
        self.report = report

    # -- tree ------------------------------------------------------------------

    def tree(self, pathspecs: Iterable[str] = (), untracked: bool = False) -> None:
        """Every tracked file; under an upstream subtree only what differs from it."""
        pathspecs = tuple(pathspecs)
        files = gitio.ls_files(self.repo, pathspecs)
        if untracked:
            out = gitio.git(self.repo, "ls-files", "-z", "--others", "--exclude-standard", "--",
                            *pathspecs)
            files += [p for p in out.decode("utf-8", errors="surrogateescape").split("\0") if p]
        baselines: dict[str, dict[str, str]] = {}
        index: dict[str, str] = {}
        modified: set[str] = set()
        for sub in UPSTREAM_SUBTREES:
            if not any(p.startswith(sub + "/") for p in files):
                continue
            squash = gitio.latest_squash(self.repo, sub)
            if squash is None:
                self.report.notes.append(
                    "%s/ has no subtree import yet; scanned whole" % sub)
                continue
            baselines[sub] = gitio.tree_blobs(self.repo, squash)
            index.update(gitio.index_blobs(self.repo, sub + "/"))
            modified |= gitio.worktree_modified(self.repo, sub + "/")
        for path in files:
            full = os.path.join(self.repo, path)
            if os.path.islink(full):
                # A link's target is content too (git stores it as a blob).
                self._scan_file(path, os.fsencode(os.readlink(full)))
                continue
            if not os.path.isfile(full):
                continue
            lines = None
            check_size = True
            sub = _upstream_dir(path)
            if sub in baselines:
                base = baselines[sub].get(path[len(sub) + 1:])
                if base is not None:
                    if path not in modified and index.get(path) == base:
                        continue  # pristine upstream file
                    lines = self._changed_lines(base, path, index.get(path), path in modified)
                    check_size = False
            with open(full, "rb") as handle:
                data = handle.read()
            self._scan_file(path, data, lines=lines, check_size=check_size)

    def _changed_lines(self, base: str, path: str, index_blob: Optional[str],
                       worktree_modified: bool) -> set[int]:
        """Lines of ``path`` that are not in the upstream blob ``base``."""
        if index_blob and not worktree_modified:
            return _hunk_lines(gitio.git(self.repo, "diff", "-U0", "--no-color", base, index_blob))
        with tempfile.TemporaryDirectory() as tmp:
            old = os.path.join(tmp, "upstream")
            with open(old, "wb") as handle, gitio.BlobReader(self.repo) as blobs:
                handle.write(blobs.read(base))
            return _hunk_lines(gitio.diff_no_index(self.repo, old, os.path.join(self.repo, path)))

    def directory(self, root: str) -> None:
        """Every regular file of a plain directory (no git, no baseline)."""
        for path in gitio.walk_files(root):
            with open(os.path.join(root, path), "rb") as handle:
                self._scan_file(path, handle.read())

    def _scan_file(self, path: str, data: bytes, *, lines: Optional[set[int]] = None,
                   check_size: bool = True, commit: Optional[str] = None) -> None:
        findings, text = self.content.check_blob(path, data, check_size=check_size, commit=commit)
        self.report.extend(findings)
        if text is not None:
            self.report.extend(self.content.scan_text(path, text, lines=lines, commit=commit))
        self.report.scanned_files += 1

    # -- history ---------------------------------------------------------------

    def range(self, spec: str) -> None:
        """Every commit of a revision range: its message and what it adds."""
        exists = lambda rev: gitio.commit_exists(self.repo, rev)  # noqa: E731
        with gitio.BlobReader(self.repo) as blobs:
            for commit in gitio.commits(self.repo, spec):
                self.report.scanned_commits += 1
                self.report.extend(self.content.scan_text(
                    None, commit.message, kind=KIND_MESSAGE, commit=commit.sha))
                if commit.is_merge or gitio.subtree_squash_dir(commit):
                    continue  # merges add nothing of their own; imports are pristine upstream
                changes = gitio.changes(self.repo, commit)
                paths = [change.path for change in changes]
                self.report.extend(component_findings(paths, commit.sha))
                self.report.extend(lint_message(
                    commit.message, component=commit_component(paths), paths=paths,
                    author="%s <%s>" % (commit.author_name, commit.author_email),
                    commit=commit.sha, commit_exists=exists))
                self._scan_changes(blobs, changes, gitio.added_lines(self.repo, commit),
                                   commit.sha)

    def changes(self, base: str, rev: str = "HEAD") -> None:
        """What ``rev`` adds on top of ``base``, as one change.

        Files ``rev`` adds are scanned whole and files it modifies by the
        lines it adds; commit messages are not read. This checks a branch
        against the upstream it is built on -- a kernel series on top of a
        release, say -- the way the tree check treats a subtree.
        """
        base_sha = gitio.git(self.repo, "rev-parse", "--verify", base + "^{commit}").decode().strip()
        rev_sha = gitio.git(self.repo, "rev-parse", "--verify", rev + "^{commit}").decode().strip()
        with gitio.BlobReader(self.repo) as blobs:
            self._scan_changes(blobs, gitio.diff_changes(self.repo, base_sha, rev_sha),
                               gitio.diff_added_lines(self.repo, base_sha, rev_sha), None,
                               base_is_upstream=True)

    def _scan_changes(self, blobs: gitio.BlobReader, changes: list[gitio.Change],
                      added: dict[str, set[int]], commit: Optional[str], *,
                      base_is_upstream: bool = False) -> None:
        for change in changes:
            if change.blob is None:
                continue
            data = blobs.read(change.blob)
            # A file that already existed upstream keeps upstream's size.
            upstream = base_is_upstream or _upstream_dir(change.path) is not None
            check_size = change.status == "A" or not upstream
            findings, text = self.content.check_blob(change.path, data, check_size=check_size,
                                                     commit=commit)
            self.report.extend(findings)
            if text is not None:
                lines = None if change.status == "A" else added.get(change.path, set())
                self.report.extend(self.content.scan_text(change.path, text, lines=lines,
                                                          commit=commit))
            self.report.scanned_files += 1

    # -- one message -----------------------------------------------------------

    def message_file(self, message: str, paths: list[str], author: Optional[str]) -> None:
        """A commit message before it is committed (for a commit-msg hook)."""
        self.report.extend(self.content.scan_text(None, message, kind=KIND_MESSAGE))
        self.report.extend(component_findings(paths, None))
        self.report.extend(lint_message(message, component=commit_component(paths), paths=paths,
                                        author=author))
