.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==========================================
ADR 0006: Register and identifier naming
==========================================

:Status: Accepted; the prefix is amended by ADR 0009
:Date: 2026-09-21

Context
=======

A register or field name can coincide with a name used in non-public material.
Even where no code is copied, a transcribed name is a provenance and legal
risk, and a name learnt that way tells a reader nothing about function.

Decision
========

Rename every register and field macro once, systematically, from function:

* ``RTL9601D_<BLOCK>_<REG>`` for a register offset, and
  ``..._<REG>_<FIELD>`` for a field, with block mnemonics chosen by us.
* Standards terms are used wherever a register implements a standard concept --
  the ITU-T GPON vocabulary (GTC, BWMAP, PLOAM, GEM, T-CONT, Alloc-ID, ONU-ID,
  EqD) and the IEEE and SFF names.
* Only the registers the driver actually uses are defined; unused macros are
  removed.

The aim is not to avoid every coincidence with a generic word, but to guarantee
that no name was transcribed: an audit reports zero full-name matches against
the non-public tables.

Consequences
============

* Renaming and removing macros must not change the object code. That is the
  proof the naming is mechanical and correct: the compiled objects before and
  after are byte-identical, because macros vanish in the preprocessor.
* Documentation, log lines and devlink strings use the same names.
* The old-to-new mapping is kept on the laboratory side only, because its
  left-hand column would be the non-public names; it is never part of this
  repository.
