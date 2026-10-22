.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=============================
Architecture decision records
=============================

Each record captures one decision: the problem it answers, the options that
were weighed, what was decided and what follows from it. The records explain
why the system is built as it is; how it works is in the handbook
(``docs/handbook/``), and how a program is used is in its README.

The form of a record
====================

Every record has the same parts, in this order:

The header
    ``:Status:``, ``:Date:``, the day of the latest decision the record
    holds, and ``:Deciders:``.
Summary
    The decision in a few plain sentences, for a reader who reads nothing
    else.
Context
    The problem and the facts the decision rests on.
Options considered
    The alternatives that were weighed, and why each was or was not taken.
    A record whose decision had no real alternative leaves this part out.
Decision
    What was decided, stated so that it can be checked.
Consequences
    What follows, the costs as well as the gains.

A record states the decision as it stands, and names programs and files by
their current names. A status is **Proposed** while a decision still needs
confirmation, **Accepted** once it holds, and **Superseded** when a later
record replaces it.

How a record changes
====================

* **A decision that changes a part of a record** is written into the record,
  which takes the new date. The history of the file keeps the earlier text.
* **A new decision is a new record.** A change that stands on its own, such
  as the choice of another mechanism, takes the next number; the record it
  replaces becomes **Superseded** and names its successor.
* A number is never reused.

The results of trials do not belong in a record: they belong in the handbook
page of the thing that was tried, or, for a run of the device tests, in
``docs/testing``. A record may state what a decision cost, such as a size
with its date, because the decision rests on it.

The index
=========

.. list-table::
   :header-rows: 1
   :widths: 10 72 18

   * - ADR
     - Title
     - Status
   * - 0001
     - Name the repository ``open-dfp-34x-2c``
     - Accepted
   * - 0002
     - Put our own work under GPL-2.0-only
     - Accepted
   * - 0003
     - Follow mainline structure, with a device maker's scope
     - Accepted
   * - 0004
     - Keep one repository of upstream-shaped components
     - Accepted
   * - 0005
     - Build the distribution as a Buildroot external tree
     - Accepted
   * - 0006
     - Name registers and identifiers by function
     - Accepted
   * - 0007
     - Start from a fresh git history
     - Accepted
   * - 0008
     - Share one C library, cut to what the programs use
     - Accepted
   * - 0009
     - Use one account, root, for the console, ssh and the panel
     - Accepted
   * - 0010
     - Run procd as the first process and the supervisor
     - Accepted
   * - 0011
     - Drive the stick through one control plane
     - Accepted
   * - 0012
     - Name the stick's own programs after what they do
     - Accepted
   * - 0013
     - Serve the panel over HTTPS with the stick's own key
     - Accepted
   * - 0014
     - Carry no signature in update files
     - Superseded by 0022
   * - 0015
     - Hold the kernel series to mainline's rules without submitting it
     - Accepted
   * - 0016
     - Keep the settings as a file in the config partition's file system
     - Accepted
   * - 0017
     - Leave iproute2 out of the flash image
     - Accepted
   * - 0018
     - Write our own PPPoE, DHCPv6 and time programs
     - Accepted
   * - 0019
     - Put firewall rules in the switch's ACL, defaults in netfilter
     - Accepted
   * - 0020
     - Build the images with a pinned toolchain release
     - Accepted
   * - 0021
     - Use the core's ll and sc for atomic operations, without threads
     - Accepted
   * - 0022
     - Sign each release, and say what a file is signed with
     - Accepted
