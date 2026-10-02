.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

====
docs
====

Project documentation that is not kernel documentation. (Kernel documentation
lives under ``linux/Documentation/`` in mainline layout.)

* ``policy/`` -- the provenance policy (``provenance.rst``), the rule for what
  may and may not enter this repository. In place now.
* ``adr/`` -- the architecture decision records. In place now.
* ``handbook/`` -- the device maker's handbook: building, flashing, recovery
  over the serial console, bank discipline, the boot contract, and optical
  safety. Filled by **WI-20** (with the board readme and the boot contract from
  **WI-15**).
* ``testing/`` -- the records of the device tests run on the stick: a page per
  run, with the table of every case, its result and its measured values. In
  place now.

The handbook is written in reStructuredText and is meant to build with Sphinx,
alongside the kernel documentation, through ``make docs``.
