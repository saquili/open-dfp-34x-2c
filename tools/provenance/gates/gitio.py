# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The few git operations the gates need, through the git command line."""

from __future__ import annotations

import os
import re
import subprocess
from dataclasses import dataclass
from typing import Iterator, Optional


class GitError(RuntimeError):
    """A git command failed."""


# Settings that would change the output the gates parse, whatever the user's
# configuration says; and no optional locks, so that reading a repository
# never writes to it.
_GIT = ["git", "-c", "log.showSignature=false", "-c", "core.quotePath=true",
        "-c", "diff.noprefix=false", "-c", "diff.mnemonicPrefix=false"]


def _env() -> dict[str, str]:
    return dict(os.environ, GIT_OPTIONAL_LOCKS="0", LC_ALL="C")


def git(repo: str, *args: str, input_bytes: Optional[bytes] = None) -> bytes:
    proc = subprocess.run([*_GIT, "-C", repo, *args], input=input_bytes, capture_output=True,
                          env=_env())
    if proc.returncode != 0:
        raise GitError("git %s: %s" % (" ".join(args), proc.stderr.decode(errors="replace").strip()))
    return proc.stdout


def diff_no_index(repo: str, old: str, new: str) -> bytes:
    """``git diff --no-index -U0`` of two files.

    Exit status 1 means "they differ" -- and also "could not read one of
    them", so an error message without a diff is a failure too.
    """
    proc = subprocess.run([*_GIT, "-C", repo, "diff", "--no-index", "-U0", "--no-color", "--",
                           old, new], capture_output=True, env=_env())
    if proc.returncode not in (0, 1) or (proc.stderr.strip() and not proc.stdout):
        raise GitError("git diff --no-index: %s" % proc.stderr.decode(errors="replace").strip())
    return proc.stdout


def toplevel(path: str) -> Optional[str]:
    try:
        return git(path, "rev-parse", "--show-toplevel").decode().strip()
    except (GitError, OSError):
        return None


def ls_files(repo: str, pathspecs: tuple[str, ...] = ()) -> list[str]:
    out = git(repo, "ls-files", "-z", "--", *pathspecs)
    return [p for p in out.decode("utf-8", errors="surrogateescape").split("\0") if p]


def index_blobs(repo: str, prefix: str) -> dict[str, str]:
    """Index blob ids of the files under ``prefix`` (repository paths)."""
    out = git(repo, "ls-files", "-s", "-z", "--", prefix)
    blobs = {}
    for record in out.decode("utf-8", errors="surrogateescape").split("\0"):
        if record:
            meta, path = record.split("\t", 1)
            blobs[path] = meta.split()[1]
    return blobs


def worktree_modified(repo: str, prefix: str) -> set[str]:
    out = git(repo, "diff", "--name-only", "-z", "--", prefix)
    return {p for p in out.decode("utf-8", errors="surrogateescape").split("\0") if p}


def tree_blobs(repo: str, treeish: str) -> dict[str, str]:
    out = git(repo, "ls-tree", "-r", "-z", treeish)
    blobs = {}
    for record in out.decode("utf-8", errors="surrogateescape").split("\0"):
        if record:
            meta, path = record.split("\t", 1)
            mode, kind, sha = meta.split()
            if kind == "blob":
                blobs[path] = sha
    return blobs


class BlobReader:
    """Reads blob contents through one ``git cat-file --batch`` process."""

    def __init__(self, repo: str) -> None:
        self._proc = subprocess.Popen([*_GIT, "-C", repo, "cat-file", "--batch"],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=_env())

    def read(self, sha: str) -> bytes:
        assert self._proc.stdin and self._proc.stdout
        self._proc.stdin.write(sha.encode() + b"\n")
        self._proc.stdin.flush()
        header = self._proc.stdout.readline().split()
        if len(header) < 3 or header[1] == b"missing":
            raise GitError("cannot read object %s" % sha)
        size = int(header[2])
        data = self._proc.stdout.read(size)
        self._proc.stdout.read(1)
        return data

    def close(self) -> None:
        if self._proc.stdin:
            self._proc.stdin.close()
        self._proc.wait()

    def __enter__(self) -> "BlobReader":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


@dataclass
class Commit:
    sha: str
    parents: list[str]
    author_name: str
    author_email: str
    message: str

    @property
    def subject(self) -> str:
        return self.message.split("\n", 1)[0]

    @property
    def is_merge(self) -> bool:
        return len(self.parents) > 1


def commits(repo: str, spec: str) -> list[Commit]:
    """Commits selected by a revision range such as ``main..HEAD``, oldest first."""
    out = git(repo, "log", "--reverse", "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%B%x1e", spec)
    result = []
    for record in out.decode("utf-8", errors="replace").split("\x1e"):
        record = record.lstrip("\n")
        if not record:
            continue
        sha, parents, name, email, message = record.split("\x1f", 4)
        result.append(Commit(sha, parents.split(), name, email, message.rstrip("\n") + "\n"))
    return result


