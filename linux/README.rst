.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=====
linux
=====

The Linux kernel, in mainline layout, with our work on top.

This directory is a byte-for-byte kernel tree imported with ``git subtree``
(pinned to a 6.18.y stable release), carrying our board, SoC, switch, PON and
documentation commits as ordinary kernel commits. Commits here touch only this
directory, so a subtree split yields a clean kernel history (see ADR 0004).

Empty for now. It is populated by:

* **WI-11** -- import the pinned kernel release as a subtree, and add the
  kernel continuous-integration jobs.
* **WI-12** -- author the logical kernel series (MIPS core, bindings, platform,
  drivers, PON), plus the defconfig and the MAINTAINERS entries.
* **WI-13** -- kernel documentation and device-tree bindings.

The kernel keeps its own ``COPYING`` and ``LICENSES/`` once imported.
