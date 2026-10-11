.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

================================================
ADR 0003: Mainline structure, device-maker scope
================================================

:Status: Accepted
:Date: 2026-09-21

Context
=======

A device port can be shaped in two ways. It can be a single all-in-one tree
with an ad hoc layout, convenient for one team but unfamiliar to everyone else.
Or it can follow the conventions of the upstreams it draws from, so that each
part is recognisable and can be contributed back or lifted out.

Separately, the deliverable is not just a kernel. It is the whole product a
device maker ships from source.

Decision
========

Two axes, decided independently:

* **Structure** follows mainline Linux conventions. Kernel code uses kernel
  layout, coding style, device-tree bindings and documentation placement; the
  distribution follows the embedded-Linux from-source conventions; the OMCI
  daemon follows the layout of a standalone protocol daemon.
* **Scope** is what a device maker ships: the cross toolchain (through the
  distribution build), the kernel, the userspace including the OMCI daemon, the
  image and flash tooling, and the hardware documentation.

Consequences
============

* Each part is recognisable to its own upstream community and is liftable
  without re-layout.
* Contributors follow established upstream process (kernel coding style,
  binding review, the Developer Certificate of Origin) rather than a
  project-specific one.
* The concrete one-repository layout that carries this decision is ADR 0004.
