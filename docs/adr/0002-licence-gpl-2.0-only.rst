.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=============================================
ADR 0002: Put our own work under GPL-2.0-only
=============================================

:Status: Accepted
:Date: 2026-09-21
:Deciders: the maintainer

Summary
=======

Everything written for this project is licensed ``GPL-2.0-only``.
Third-party code keeps its own licence and is fetched at build time rather
than committed, with its sources handed on with each release.

Context
=======

The kernel work derives from the Linux kernel and is therefore GPLv2. The
user space, above all the OMCI daemon, was first written under a permissive
licence. Two licence regimes complicate compliance, auditing and the
``reuse`` tooling, and invite accidental mixing.

Decision
========

* Everything authored in this repository is ``GPL-2.0-only``: the kernel
  work, the OMCI daemon (relicensed by its author on import), the tools, the
  documentation and the distribution tree. Every authored file carries its
  SPDX tag inline.
* Third-party packages are not committed. They are fetched by hash at build
  time, keep their own licences, and are shipped as a bundle of sources and
  licences with each release.
* A tool that would have to link code under a licence incompatible with
  GPLv2 stays outside the repository and runs only in continuous
  integration.
* Inside ``linux/`` the three licence forms that mainline requires remain,
  all based on GPL-2.0: device-tree bindings
  (``GPL-2.0-only OR BSD-2-Clause``), UAPI headers
  (``GPL-2.0 WITH Linux-syscall-note``) and netlink specifications
  (``(GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause``). The kernel's own
  ``COPYING`` and ``LICENSES/`` are kept as imported.

Consequences
============

* There is one licence to reason about for our own work; ``reuse lint`` runs
  in continuous integration, and every file is tagged.
* A non-GPL project cannot reuse our user space under permissive terms; this
  is accepted.
* Each release owes a complete bundle of the third-party packages' sources
  and licences, which the build system produces (``make legal-info``).
