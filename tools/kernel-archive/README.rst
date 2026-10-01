.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

====================
tools/kernel-archive
====================

An archive of the ``linux/`` tree as committed, for the distribution build.

The cross toolchain's C library is built against the kernel headers of this
repository's own kernel, so that the interfaces of our drivers --
``include/uapi/linux/pon.h`` is one -- are in the toolchain's sysroot. The build
system takes a kernel from an archive; this tool makes one.

Usage
=====

::

    tools/kernel-archive/kernel-archive [-o DIR] [REV]

Run inside the repository. It writes ``DIR/linux-<release>-<tree>.tar.xz`` and
prints the path. ``<release>`` is the kernel's version as ``linux/Makefile``
gives it (``6.18.48``), and ``<tree>`` is the first twelve digits of the id of
the ``linux/`` tree in ``REV``, the commit to archive (default ``HEAD``). ``DIR``
defaults to ``build/kernel``.

The archive holds the files of that tree under ``linux-<release>/``, in the order
git lists them. Owners are root, modes are 0644 and 0755, every modification time
is the commit's, and ``xz`` runs with a fixed preset, so one commit gives one
archive, byte for byte, wherever it is made. An archive that is already in
``DIR`` is kept and not made again.

Why from the commit
===================

* The name says which kernel is inside. The build system's download cache serves
  a file it already has under the same name without looking at it, so a name
  that did not change with the kernel would hand out the headers of an older
  one.
* The files come from git's objects and not from the working tree. The kernel
  holds 13 pairs of paths that differ only in case (``CONTRIBUTING.rst``), and 8
  of them are uapi headers, such as ``xt_DSCP.h`` and ``xt_dscp.h``. A file
  system that ignores case keeps one file of each pair, so an archive of such a
  working tree would lack headers that the sysroot of a toolchain built on a
  case-sensitive file system has.

Cost
====

For the tree at the time of writing (Linux 6.18.48 with our series, tree
``7347e4e52873``) the archive is 158 MB, and on an 18-core machine::

    time tools/kernel-archive/kernel-archive -o /tmp/kernel

takes 28 seconds, most of it compression.

Tests
=====

::

    tools/kernel-archive/tests/run [-v]

They build small repositories and read the archives back: the name, the content,
the fixed owners, modes and times, a pair of paths that differ only in case, a
release with an extra version, the same tree at another commit, a revision
argument, identical bytes from two runs, and the errors. They need Python 3, git
and xz, and no network.
