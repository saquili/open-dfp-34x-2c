.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==================================================
ADR 0002: Licence -- GPL-2.0-only for our own work
==================================================

:Status: Accepted
:Date: 2026-09-21

Context
=======

The kernel work is a derivative of the Linux kernel and is therefore GPLv2.
The userspace -- most importantly the OMCI daemon -- was first written under a
permissive licence. Carrying two licence regimes complicates compliance,
auditing and the ``reuse`` tooling, and it invites accidental mixing.

Decision
========

Everything authored in this repository is ``GPL-2.0-only``: the kernel work,
the OMCI daemon (relicensed to ``GPL-2.0-only`` by its author on import), the
tools, the documentation and the distribution tree. Every authored file carries
the SPDX tag inline.

Third-party packages are not committed here: they are fetched by hash at build
time, keep their own licences, and are shipped as a source-and-licence bundle
with each release. A tool that would have to link code under a licence that is
not GPLv2-compatible stays outside the repository and is run only in
continuous integration.

Inside ``linux/`` the three licence forms mainline requires remain, all
GPL-2.0-based: dual-licensed device-tree bindings
(``GPL-2.0-only OR BSD-2-Clause``), UAPI headers
(``GPL-2.0 WITH Linux-syscall-note``), and netlink specifications
(``(GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause``). The kernel's own
``COPYING`` and ``LICENSES/`` are kept as they are once the kernel is imported.

Consequences
============

* One licence to reason about for our own work; ``reuse lint`` runs in
  continuous integration and every file is tagged.
* Permissive reuse of our userspace by a non-GPL project is foreclosed; this is
  accepted.
* Each release owes a complete source-and-licence bundle for the third-party
  packages, produced by the build system.
