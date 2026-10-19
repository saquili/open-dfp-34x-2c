.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

============================================
ADR 0014: Carry no signature in update files
============================================

:Status: Accepted
:Date: 2026-10-11
:Deciders: the maintainer

Summary
=======

The stick installs any update file that is intact and made for this board,
from anyone who knows root's password. No one's key decides what may run on
a stick; its owner does.

Context
=======

An update file (``onu-utils/src/update.h``) carries a header with the
board, the version and the sizes, and checksums of its two images, which
catch a damaged or truncated file. It carries no signature. A review of the
architecture asked for a signature, or for a recorded decision that there
is none.

The status daemon faces the network. If it could have ``onu-act`` write an
update file and start it with no more than a session, a status daemon taken
over could run a system of its own choosing.

A signature would let only the holder of one key make a file the stick
accepts. The stick belongs to its owner, who may build the system from this
repository and run what they built.

Decision
========

* Update files carry no signature. ``onu-update`` accepts a file that is
  intact, made for this board and fits the bank.
* Writing an update and starting the other bank need root's password in the
  call, which ``onu-act`` checks with ``onu-config password-check`` for any
  caller, the status daemon included; the panel asks for the password for
  both.

Consequences
============

* Whoever knows root's password, or has root on the stick, can install any
  system. The password, and who knows it, is what guards the stick.
* A status daemon taken over cannot install a system until the owner signs
  in through it again, when it would see the password.
* No key needs keeping, and an owner's own build installs as ours does.
* The shipped firmware's way in, its web page
  (``docs/handbook/updating.rst``), is not guarded by this: it runs the
  package's script as the shipped firmware allows.
* A release is made only by the repository's release workflow
  (``.github/workflows/release.yml``), from the commit it is started on, and
  lists the SHA-256 digest of every file. A user who checks the digests of
  what they downloaded knows they hold the released files; the stick itself
  checks nothing beyond integrity and the board.
