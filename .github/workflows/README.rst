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
``buildroot-fetch``
    The self-tests of ``buildroot/fetch`` and of ``tools/kernel-archive``, and
    then ``make buildroot``: the pinned Buildroot release is fetched from its
    URL, checked against the pin, and the carried patches are applied to it.
``image-budget``
    The self-tests of ``tools/image-budget``, the size check of the flash
    images. They include a reading of the top-level ``Makefile``: the build has
    to hold ``k0`` and ``r0`` both to their partitions, so a change that stops
    enforcing either fails this job.
``image-scripts``
    ``br2-external/tests/run``: the tests of the post-image script (with the real
    ``mksquashfs``), of the boot script and the inittab, of the board's
    defconfig, of the kernel's flash profile and the check of its final
    configuration (against the kernel's own Kconfig files), of the listing that
    compares two root filesystem trees, and of the comparison of the
    instruction-set gate's findings with the list of the known ones. A skipped
    test fails the job.
``licensing``
    ``reuse lint``, with the REUSE tool pinned.

``toolchain.yml`` -- when started by hand
=========================================

The cross toolchain, built from source by the pinned Buildroot with
``br2-external/configs/toolchain_defconfig`` and checked; two builds take the
better part of an hour, so the workflow does not run on a push. One job, in the
same pinned container, with these steps in order: the pinned Buildroot and the
kernel archive (``make buildroot`` and ``tools/kernel-archive``); the first
build (``make sdk``); the toolchain tarball unpacked and relocated; what was
built (versions and the compiler's defaults); the negative control of the
instruction-set gate; the test programs built with the toolchain, passed through
the gate and run under ``qemu-mips``; the gate over every target object of the
toolchain; a second build in another directory; and the comparison of the two,
which have to be identical but for the name of the directory and two values
computed from the contents (``br2-external/README.rst`` says which). The steps
after the gate over the toolchain's objects run whatever its result. With the
toolchain as the defconfig builds it, the gate reports findings in three
functions of the libraries (``br2-external/README.rst`` lists them), and the step
holds them to a list: it passes when the findings are exactly the list in
``br2-external/tests/toolchain/known-findings.txt``, fails when one is new, gone
or different, and shows that its comparison can fail by running it against a
list without one of the functions. See ``br2-external/README.rst`` for the
checks.

``image.yml`` -- when started by hand
=====================================

The flash images of the board, built from source with
``br2-external/configs/odi_dfp_34x_2c3_defconfig`` by ``make`` in the top
directory, and checked. One job, in the same pinned container, with these steps
in order: the build (``make``, which also judges the result: ``k0`` and ``r0``
have to fit their partitions, the kernel's final configuration has to hold the
options the product needs and none that the flash profile drops, the
instruction-set gate has to pass over the whole root filesystem tree, and the
digests have to check); what was built (the compiler, the image header, the
absence of any shared object); both images against their partitions again
(``tools/image-budget/image-budget --enforce``, whatever ``IMAGE_ENFORCE`` says
in the environment); the kernel's configuration again, and the check's negative
control, the board's complete configuration, which the check has to refuse with
status 1; the negative control of the gate; the static programs run under
``qemu-mips`` (``br2-external/tests/image/check-applets``); the bench's dry run
of both images (``make flash ... ARGS=--dry-run``, which names no board); the
images kept as the workflow artifact ``images``; and a second build in another
directory, with the comparison of the two: the images byte for byte and the root
filesystem trees by mode, size and digest of every file
(``br2-external/tests/image/tree-digest``). The steps after the first build run
whatever its result. Two builds take the better part of an hour, so the workflow
does not run on a push.

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
with the kernel series; the OMCI daemon with the userspace; and the nightly
reproducibility build of the whole distribution.
