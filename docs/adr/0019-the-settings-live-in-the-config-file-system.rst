.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=======================================================================
ADR 0019: The settings are a file in the config partition's file system
=======================================================================

:Status: Accepted
:Date: 2026-10-11
:Decided-by: the owner, 2026-10-10 and 2026-10-11
:Amends: ADR 0014

.. note::

   In plain words: the stick keeps its settings as a file beside the shipped
   firmware's own files, in the file system that firmware keeps in the
   ``config`` partition, instead of taking the whole partition. The shipped
   firmware still finds everything it needs there, so going back to it is a
   switch of the bank.

Context
=======

ADR 0014 gave this as its step 9, added on 2026-10-10; this record takes it
over with what was decided about it on 2026-10-11. The words of ADR 0014 as
they stood are in its history.

The ``config`` partition is the user's: the shipped firmware's data there is a
matter of compatibility, and the user's settings have a limit set in advance,
which never takes room from a kernel or a root file system. Until this
decision the store split the whole partition into two slots of its own, and
its first write overwrote the shipped firmware's JFFS2, which that firmware
erases and formats when it cannot mount it: a stick that goes back to it
loses its registration identity and its laser's calibration.

Decision
========

* The kernel mounts the partition's JFFS2, with zlib alone and without
  extended attributes, summaries or the write buffer of NAND, as the shipped
  firmware writes it.
* The settings are one file of their own in it, under a limit of 64 KiB.
* The identity is read from the shipped firmware's files, with what this
  system changed kept in a file of its own, and is written to them only once
  the way that firmware checks them is known.
* A partition that still holds the store of before is taken into the file at
  the first boot, before the mount, since JFFS2 erases what it does not know.
  ``docs/handbook/updating.rst`` says how that start is attended.
* A return to the shipped firmware is a switch of the bank. A stick as
  shipped takes this system through the shipped firmware's web page, with a
  package that the repository builds and tests.

Consequences
============

* The kernel takes JFFS2, which adds 36,711 bytes to the compressed kernel and
  leaves bank 0's kernel partition 201,710 bytes (2026-10-11).
* The settings' file has 64 KiB at most, about 5 KB of which are used. With a
  replacement in flight and the shipped firmware's files it fills less than
  half of the partition.
* A test on a stick of the bench boots the shipped firmware in the other bank
  and checks that it reads its own files and leaves this system's alone.
* A package other than the one a stick shipped with may write the shipped
  firmware's own files anew. On the bench one wrote the PLOAM password's
  format differently, so going back to another package than the stick's own
  can change how the stick introduces itself to an OLT.
  ``docs/handbook/updating.rst`` has the packages that were tried.
