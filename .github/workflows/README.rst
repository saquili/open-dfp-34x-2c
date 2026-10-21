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
the board, and the kernel documentation. The flash images, built twice and
compared for reproducibility, and the cross toolchain, built once and compared
with a second build on request, are built when started by hand, and so are
their releases. A change to the panel on main
builds its demo, in which a mock stick answers it in the browser, and puts it
on the repository's GitHub Pages site (``demo.yml``). The merge rule is a
green suite, full line coverage where it is measured, and a device run
(``tests/device``).

Every job that builds or tests runs in one pinned host build environment, a
Debian 13 container named by digest, and installs its tools from that Debian
release -- except the REUSE tool, pinned to a version, and the device-tree
schema checker, which is the latest release of dtschema (pin it if a release
disagrees with the kernel's bindings). The jobs that only compare or release
files that another job built, or look up the runs of another workflow, run on
the runner itself.

A release of the firmware is made by ``release.yml`` alone, and a release of
the toolchain by ``toolchain.yml`` alone, each from the commit it is started
on: nothing built elsewhere goes into one. The firmware's is made only of a
commit that ``gates.yml`` passed on, and is the repository's latest release
once it is published; the toolchain's never is.

The jobs run on GitHub's hosted runners (``ubuntu-24.04``), which start each
job's container on a machine of their own and discard it afterwards. No job
asks for a self-hosted runner: the workflows run on pull requests, and on a
public repository a pull request from a fork could otherwise run its code on
a machine of the project's.

Room on the runner
==================

A build of the images or of the toolchain needs some 9 GB of disk. The first run
of ``release.yml`` on a hosted runner stopped after 43 minutes with "No space
left on device". The runner's disk holds tools that no build here uses, so every
job that builds mounts their directories into its container -- .NET
(``/usr/share/dotnet``), the Android tools (``/usr/local/lib/android``), GHC
(``/opt/ghc``) and the runner's cache of tools (``/opt/hostedtoolcache``) -- and
removes what they hold: ``toolchain.yml`` in its first step, "Make room on the
runner", and ``image.yml`` and ``release.yml`` in the background of the step
that builds, which needs that room only later; removing it first took five of
their minutes. Each prints the free space before and after.

What a build of the images takes, and what is kept
==================================================

A build of the images with the pinned toolchain took 32 minutes on a hosted
runner, of which 13 went to building CMake for the host and 4 to the archive
of ``linux/``. The jobs install CMake from Debian, which Buildroot takes in
place of its own when it is new enough, and keep two things between runs with
``actions/cache``: the archive of ``linux/`` (``build/kernel``), under the id
of the tree it is made from, which ``tools/kernel-archive`` takes as it would
the one it makes; and the downloads (``build/dl``), under the files of the
packages that name them, which Buildroot checks against their hashes and the
toolchain's fetch against its pin. A cache holds nothing that the commit does
not determine.

The pinned toolchain
====================

``image.yml`` and ``release.yml`` build the images with the toolchain of the
release that ``br2-external/toolchain/PIN`` names, as ``make`` does
(``docs/handbook/building.rst``). ``br2-external/toolchain/fetch`` takes its
files through GitHub's API with the job's own token (``GH_TOKEN``, set to
``github.token``), which reaches the releases of a private repository too,
and from the repository the workflow runs in (``GITHUB_REPOSITORY``); the
jobs ask for no permission beyond reading the repository's contents.

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
    The self-tests of ``tools/image-budget``, the size check of the flash
    images.
``lib-trim``
    The self-tests of ``tools/lib-trim``, the cut of the image's shared
    libraries, with the end-to-end tests built by Debian's MIPS cross compiler.
``image-scripts``
    ``br2-external/tests/run``: the tests of the post-image script (with the
    real ``mksquashfs``), of the boot script and the inittab, of the board's
    defconfig, of the listing that compares two root filesystem trees, of
    the comparison of the instruction-set gate's findings with the list of the
    known ones, and of ``br2-external/toolchain/fetch``, which fetches the
    pinned toolchain. A skipped test fails the job.
``licensing``
    ``reuse lint``, with the REUSE tool pinned.

``toolchain.yml`` -- when started by hand
=========================================

The cross toolchain, built from source by the pinned Buildroot with
``br2-external/configs/toolchain_defconfig``, checked, and released; two
builds take the better part of an hour, so the workflow does not run on a
push. Its input "release", on by default, says whether it makes a release.

``toolchain``
    In the same pinned container, with these steps in order: room made on
    the runner; the pinned Buildroot and the kernel archive
    (``make buildroot`` and ``tools/kernel-archive``); the first build
    (``make sdk``); the toolchain tarball unpacked and relocated; what was
    built (versions and the compiler's defaults); the negative control of the
    instruction-set gate; the test programs built with the toolchain, passed
    through the gate and run under ``qemu-mips``; the gate over every target
    object of the toolchain; with "release" on, the files of the release,
    kept as the artifact ``toolchain``; and, with "compare" on, a second
    build in another directory and the comparison of the two, which have to
    be identical but for the name of the directory and two values computed
    from the contents (``br2-external/README.rst`` says which). The second
    build and the comparison run whatever the result of the steps before
    them. A release is made of one build, as toolchains are released
    elsewhere; the comparison is for a change of the build's inputs. With the
    toolchain as the defconfig builds it, the gate reports findings in one
    function of libgcc (``br2-external/README.rst`` says which), and the
    step holds them to a list: it passes when the findings are exactly the
    list in ``br2-external/tests/toolchain/known-findings.txt``, fails when
    one is new, gone or different, and shows that its comparison can fail
    by running it against a list without the function's group in
    ``libgcc.a``. The
    files of the release are the tarball of the first build,
    ``open-dfp-34x-2c-toolchain.tar.gz``, the sources and licences of
    everything in it (``make legal-info`` of that build) as one archive, and
    their ``SHA256SUMS``.
``release``
    With "release" on, once ``toolchain`` passed, the comparison included;
    the only job that may write to the repository. It takes the artifact,
    checks the digests and makes the release ``toolchain-<version>`` on that
    commit, the version being the time of the commit in UTC, refused when the
    tag exists already. The release is not marked as the repository's
    latest, which stays the firmware's, and its notes give the lines of
    ``br2-external/toolchain/PIN`` that name it; a commit that puts them in
    the file has the images built with it (``br2-external/README.rst``, "The
    toolchain of a release").

``image.yml`` -- when started by hand
=====================================

The flash images of the board, built from source with
``br2-external/configs/odi_dfp_34x_2c3_defconfig`` by ``make`` in the top
directory, with the pinned toolchain, and checked. One job, in the same pinned
container, with these steps in order: room made on the runner; the build
(``make``, which also judges the result: ``k0`` and ``r0`` have to fit their
partitions, the instruction-set gate has to pass over the whole root
filesystem tree, and the digests have to check); what was built (the
compiler, the image header, the shared objects, and no ``libgcc_s`` among
them); the negative control of the gate; the programs run under ``qemu-mips``
(``br2-external/tests/image/check-applets``); the bench's dry run of the root
filesystem image (``make flash ... ARGS=--dry-run``, which names no board);
the images, the update file and the package of the shipped firmware's page
kept as the workflow artifact ``images``; and a second build in another
directory, with the same unpacked toolchain, and the comparison of the two:
the images byte for byte and the root filesystem trees by mode, size and
digest of every file (``br2-external/tests/image/tree-digest``). The steps
after the first build run whatever its result. Two builds take the better
part of an hour, so the workflow does not run on a push.

``release.yml`` -- when started by hand
=======================================

A release of the commit it is started on, in four jobs.

``gates``
    The runs of ``gates.yml`` on exactly that commit (``gh run list
    --commit``): one of them, of a push or started by hand, has to be
    completed with success. The run of a pull request does not count, as it
    tests the pull request merged into its base, and neither does a
    cancelled one, which a push to the same branch makes of the run before
    it. Otherwise the job fails and says so: run ``gates.yml`` by hand on the
    branch while that commit is its head, and start the release again once it
    has passed. The result is looked up, not made again: the commit has it.
``build``
    Once ``gates`` passed: two builds at once, ``a`` and ``b``, each on a
    runner of its own, in the same pinned container as ``image.yml`` and in an
    output directory of its own. Each makes room on the runner, builds the
    images with the pinned toolchain, takes the version the image names itself
    by (``VERSION_ID`` of its ``/etc/os-release``, the time of the commit in
    UTC, as ``2026.10.19-2130``) and refuses one of uncommitted sources,
    checks that no bench's files (``/etc/board.conf``, the token of scripts,
    ssh keys) are in the image, runs the gate's negative control and the
    programs under ``qemu-mips``, and keeps the images, the update file, the
    package of the shipped firmware's page, their digests and the listing of
    the root filesystem tree as the artifact ``build-a`` or ``build-b``. Build
    ``a`` also gathers the licences and sources of everything in the images,
    the toolchain's among them (``make legal-info``), and keeps the files of
    the release and their SHA-256 digests as the artifact ``release``.
``compare``
    Once both builds passed: it takes their artifacts, and the images, the
    update file and the package have to be identical byte for byte, and
    their digests and the listings of the trees the same. A release is only
    made of a build that another build repeats.
``draft``
    Once ``build`` and ``compare`` passed; the only job that may write to the
    repository. It takes the artifact ``release``, checks the digests, and
    makes a draft release of the tag ``v<version>`` on that commit with the
    files; it is refused when the tag or its release exists already. A person
    tests the draft's files on two sticks against the lab OLT and publishes
    it, which makes the tag. It is a full release, not a pre-release, and
    published it is the repository's latest ("Set as the latest release" on
    GitHub's page, the default).

As the two builds run at once, a release takes about the time of one build.

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
    ``scripts/checkpatch.pl --strict`` on every commit of the series, with
    SPDX expressions validated by ``scripts/spdxcheck.py``. The series is
    the commits of the split that descend from the release of the first
    import, but for the releases imported since (``kernel-series base`` and
    ``upstream``). The split holds more: before the first import,
    ``linux/`` held a README, which a commit removed again, and the split
    keeps the README's commit, and the project's commits up to the one that
    removed it, which hold no kernel and are not checked.
    The ``Co-Authored-By:`` trailer, which checkpatch does not know, is
    taken out of the patches first, and five types are not reported:
    ``FILE_PATH_CHANGES``, as a series adds its MAINTAINERS entries in its
    last commit; ``REPEATED_WORD``, which takes a correct "had had" in a
    published commit message for a typo; ``DT_SPLIT_BINDING_PATCH``, which
    takes a binding a commit deletes for a file outside the bindings;
    ``MACRO_ARG_PRECEDENCE``, which asks for parentheses around a macro
    argument that names a member, where C allows none; and
    ``UNCOMMENTED_DEFINITION``, which misses the comment of a lock when the
    patch removes the old definition between the comment and the new one.
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
