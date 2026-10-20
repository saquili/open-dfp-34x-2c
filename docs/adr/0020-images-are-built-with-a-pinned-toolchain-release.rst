.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==========================================================
ADR 0020: Build the images with a pinned toolchain release
==========================================================

:Status: Accepted
:Date: 2026-10-20
:Deciders: the maintainer

Summary
=======

The cross toolchain is released on its own, by the repository's toolchain
workflow, and the images are built with the release that a file in the
repository pins by its digest. A release of the firmware no longer builds the
compiler twice, and a commit always builds with the same toolchain.

Context
=======

Every build of the images built its own cross toolchain first: binutils,
gcc in two stages and uClibc-ng (ADR 0005). On GitHub's runners of a private
repository, with two processors, that is a large part of the build, and the
release workflow builds twice to compare the two. The first release took over
an hour per build.

The toolchain changes far less often than the images: its inputs are the
pinned Buildroot, its carried patches, ``toolchain_defconfig`` and the C
library's configuration. Its kernel headers come from ``linux/``, but the
programs of this tree need the uapi headers of the commit they are built at,
whatever toolchain they are built with.

Options considered
==================

A toolchain built in every image build
    As before. Self-contained, but slow, and every build repeats the same
    work.
The latest toolchain release, whatever it is
    Simple, but the same commit could build another image a month later.
A toolchain release pinned in the repository
    A file names the release by its tag and the size and SHA-256 of its
    files; a new toolchain is a release and a commit that changes the file.
    Chosen.

Decision
========

* ``.github/workflows/toolchain.yml`` releases the toolchain, tagged
  ``toolchain-<the time of its commit in UTC>``: the relocatable tarball
  that ``make sdk`` packs for ``toolchain_defconfig``, built twice and
  compared, with the pieces of the C library's link beside its sysroot
  (for ``tools/lib-trim``), the sources and licences of everything in it,
  and their digests. It is not the
  repository's latest release, which stays the firmware's.
* ``br2-external/toolchain/PIN`` names the release. ``make`` fetches its
  tarball, refuses it unless its size and SHA-256 are the pinned ones,
  unpacks it, and merges ``configs/external-toolchain.config`` over the
  board's defconfig, which takes it as Buildroot's external toolchain. A
  pin that names no release, or ``TOOLCHAIN=source``, builds the toolchain
  from source as before.
* ``toolchain_defconfig`` builds the C library and libgcc with the board's
  flags (size, no position independence, partial RELRO), so that they are
  the ones the board's own build of the toolchain makes.
* The uapi headers of the commit's ``linux/`` are installed over the
  toolchain's, from the archive the kernel is built from
  (``br2-external/external.mk``).
* A release of the firmware carries the toolchain release's sources and
  licences in its own (``make legal-info``), since the image holds the C
  library.
* The release workflow builds its two images on two runners at once and
  compares them in a job of its own.

Consequences
============

* A release of the firmware builds no compiler: on two processors it takes
  the time of one build of the images without the toolchain, since its two
  builds run at once.
* A commit names its toolchain exactly; building it again later fetches the
  same files, refused if they changed.
* A change of the toolchain's inputs takes a toolchain release and a commit
  of the pin before the images use it; until then the images keep the
  pinned one. ``TOOLCHAIN=source`` builds an image with the toolchain of the
  checked-out tree, for that work.
* A build needs the release's files: from the network, or in the download
  directory. The workflows fetch them through GitHub's API with their own
  token, which also reaches a private repository.
* The toolchain's kernel headers are those of the commit it was built from;
  the C library was built against them, and the programs see this commit's.
