.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=====================
tools/kernel-series
=====================

What this repository's kernel series changes in code that the Linux kernel
owns, and the patches of the networking core, ready to be offered to their
maintainers (ADR 0018). ``docs/handbook/kernel-series.rst`` says why each file
was changed.

Usage
=====

::

    tools/kernel-series/kernel-series list   [REV]
    tools/kernel-series/kernel-series check  [REV]
    tools/kernel-series/kernel-series export [-o DIR] [REV]

Run inside the repository. REV is the commit to look at (default ``HEAD``). The
series is every commit from the one that imported the kernel (subject
``linux: import Linux ...``) to REV.

``list``
    The files of ``linux/`` that the series modifies or deletes, one path a
    line, relative to ``linux/``. The files it adds are its own and are not
    listed.

``check``
    Fails, naming them, when a file that ``list`` prints is missing from the
    page, written between double backticks as the page writes its paths. The
    page is read from the working tree, or from REV when one is given. This is
    the guard that keeps the page complete: a change to a file of the kernel's
    that the page does not explain fails ``make check``.

``export``
    Writes the commits that touch the networking core -- DSA, flow offload, the
    PTP core and the YNL generator -- as patch files in the order of the
    series, to DIR (default ``build/kernel-series``), and prints their names.
    A commit that adds a driver beside such a change, as the tag driver's
    commit does, is exported whole, as an upstream submission would carry it.

Exit status is 0 on success, 1 when a check fails or there is nothing to export,
and 2 for a bad command line.

Tests
=====

::

    tools/kernel-series/tests/run [-v]

They need Python 3 and git; each test makes a small repository of its own and
uses no network.
