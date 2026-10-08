.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=============================
Architecture decision records
=============================

Each record captures one decision: its context, the decision, and its
consequences. A record is **Accepted** once the decision holds, **Proposed**
while it still needs confirmation, and would become **Superseded** if a later
record replaces it. Records are immutable once accepted; a change is a new
record.

.. list-table::
   :header-rows: 1
   :widths: 12 58 20

   * - ADR
     - Title
     - Status
   * - 0001
     - Repository name: ``rtl9601d-linux``
     - Superseded by 0008
   * - 0002
     - Licence: GPL-2.0-only for everything we author
     - Accepted
   * - 0003
     - Mainline structure, device-maker scope
     - Accepted
   * - 0004
     - One repository of upstream-shaped components
     - Accepted
   * - 0005
     - Distribution build: a Buildroot external tree
     - Accepted
   * - 0006
     - Register and identifier naming
     - Accepted, amended by 0009
   * - 0007
     - Fresh git history, separate from the laboratory repository
     - Accepted
   * - 0008
     - Repository name: ``open-dfp-34x-2c``
     - Accepted
   * - 0009
     - Register macros take the driver's prefix
     - Accepted
   * - 0010
     - Target executables are linked statically
     - Superseded by 0011
   * - 0011
     - Target executables share one C library, cut to what they use
     - Accepted
