.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

======================
tools/shipped-fw-check
======================

``check-shipped-fw.sh`` is a quick, read-only check that a DFP-34X-2C stick
which runs its shipped firmware is fit for an update: its laser and optics,
processor, memory, flash banks and boot environment. It writes nothing and
changes nothing, and it needs only what every build of the shipped firmware
has: busybox 1.12.4's ``ash``, ``awk``, ``sed``, ``cut``, ``grep``,
``md5sum``, ``sleep`` and the tools ``nv`` and ``diag``.

Run it in the shipped firmware's shell, over ssh or telnet (both want the
shipped firmware's own password) or on the serial console::

    sh check-shipped-fw.sh
    ssh root@192.0.2.1 'sh -s' < check-shipped-fw.sh

Each line starts with ``PASS``, ``WARN`` or ``FAIL``, or with ``INFO`` for a
figure that is not judged. The exit status is 0 when nothing needs a look, 1
for warnings, 2 for a failure and 3 when the stick does not run the shipped
firmware.

What it checks
==============

.. list-table::
   :header-rows: 1
   :widths: 18 82

   * - Name
     - What makes it pass
   * - ``env``
     - The boot environment reads with a good CRC (``nv getenv``).
   * - ``bank``
     - ``sw_active`` is the bank that runs. The shipped firmware's page writes
       the bank ``1 - sw_active``, so a stick whose ``sw_active`` is not the
       running bank would have its running system written over. This is the
       one check that fails for a stick started once from the boot loader
       without ``sw_active`` following.
   * - ``trial``
     - ``sw_tryactive`` is 2: no trial boot is pending.
   * - ``split``
     - The partitions are the shipped firmware's: kernel 1328K and root
       filesystem 2512K, in both banks.
   * - ``bank0``, ``bank1``
     - The MD5 of each bank's kernel and root filesystem partitions is that of
       a known build: the eight builds that the repository Anime4000/RTL960x
       keeps for this stick, or the first release of this system. Another
       value is a warning, not a failure: it may be a modified or damaged
       bank, or a build the table does not know.
   * - ``config``
     - The partition for the settings is mounted and has 40K free.
   * - ``cpu-idle``, ``cpu-load``
     - The processor is at least 50 % idle over two seconds and the load of
       the last minute is below 2.5 (warning up to 6 and down to 10 % idle).
   * - ``memory``
     - Free plus cached memory is at least 8000 kB: the page keeps the update
       file of about 3.4 MB in RAM while it reads it.
   * - ``onu-state``
     - The GPON state is O5. A build that picks the PON mode itself (the
       hybrid builds) says that the GPON device is not activated until it has
       light of the right kind; that is a warning, and the readings that need
       bursts then do not count.
   * - ``temperature``, ``supply``
     - The module's temperature is between -10 and 75 C and its supply
       between 3.135 and 3.465 V (the SFP's 3.3 V, 5 %).
   * - ``bias-current``, ``tx-power``, ``rx-power``
     - Judged at O5 only: bias 2 to 60 mA, transmit 0 to 5.5 dBm (class B+
       is 0.5 to 5), receive between the sensitivity, -28 dBm, and the
       overload, -8 dBm.

The known builds
================

``bank0`` and ``bank1`` are looked up in a table in the script: for each build
the MD5 of the 1328K kernel partition and of the 2512K root filesystem
partition as the shipped firmware's kernel sees them, which is the first
1328K of the bank and the rest of it. A build is added by appending a line of
the two sums and its name.

Measured
========

On a stick of the bench, on 2026-10-25, from the shell that the shipped
firmware gives with ``init=/bin/sh`` once its start script has run, on seven
of the eight builds (all but the one the stick shipped with). Every build gave
``PASS`` for all of the flash, the memory, the processor and the module's
temperature and supply. The M110 builds were at O5 and passed the optics
(bias 13.0 to 13.7 mA, transmit 2.23 to 2.30 dBm, receive -27.4 dBm); the
M114 builds were not activated at GPON and warned. The check failed, as it is
meant to, for a stick started once from the boot loader with ``sw_active``
other than the running bank.
