.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=============
tools/isa-gate
=============

The instruction-set gate for target binaries.

This core is MIPS I only. It runs no MIPS32 instruction, and although it
executes the load-linked instruction it never completes a store-conditional, so
any such pairing must be absent from shipped code. The gate scans every target
binary -- kernel and userspace, including the contents of static archives --
and fails the build on a forbidden instruction.

Empty for now. It is populated by **WI-14**, wired into the distribution build
so that no target binary can ship without passing it.
