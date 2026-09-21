# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Command line of tools/provenance/check."""

from __future__ import annotations

import argparse
import os
import sys
import unittest

from . import GATES, gitio
from .allowlist import Allowlist, AllowlistError
from .content import DEFAULT_SIZE_CAP, ContentScanner
from .denylist import DenyList, DenyListError
from .findings import Report
from .scan import RepoScanner

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DENYLIST = os.path.join(HERE, "denylist.sha256")
DEFAULT_ALLOWLIST = os.path.join(HERE, "allowlist.toml")

EPILOG = """\
commands:
  all [--base REV | --history]  the tree, then the commits in REV..HEAD
                                (default base: main); --history checks every
                                commit reachable from HEAD
  tree [--untracked] [PATH...]  tracked files (or those under PATH), plus
                                untracked ones with --untracked; under an
                                upstream subtree only what differs from the
                                latest pristine import
  tree --dir DIR                every regular file of a plain directory
  range RANGE                   the commits of a revision range, e.g.
                                main..HEAD: messages, the one-component rule
                                and the lines each commit adds
  changes BASE [REV]            what REV (default HEAD) adds on top of BASE,
                                as one change: added files whole, modified
                                files by their added lines, no messages
  message FILE [--paths P...]   one commit message before it is committed
                                (for a commit-msg hook); paths default to
                                what is staged
  selftest                      run the gates' own tests

exit status: 0 no findings, 1 findings, 2 usage or configuration error
"""


def _default_repo() -> str:
    return gitio.toplevel(os.getcwd()) or gitio.toplevel(HERE) or os.getcwd()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="tools/provenance/check",
        description="Run the provenance gates of docs/policy/provenance.rst.",
        epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", help="repository to check (default: the current one)")
    parser.add_argument("--denylist", default=DEFAULT_DENYLIST, help="hashed deny-list file")
    parser.add_argument("--allowlist", default=DEFAULT_ALLOWLIST, help="allowlist file")
    parser.add_argument("--gates", default=",".join(GATES),
                        help="comma-separated gates to run (default: all)")
    parser.add_argument("--size-cap", type=int, default=DEFAULT_SIZE_CAP,
                        help="largest text file accepted, in bytes (default: %(default)s)")
    parser.add_argument("--format", choices=("text", "json"), default="text")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="also list allowed findings and unused allowlist entries")
    sub = parser.add_subparsers(dest="command")
    p_all = sub.add_parser("all")
    p_all.add_argument("--base", default="main")
    p_all.add_argument("--history", action="store_true")
    p_tree = sub.add_parser("tree")
    p_tree.add_argument("paths", nargs="*")
    p_tree.add_argument("--dir")
    p_tree.add_argument("--untracked", action="store_true")
    p_range = sub.add_parser("range")
    p_range.add_argument("spec")
    p_changes = sub.add_parser("changes")
    p_changes.add_argument("base")
    p_changes.add_argument("rev", nargs="?", default="HEAD")
    p_msg = sub.add_parser("message")
    p_msg.add_argument("file")
    p_msg.add_argument("--paths", nargs="*")
    sub.add_parser("selftest")
    return parser


def _selftest() -> int:
    tests = os.path.join(HERE, "tests")
    suite = unittest.defaultTestLoader.discover(tests, top_level_dir=HERE)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    return 0 if result.wasSuccessful() else 1


def main(argv=None) -> int:
    args = _parser().parse_args(argv)
    command = args.command or "all"
    if command == "selftest":
        return _selftest()

    gates = [g for g in args.gates.split(",") if g]
    unknown = set(gates) - set(GATES)
    if unknown:
        print("unknown gates: %s" % ", ".join(sorted(unknown)), file=sys.stderr)
        return 2
    try:
        denylist = DenyList.load(args.denylist) if "denylist" in gates else None
        allowlist = Allowlist.load(args.allowlist) if os.path.exists(args.allowlist) else None
    except (DenyListError, AllowlistError, OSError) as exc:
        print("tools/provenance: %s" % exc, file=sys.stderr)
        return 2

    report = Report(allowlist)
    content = ContentScanner(denylist, gates, args.size_cap)
    try:
        if command == "tree" and args.dir:
            RepoScanner(os.path.abspath(args.dir), content, report).directory(args.dir)
        else:
            repo = os.path.abspath(args.repo) if args.repo else _default_repo()
            scanner = RepoScanner(repo, content, report)
            if command == "tree":
                scanner.tree(args.paths, untracked=args.untracked)
            elif command == "range":
                scanner.range(args.spec)
            elif command == "changes":
                scanner.changes(args.base, args.rev)
            elif command == "message":
                with open(args.file, encoding="utf-8", errors="replace") as handle:
                    message = "".join(l for l in handle if not l.startswith("#"))
                paths = args.paths
                if paths is None:
                    out = gitio.git(repo, "diff", "--cached", "--name-only", "-z")
                    paths = [p for p in out.decode("utf-8", errors="surrogateescape").split("\0") if p]
                author = None
                try:
                    ident = gitio.git(repo, "var", "GIT_AUTHOR_IDENT").decode().strip()
                    author = ident[:ident.rindex(">") + 1]
                except (gitio.GitError, ValueError):
                    pass
                scanner.message_file(message, paths, author)
            else:
                scanner.tree()
                if args.history:
                    scanner.range("HEAD")
                elif gitio.commit_exists(repo, args.base):
                    scanner.range("%s..HEAD" % args.base)
                else:
                    report.notes.append("base %s not found; commits not checked" % args.base)
    except gitio.GitError as exc:
        print("tools/provenance: %s" % exc, file=sys.stderr)
        return 2

    if args.format == "json":
        print(report.render_json())
    else:
        print(report.render_text(verbose=args.verbose))
    return 0 if report.ok() else 1
