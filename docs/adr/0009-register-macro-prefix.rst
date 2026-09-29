.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=====================================================
ADR 0009: Register macros take the driver's prefix
=====================================================

:Status: Accepted
:Date: 2026-09-29
:Amends: ADR 0006

Context
=======

ADR 0006 gave register and field macros the form ``RTL9601D_<BLOCK>_<REG>``,
after the system-on-chip. ADR 0008 then kept ``rtl9602c`` for everything else
inside ``linux/``: the driver and file names, the functions, the Kconfig
symbols and the device-tree compatibles, because that is the name of the
hardware block they drive. Macros under the chip's name would give one driver
two prefixes, and every register access would read differently from the
function around it.

Decision
========

Register and field macros take the prefix of the code that uses them:
``RTL9602C_<BLOCK>_<REG>`` and ``RTL9602C_<BLOCK>_<REG>_<FIELD>``. The rest of
ADR 0006 stands: block mnemonics chosen by us from function, standards terms
where a register implements a standard concept, only the registers the driver
uses, and a rename that leaves the object code unchanged.

Consequences
============

* The rename is still done once, now with this prefix.
* ``RTL9601D`` keeps naming the system-on-chip where the text is about the chip
  or the board, as in the README and the handbook.
* Renaming the drivers or the compatibles, should upstream review ask for the
  chip's name, is a change of its own: unlike a macro rename it changes the
  object code and the device tree, so it needs its own test on the board.
