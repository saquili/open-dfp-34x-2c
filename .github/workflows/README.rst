.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=================
.github/workflows
=================

Continuous-integration workflows.

The gate that a change must pass includes: kernel checkpatch and a
warnings-clean build of the board configuration; sparse; device-tree binding
and schema checks; a documentation build; the kernel selftests build; the OMCI
daemon build, unit tests and coverage; the distribution build; the image-size
and instruction-set gates; reproducibility; ``reuse lint``; and all the
provenance gates. The merge rule is a green suite, full line coverage where it
is measured, and a device run.

Every job runs in one pinned host build environment, a Debian 13 container
named by digest, and installs its tools from that Debian release -- except the
REUSE tool, pinned to a version, and the device-tree schema checker, which is
the latest release of dtschema (pin it if a release disagrees with the
kernel's bindings).

``gates.yml`` -- on every change
================================

``provenance``
    ``tools/provenance/check selftest``, then ``check all --history``: the
    tree and every commit. ``make check`` runs the same gates against a
    local ``main``, which a checkout in continuous integration does not
    have.
``equivalence-tools``
    The self-tests of ``tools/equiv``.
``licensing``
    ``reuse lint``, with the REUSE tool pinned.

``kernel.yml`` -- on changes to ``linux/``
==========================================

``linux/`` is a kernel release imported with ``git subtree add --squash``,
with our commits on top. ``.github/scripts/kernel-series`` finds the latest
import and the files our series adds or changes. The jobs judge those: a
warning about an upstream file is printed and does not fail a job; a tool
that stops with an error does.

``build``
    A ``W=1`` build of each configuration in the matrix; it fails on any
    diagnostic in a file of our series.
``checkpatch``
    ``git subtree split -P linux`` -- the kernel history as it would be
    published -- checked out as a tree of its own, and
    ``scripts/checkpatch.pl --strict`` on every commit above the imported
    release, with SPDX expressions validated by ``scripts/spdxcheck.py``.
    The ``Co-Authored-By:`` trailer, which checkpatch does not know, is
    taken out of the patches first, and ``FILE_PATH_CHANGES`` is not
    reported: a series adds its MAINTAINERS entries in its last commit.
    With no commits there yet, it checks that the split is the imported
    release. The split needs the commit the import was squashed from, which
    the repository keeps with a tag (``CONTRIBUTING.rst``); push that tag
    with the branch.
``dt-bindings``
    ``make dt_binding_check`` on the bindings the series adds or changes,
    or on all of them while it has none.
``docs``
    ``make htmldocs`` for the documentation directories our pages go to,
    and ``scripts/kernel-doc -none -Wall -Werror`` on the sources of the
    series.

Still to come, each with the work that needs it: the board configuration
and ``dtbs_check W=1`` in the build matrix, sparse, and the selftests build
with the kernel series; the OMCI daemon with the userspace; the distribution
build, the image-size and instruction-set gates and the nightly
reproducibility build with the distribution.
