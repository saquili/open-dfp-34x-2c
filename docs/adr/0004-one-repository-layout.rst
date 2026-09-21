.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

======================================================
ADR 0004: One repository of upstream-shaped components
======================================================

:Status: Accepted
:Date: 2026-09-21

Context
=======

Under ADR 0003 the structure follows mainline conventions and the scope is the
whole product. The usual community shape for that is **two** repositories: a
kernel tree (a maintainer-style fork of stable with our commits on top) and a
distribution tree that consumes it. The owner wants **one** repository.

Three ways to reconcile this were considered:

* **M1** -- the repository *is* a kernel tree, with the distribution in a
  non-kernel top-level directory. Rejected: a distribution directory at the
  root of a kernel tree is exactly the all-in-one anti-pattern, and a full OMCI
  stack is not what a kernel ``tools/`` directory is for.
* **M2** -- one repository that is a set of upstream-shaped components.
  Chosen.
* **M3** -- two repositories, the textbook shape. Rejected: it contradicts the
  one-repository decision.

Decision
========

Use **M2**: one repository whose top-level directories each follow their own
upstream's layout.

* ``linux/`` is a byte-for-byte Linux tree, imported with ``git subtree`` and
  carrying our work as logical kernel commits.
* ``omcid/`` is a standalone daemon project.
* ``br2-external/`` is a distribution build external tree, with ``buildroot/``
  pinning the build system.
* ``tools/``, ``tests/`` and ``docs/`` follow suit.

One rule, enforced by continuous integration, makes this work: **one component
per commit**. A commit touches only ``linux/``, only ``omcid/``, or only the
distribution and tooling directories -- never a mix. A subtree split of
``linux/`` then yields a clean kernel history whose commits are exactly our
kernel commits, publishable as an ordinary kernel tree.

Consequences
============

* The checkout contains the whole kernel; this is the cost of one tree.
* A **case-sensitive filesystem is required**, because the kernel tree contains
  paths that differ only in case; a default case-insensitive volume would
  silently lose one of each.
* The kernel branch stays publishable upstream unchanged: a subtree split,
  rebased onto the matching stable tag, gives the same commits on real upstream
  history, ready for the usual patch tooling.
