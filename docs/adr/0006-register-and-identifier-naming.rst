.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

====================================================
ADR 0006: Name registers and identifiers by function
====================================================

:Status: Accepted
:Date: 2026-09-29
:Deciders: the maintainer

Summary
=======

Every register and field macro is named after what it does, with the
standards' vocabulary where one applies and with the prefix of the drivers
that use it. What a register does is established on the stick in the
project's local lab, and a rename is proven correct by unchanged object
code.

Context
=======

A register's name should tell a reader what the register does. The project
works out the function of each register on the stick in its local lab, by
measuring it or by testing the function it drives, and records how
(``docs/policy/provenance.rst``, "How hardware facts are written"). The name
should carry that function.

Inside ``linux/`` the drivers, their files, functions and Kconfig symbols
and the device-tree compatibles use ``rtl9602c``, the name of the hardware
block they drive (ADR 0001). Macros under another prefix would give one
driver two prefixes, and every register access would read differently from
the function around it.

Decision
========

Name every register and field macro systematically, from function:

* ``RTL9602C_<BLOCK>_<REG>`` for a register offset and
  ``RTL9602C_<BLOCK>_<REG>_<FIELD>`` for a field: the prefix of the code that
  uses them, with block mnemonics chosen by us.
* Standard terms wherever a register implements a standard concept: the
  ITU-T GPON vocabulary (GTC, BWMAP, PLOAM, GEM, T-CONT, Alloc-ID, ONU-ID,
  EqD) and the IEEE and SFF names.
* Only the registers the drivers use are defined; unused macros are removed.

Consequences
============

* A rename or a removal of macros must not change the object code. That is
  the proof that a renaming is mechanical and correct: macros vanish in the
  preprocessor, so the objects before and after are byte-identical.
* The documentation, log lines and devlink strings use the same names.
* ``RTL9601D`` names the system-on-chip where a text is about the chip or
  the board, as in the README and the handbook.
* Renaming the drivers or the compatibles after the chip, should upstream
  review ask for it, is a change of its own: unlike a macro rename it
  changes the object code and the device tree, so it needs its own test on
  the board.
