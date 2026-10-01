.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========
buildroot
=========

The pin for the distribution build system (see ADR 0005).

Buildroot itself is not committed here. This directory pins one Buildroot
release by hash and holds the script that turns the pin into a tree ready to
build, so that every build starts from the same bytes. The tree is made in
``build/buildroot`` and is never committed. The patches we carry against the
release go in ``patches/``; there are none yet.

What is here
============

``VERSION``
    The pin: name, version, URL, size and SHA-256 of the release tarball, and
    the fingerprint of the key that signs it. The release is Buildroot
    2025.02.18, the last point release of its long-term-support series. Moving
    to another release means editing this file and nothing else.
``fetch``
    The script that makes the pinned release ready to build (below).
``patches/``
    The patches applied to the release, in name order.
``tests/``
    The self-tests of ``fetch``.

Running it
==========

::

    buildroot/fetch

does the following:

#. looks for the tarball named in the pin's URL in the download directory
   (``$BUILDROOT_DL``, else ``build/dl``) and downloads it from the URL when it
   is not there;
#. refuses the tarball -- exit status 1, nothing extracted -- unless its size
   and SHA-256 are the pinned ones. A download is checked before it gets its
   final name, so a tarball that was refused never stays in the directory under
   the name a later run would trust;
#. unpacks it into ``build/buildroot/buildroot-<version>/``, in a scratch
   directory that is swapped in only when everything has worked;
#. applies ``patches/*.patch`` in name order with ``patch -p1``, without fuzz.
   A patch that does not apply stops the run, is named in the message and
   leaves no tree behind; and
#. writes a stamp, ``build/buildroot/buildroot-<version>.stamp``, that holds the
   pinned SHA-256 and the SHA-256 of each patch.

A run that finds a tree and a stamp equal to what the pin and the patches now
say changes nothing and does not read the tarball or use the network. A changed
pin, or a patch that is changed, added or removed, makes the next run redo the
work from the tarball. The only thing printed on standard output is the path of
the tree. ``-q`` prints errors only; ``--help`` lists the options that name
another pin, patch directory, download directory or build directory.

It needs Python 3.11 or later (standard library only) and ``patch``. On
Debian::

    apt-get install python3 patch

Checking the pin
================

The size and the SHA-256 in ``VERSION`` are checked on every fetch. The
release manager signs a message that lists the same SHA-256; to check that the
pin is the published release, import the key whose fingerprint is in
``VERSION`` and verify the signature::

    gpg --verify buildroot-2025.02.18.tar.xz.sign
    grep SHA256 buildroot-2025.02.18.tar.xz.sign

Tests
=====

::

    buildroot/tests/run [-v]

Each test builds a small release tarball, a pin for it and some patches in a
scratch directory and runs the script as a program. They show that a wrong
hash or size is refused and nothing is extracted, that a patch that does not
apply is named and leaves nothing, that the patches go on in name order, that a
second run is a no-op that needs neither tarball nor network, that a changed,
added or removed patch or a changed pin redoes the work, that a path that
leaves the tree is refused, and that a malformed pin is an error. None uses
the network.
