.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========================================================
ADR 0017: Update files carry no signature
=========================================================

:Status: Accepted
:Date: 2026-10-11
:Decided-by: the owner, 2026-10-11

.. note::

   In plain words: the stick takes any update file that is whole and made
   for this board, from anyone who knows root's password. Nobody's key
   decides what may run on a stick; its owner does.

Context
=======

An update file (``onu-utils/src/update.h``) carries a header with the board,
the version and the sizes, and checksums of its two images, which catch a
damaged or cut file. It carries no signature. A review of the architecture
asked for a signature, or for a recorded decision that there is none.

The status daemon faces the network. Until 2026-10-11 it could have
``onu-act`` write an update file and start it without more than a session,
so that a status daemon taken over could run a system of its own choosing.
Since then ``onu-act`` writes an update and starts the other bank only with
root's password in force in the call, which it checks itself; the panel asks
for the password for both.

A signature would let only the holder of one key make a file the stick
takes. The stick is sold to its owner, who may build the system from this
repository and run what they built.

Decision
========

* Update files carry no signature. ``onu-update`` takes a file that is whole,
  made for this board and fits the bank.
* Writing an update and starting the other bank need root's password in
  force, which ``onu-act`` checks with ``onu-config password-check`` for any
  caller, the status daemon included.

Consequences
============

* Whoever knows root's password, or has root on the stick, can install any
  system. The password, and who knows it, is what guards the stick.
* A status daemon taken over cannot install a system until the owner signs
  in through it again, when it would see the password.
* No key needs keeping, and an owner's own build installs as ours does.
* The shipped firmware's way in, its web page (``updating.rst``), is not
  guarded by this: it runs the package's script as the shipped firmware
  allows.
