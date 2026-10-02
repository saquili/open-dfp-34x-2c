.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

======================================================
ADR 0010: Target executables are linked statically
======================================================

:Status: Accepted
:Date: 2026-10-02
:Decided-by: the owner, 2026-10-02

.. note::

   In plain words: every program on the stick is one self-contained file that
   carries the parts of the C library it uses, instead of sharing a library that
   lives in the image. Each program is a little bigger, and the instruction-set
   gate can check everything that is shipped, with no list of exceptions. The
   owner accepted this on 2026-10-02 and left the choice to the recommendation,
   which is this decision.

Context
=======

The core runs MIPS I only. It executes the load-linked instruction but never
completes a store-conditional, so an atomic sequence retries for ever, and it
runs no MIPS32 instruction (``docs/handbook/toolchain.rst``,
``tools/isa-gate/README.rst``). The distribution builds a MIPS I toolchain of its
own (ADR 0005), and the instruction-set gate refuses every other instruction in
a binary that is shipped.

Run over the toolchain's own libraries, the gate reports 40 findings in 22
files. They are in three functions that gcc 14.4.0 and uClibc-ng 1.0.59 build for
every MIPS target, and no option of either leaves them out:

* ``__speculation_barrier`` of libgcc, in ``libgcc.a`` and ``libgcc_s.so.1``: a
  ``sync`` and a ``jr.hb``;
* ``__cxa_finalize`` of uClibc-ng, in ``libc.a`` and ``libuClibc.so``: a
  compare-and-exchange made of a load-linked and store-conditional pair, and a
  ``sync``;
* ``_test_and_set`` of uClibc-ng, in the same two files: a load-linked and
  store-conditional loop.

``br2-external/README.rst`` lists the files and the counts, and
``br2-external/tests/toolchain/known-findings.txt`` holds them as a list that the
toolchain job compares with the gate's output.

A program that links the C library statically takes only the objects it refers
to, so none of the three is in a static program: the test programs of
``br2-external/tests/toolchain``, static and dynamic, pass the gate. A dynamic
program is different. The shared libraries it needs, the C library and
``libgcc_s``, hold the three functions whole, so a root filesystem that carries
them fails the gate on the target tree.

Two ways to ship dynamic programs were weighed, and one to ship none:

* **Patch the libraries.** Leave the three functions out for MIPS I: one line in
  libgcc and a few lines in two files of uClibc-ng, carried beside the patch that
  adds the MIPS I variant to the build system. It works (the gate then reports
  nothing in the toolchain's libraries), at the price of a second and third
  carried patch against sources that the project does not own, to be checked
  at every update of either.
* **Ship the libraries and exempt them.** The gate would need a list of
  functions it ignores in the target tree. A list that grows with the image is
  the opposite of a gate.
* **Ship no shared object.** Link every program statically.

Decision
========

Every executable in the root filesystem is linked statically
(``BR2_STATIC_LIBS``), and the image holds no shared object: no C library, no
dynamic loader and no ``libgcc_s``. The instruction-set gate runs over the whole
target tree of every build and has to report nothing. The toolchain's own
libraries keep their three functions; the toolchain job holds them to the list
of known findings, so that a change in them is seen.

Consequences
============

* The gate is run where it matters, on what is shipped, as
  ``tools/isa-gate/README.rst`` asks, and needs no exception list.
* Every program carries the C library objects it uses. For the image of today
  (BusyBox and dropbear) the root filesystem is a fraction of its partition
  (``docs/handbook/building.rst`` gives the sizes), so the cost is paid in bytes
  that are there. It grows with the number of programs, because nothing is
  shared between them.
* Shared libraries, ``dlopen`` and plug-in loading are not available. A package
  that needs a shared library cannot join the image under this decision. The
  packages that need atomic operations or threads are not offered by the build
  system already (the carried patch, ``buildroot/README.rst``).
* A security fix in the C library is a rebuild of every program. The build is
  one command, so this costs nothing that the build does not do anyway.
* The developer's toolchain, ``make sdk``, keeps its shared libraries. A dynamic
  program built with it passes the gate, and the libraries it needs do not, so it
  does not go to the board (``docs/handbook/toolchain.rst``).
* If the three functions leave the toolchain's libraries, by a carried patch or
  by a change upstream, dynamic linking becomes possible and this record would be
  superseded. The list of known findings would then be emptied, not adjusted.
