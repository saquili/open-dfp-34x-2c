.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=======================
A copy of the flash
=======================

Take a copy of the whole flash before the first update. Until then, the bank
this system does not run holds the shipped firmware, and the first update
writes over it (``updating.rst``, "The first update"). The copy is the only
record of what was there.

What the copy holds
===================

The flash is 8 MiB in eight partitions that cover it exactly, in this order
(``boot-contract.rst``):

.. list-table::
   :header-rows: 1
   :widths: 12 12 16 60

   * - Device
     - Name
     - Size, 4 KiB blocks
     - What it holds
   * - ``mtd0``
     - ``boot``
     - 64
     - The boot loader.
   * - ``mtd1``, ``mtd2``
     - ``env``, ``env2``
     - 2 each
     - The two copies of the loader's environment, with the board's MAC
       address.
   * - ``mtd3``
     - ``config``
     - 60
     - The shipped firmware's file system: the unit's registration identity
       (serial number, PLOAM password), and this system's store (root's
       password hash, the PPPoE accounts, the panel's key).
   * - ``mtd4``, ``mtd5``
     - ``k0``, ``r0``
     - 464, 496
     - Bank 0, in this system's split.
   * - ``mtd6``, ``mtd7``
     - ``k1``, ``r1``
     - 464, 496
     - Bank 1, in this system's split. A bank of the shipped firmware is in
       its own split within the same 3840 KiB, so the two partitions of a
       bank together hold it whole.

The sizes are those ``/proc/mtd`` gives on the board (M: the command below,
2026-10-11).

**The copy is private.** It holds the unit's registration identity, its MAC
address and this system's secrets. Keep it off shared storage, out of this
repository and out of any report or issue.

Taking the copy
===============

From a host that reaches the stick by ssh as root, while no change of the
settings is being saved::

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
``sha256sum`` and compare with ``sha256sum flash.bin`` on the host. Keep the
digest with the copy.

Writing it back
===============

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
   copy and its length, in 4 KiB blocks (the table above gives the lengths; a
   partition starts where the one before ends)::

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
