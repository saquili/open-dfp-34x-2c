.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=================================================
ADR 0001: Name the repository ``open-dfp-34x-2c``
=================================================

:Status: Accepted
:Date: 2026-09-29
:Deciders: the maintainer

Summary
=======

The repository is named after the product it supports, the ODI DFP-34X-2C
GPON stick, rather than after its chip, because other products built on the
same chip differ in exactly the parts this repository provides.

Context
=======

The project supports one product: the ODI DFP-34X-2C2 and DFP-34X-2C3 GPON
SFP ONU sticks, built around the Realtek RTL9601D system-on-chip. The device
tree describes that board, the optics support is for the laser driver fitted
to it, and the image layout is its flash layout. Other products built on the
same chip differ in exactly those parts.

Options considered
==================

A name after the chip
    Promises support for every product built on the chip, which the
    repository does not give.
An invented code name
    Says nothing about what the repository is.
A name after the product
    Chosen.

Decision
========

Name the repository ``open-dfp-34x-2c``: "open" for what it is, an open
firmware built from source, and "dfp-34x-2c" for the product family it
supports, the 2C2 and 2C3 variants.

Names of the silicon stay where they name the silicon: inside ``linux/``,
the device-tree compatibles, the driver names and the file names use
``rtl9602c``, the name of the hardware block they drive.

Consequences
============

* The name states the scope as the product, which is what a user holding
  the stick looks for.
* Supporting another product would need a rename or an umbrella project;
  that trade is accepted for clarity.
* The name contains a third-party product name. The README carries a
  trademark and non-affiliation notice that names Realtek and the stick's
  manufacturer, and no third-party logo is used.
