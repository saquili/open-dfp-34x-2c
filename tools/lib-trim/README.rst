.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==============
tools/lib-trim
==============

The cut of a root filesystem's shared libraries to what its programs use.

A shared library holds every function it offers, because a program installed
later might call any of them. The root filesystem of the stick is closed: the
programs it carries are the programs it will ever run, since an update replaces
the whole image. Everything else in the libraries is flash spent on nothing.
The C library is the largest of them, and the programs of the image use about a
quarter of what it exports. The tool keeps that quarter, the way a static link
keeps the objects a program refers to, and keeps the sharing.

The technique is the one ``mklibs`` uses for the Debian installer: link the
library again from its archive of position-independent objects, pulling only
what the programs import.

Usage
=====

::

    tools/lib-trim/lib-trim --cross PREFIX --uclibc DIR [-n] [-q] ROOT

``ROOT`` is the target tree. ``PREFIX`` is the prefix of the cross tools
(``gcc``, ``readelf``, ``strip``); ``DIR`` is uClibc-ng's build directory, which
holds the archive of position-independent objects, ``libc/libc_so.a``, and the
other pieces of its link. ``-n`` reports what would be done and changes nothing;
``-q`` prints errors only. The exit status is 0 when the tree passed, 1 when a
check failed (a library or a symbol that a program needs is missing), and 2
when the work could not be done. The build runs it from
``br2-external/board/dfp-34x-2c3/post-build.sh`` on every image.

What it does
============

In this order:

#. **Libraries that nothing loads go.** A program is an ELF file with a program
   interpreter. Its libraries are found as the dynamic loader finds them, by the
   name in ``NEEDED``, in ``lib`` and ``usr/lib``, following symbolic links inside
   the tree, and so on through their own ``NEEDED``. A shared library in ``lib``
   or ``usr/lib`` that no program reaches is removed, with the symbolic links
   that name it. ``libgcc_s.so.1``, which the toolchain installs and nothing of
   the image needs, is the case it was written for. A library that a program
   opens with ``dlopen`` has no ``NEEDED`` to show for it; such a library belongs
   in a directory of its own, which the tool does not touch. A program that
   needs a library the tree lacks fails the run before anything is changed.

#. **The C library is linked again with what is used.** The symbols that the
   C library exports and that any other ELF file of the tree imports, weakly
   or not, are passed to the link as ``-u`` options, and the archive is given
   whole but not with ``--whole-archive``: the linker takes the members that
   define those symbols and the members they need, and no other. Everything
   else is uClibc-ng's own link of ``libuClibc-VERSION.so`` (``link.so`` in its
   ``Makerules``, ``LDFLAGS-libc.so`` in ``libc/Makefile.in``): the same options
   for the options its configuration sets (``-z relro``, ``-z now``, the hash
   style), the same initializer (``-init __uClibc_init``), the same soname, no
   start files, and the same objects after the archive (the interpreter's name,
   the dynamic loader, the non-shared parts, libgcc, the end of the unwind
   table). The result is stripped as the build system strips a library of the
   target. The tool then holds it to the library it replaces: every symbol it
   had to keep has to be there, and the ELF flags, the dynamic tags (with the
   values of those that name something, such as ``SONAME``, ``NEEDED`` and
   ``FLAGS_1``) and the kinds of segments have to be the same. A difference
   stops the run, so that a change in uClibc-ng's link is seen and not shipped.

#. **Every program is checked.** For each program, the objects it loads are the
   program, its libraries and the dynamic loader, and every symbol that one of
   them has the loader resolve, other than a weak one, has to be defined by one
   of them. The loader resolves the symbols that a dynamic relocation names and,
   on MIPS, those of the global part of the GOT (from ``MIPS_GOTSYM`` on); an
   undefined symbol that neither names is never looked up. The ``__gnu_local_gp``
   that ``-export-dynamic`` leaves undefined in a MIPS executable (``ip`` and
   ``tc`` of iproute2) is one. Symbol versions are compared without the version:
   uClibc-ng has none, and the libraries that have them (``libmnl``) define each
   name once.

A second run on a trimmed tree gives the same library byte for byte, and two
builds give the same tree.

Why the instruction-set gate still has the last word
====================================================

The cut is not what keeps the image to the instructions the core runs; the gate
over the target tree is (``tools/isa-gate``). The C library's ``_test_and_set``,
which the core cannot run, is left out because no program imports it, and an
image whose programs started to import it would fail the gate, not slip
through.

Tests
=====

::

    tools/lib-trim/tests/run [-v]

The parsers are tested on what ``readelf`` prints. The end-to-end tests build a
stand-in for uClibc-ng's build directory (a C library of a few functions, an
initializer, a dynamic loader) and a root filesystem with a program, a library
it loads and one that nothing loads, with a MIPS cross compiler
(``gcc-mips-linux-gnu`` on Debian, or another named by ``CROSS_COMPILE``), and run
the tool on it: the library keeps what is imported and what that needs and
nothing else, the initializer and soname stay, the unused library goes with its
link, a second run changes nothing, a dry run changes nothing, and a missing
library, a missing symbol and the build directory of another C library each
fail. Without a cross compiler those tests are skipped. They run in
``make check`` and in the ``lib-trim`` job of ``.github/workflows/gates.yml``.
