.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=================
.github/workflows
=================

Continuous-integration workflows.

On every change, the workflows run the provenance gates and ``reuse lint``;
the tests of the tools, of the bench, of the device tests' own code and of
the distribution's scripts; the tests of each program of this tree on the
host, with checkpatch over their sources and full line coverage of
``omcid/src``; and, on a change to ``linux/``, a warnings-clean build of
``tinyconfig`` and of the board's configuration, checkpatch over every commit
of the kernel series, the device-tree binding checks with ``dtbs_check`` of
the board, and the kernel documentation. The cross toolchain and the flash
images, each built twice and compared for reproducibility, are built when
started by hand. The merge rule is a green suite, full line coverage where it
is measured, and a device run (``tests/device``).

Every job runs in one pinned host build environment, a Debian 13 container
named by digest, and installs its tools from that Debian release -- except the
REUSE tool, pinned to a version, and the device-tree schema checker, which is
the latest release of dtschema (pin it if a release disagrees with the
kernel's bindings).

The jobs run on GitHub's hosted runners (``ubuntu-24.04``), which start each
job's container on a machine of their own and discard it afterwards. No job
asks for a self-hosted runner: the workflows run on pull requests, and on a
public repository a pull request from a fork could otherwise run its code on
a machine of the project's. The two builds of ``image.yml`` and
``toolchain.yml`` need some 9 GB of disk each; neither workflow has run on a
hosted runner yet.

``gates.yml`` -- on every change
================================

``provenance``
    ``tools/provenance/check selftest``, then ``check all --history``: the
    tree and every commit. ``make check`` runs the same gates against a
    local ``main``, which a checkout in continuous integration does not
    have.
``equivalence-tools``
    The self-tests of ``tools/equiv``.
``bench``
    The unit tests of the bench (``tools/bench``), against a simulated
    console and a scripted boot loader, and of the device tests' own code
    (``tests/device``), against canned replies of the stick.
``programs``
    The tests of omcid on the host, its style gate and its coverage gate,
    and its programs for the board; then the tests of onu-utils, pppoed,
    dhcp6d, timed and pond, and checkpatch over their sources, which may report
    checks but no warning or error. They build against the user-space
    headers of this tree's kernel.
``isa-gate``
    The self-tests of ``tools/isa-gate``, the instruction-set gate.
``buildroot-fetch``
    The self-tests of ``buildroot/fetch`` and of ``tools/kernel-archive``, and
    then ``make buildroot``: the pinned Buildroot release is fetched from its
    URL, checked against the pin, and the carried patches are applied to it.
``image-budget``
    The self-tests of ``tools/image-budget``, the size check of the flash images.
``lib-trim``
    The self-tests of ``tools/lib-trim``, the cut of the image's shared
    libraries, with the end-to-end tests built by Debian's MIPS cross compiler.
``image-scripts``
    ``br2-external/tests/run``: the tests of the post-image script (with the real
    ``mksquashfs``), of the boot script and the inittab, of the board's
    defconfig, of the listing that compares two root filesystem trees, and of
    the comparison of the instruction-set gate's findings with the list of the
    known ones. A skipped test fails the job.
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
toolchain as the defconfig builds it, the gate reports findings in two
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
in order: the build (``make``, which also judges the result: ``k0`` and ``r0`` have to
fit their partitions, the instruction-set gate has to pass over the whole root
filesystem tree, and the digests have to check);
what was built (the compiler, the image header, the shared objects, and no
``libgcc_s`` among them); the negative control of the gate; the programs run under
``qemu-mips`` (``br2-external/tests/image/check-applets``); the bench's dry run
of the root filesystem image (``make flash ... ARGS=--dry-run``, which names no
board); the images kept as the workflow artifact ``images``; and a second build
in another directory, with the comparison of the two: the images byte for byte
and the root filesystem trees by mode, size and digest of every file
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
    taken out of the patches first, and two types are not reported:
    ``FILE_PATH_CHANGES``, as a series adds its MAINTAINERS entries in its
    last commit, and ``REPEATED_WORD``, which takes a correct "had had" in a
    published commit message for a typo.
    With no commits there yet, it checks that the split is the imported
    release. The split needs the commit the import was squashed from, which
    the repository keeps with a tag (``CONTRIBUTING.rst``); push that tag
    with the branch.
``dt-bindings``
    ``make dt_binding_check`` on the bindings the series adds or changes,
    or on all of them while it has none, and ``make dtbs_check W=1`` on the
    board's device tree.
``docs``
    ``make htmldocs`` for the documentation directories our pages go to,
    and ``scripts/kernel-doc -none -Wall -Werror`` on the sources of the
    series.

Not run yet: sparse, a build of the kernel's selftests, and a scheduled
reproducibility build of the whole distribution.
