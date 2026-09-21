# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Equivalence tools for kernel trees.

The modules of this package turn a source file into a canonical token stream
(``ctok`` for C, assembly and device-tree source; ``kconfig``, ``make`` and
``yaml`` for the build and schema languages), classify the difference between
two streams (``compare``), walk two trees (``trees``) and record the hashes of
a kernel build (``manifest``).  ``cli`` holds the command-line front ends.

Only the Python 3 standard library is used.
"""
