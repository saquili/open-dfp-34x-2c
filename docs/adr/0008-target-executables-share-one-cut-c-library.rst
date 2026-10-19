.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===========================================================
ADR 0008: Share one C library, cut to what the programs use
===========================================================

:Status: Accepted
:Date: 2026-10-08
:Deciders: the maintainer

Summary
=======

The programs on the stick share one C library, as on any Linux system, but
the library in the image holds only the functions those programs use: the
image is closed, and nothing else will ever call the rest. This keeps code
the core cannot run out of the image and each library function in the flash
once.

Context
=======

The core runs MIPS I only. It executes the load-linked instruction but
never completes a store-conditional, so an atomic sequence retries for ever,
and it runs no MIPS32 instruction (``docs/handbook/toolchain.rst``,
``tools/isa-gate/README.rst``). The distribution builds a MIPS I toolchain of
its own (ADR 0005), and the instruction-set gate refuses every other
instruction in a shipped binary.

Run over the toolchain's own libraries, the gate reports findings in three
functions that gcc 14.4.0 and uClibc-ng 1.0.59 build for every MIPS target,
and that no option of either leaves out:

* ``__speculation_barrier`` of libgcc, in ``libgcc.a`` and
  ``libgcc_s.so.1``: a ``sync`` and a ``jr.hb``;
* ``__cxa_finalize`` of uClibc-ng, in ``libc.a`` and ``libuClibc.so``: a
  compare-and-exchange built from a load-linked and store-conditional pair,
  and a ``sync``;
* ``_test_and_set`` of uClibc-ng, in the same two files: a load-linked and
  store-conditional loop.

``br2-external/README.rst`` lists the files and the counts, and
``br2-external/tests/toolchain/known-findings.txt`` holds them as a list that
the toolchain job compares with the gate's output.

A program linked statically takes only the objects it refers to, so none of
the three is in a static program. A shared library holds them whole, so a
root file system that carries the C library and ``libgcc_s`` as the build
system makes them fails the gate.

The flash is the other constraint. The root file system has to fit its
partition, ``r0`` (``docs/handbook/boot-contract.rst``), and with every
program linked statically the image used 2,004,150 bytes of it: each program
carried its own copy of the C library functions it uses. The network layer,
netifd and ubusd, adds programs, and with them more copies.

Options considered
==================

Three layouts were built and measured on one set of programs (the image's
programs without iproute2, with netifd and ubusd), as the size of ``r0``:

Separate static programs
    About 1,653,000 bytes. No shared object, so the gate passes with no list
    of exceptions, but the same functions are in the flash once per program.
One static program for all, called by its name, as BusyBox is
    1,113,930 bytes: the smallest, but an unusual shape for a system.
Programs linked against the shared C library
    1,427,354 bytes as the build system makes them. That figure holds three
    costs unrelated to sharing: the whole C library, with every function it
    offers (712,556 bytes, of which the programs import about a quarter);
    ``libgcc_s``, which no program needs; and position-independent code,
    some 15% larger, which the build system's full RELRO turns on. BusyBox
    was also still static, with its own copy. Without those, the shared
    layout is 1,143,122 bytes, 29,192 more than the single program. Chosen,
    with the library cut.

For the three functions, two other ways were weighed: patching libgcc and
uClibc-ng to leave them out for MIPS I, which works at the price of more
carried patches against sources the project does not own; and shipping the
libraries with a list of functions the gate ignores, which, growing with the
image, is the opposite of a gate. The cut leaves ``_test_and_set`` out, as
no program imports it, and ships no ``libgcc_s``; ``__cxa_finalize``
remains, because the start files of every shared object refer to it, and
takes one small patch.

The cut is the technique of ``mklibs``, which the Debian installer uses: the
shared library is linked again from its archive of position-independent
objects, with only what the programs import. It is sound for a closed image,
whose update replaces every program and the library together.

Decision
========

* The programs of the image are linked against the shared C library
  (``BR2_SHARED_LIBS``) and are not position-independent (``BR2_PIC_PIE``
  off, so RELRO is partial).
* The post-build script runs ``tools/lib-trim`` on the target tree on every
  build. It removes the libraries no program loads, links the C library
  again with only the functions that the programs and the other libraries
  import, holds the new library to the old one's headers, and fails the
  build when a program would miss a library or a symbol.
* A carried patch of uClibc-ng claims an exit handler with a plain test and
  store when there are no threads (``buildroot/README.rst``).
* The instruction-set gate runs over the whole target tree of every build
  and must report nothing. The toolchain's own libraries keep their
  findings, and the toolchain job holds them to the list of known findings,
  so that any change in them is seen.

Consequences
============

* The gate judges everything that is shipped, with no list of exceptions.
  It is the gate, not the cut, that finally keeps ``_test_and_set`` out of
  the image: an image that started to import it would fail.
* The flash holds each C library function once. Adding a program grows the
  image by the program's own code and by the C library functions it brings
  that no other program used. The image of the day, iproute2 included, went
  from 2,004,150 bytes of ``r0`` to 1,516,938.
* The image's C library is the image's own. A program built elsewhere and
  copied onto the stick may need a function the library does not hold; it
  then fails to start, and the loader names the missing symbol. Programs for
  tests on the board are built static against the developer's toolchain
  (``make sdk``), which keeps both kinds of library, or go into the RAM
  image.
* Shared libraries and ``dlopen`` are available. A library that a program
  opens by name, with no ``NEEDED`` entry, is kept only outside ``lib`` and
  ``usr/lib``, which the cut does not touch.
* The programs are not position-independent: executables are not loaded at
  random addresses, and RELRO is partial, so not all that the loader fills
  in is made read-only afterwards.
* The build carries a patch against uClibc-ng, to be checked at every update
  of it, beside the one that adds the MIPS I variant to the build system.
* A security fix in the C library means rebuilding the image; the build is
  one command.
