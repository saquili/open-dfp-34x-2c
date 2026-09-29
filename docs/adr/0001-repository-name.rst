.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==================================
ADR 0001: Repository name
==================================

:Status: Superseded by ADR 0008
:Date: 2026-09-21

Context
=======

The project needs a name. It supports exactly one system-on-chip and the SFP
GPON ONU stick built around it. A broader family name would imply support this
project does not have; an invented code name would say nothing about what the
repository is.

Decision
========

Name the repository ``rtl9601d-linux``, after the one SoC it supports.

Consequences
============

* The name states the scope plainly: a Linux distribution for the RTL9601D.
* Supporting a different part later would need either a rename or a broader
  umbrella project; that trade is accepted for the clarity now.
* The name incorporates a third-party product name. The README carries a
  trademark and non-affiliation notice, and no third-party logo is used.
