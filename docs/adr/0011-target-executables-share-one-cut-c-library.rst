.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

======================================================================
ADR 0011: Target executables share one C library, cut to what they use
======================================================================

:Status: Accepted
:Date: 2026-10-08
:Decided-by: the owner, 2026-10-08
:Supersedes: ADR 0010

.. note::

   In plain words: the programs on the stick share one C library again, as on
   any Linux system, but the library in the image holds only the functions those
   programs use, because the image is closed and nothing else will ever call
   the rest. The owner chose this over one big program that holds everything,
   which saves a little more flash but is a strange shape for a system.

Context
=======

ADR 0010 linked every program statically because the toolchain's shared
libraries hold code the core cannot run, and a static program takes only the
objects it uses. It said what would supersede it: the offending functions
leaving the libraries, which would make dynamic linking possible.

The flash is now the constraint. The root filesystem has to fit its partition,
``r0`` (``docs/handbook/boot-contract.rst``), and the image of the day used
2004150 bytes of it. Every
static program carries its own copy of the C library functions it uses; the
same functions are in the flash once per program. The network layer that is
to come, netifd and ubusd, adds programs, and with them more copies.

Three layouts were built and measured on one set of programs (the image's
programs, without iproute2, with netifd and ubusd), as the size of ``r0``:

* separate static programs, as ADR 0010 has it: about 1653000 bytes;
* one static program for all of them, called by its name (as BusyBox is):
  1113930 bytes;
* the programs linked against the shared C library: 1427354 bytes as the build
  system makes them, which looked like the worse choice. That measurement held
  three costs that have nothing to do with sharing: the C library whole, with
  every function it offers (712556 bytes, of which the programs import about a
  quarter); ``libgcc_s``, which no program needs; and position-independent
  code, some 15% larger, which the build system's full RELRO turns on. BusyBox
  was also still static, with its own copy. Without those, the shared layout
  is 1143122 bytes, 29192 more than the single program.

The cut is the technique of ``mklibs``, which the Debian installer uses: link
the shared library again from its archive of position-independent objects,
pulling only what the programs import. It holds for a closed image, whose
update replaces every program and the library together.

Of the three functions that ADR 0010 listed, the cut leaves out
``_test_and_set`` (no program imports it) and does not ship ``libgcc_s`` with its
``__speculation_barrier``. ``__cxa_finalize`` stays, because the start files of
every shared object refer to it; its compare-and-exchange needs a patch.

Decision
========

The programs of the image are linked against the shared C library
(``BR2_SHARED_LIBS``), not position-independent (``BR2_PIC_PIE`` off, so RELRO
is partial). The post-build script runs ``tools/lib-trim`` on the target tree on
every build: it removes the libraries that no program loads, links the C library
again with only the functions that the programs and the other libraries import,
holds the new library to the old one's headers, and fails the build when a
program would miss a library or a symbol. A carried patch of uClibc-ng claims an
exit handler with a plain test and store when there are no threads
(``buildroot/README.rst``). The instruction-set gate keeps running over the whole
target tree and has to report nothing.

Consequences
============

* The gate still judges everything that is shipped, with no list of
  exceptions. The cut is not what keeps ``_test_and_set`` out of the image in
  the end; the gate is, and an image that started to import it would fail.
* The flash holds each C library function once. The image grows by the size of
  a program's own code when a program is added, and by the C library functions
  it brings that no other program used. The image of the day, iproute2
  included, went from 2004150 bytes of ``r0`` to 1516938.
* The C library of the image is the image's own. A program built elsewhere and
  copied onto the stick can need a function the image's library does not hold,
  and then it does not start: the loader names the missing symbol. Programs for
  tests on the board are built static, against the developer's toolchain
  (``make sdk``), which keeps both kinds of library, or go into the RAM image.
* Shared libraries and ``dlopen`` are available again. A library that a program
  opens by name, with no ``NEEDED`` entry, is kept only outside ``lib`` and
  ``usr/lib``, which the cut does not touch.
* The programs are not position-independent, as they were not when static; the
  executable is not placed at a random address. RELRO is partial: not all of
  what the loader fills in is made read-only after loading.
* The build carries a second patch against sources the project does not own, to
  be checked at every update of uClibc-ng, and the toolchain's list of known
  findings loses ``__cxa_finalize``.
* A security fix in the C library is a rebuild of the image, as before.
