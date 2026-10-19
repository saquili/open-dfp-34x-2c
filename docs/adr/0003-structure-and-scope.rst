.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

================================================================
ADR 0003: Follow mainline structure, with a device maker's scope
================================================================

:Status: Accepted
:Date: 2026-09-21
:Deciders: the maintainer

Summary
=======

Each part of the project is laid out the way its upstream community lays
it out, and the project delivers everything a device maker ships: the
toolchain, the kernel, the user space, the images and the documentation.

Context
=======

A device port can take one of two shapes. It can be a single all-in-one
tree with an ad hoc layout, convenient for one team but unfamiliar to
everyone else. Or it can follow the conventions of the upstreams it draws
from, so that each part is recognisable and can be contributed back or
lifted out.

Separately, the deliverable is not only a kernel. It is the whole product
that a device maker builds and ships from source.

Decision
========

Two axes, decided independently:

* **Structure** follows mainline conventions. Kernel code follows the
  kernel's layout, coding style, device-tree bindings and documentation
  placement; the distribution follows the conventions of from-source
  embedded Linux; the OMCI daemon follows the layout of a standalone
  protocol daemon.
* **Scope** is what a device maker ships: the cross toolchain (through the
  distribution build), the kernel, the user space including the OMCI
  daemon, the image and flash tooling, and the hardware documentation.

Consequences
============

* Each part is recognisable to its own upstream community and can be lifted
  out without a new layout.
* Contributors follow established upstream processes (the kernel's coding
  style, binding review, the Developer Certificate of Origin) rather than a
  process of the project's own.
* ADR 0004 gives the concrete layout of one repository that carries this
  decision.
