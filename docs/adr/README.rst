.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=============================
Architecture decision records
=============================

Each record captures one decision: its context, the decision, and its
consequences. A record is **Accepted** once the decision holds, **Proposed**
while it still needs confirmation, and **Superseded** when a later record
replaces it.

How an accepted record changes
==============================

A record is the reasoning of the day it was written. Once it is Accepted its
text is not rewritten, so that a reader of the history sees what was decided
and why. Three things may still be done to it, and nothing else:

* **Its status line may change.** "Accepted" becomes "Accepted, amended by
  0013" when a later record changes it (ADR 0012), or "Accepted, amended"
  when its own amendments do (ADR 0015), and "Superseded by 0011" when a
  record replaces it (ADR 0010).
* **An amendment may be added at its end**, in a section called
  "Amendments", one entry for each change, the oldest first. An entry begins
  with its date and who decided, and says which part of the record it changes
  and why. It carries the decision out, narrows it or gives a part of it up,
  in a few sentences; it does not restate the record. A fact that the record
  got wrong, a number, a name or a count, is corrected in an entry in the same
  way, not in the text above it.
* **A new decision is a new record.** A change that stands on its own, a
  choice of another mechanism or one with a context and consequences of its
  own, takes the next number and a line ``:Amends:`` that names the record it
  changes, as ADR 0009 does for ADR 0006. The record it amends then says so
  in its status line.

What does not belong in a record is the result of a trial: a trial belongs
to the handbook page of the thing it tried, or, for a run of the device tests,
to ``docs/testing``. A record may say what a decision cost when it was taken,
a size or a time with its date, since the decision rests on it.

The index
=========

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
   * - 0012
     - One account, root, for the console, ssh and the panel
     - Accepted, amended by 0013 and 0014
   * - 0013
     - procd is the first process and watches the stick's programs
     - Accepted, amended by 0014
   * - 0014
     - One control plane turns the settings into what procd and netifd keep
     - Accepted, amended by 0019 and 0020
   * - 0015
     - The names of the stick's own programs say what they do
     - Accepted, amended
   * - 0016
     - The panel answers over HTTPS with its own key
     - Accepted, amended
   * - 0017
     - Update files carry no signature
     - Accepted
   * - 0018
     - The kernel series aims at mainline
     - Accepted, amended, and amended by 0021
   * - 0019
     - The settings are a file in the config partition's file system
     - Accepted
   * - 0020
     - The flash's image has no iproute2 and the scripts run no ip
     - Accepted
   * - 0021
     - The kernel series keeps mainline's rules, without a submission
     - Accepted
   * - 0022
     - Programs of our own for PPPoE, DHCPv6 and the time
     - Accepted
