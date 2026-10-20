.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========================================================================
ADR 0021: Use the core's ll and sc for atomic operations, without threads
=========================================================================

:Status: Accepted
:Date: 2026-10-20
:Deciders: the maintainer

Summary
=======

The core runs the load-linked and store-conditional pair, ``ll`` and ``sc``,
and ``sync``, as MIPS II defines them; the repository had said that its
``sc`` never stores, on a measurement that could not tell. The toolchain now
makes atomic operations in place as ``ll``/``sc`` sequences, the
instruction-set gate allows the three instructions, and the carried patches
that worked round the missing atomic operations are gone. There are still no
threads: the core has no thread pointer for thread-local storage.

Context
=======

The repository said that the core executes ``ll`` but never completes an
``sc``, so that an atomic sequence retries for ever. That came from a probe
that made one ``sc`` as the first store to a page that had not been written:
the page fault between ``ll`` and ``sc`` ends the reservation, and an ``sc``
that works as the architecture defines it fails there. On that reading the
toolchain had no atomic operations. gcc was configured ``--without-llsc`` and
the atomic builtins were not claimed; a carried patch of uClibc-ng claimed an
entry of the exit-handler table without a compare-and-exchange; libubox and
procd carried options to leave out udebug, whose ring buffer is updated with
atomic operations; json-c and ubus were offered to the MIPS I variant by a
patch of their recipes; and the gate refused ``ll`` and ``sc``, which a
listing as MIPS I shows as ``lwc0`` and ``swc0``.

New measurements on the DFP-34X-2C3, on 2026-10-20, with static user-space
programs and an out-of-tree kernel module, found the following (M; the
programs and every count are in ``linux/Documentation/arch/mips/rtl9602c.rst``,
"Instructions beyond MIPS I"):

* ``ll`` and ``sc`` work, in user and in kernel mode, on cached and uncached
  words. Of 1,000,000 tries of ``ll``, ``addiu``, ``sc`` on a word in the data
  segment, on the stack or on the heap, 2 to 9 fail and the rest store their
  value; a loop that tries again made 1,000,000 increments of 1,000,000. In
  kernel mode with interrupts disabled, every ``sc`` succeeds. ``ll`` is a load
  of the core, not a trap that the kernel emulates; a try takes about 140 ns.
* The reservation ends with the ``sc``, and with an exception between ``ll``
  and ``sc``: a system call, a page fault, and an interrupt with its return. A
  task is therefore never switched out between the two and left to store over
  another task's write, which is what atomicity on one CPU needs. The first
  ``sc`` to a page not yet written fails, and the second succeeds.
* The instruction after an ``ll``, as after an ``lw``, sees the loaded value,
  so the compiler's sequences need no ``nop`` after the ``ll``; the
  ``__sync`` operations as gcc makes them counted 1,000,000 increments to
  exactly 1,000,000.
* ``sync`` executes, in about 36 ns.
* Of the other instructions of MIPS II and later that were tried, ``mul``,
  ``clz``, ``teq``, ``beql`` and ``bnel`` trap, and ``bltzl`` runs wrongly,
  without a signal.
* ``rdhwr $3, $29``, with which the MIPS ABI reads the thread pointer, raises
  no reserved-instruction exception but a fault at address 0, so the kernel
  cannot emulate it. uClibc-ng's NPTL needs the thread pointer for each
  thread's storage; its other thread library, linuxthreads, is not kept up.

Options considered
==================

Stay without atomic operations
    As before: the gate refuses ``ll`` and ``sc``, and the patches that work
    round them stay. It works, but it rests on a fact that was wrong, keeps
    patches against sources the project does not own for no reason, and keeps
    the packages that need the atomic builtins out of the build.
Atomic operations as ll/sc sequences, without threads
    gcc makes the ``__sync`` and ``__atomic`` operations in place, the gate
    allows ``ll``, ``sc`` and ``sync``, and the patches go. Chosen.
Threads as well
    Not possible: NPTL needs the thread pointer, which the core does not have
    and the kernel cannot emulate.
Code for MIPS II
    The compiler would use every instruction of MIPS II, the branch-likely
    forms and the traps among them, which the core traps on or runs wrongly.
    The core runs three instructions of MIPS II, and the code stays MIPS I
    with those three.

Decision
========

* gcc of the MIPS I variant is configured with ``--with-llsc``
  (``buildroot/patches/0001``), beside ``--with-arch=mips1``,
  ``--with-divide=breaks`` and ``--without-synci``. It makes the atomic
  operations on 1, 2 and 4 bytes in place as ``ll``/``sc`` sequences that
  start again when the ``sc`` fails, with ``sync`` where the memory order asks
  for it, and the build system claims them (``BR2_TOOLCHAIN_HAS_SYNC_1``,
  ``_2`` and ``_4``).
* There are no threads: the variant offers neither NPTL nor linuxthreads, and
  the toolchain is configured ``BR2_PTHREADS_NONE``. Without threads the build
  system builds no libatomic, so there is no atomic operation on 8 bytes and no
  ``BR2_TOOLCHAIN_HAS_ATOMIC``.
* uClibc-ng carries no patch of this tree: ``__cxa_finalize`` claims an entry
  of the exit-handler table with its compare-and-exchange. The patch of the
  libraries of netifd (``buildroot/patches/0004``) keeps only the newer libubox
  and ubus and json-c without thread-local storage; json-c and ubus are offered
  to the variant as to any MIPS target, and libubox is built whole, with its
  own udebug code.
* procd is built without udebug (its patch 0001), as the image carries neither
  OpenWrt's udebug library nor the daemon that collects the buffers (ADR 0010);
  the fork of netifd leaves udebug out too (``netifd/README.rst``).
* ``tools/isa-gate`` lists code as MIPS II and allows the MIPS I integer set
  and ``ll``, ``sc`` and ``sync``; every other instruction of MIPS II and later
  stays a finding, by name.
* What the kernel does with ``ll`` and ``sc`` is the kernel's, and
  ``linux/Documentation/arch/mips/rtl9602c.rst`` describes it.

Consequences
============

* Fewer carried patches: Buildroot's are three (``buildroot/README.rst``), and
  neither uClibc-ng nor libubox has a patch of this tree. procd keeps its
  patch of udebug, for the reason above.
* A program built with the toolchain may use atomic operations on 1, 2 and 4
  bytes, and the packages that need the atomic builtins are offered. One on 8
  bytes does not link. A sequence written by hand must start again when its
  ``sc`` fails, as on the first store to a page not yet written.
* The gate no longer finds ``ll``, ``sc`` and ``sync``. Over the toolchain's
  own objects it reports 27 findings, all in libgcc's speculation barrier,
  where it reported 34 in it and in uClibc-ng's ``_test_and_set``
  (``br2-external/tests/toolchain/known-findings.txt``). ``t-atomic``, among
  the toolchain's test programs, checks the atomic operations, and on the
  board puts them through interrupts and signals.
* The root file system grows by 5,380 bytes, from 1,430,363 to 1,435,743 bytes
  of ``r0``, nearly all of it the udebug code that libubox now builds; the
  programs import 372 of the C library's symbols instead of 370, and the cut
  library is 108 bytes larger. The kernel
  image, built with the new compiler, grows by 111 bytes, to 1,828,084 (M,
  2026-10-20, both images built with the toolchain from source, before and
  after this decision).
* The toolchain's inputs changed, so the images take this toolchain through
  a release and a commit of the pin (ADR 0020). While no release is pinned,
  as on 2026-10-20, they build the toolchain from source, with these inputs.
