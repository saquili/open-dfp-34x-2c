.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

============
br2-external
============

The distribution: a Buildroot external tree (see ADR 0005).

This is where the product is assembled from source -- the board definition, the
target configuration, the C-library and BusyBox configurations, the package
recipes (including the OMCI daemon), and the post-image script that lays out the
two partition images the bootloader loads. The kernel comes from ``linux/`` and
the pinned build system from ``buildroot/``.

Empty for now. It is populated by **WI-14**, which adds ``external.desc``,
``Config.in`` and ``external.mk``, the board directory and defconfig, the
carried configurations, and the OMCI daemon package.
