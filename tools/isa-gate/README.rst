.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==============
tools/isa-gate
==============

The instruction-set gate for target binaries.

This core is MIPS I only. It runs no MIPS32 instruction, and although it
executes the load-linked instruction it never completes a store-conditional, so
any such pairing must be absent from shipped code. A program that uses one does
not crash: it spins in the first retry loop of its first atomic operation. The
gate scans the target binaries and fails the build on an instruction that this
core does not run.

Usage
=====

::

    tools/isa-gate/isa-gate [--objdump PROGRAM] [--allow MNEMONIC]...
                            [--archives] [--no-flags] [--limit N] [-q] PATH...

``PATH`` is an ELF file or a directory. A directory is searched for ELF files,
without following symbolic links, in a stable order; a static archive is
scanned when it is named, or found under a directory with ``--archives``. The
listing program is ``$OBJDUMP``, else ``${CROSS_COMPILE}objdump``, else
``mips-linux-gnu-objdump``; ``--objdump`` names another. The exit status is 0
when nothing was found, 1 when something was, and 2 when the gate could not do
its work: no objdump, an unreadable path, a named file that is not an
object, or nothing to scan. A gate that scanned nothing has proved nothing, so
that is an error and not a pass.

A finding is printed as ``file: symbol+offset: instruction``, or
``archive: member: symbol+offset: instruction``; ``--limit`` caps what is
printed per file, and the count in the last line is exact.

What it checks
==============

**Instructions.** Every code section is listed as MIPS I (``objdump -d -m
mips:3000``). The set that is allowed is the MIPS I integer instruction set, by
the names objdump prints for it, aliases included (``move``, ``li``,
``nop``, ``b``, ``bal``, ``beqz``, ``bnez``, ``negu``, ``not``). Anything else
is a finding. That is an allowlist on purpose: a listing as MIPS I leaves every
instruction of a later architecture as ``.word``, or as a coprocessor access,
and a denylist would have to name each of them. The two that matter most show up
in disguise: ``ll`` and ``sc`` share their opcodes with the coprocessor-0 loads
``lwc0`` and ``swc0`` of MIPS I, and appear under those names. The unaligned
loads and stores (``lwl``, ``lwr``, ``swl``, ``swr``) run correctly at all four
byte offsets and are allowed.

**ELF headers.** The header of every object must say big-endian, 32-bit, MIPS,
and the architecture ``mips1``. The linker gives its output the highest
architecture of its inputs, so a final binary that says ``mips32r2`` has had an
object built for another ISA linked into it. Debian's cross compiler runtime,
for one, is built for ``mips32r2`` and has no MIPS I variant: it is the usual
way a build that passes every flag it was given still ships ``clz`` and ``mul``,
from the division and bit-counting helpers that large-file support pulls in.
``--no-flags`` turns this check off. An ELF file of another machine or byte
order in a target tree is a finding too: it is a host program that got in.

Where to run it
===============

On what is shipped: the target root filesystem as the build assembles it, and
the archives of our own code before they are linked. Not on a toolchain's own
static libraries or on a build tree: they hold members that no binary links, and
the C library's atomics, for one, are safe by configuration (no threads) and not
by construction. They are judged where they end up, in the final binaries.

The kernel is not covered yet. Its binary holds the compare-and-swap paths and
the code for coprocessor registers of other cores behind tests that run at boot
(``cpu_has_llsc`` and its relatives), so a scan of ``vmlinux`` finds them
although none runs on this core. A feature-override header for the platform that
fixes those tests at build time would let the compiler drop them, and the kernel
could then be held to the same rule; until it exists the gate is run on
userspace only.

Tests
=====

::

    tools/isa-gate/tests/run [-v]

The tests build small ELF files and archives in memory with chosen instruction
words, so that every forbidden instruction is shown to fail the gate and the
gate is shown able to fail. The ones that list instructions need a MIPS
``objdump`` (``binutils-mips-linux-gnu`` on Debian, or ``OBJDUMP=``) and are
skipped without one. The distribution build runs the gate over the target tree
(``make image-check``).
