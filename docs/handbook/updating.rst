.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===================
Updating the system
===================

The stick's flash has two banks, each a kernel and a root filesystem, and both
hold this system. An update goes into the bank that does not run. The boot
loader is then told to start that bank once, and the new system keeps it when
it comes up; when it does not, the stick restarts into the bank it ran before.
An update needs neither the boot loader's prompt nor a console, and a power loss
at any point leaves a stick that starts.

The bench's flashing through the boot loader (``flashing.rst``) remains for the
first installation and for a recovery.

The parts
=========

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - Part
     - What it does
   * - ``update.bin``
     - The update file that ``make image`` leaves beside ``k0.uImage`` and
       ``r0.squashfs`` (``building.rst``): a header with the board, the version
       (the image's ``BUILD_ID``), the build's time and the two lengths, the
       kernel image, the root filesystem image, and the SHA-256 of the three.
       ``onu-utils/src/update.h`` gives the layout. The file is not signed: its
       checksum guards against damage only.
   * - ``onu-update``
     - The stick's program for the banks (``onu-utils``): it checks an update
       file, writes it into the bank that does not run and reads it back, reads
       a bank out as an update file, arms a start of a bank once, commits the
       running bank, and prints or changes the boot loader's environment.
   * - ``onu-trial``
     - Run last by the boot script: after a start of a bank on trial, it
       commits the bank once the system is up, and restarts into the committed
       bank when it is not.
   * - The panel
     - The System page lists system A (bank 0) and system B (bank 1), uploads an
       update file, offers to start the new system, downloads the running one,
       and starts the other one. pond streams the upload into ``onu-update``.

Which bank runs
===============

The boot loader hands the kernel ``root=31:5`` for bank 0 and ``root=31:7`` for
bank 1 (``boot-contract.rst``), so the bank that runs is the one whose root
filesystem the kernel mounted. After a start from RAM the root filesystem is the
image's own and no bank runs, whatever ``root=`` says: ``onu-update`` then
writes only a bank that is named, and ``onu-trial`` does nothing.

``onu-update status`` prints the state as ``KEY=VALUE`` lines:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Key
     - Meaning
   * - ``RUNNING``
     - The bank that runs, or -1 after a start from RAM.
   * - ``COMMITTED``
     - The bank the boot loader starts (``sw_commit``).
   * - ``NEXT``
     - The bank the next start takes: the one armed for a trial, else the
       committed one.
   * - ``TRIAL``
     - Present while the running bank runs on trial, not committed yet.
   * - ``FAILED``
     - Present when the last trial of that bank did not come up: the loader
       took the trial and another bank runs.
   * - ``BANK<N>_STATE``
     - ``valid``, ``incomplete`` (a write that did not finish) or ``empty`` (no
       kernel image or no root filesystem image where they belong).
   * - ``BANK<N>_VERSION``
     - The running bank's ``BUILD_ID``; for the other bank the version that
       ``onu-update`` recorded when it wrote it.
   * - ``BANK<N>_TIME``, ``_KERNEL_SIZE``, ``_ROOTFS_SIZE``
     - The time in the bank's kernel image, and the sizes of its partitions.

Updating
========

From the panel: System, "Upload new software", and the file ``update.bin``. The
page shows how much has gone; the stick then checks and reads back the bank,
some 45 seconds in all, and says which version it wrote where. "Start the new
software" restarts the stick into it, once.

From a shell on the stick, with the file streamed in over ssh, or from
``/tmp``::

    ssh root@<stick> 'onu-update write -' < update.bin
    ssh root@<stick> 'onu-update try && reboot'

``onu-update write`` never touches the running bank. Before it writes anything
it checks the file's header: the board, and that the images fit the bank. A
file for another board, or one too big, leaves the bank as it was. It then marks
the bank ``sw_valid<N>=0``, writes the kernel and the root filesystem as they
arrive, erases what follows them in their partitions, checks the kernel image's
own CRC and the file's checksum, reads the two partitions back against the
checksum, and only then marks the bank ``sw_valid<N>=1`` with
``sw_version<N>``. A file whose checksum fails, or a write that does not finish,
leaves the bank marked invalid and its kernel's first sector erased.

``onu-update try`` arms ``sw_tryactive`` for the bank and refuses a bank that
holds no whole system. ``onu-update read`` writes the running bank as an update
file, which is the file that was written into it, byte for byte; the panel's
"Download the current software" is the same.

What happens at the start of a tried bank
=========================================

The boot loader sets ``sw_tryactive`` back to 2 and ``sw_active`` to the bank,
saves its environment, arms the watchdog and starts the bank. From then on,
every reset that is not a commit leads to the committed bank.

``onu-trial`` waits for omcid to run, for the status daemon to answer on the
host link, and for the PON to reach O5, operation, unless the receiver has no
light: without a fiber the PON says nothing about the new system. When all hold
it runs ``onu-update commit``, which sets ``sw_commit`` to the running bank and
removes the trial's mark. When they do not hold within 300 seconds it restarts
the stick. What it found is in ``/run/onu-trial`` and its log in
``/run/onu-trial.log``.

The environment as this system uses it
======================================

.. list-table::
   :header-rows: 1
   :widths: 24 20 56

   * - Variable
     - Written by
     - Meaning
   * - ``sw_commit``
     - ``onu-update commit``
     - The bank a plain start takes.
   * - ``sw_tryactive``
     - ``onu-update try``; the loader
     - 0 or 1: start that bank once; 2: no trial. The loader sets it to 2
       before it starts the tried bank.
   * - ``sw_active``
     - The loader
     - The bank it started last.
   * - ``sw_valid<N>``
     - ``onu-update write``
     - 0 while bank N is written or after a write that failed, 1 once it read
       back whole. ``try`` refuses a bank at 0.
   * - ``sw_version<N>``
     - ``onu-update write``
     - The version written into bank N. The shipped firmware wrote its own
       version names here.
   * - ``onu_trial``
     - ``onu-update try``, ``commit``
     - The bank armed for a trial, until a commit; with ``sw_tryactive`` back at
       2 and another bank running, that trial failed.

``onu-update`` saves the environment as the loader does: the whole new copy
into the copy that is not current, flagged 1, read back, and then the flag
byte of the old copy cleared to 0. Measured on the board, the loader's own save
writes 8192 bytes at the start of the other copy and then 1 byte at offset 4 of
the old one. A save cut short leaves one copy whose CRC checks out and whose
flag the loader takes.

What a failure costs
====================

.. list-table::
   :header-rows: 1
   :widths: 40 46 14

   * - Failure
     - What follows
     - How known
   * - A tried system that panics (a root filesystem without ``/sbin/init``)
     - ``panic=10`` restarts the stick after 10 s, and the loader starts the
       committed bank.
     - Measured
   * - A tried kernel that the loader cannot unpack (64 KiB of its data
       damaged, its CRCs made right)
     - The loader stops with ``LZMA: uncompress or overwrite error``; the
       watchdog it armed for the trial resets the chip, and the committed bank
       starts.
     - Measured
   * - A tried system that boots but does not come up
     - ``onu-trial`` restarts the stick after 300 s, into the committed bank.
     - Tested by function (``onu-utils/tests/test_onu_trial.sh``)
   * - A power loss during a write
     - The committed bank starts; the bank written reads ``incomplete`` and
       ``try`` refuses it until a new write.
     - Measured: the stick pulled from its cage during the second of a loop of
       writes of bank 1
   * - A damaged file
     - Refused when its checksum fails; the bank is left empty.
     - Measured through the panel's route
   * - A file for another board, or one too big
     - Refused before the bank is touched.
     - Tested by function

After every measured failure both copies of the environment read back with a
CRC that checks out, and the panel shows a failed trial as "last trial failed".

From the shipped firmware's web page
====================================

A stick as it ships runs the shipped firmware, and its web page takes an
update as a tar file that the shipped firmware installs into the bank it does
not run: ``web-install.tar``. The page takes a file smaller than its bank,
3932160 bytes, that holds ``md5.txt``; the shipped firmware's configd copies
it, ``/etc/scripts/fwu_starter.sh`` checks the digest of the file's
``fwu.sh`` against ``md5.txt`` and runs it as root with the bank to write, and
the stick restarts. Nothing is signed or checked against the model.

The package's ``fwu.sh`` (``br2-external/board/dfp-34x-2c3/fwu.sh``) writes
this system into that bank in the shipped firmware's split of it: ``k``, the
first 1328 KiB of this system's bank, into the shipped kernel partition, and
``r``, the rest, into the shipped root filesystem partition, so that the
kernel and the root filesystem land where this system's split has them. It
refuses a stick whose partitions are not this stick's, reads both partitions
back against the digests that ``mkweb.py`` wrote into it, and then sets
``sw_version``, ``sw_valid``, ``onu_trial`` and ``sw_tryactive`` for the
bank, as ``onu-update`` does. It exits 1 on purpose, and the page says the
update failed: configd then leaves ``sw_commit`` on the shipped firmware's
bank, and the boot loader starts this system once. ``onu-trial`` commits it
when it is up; a system that does not come up is restarted by the watchdog
into the shipped firmware.

The new system finds the shipped firmware's files in the partition
``config``, mounts them, takes the identity from them and starts with its
settings' defaults, which it then keeps in a file of its own there
(``boot-contract.rst``). The shipped firmware stays in the other bank until
an update overwrites it: going back is a switch of the bank, from the boot
loader's prompt (``run b<bank>``) or, for good, ``onu-update`` with
``sw_commit`` (``onu-update env``).

The first update
================

A stick that came with the shipped firmware holds it in bank 1, in the shipped
split, which ``onu-update`` reports as ``empty``. The first update from bank 0
writes this system into bank 1 and the shipped firmware is gone: keep a copy of
the whole flash first (``flashing.rst`` shows how to read the partitions).

Measured on the board
=====================

.. list-table::
   :header-rows: 1
   :widths: 60 40

   * - Step
     - Time
   * - Writing and reading back a 3.6 MB update file streamed over ssh
     - 37 s
   * - The same, uploaded through the panel's route
     - 46 s
   * - From the end of the boot script to the commit of a tried bank, the PON
       in O5
     - 5 s
