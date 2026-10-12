.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===================
tools/flash-layout
===================

A check that the layout of the board's flash and the kernel agree on its erase
sector.

The chip is erased in 4 KiB sectors, and the layout is built on them: the two
copies of the boot loader's environment are 8 KiB each. The kernel erases in
4 KiB sectors only with ``CONFIG_MTD_SPI_NOR_USE_4K_SECTORS``, which is on by
default and which nothing in the defconfig said. Without it the kernel erases in
larger blocks and, when it registers the partitions, forces every one that is not
made of whole blocks to be read-only (``drivers/mtd/mtdpart.c``): the
environment, so that no bank can be chosen, and with it the configuration store.
The first sign was a save that failed on the board.

Usage
=====

::

    tools/flash-layout/flash-layout [--dts FILE] [--kernel-config FILE] [--sector BYTES]

Without options it reads the board's device tree in ``linux/``. It prints the
partitions of the node ``fixed-partitions`` and checks that each

* has a label, which no other has;
* starts and ends on the sector (4096 bytes; ``--sector`` changes it), and is not
  empty;
* has the unit address that its ``reg`` starts at;
* does not overlap the one before it in the flash.

``--kernel-config`` names the ``.config`` of a built kernel and checks that the
flash driver is built in (``MTD_SPI_NOR=y``) and uses the 4 KiB sectors. ``make
image-check`` gives it the kernel of the image build, ``make check`` runs the
tool on the device tree.

Exit status is 0 when all is as it should be, 1 on a finding, which is named on
the standard error, and 2 when a file cannot be read or the device tree has no
partitions.

What else keeps the sector
==========================

* ``arch/mips/configs/rtl9602c_defconfig`` states the option, with the reason; the
  tests of this tool fail if it is not there.
* The boot script, ``rcS``, warns on the console and in the log when
  ``/proc/mtd`` shows that the partition ``config`` has another erase size,
  which is what a chip that cannot erase 4 KiB sectors would show.
* The tests compare the device tree with the bench's table of partitions
  (``tools/bench``) and with the budgets of ``tools/image-budget``, so that the
  three do not drift apart: the bench's bank 1 is the shipped firmware's own
  split, which it never writes, and is left out.

Tests
=====

::

    tools/flash-layout/tests/run [-v]

They need Python 3 and make the device trees and kernel configurations they
check; they also check the board's own files.
