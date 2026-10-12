.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=====
tools
=====

Host-side tooling for building, checking and bringing up the product. Each
subdirectory has a README of its own, and ``make check`` runs their tests:

* ``bench/`` -- the serial-console bench: console attach, the bootloader
  driver, ramboot, and flashing with the bank guard.
* ``provenance/`` -- the provenance gates that keep the tree clean.
* ``equiv/`` -- the equivalence tools that prove the kernel code is unchanged
  across the clean-up steps.
* ``isa-gate/`` -- the instruction-set gate that rejects any non-MIPS-I code in
  a target binary.
* ``kernel-archive/`` -- the archive of the committed ``linux/`` tree that the
  distribution build takes its kernel and its kernel headers from.
* ``kernel-series/`` -- what the kernel series changes in the kernel's own
  files, the check that the handbook page explains each, and the export of the
  networking-core patches.
* ``image-budget/`` -- the size check of the two flash images against their
  partitions.
* ``lib-trim/`` -- the cut of the image's shared libraries to what its programs
  use, with the check that every program finds what it needs.
* ``docs/`` -- the rendering of the repository's pages to HTML, which fails on
  any warning of docutils.

One script stands alone:

* ``case-insensitive-checkout`` -- sets up a sparse checkout that leaves out
  the kernel's paths that differ only in case, for a working tree on a file
  system that ignores case (see ``CONTRIBUTING.rst``).
