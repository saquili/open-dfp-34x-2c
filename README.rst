.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==============
rtl9601d-linux
==============

A from-source Linux distribution for GPON SFP ONU sticks built on the
Realtek RTL9601D system-on-chip.

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
first architecture decision records are in place. The kernel, the distribution
build and the userspace are populated by later work items; each top-level
directory carries a short README naming the component that will fill it.

Building
========

The top-level ``Makefile`` defines the entry points. Until the component
behind a target lands, the target prints what it will do and which work item
implements it, then exits::

    make               # build the flashable images (default)
    make linux         # build the kernel
    make omcid         # build the OMCI daemon
    make sdk           # build a relocatable cross toolchain
    make legal-info    # collect the source-and-licence bundle for a release
    make docs          # build the documentation
    make check         # run the offline gates
    make ramboot       # boot a trial kernel over the bench
    make flash BANK=0  # flash an image to the scratch bank
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

"Realtek" and "RTL9601D" are names of Realtek Semiconductor Corp. This project
is not affiliated with, authorised by, or endorsed by Realtek Semiconductor
Corp. Other product and company names that appear here may be the trademarks of
their respective owners. No third-party logos are used or distributed.
