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

No procedure for writing a copy back is recorded yet, and no tool of this
repository does it. The bench writes bank 0 from the loader and never bank 1
(``flashing.rst``); ``onu-update`` writes only update files of this system.
The copy still proves what the stick held, and is what a later procedure will
write back.
