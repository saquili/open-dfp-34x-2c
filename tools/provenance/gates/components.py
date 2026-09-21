# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The repository's components, for the one-component rule and subject prefixes.

A commit touches exactly one component, so that each can be lifted out on
its own -- above all so that a subtree split of ``linux/`` yields a kernel
history that is exactly our kernel commits:

=========  ==========================================================
linux      ``linux/`` -- the kernel tree
omcid      ``omcid/`` -- the OMCI daemon
distro     ``br2-external/`` and ``buildroot/`` -- the distribution
tools      ``tools/``
tests      ``tests/``
docs       ``docs/`` and the repository's top level: the files at the
           root, ``LICENSES/`` and ``.github/``
=========  ==========================================================

Any other top-level directory is a component of its own, so that a new
directory is never silently merged into an existing component.
"""

from __future__ import annotations

import posixpath

LINUX = "linux"
OMCID = "omcid"
DISTRO = "distro"
TOOLS = "tools"
TESTS = "tests"
DOCS = "docs"

_BY_DIRECTORY = {
    "linux": LINUX,
    "omcid": OMCID,
    "br2-external": DISTRO,
    "buildroot": DISTRO,
    "tools": TOOLS,
    "tests": TESTS,
    "docs": DOCS,
    "LICENSES": DOCS,
    ".github": DOCS,
}

DESCRIPTIONS = {
    LINUX: "linux/",
    OMCID: "omcid/",
    DISTRO: "br2-external/ and buildroot/",
    TOOLS: "tools/",
    TESTS: "tests/",
    DOCS: "docs/ and the top level",
}

# Directories imported from upstream with "git subtree --squash": the gates
# look only at what differs from the latest pristine import.
UPSTREAM_SUBTREES = ("linux",)

# Buildroot names its commits after paths inside the external tree.
_DISTRO_TOP = ("package", "board", "configs", "Config.in", "external.mk", "external.desc",
               "patches")


def component_of(path: str) -> str:
    """The component a repository path belongs to."""
    head, sep, _rest = path.partition("/")
    if not sep:
        return DOCS  # a file at the top level
    return _BY_DIRECTORY.get(head, head + "/")


def describe(component: str) -> str:
    return DESCRIPTIONS.get(component, component)


def _names_path(prefix: str, path: str) -> bool:
    """Does a subject prefix name this path or a directory above it?"""
    if prefix == path or path.startswith(prefix.rstrip("/") + "/"):
        return True
    stem, _ext = posixpath.splitext(path)
    return prefix == stem


def prefix_names_component(prefix: str, component: str, paths: list[str]) -> bool:
    """Is ``prefix`` (the first subject prefix) right for a commit's component?

    Kernel commits use kernel subsystem prefixes, which do not map onto
    paths; any well-formed prefix is accepted there. Elsewhere the prefix is
    the component's directory or a path inside it; Buildroot-style prefixes
    are relative to ``br2-external/``; top-level files are named by their
    path with or without the extension.
    """
    if component == LINUX:
        return True
    if component == DISTRO:
        if prefix in ("buildroot", "br2-external"):
            return True
        if prefix.split("/")[0] in _DISTRO_TOP:
            return any(_names_path(prefix, p[len("br2-external/"):]) for p in paths
                       if p.startswith("br2-external/"))
        return any(_names_path(prefix, p) for p in paths)
    root = DOCS if component == DOCS else component.rstrip("/")
    return prefix == root or any(_names_path(prefix, p) for p in paths)
