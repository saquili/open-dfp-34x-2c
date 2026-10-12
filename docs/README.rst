.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

====
docs
====

Project documentation that is not kernel documentation, which lives under
``linux/Documentation/`` in mainline layout. Each program of this tree has a
README of its own beside its code, for its interfaces and files.

``handbook/``
    The device maker's handbook: the board, how the system works, the flash
    and the boot contract, building, installing and updating, the bench,
    copying and restoring the flash, recovery over the console, optical
    safety, the toolchain, and the panel's design (``handbook/README.rst``).
``adr/``
    The architecture decision records: why the repository, the build, the
    libraries, the accounts, the supervision, the control plane and the panel
    are as they are (``adr/README.rst``).
``policy/``
    The provenance policy (``provenance.rst``): what may and may not enter
    this repository, and how a fact about the hardware is written.
``testing/``
    The records of the device tests run on the stick: a page per run, with
    the table of every case, its result and its measured values
    (``testing/README.rst``).

The pages are reStructuredText and are read as they are; ``make docs`` renders
them to HTML in ``build/docs`` and fails on any warning (``tools/docs``).
``make -C linux htmldocs`` renders the kernel's.
