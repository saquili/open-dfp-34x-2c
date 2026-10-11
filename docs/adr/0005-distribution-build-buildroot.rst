.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========================================================
ADR 0005: Distribution build -- a Buildroot external tree
=========================================================

:Status: Accepted
:Date: 2026-09-21
:Decided-by: the owner, 2026-09-21

.. note::

   The owner accepted Buildroot as the distribution build system on 2026-09-21.
   Option B below is retained only as a documented fallback, not as a pending
   alternative.

Context
=======

The distribution must build from source -- toolchain, kernel and userspace --
and must produce the source-and-licence bundle that distributing a GPL system
owes. Four shapes were weighed:

* **A. Buildroot external tree.** A pinned Buildroot release plus our tree in
  ``br2-external/``. Kconfig/defconfig like the kernel; an internal toolchain
  built from source; ``make legal-info`` for the licence bundle; ``make sdk``
  for a relocatable toolchain; existing configuration files taken almost
  unchanged. Buildroot has **no MIPS I variant**, so it needs one carried
  patch.
* **B. Kernel-style monorepo scripts** (today's approach, cleaned). Full
  control and fast, but it re-implements what a build system already
  standardises -- licence manifest, package pinning, a relocatable toolchain --
  and every new
  package is new plumbing.
* **C. A Yocto/OpenEmbedded layer.** The manufacturer-BSP norm in industry, but
  heavy for a small flash budget and a single maintainer.
* **D. An OpenWrt target.** A large package set, but its own conventions and an
  older kernel; out of scope.

Decision
========

Adopt **option A**: a Buildroot ``BR2_EXTERNAL`` tree.

* A pinned Buildroot release (by hash) in ``buildroot/``, with one carried
  patch adding the MIPS I variant needed by this core.
* Our board, configuration and the OMCI daemon package in ``br2-external/``.
* The kernel comes from ``linux/`` -- an override source directory for
  development and an archived tarball for releases.
* The hard requirements of the target carry over as gates rather than as
  hand-built compilers: a MIPS I default, soft-float, C only, and an
  instruction-set gate over every target binary.

Consequences
============

* Buildroot's internal toolchain replaces hand-built compiler scripts;
  ``make legal-info`` and ``make sdk`` come with it.
* The carried MIPS I patch is a maintenance cost, and the build system's
  compiler and C-library versions follow the chosen release, so they must be
  matched or re-verified.
* The switch is gated by the instruction-set and image-size checks and by a
  device run, so a compiler change cannot regress the target unnoticed.
* Option B remains a documented fallback should the carried MIPS I patch prove
  unsustainable; the other components do not depend on this choice either way.
