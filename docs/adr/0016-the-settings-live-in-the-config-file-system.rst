.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===========================================================================
ADR 0016: Keep the settings as a file in the config partition's file system
===========================================================================

:Status: Accepted
:Date: 2026-10-11
:Deciders: the maintainer

Summary
=======

The stick keeps its settings as one file beside the shipped firmware's own
files, in the file system that the shipped firmware keeps in the ``config``
partition, instead of taking the whole partition. The shipped firmware
still finds everything it needs there, so going back to it is a switch of
the bank.

Context
=======

The ``config`` partition is the user's: the shipped firmware's data there is
a matter of compatibility, and the user's settings have a fixed limit, which
never takes room from a kernel or a root file system. The shipped firmware
keeps its files there in a JFFS2, and erases and formats the partition when
it cannot mount it. A store that took the whole partition for itself would
make a stick that goes back to the shipped firmware lose its registration
identity and its laser's calibration.

Decision
========

* The kernel mounts the partition's JFFS2 with zlib only, and without
  extended attributes, summaries or the NAND write buffer, as the shipped
  firmware writes it.
* The settings are one file of their own in it, limited to 64 KiB.
* The identity is read from the shipped firmware's files, with what this
  system changed kept in a file of its own, and is written to those files
  only once it is known how that firmware checks them.
* A partition that holds a store of the older two-slot form, from a build
  before 2026-10-11, is converted into the file at the first boot, before
  the mount, since JFFS2 erases what it does not recognise.
  ``docs/handbook/updating.rst`` says how that start is attended.
* Returning to the shipped firmware is a switch of the bank. A stick as
  shipped takes this system through the shipped firmware's web page, with a
  package that the repository builds and tests.

Consequences
============

* The kernel includes JFFS2, which adds 36,711 bytes to the compressed
  kernel and leaves 201,710 bytes free in bank 0's kernel partition
  (2026-10-11).
* The settings file is at most 64 KiB, of which about 5 KB are used. With a
  replacement in flight and the shipped firmware's files, it fills less than
  half of the partition.
* A test on a bench stick boots the shipped firmware in the other bank and
  checks that it reads its own files and leaves this system's alone.
* A package other than the one a stick shipped with may write the shipped
  firmware's own files anew. On the bench one wrote the PLOAM password in
  another format, so going back to a package other than the stick's own can
  change how the stick introduces itself to an OLT.
  ``docs/handbook/updating.rst`` lists the packages that were tried.
