.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===========
tools/vulns
===========

The known vulnerabilities of what the image is made of, looked up every day by
``.github/workflows/vulns.yml``, which fails while there is one to act on.

A vulnerability of the stick's software is published somewhere else each time:
the NVD for the packages of the build system, the kernel's own CVE records for
Linux, the release notes of Mongoose for the panel's server, the point
releases of Buildroot for its long-term-support series. Each is looked at in
its own way, and each check says what it found as Markdown, for the summary of
the workflow's run and for a person at a terminal.

Usage
=====

::

    tools/vulns/vulns packages PKG_STATS_JSON [--accepted FILE]
    tools/vulns/vulns kernel [--makefile FILE] [--releases FILE] [--cves DIR]
    tools/vulns/vulns mongoose [--header FILE] [--latest VERSION]
    tools/vulns/vulns buildroot [--pin FILE] [--index FILE]

The exit status is 0 with no finding, 1 with a finding and 2 when a file
cannot be read or a page fetched. What is not given on the command line is
fetched: kernel.org's ``releases.json``, the newest release of Mongoose from
GitHub's API, and Buildroot's download index. It needs Python 3, standard
library only.

``packages``
    Judges the JSON report of Buildroot's ``support/scripts/pkg-stats`` for the
    board's configuration, made with ``--nvd-path`` so that it holds each
    package's CVEs. A CVE of a package the stick runs (one with a ``target``
    infrastructure) is a finding, unless ``accepted`` names it with the reason
    it does not apply. A CVE that Buildroot's recipe of the package ignores,
    as for a patch the tree carries (``BUSYBOX_IGNORE_CVES``), is not reported
    by pkg-stats at all. The CVEs of the packages that only build the image,
    such as the host's ``mkimage`` or ``bison``, are listed and not judged, and
    so are the CVEs that pkg-stats cannot place against a version. The kernel
    is left to ``kernel``, since pkg-stats cannot tell what our tree holds, and
    the programs of this tree have no CVE records.

    The workflow runs pkg-stats on the board's own defconfig, which builds the
    toolchain from source, so that the C library the stick runs, uClibc-ng, is
    among the packages; the release toolchain that the images are built with
    is made from the same versions.

``kernel``
    Compares the version of ``linux/`` with the newest release of its series
    that kernel.org lists as stable or long-term, and is a finding when a newer
    one is out, or when the series is no longer listed. With ``--cves``, a
    checkout of the kernel's CVE records
    (``https://git.kernel.org/pub/scm/linux/security/vulns.git``; only the
    ``.dyad`` files are read), it names the CVEs that a later release of the
    series fixes, which updating fixes, and those that no release of the
    series fixes yet, which updating does not. The list is not cut down to the
    code that the board's configuration builds: that is judged when the kernel
    is updated.

``mongoose``
    Compares the ``MG_VERSION`` of ``pond/third_party/mongoose/mongoose.h``
    with the newest release of Mongoose. A newer release is a finding, to be
    read for security fixes: Mongoose publishes no CVE list of its own.

``buildroot``
    Compares the pinned release (``buildroot/VERSION``) with the newest point
    release of its series in Buildroot's download index; the point releases
    of a long-term-support series carry its security fixes.

The accepted list
=================

``accepted`` holds the CVEs of packages the stick runs that do not apply to the
image, one per line: the CVE, the package as pkg-stats names it, and the
reason. An entry is only for a CVE that cannot apply, such as one in a part of
the package that the image does not build; a CVE that a patch fixes belongs in
the package's ``IGNORE_CVES`` beside the patch. An entry that pkg-stats no
longer reports is named by the check, and can go.

Tests
=====

``tests/run`` runs the self-tests, on files the tests make; they use no
network. ``make check`` and the workflow ``gates.yml`` run them.