def commit_exists(repo: str, rev: str) -> bool:
    try:
        git(repo, "cat-file", "-e", rev + "^{commit}")
        return True
    except GitError:
        return False


@dataclass
class Change:
    status: str          # A, M, D, T (no renames: they appear as D + A)
    path: str
    blob: Optional[str]  # new blob, None when deleted


_NULL_SHA = re.compile(r"^0+$")
_GITLINK = "160000"


def _diff_args(old: Optional[str], new: str) -> list[str]:
    return ["--root", new] if old is None else [old, new]


def diff_changes(repo: str, old: Optional[str], new: str) -> list[Change]:
    """Files that differ between two revisions; ``old`` None means "from nothing".

    Submodule entries are left out: they carry a commit id, not a blob.
    """
    args = ["diff-tree", "-r", "-z", "--no-commit-id", "--no-renames", "--raw"]
    out = git(repo, *args, *_diff_args(old, new)).decode("utf-8", errors="surrogateescape")
    fields = out.split("\0")[:-1]  # ":<modes> <ids> <status>", "<path>", ...
    result = []
    for meta, path in zip(fields[::2], fields[1::2]):
        _old_mode, new_mode, _old_sha, new_sha, status = meta[1:].split()
        if new_mode == _GITLINK:
            continue  # a submodule: its new side is a commit id, not a blob
        result.append(Change(status[0], path, None if _NULL_SHA.match(new_sha) else new_sha))
    return result


def changes(repo: str, commit: Commit) -> list[Change]:
    """Files a commit changes relative to its first parent (all files for a root)."""
    return diff_changes(repo, commit.parents[0] if commit.parents else None, commit.sha)


_HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def diff_added_lines(repo: str, old: Optional[str], new: str) -> dict[str, set[int]]:
    """Line numbers each file gains between two revisions (new-file numbering)."""
    args = ["diff-tree", "-r", "-p", "-U0", "--no-color", "--no-renames", "--no-commit-id",
            "--no-ext-diff"]
    result: dict[str, set[int]] = {}
    current: Optional[set[int]] = None
    for raw in git(repo, *args, *_diff_args(old, new)).split(b"\n"):
        if raw.startswith(b"+++ "):
            target = raw[4:].decode("utf-8", errors="surrogateescape")
            if target == "/dev/null":
                current = None
            else:
                if target.startswith('"'):
                    target = _unquote(target)
                elif target.startswith("b/"):
                    target = target[2:]
                current = result.setdefault(target, set())
            continue
        m = _HUNK.match(raw.decode("utf-8", errors="replace"))
        if m and current is not None:
            start = int(m.group(1))
            count = 1 if m.group(2) is None else int(m.group(2))
            current.update(range(start, start + count))
    return result


def added_lines(repo: str, commit: Commit) -> dict[str, set[int]]:
    """Line numbers each file gains in a commit (new-file numbering)."""
    return diff_added_lines(repo, commit.parents[0] if commit.parents else None, commit.sha)


def _unquote(quoted: str) -> str:
    """Undo git's C-style path quoting ("b/p\\303\\251.c")."""
    body = quoted.strip('"')
    raw = body.encode("latin-1", errors="backslashreplace").decode("unicode_escape").encode("latin-1")
    text = raw.decode("utf-8", errors="surrogateescape")
    return text[2:] if text.startswith("b/") else text


_SQUASH_SUBJECT = re.compile(r"^Squashed '([^']+)/' ")
_SUBTREE_DIR = re.compile(r"^git-subtree-dir: (\S+)\s*$", re.M)


def subtree_squash_dir(commit: Commit) -> Optional[str]:
    """The subtree directory a ``git subtree --squash`` import commit is for."""
    m = _SQUASH_SUBJECT.match(commit.subject)
    d = _SUBTREE_DIR.search(commit.message)
    if m and d and m.group(1).rstrip("/") == d.group(1).rstrip("/"):
        return d.group(1).rstrip("/")
    return None


def latest_squash(repo: str, subdir: str, rev: str = "HEAD") -> Optional[str]:
    """The newest subtree squash commit for ``subdir`` reachable from ``rev``."""
    try:
        out = git(repo, "log", "--format=%H", "--fixed-strings",
                  "--grep=git-subtree-dir: %s" % subdir, rev)
    except GitError:
        return None
    for sha in out.decode().split():
        if subtree_squash_dir(commit_info(repo, sha)) == subdir:
            return sha
    return None


def commit_info(repo: str, sha: str) -> Commit:
    """One commit's metadata and message."""
    out = git(repo, "log", "-1", "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%B", sha)
    full, parents, name, email, message = out.decode("utf-8", errors="replace").split("\x1f", 4)
    return Commit(full, parents.split(), name, email, message.rstrip("\n") + "\n")


def walk_files(root: str) -> Iterator[str]:
    """Regular files under a directory that is not a git work tree."""
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d != ".git")
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            if os.path.isfile(full) and not os.path.islink(full):
                yield os.path.relpath(full, root)
