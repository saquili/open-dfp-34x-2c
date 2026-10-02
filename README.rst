.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===============
open-dfp-34x-2c
===============

A from-source Linux distribution for one product: the ODI DFP-34X-2C2 and
DFP-34X-2C3 GPON SFP ONU sticks, which are built on the Realtek RTL9601D
system-on-chip. Other products built on the same chip are not supported.

Scope
=====

This repository holds what a device maker ships to build the product from
source:

* the cross toolchain (through the distribution build system);
* the Linux kernel, with the board and SoC support this device needs;
* the userspace, including the OMCI management daemon;
* the image and flash tooling; and
* the hardware documentation.

The **structure** follows mainline Linux conventions -- each part is laid out
the way its own upstream community lays it out, so it stays recognisable and
liftable. The **scope** is the whole product, not one kernel tree. See
``docs/adr/`` for the reasoning and ``docs/policy/provenance.rst`` for what may
and may not enter.

Status
======

Early bring-up. The repository skeleton, licence, provenance policy and the
architecture decision records are in place. ``linux/`` holds the Linux 6.18.48
release with our kernel series on top (see "The kernel tree" in
``CONTRIBUTING.rst``): support for the RTL9602C system-on-chip and its Lexra
core, the DFP-34X-2C3 board and ``rtl9602c_defconfig``, and the drivers of the
switch, its CPU port, the GPON and PON MACs, the optics interface and the PON
class, with their device tree bindings and hardware documentation under
``linux/Documentation/``. The continuous integration builds the kernel for
``tinyconfig`` and for the board's configuration, checks the bindings and the
board's device tree, builds the documentation and runs ``checkpatch.pl`` over
every commit of the series. The kernel has been started from RAM on the
stick, where it brings the GPON link up against a laboratory OLT, exchanges
OMCI frames and passes the laboratory tests of the switch's hardware paths
(bridging, routing with address translation, PPPoE and shaping). Running on
an operator's line is not tested yet. The bench for bring-up and flashing
(``tools/bench``), the instruction-set gate for
target binaries (``tools/isa-gate``) and the first pages of the handbook
(``docs/handbook``) are in place. The distribution build makes the two flash
images of the stick from source: ``buildroot/`` pins the build system and carries
the patch that adds the MIPS I variant, and ``br2-external/`` builds the cross
toolchain, the kernel and a root filesystem of BusyBox, dropbear and a boot
script, every program static, with checks of its own
(``br2-external/README.rst``, ``docs/handbook/building.rst``). Both images
fit their partitions, and the build fails if either does not: the kernel in the
flash is the board's configuration less seven options that the product does not
use (the flash profile), and the build checks the kernel's final configuration
against the options the product needs. The images have been written to the
stick with the bench and start from its flash, from the boot loader's own
countdown, to a shell and an ssh server. The userspace beyond them, the OMCI
daemon first, is populated by later work items; each top-level directory still
waiting for its component carries a short README naming it.

Building
========

The top-level ``Makefile`` defines the entry points. ``make`` builds the flashable
images and ``make sdk`` the cross toolchain as a tarball: both run Buildroot, need
Linux and the packages that ``docs/handbook/building.rst`` names (it also says what
the build makes, how big each image is and how to put it on the board), and fetch
the sources of their packages by hash. ``make linux`` builds the kernel alone with
the Debian cross compiler and needs a MIPS cross compiler (``mips-linux-gnu-gcc``
on Debian, or name yours with ``CROSS_COMPILE=``) and ``mkimage``. ``make
ramboot`` and ``make flash`` run the bench in ``tools/bench`` (Python 3.11 or
later; its README describes the settings, the commands and the bank guard).
``make buildroot`` needs Python 3 and ``patch``. Until the component behind any
other target lands, the target prints what it will do and which work item
implements it, then exits::

    make               # build the flashable images (build/image/images, default)
    make linux         # build the kernel (build/linux/arch/mips/boot/uImage)
    make buildroot     # fetch the pinned Buildroot and patch it (build/buildroot)
    make omcid         # build the OMCI daemon
    make sdk           # build a relocatable cross toolchain (build/toolchain/images)
    make legal-info    # collect the source-and-licence bundle for a release
    make docs          # build the documentation
    make check         # run the offline gates
    make ramboot       # boot a trial kernel from RAM over the bench
    make flash IMAGE=F TARGET=kernel   # flash an image to the scratch bank
    make clean / make distclean

Licensing
=========

Everything authored in this repository is ``GPL-2.0-only``; see ``COPYING`` and
``LICENSES/``. Third-party packages are not committed here: they are fetched by
hash at build time and keep their own licences, and each release ships the
build system's ``legal-info`` bundle. Contributions follow the Developer
Certificate of Origin and the provenance policy; see ``CONTRIBUTING.rst`` and
``docs/policy/provenance.rst``.

Trademarks and affiliation
==========================

"Realtek" and "RTL9601D" are names of Realtek Semiconductor Corp. "ODI" and
"DFP-34X" are names of the stick's manufacturer. This project is not affiliated
with, authorised by, or endorsed by Realtek Semiconductor Corp. or by that
manufacturer. Other product and company names that appear here may be the
trademarks of their respective owners. No third-party logos are used or
distributed.
