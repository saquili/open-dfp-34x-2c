.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===========================================================
ADR 0004: Keep one repository of upstream-shaped components
===========================================================

:Status: Accepted
:Date: 2026-09-21
:Deciders: the maintainer

Summary
=======

The whole project lives in one repository, whose top-level directories each
follow their own upstream's layout. Every commit touches only one of them,
so that the kernel's history can be split out as an ordinary kernel tree.

Context
=======

Under ADR 0003 the structure follows mainline conventions and the scope is
the whole product. The usual community shape for that is two repositories:
a kernel tree (a maintainer's fork of stable with our commits on top) and a
distribution tree that consumes it. The maintainer wants one repository.

Options considered
==================

M1: the repository is a kernel tree
    The distribution would sit in a non-kernel top-level directory. Rejected:
    a distribution at the root of a kernel tree is the all-in-one pattern
    that ADR 0003 avoids, and a full OMCI stack is not what a kernel's
    ``tools/`` directory is for.
M2: one repository of upstream-shaped components
    Chosen.
M3: two repositories
    The textbook shape. Rejected: it contradicts the requirement of one
    repository.

Decision
========

One repository whose top-level directories each follow their own
upstream's layout:

* ``linux/`` is a byte-for-byte Linux tree, imported with ``git subtree``,
  with our work on top as logical kernel commits.
* ``omcid/`` is a standalone daemon project.
* ``br2-external/`` is a Buildroot external tree, and ``buildroot/`` pins
  the build system.
* ``tools/``, ``tests/`` and ``docs/`` follow suit, as do the other programs'
  directories.

One rule, enforced by continuous integration, makes this work: **one
component per commit**. The components are ``linux/``; ``omcid/``; the
distribution, ``br2-external/`` with ``buildroot/``; ``tools/``; ``tests/``;
the documentation, ``docs/`` with the files at the top level, ``LICENSES/``
and ``.github/``; and every other top-level directory on its own
(``tools/provenance``, the one-component gate). A subtree split of
``linux/`` then yields a kernel history made of exactly our kernel commits,
publishable as an ordinary kernel tree.

Consequences
============

* A checkout contains the whole kernel; that is the cost of one tree.
* A **case-sensitive file system is required**: the kernel tree has paths
  that differ only in case, and a case-insensitive volume would silently
  lose one of each.
* The kernel series stays publishable upstream unchanged: a subtree split,
  rebased onto the matching stable tag, gives the same commits on real
  upstream history, ready for the usual patch tools.
