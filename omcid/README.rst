.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=====
omcid
=====

The OMCI management daemon, as a standalone project.

This is the ONU-side management stack for ITU-T G.988: the 48-byte message
frame and its integrity check, the engine, the management-information store,
the per-management-entity handlers, and the control socket. It is laid out like
any standalone protocol daemon (a plain ``Makefile``, ``src/``, ``tests/`` and
man pages) and it talks to the kernel only through stable interfaces -- the
OMCI control character device, and the standard networking control paths.

Empty for now. It is populated by:

* **WI-16** -- import the engine, frame, store and system layers; relicense to
  ``GPL-2.0-only``; adopt kernel coding style.
* **WI-17** -- generate the management-entity model from ITU-T G.988.
* **WI-18** -- drive the kernel interfaces (the OMCI control device and the
  switch's VLAN-translation offload) instead of any userspace shim.
