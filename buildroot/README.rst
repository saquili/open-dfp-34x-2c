.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========
buildroot
=========

The pin for the distribution build system (see ADR 0005).

Buildroot itself is not committed here. This directory holds a ``VERSION`` file
that pins a Buildroot release by hash, and a ``patches/`` directory for the
patches we carry against it -- most importantly the one adding the MIPS I
(Lexra) variant, which upstream Buildroot does not provide.

Empty for now. It is populated by **WI-14**.
