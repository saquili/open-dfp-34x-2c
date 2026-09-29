.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==============================================
ADR 0008: Repository name ``open-dfp-34x-2c``
==============================================

:Status: Accepted
:Date: 2026-09-29
:Supersedes: ADR 0001

Context
=======

ADR 0001 named the repository after the system-on-chip, ``rtl9601d-linux``.
What the repository supports is narrower than the chip: one product, the ODI
DFP-34X-2C2 and DFP-34X-2C3 GPON SFP ONU sticks. The device tree describes that
board, the optics support is for the laser driver fitted to it, and the image
layout is its flash layout. Other products built on the same chip differ in
exactly those parts, so a name after the chip promises support that this
repository does not give.

Decision
========

Rename the repository ``open-dfp-34x-2c``: "open" for what it is, an open,
from-source firmware, and "dfp-34x-2c" for the product family it supports, the
2C2 and 2C3 variants.

Names of the silicon stay where they name the silicon: the device-tree
compatibles, the driver names and the file names inside ``linux/`` keep
``rtl9602c``, because that is the hardware they drive.

Consequences
============

* The name states the scope as the product, which is what a user holding the
  stick looks for.
* Supporting another product would need a rename or an umbrella project, the
  same trade ADR 0001 accepted.
* The name incorporates a third-party product name. The README's notice now
  names the stick's manufacturer as well as Realtek, and no third-party logo is
  used.
* The rename happened before the repository was published, so nothing outside
  refers to the old name. Records written before this one keep it, as records
  do.
