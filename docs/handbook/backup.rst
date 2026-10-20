.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=======================
A copy of the flash
=======================

Take a copy of the whole flash at the latest right after the install, before
the first update. Until then, the bank this system does not run holds the
shipped firmware, and the first update writes over it (``updating.rst``, "The
first update"). The copy is the only record of what was there.

The install itself wrote this system over the other bank, the one the shipped
firmware did not run, so no copy taken after it holds what that bank held
before, which on one stick as sold was an earlier build of the shipped
firmware (``updating.rst``, "From the shipped firmware's web page"). These
pages give no way to copy the flash before the install.

What the copy holds
===================

The flash is the eight partitions of ``boot-contract.rst``, "The flash", which
say what each holds and how it is written; the copy is the chip from its first
byte to its last. The commands below need only where each partition starts and
how long it is, in blocks of 4 KiB, and ``mtd`` is its number on the stick:

.. list-table::
   :header-rows: 1
   :widths: 14 16 22 22

   * - Device
     - Name
     - First block
     - Blocks
   * - ``mtd0``
     - ``boot``
     - 0
     - 64
   * - ``mtd1``
     - ``env``
     - 64
     - 2
   * - ``mtd2``
     - ``env2``
     - 66
     - 2
   * - ``mtd3``
     - ``config``
     - 68
     - 60
   * - ``mtd4``
     - ``k0``
     - 128
     - 464
   * - ``mtd5``
     - ``r0``
     - 592
     - 496
   * - ``mtd6``
     - ``k1``
     - 1088
     - 464
   * - ``mtd7``
     - ``r1``
     - 1552
     - 496

The lengths are those ``/proc/mtd`` gives on the board (M: the command below,
2026-10-11).

**The copy is private.** It holds the unit's registration identity, its MAC
address and this system's secrets. Keep it off shared storage, out of this
repository and out of any report or issue.

Taking the copy
===============

Over ssh, the way that was tried on a stick: from a host that reaches the
stick by ssh as root, while no change of the settings is being saved::

    ssh root@STICK 'n=0; for c in 64 2 2 60 464 496 464 496; do
        dd if=/dev/mtd${n}ro bs=4096 count=$c 2>/dev/null; n=$((n + 1))
    done' >flash.bin

``STICK`` is the stick's management address. The read-only devices
``/dev/mtdNro`` are used, each with an explicit block count: without one a
read does not see the end of the device and hangs (M: ``flashing.rst``,
"Check that bank 1 is untouched"). The result is one file of 8388608 bytes,
the chip from its first byte to its last. The read takes about 6 s on the
stick (M: 2026-10-11).

Check the copy against the stick: run the same loop on the stick into
``sha256sum`` and compare with ``sha256sum flash.bin`` on the host::

    ssh root@STICK 'n=0; for c in 64 2 2 60 464 496 464 496; do
        dd if=/dev/mtd${n}ro bs=4096 count=$c 2>/dev/null; n=$((n + 1))
    done | sha256sum'
    sha256sum flash.bin

The two digests have to be the same. Keep the digest with the copy.

From the panel, which has not been tried on a stick yet: the System page's
Software tab has "Copy the whole flash", which needs root's password in "Root
password" and is made to download the eight partitions in order as one file,
``buchos-gpon-flash-<version>.bin`` of 8388608 bytes
(``GET /api/firmware/backup``, ``onu-update backup`` on the stick;
``pond/README.rst``). The file's name begins with the product name the
panel shows, in lower case with a hyphen for each gap: BuchOS's, unless
another name was set under System, tab Settings
(``fibre-box-gpon-flash-<version>.bin`` for "Fibre Box"). It is tested on the
build machine only
(``onu-utils/tests/test_onu_update.sh``): check the size of what it saved,
and its digest against the stick's as above, before you rely on it. Keep it
private: it holds the unit's registration identity and this system's secrets.

Writing it back
===============

Writing a copy back is work for a bench; the panel has no way to do it. It
needs the serial console, to stop the boot loader and start a system from RAM
(``recovery.rst``, "Start a bank, or RAM, from the prompt"); a host port that
links at 2500BASE-X, the only mode the boot loader runs the lane in
(``board.rst``, "The host side"); and a RAM image built from this repository's
sources, which is not among a release's files. Going back to the shipped
firmware after the first update takes all three, and the copy.

Write a copy back from a RAM boot, never from the flash: a system that runs
from the flash cannot write the bank it runs from, and it keeps ``config``
mounted. Start the stick from RAM with an image that carries ``flash_erase``,
which the plain ``make ram-image`` does and ``RAM_DIAG=no`` does not
(``building.rst``, "Putting the images on the board"; ``flashing.rst``, "Start
a trial kernel from RAM"). No bank runs then, and the flash is idle.

The loader is not part of this. ``boot`` is read-only in the device tree: an
erase or a write of ``/dev/mtd0`` fails with "Permission denied" (M:
``flash_erase /dev/mtd0 0 0`` on the board, 2026-10-12). Nothing in this
repository writes the loader, and a damaged one is for a programmer on the chip
(``flashing.rst``, "Rules that keep the board safe"). The copy holds it, and
the steps below leave it alone.

Put back only a copy of this very stick. A copy of another unit carries that
unit's registration identity and MAC address, and writing it makes this stick
a second one of it.

The steps, from the host that holds ``flash.bin`` and reaches the stick as
root. ``STICK`` is its management address:

1. Let go of the store, which the boot mounted read-only::

       ssh root@STICK 'umount /config'

2. Write the partitions back one at a time, the banks first, then ``config``,
   the loader's environment last, so that an interruption leaves the
   environment naming a bank that was put back before it. Each ``put`` takes
   the number of the ``mtd`` device, the first block of the partition in the
   copy and its length, in 4 KiB blocks (the table above gives both)::

       put() {
           dd if=flash.bin bs=4096 skip=$2 count=$3 2>/dev/null |
               ssh root@STICK "flash_erase /dev/mtd$1 0 0 >/dev/null &&
                   dd of=/dev/mtd$1 bs=4096 2>/dev/null"
       }
       put 4 128 464     # k0
       put 5 592 496     # r0
       put 6 1088 464    # k1
       put 7 1552 496    # r1
       put 3 68 60       # config
       put 1 64 2        # env
       put 2 66 2        # env2

   One partition alone is one ``put``: for the store, ``put 3 68 60``.

3. Check the stick against the copy with the loop of "Taking the copy"::

       ssh root@STICK 'n=0; for c in 64 2 2 60 464 496 464 496; do
           dd if=/dev/mtd${n}ro bs=4096 count=$c 2>/dev/null; n=$((n + 1))
       done' | cmp - flash.bin && echo 'the flash is the copy'

4. Restart the stick: ``ssh root@STICK reboot``. The loader starts the bank
   that the restored ``env`` and ``env2`` name.

Do not stop a ``put`` once it has begun, and do not restart or power-cycle the
stick while one runs. A partition that was cut short reads as erased or half
written; run its ``put`` again, from the start. What a half-written ``env`` or
``env2`` does to the loader has not been tried.

What the writing was measured to do (M: the steps above on the board,
2026-10-12, through an ssh jump host, from a copy taken minutes before):

* The steps as written, after ``config``, ``k1`` and ``env`` had been erased
  on purpose, took 92 s with the check, which said "the flash is the copy". A
  ``put`` took 1 s or 2 s for ``env`` and ``env2``, 3 s or 4 s for ``config``
  and 18 s or 19 s for a bank; the check read the whole flash in 11 s.
* ``config`` and ``k1`` erased and the stick started from RAM with the erased
  ``config``: it took the empty partition for a store of defaults (the root
  password and the settings gone, and the shipped identity with them, since
  the shipped firmware's files are in ``config``). The two ``put`` of the steps
  put both back, and the next start read the store (``/run/onu-store`` said
  ``loaded``), found the password set, read the identity from the shipped
  firmware's files and reached O5 with its two sessions.

The restart after the copy was written was into the RAM boot that the bench
uses, not into the flash's own system; the test did not boot the restored
banks.
