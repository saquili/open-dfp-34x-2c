.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=============================================================
ADR 0005: Build the distribution as a Buildroot external tree
=============================================================

:Status: Accepted
:Date: 2026-09-21
:Deciders: the maintainer

Summary
=======

The images are built by a pinned Buildroot release with this project's
external tree, which builds the toolchain, the kernel and the user space
from source and gathers the licences and sources a release must offer.

Context
=======

The distribution must build from source, toolchain, kernel and user space,
and must produce the bundle of sources and licences that distributing a GPL
system owes. The core runs only the MIPS I instruction set
(``docs/handbook/toolchain.rst``), which constrains the toolchain.

Options considered
==================

A: a Buildroot external tree
    A pinned Buildroot release plus our tree in ``br2-external/``.
    Kconfig and defconfig as in the kernel; an internal toolchain built from
    source; ``make legal-info`` for the licence bundle; ``make sdk`` for a
    relocatable toolchain; the existing configuration files taken almost
    unchanged. Buildroot has no MIPS I variant, so it needs one carried
    patch.
B: build scripts in the repository
    The approach of the day, cleaned up. Full control and fast, but it
    re-implements what a build system already standardises, the licence
    manifest, package pinning and a relocatable toolchain, and every new
    package is new plumbing.
C: a Yocto or OpenEmbedded layer
    The norm for a manufacturer's board support, but heavy for a small flash
    and a single maintainer.
D: an OpenWrt target
    A large package set, but conventions of its own and an older kernel;
    out of scope.

Decision
========

Adopt option A, a Buildroot ``BR2_EXTERNAL`` tree:

* a Buildroot release pinned by hash in ``buildroot/``, with one carried
  patch that adds the MIPS I variant this core needs;
* the board, its configuration and the packages of the project in
  ``br2-external/``;
* the kernel from ``linux/``: an override source directory for development,
  an archived tarball for releases;
* the target's hard requirements kept as gates rather than as hand-built
  compilers: MIPS I by default, soft float, C only, and an instruction-set
  gate over every target binary.

Option B remains a documented fallback, not a pending alternative.

Consequences
============

* Buildroot's internal toolchain replaces hand-built compiler scripts, and
  ``make legal-info`` and ``make sdk`` come with it.
* The carried MIPS I patch is a maintenance cost, and the compiler and C
  library versions follow the pinned release, so an update of the release
  has to be verified again.
* The instruction-set gate, the image-size check and a device run guard the
  build, so a compiler change cannot regress the target unnoticed.
* Should the MIPS I patch become unsustainable, option B is the fallback;
  the other components do not depend on this choice.
