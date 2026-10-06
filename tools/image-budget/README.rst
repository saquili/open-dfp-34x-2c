.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===================
tools/image-budget
===================

The size check of the flash images.

The kernel and the root filesystem of bank 0 each have a partition of fixed
size (``docs/handbook/boot-contract.rst``): ``k0`` takes the kernel image,
1856 KiB (1900544 bytes), and ``r0`` the root filesystem image, 1984 KiB
(2031616 bytes). ``image-budget`` compares the two images that the build leaves
with those sizes and prints what is left of each partition, or by how much an
image is over.

Usage
=====

::

    tools/image-budget/image-budget [--enforce] [--images DIR] [NAME[=FILE] ...]

``NAME`` is ``k0`` or ``r0``; with none, both are checked. The image is the file
``k0.uImage`` or ``r0.squashfs`` in ``DIR`` (default ``build/image/images``, where
``make`` leaves them), or the ``FILE`` given after the name. It prints one line
for each::

    k0: 2543210 bytes against a budget of 1900544 (1856 KiB): over the budget by 642666 bytes (build/image/images/k0.uImage)
    r0: 812345 bytes against a budget of 2031616 (1984 KiB): margin 1219271 bytes (60.0% of the partition) (build/image/images/r0.squashfs)

An image of exactly the size of its partition fits, with a margin of 0 bytes.
An image over its budget is reported and is not an error, so that the build can
go on reporting the size of an image that does not fit yet. ``--enforce`` makes
it one::

    tools/image-budget/image-budget --enforce r0     # fails if r0 is over
    tools/image-budget/image-budget k0               # reports k0, whatever it is

Exit status: 0 when every image named was read (and, with ``--enforce``, fits);
1 when ``--enforce`` is given and an image named is over its budget, after the
lines above and a line on standard error that names it; 2 for a usage error and
for an image that is missing, unreadable, not a file or empty. A check that read
nothing has proved nothing, so a missing image is an error whether or not
``--enforce`` is given.

What it does not check
======================

It reads sizes. Whether the file is a legacy U-Boot image with the load address
the boot loader expects, or a SquashFS image with a compression the kernel reads,
is checked by the bench before it sends anything: ``make flash IMAGE=<file>
TARGET=kernel|rootfs ARGS=--dry-run`` (``docs/handbook/flashing.rst``). The
bench holds the same two sizes, and the tests of this tool compare them.

Tests
=====

::

    tools/image-budget/tests/run [-v]

They check the verdicts at the edges (one byte under, exactly at and one byte
over the budget), the exit statuses with and without ``--enforce``, the
selection of one image, a missing, an empty and a misnamed image, and that the
two sizes are those of the flash layout page and of the bench. They need Python
3.11 or later and nothing else.
