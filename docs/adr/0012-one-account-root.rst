.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==================================================================
ADR 0012: One account, root, for the console, ssh and the panel
==================================================================

:Status: Accepted, amended by 0013 (a bench's console)
:Date: 2026-10-08
:Decided-by: the owner, 2026-10-08

.. note::

   In plain words: the stick has one user, root, and one password, which the
   serial console, ssh and the web panel all ask for. Change it in any of the
   three and it is changed in all of them. Until somebody sets one, it is
   "admin", and the panel will not do anything before it is changed.

Context
=======

The stick had three ways in, each with a rule of its own: a shell on the serial
console that asked for nothing, an ssh server that took keys only, and a web
panel with a user of its own, ``admin``, whose password the panel kept as a
PBKDF2 hash in the configuration store. A unit that is sold needs a way in
that does not depend on the maker's key, and a console that does not hand a
shell to whoever opens the case and connects to it. The stick is managed by
one person, its owner; there is nothing to separate between users.

Decision
========

The stick has one account, ``root``, with one password.

* The configuration store keeps the password's hash, in the form of
  ``crypt()`` (SHA-256, ``$5$``, 20000 rounds, a salt of 16 random characters;
  a check costs about half a second on the stick). ``onu-store password``
  hashes a new password, with BusyBox's ``cryptpw``, and stores it; at every
  boot ``onu-store load`` writes root's line of ``/run/shadow``, which
  ``/etc/shadow`` of the read-only root filesystem names.
* The console runs ``getty`` and BusyBox's ``login``; dropbear takes the
  password as well as keys; the panel's user is ``root``. All three check the
  hash with ``crypt()`` of the C library, so there is one implementation.
* ``passwd`` on the console or over ssh, and the panel's change of password,
  both go through ``onu-store password``.
* Until a password is set, it is ``admin``, on all three alike, and the panel
  allows nothing but setting one. Root's shell says so at every login.
* A hash of the panel's earlier form, PBKDF2, is still taken by the panel, and
  the first sign-in with it stores the password again in ``crypt()``'s form.
  Until then the console and ssh have no password for root (``*``) and ssh
  takes keys only.
* A forgotten password is cleared from the boot loader's console, by a RAM
  boot into a shell (``rdinit=/bin/sh``) and ``onucfg ... unset AUTH_HASH``,
  which brings the default back (``docs/handbook/building.rst``).

Consequences
============

* Whoever reaches the stick's addresses, or its console, and knows the
  password can do everything. While the default is in force, that is anyone:
  the default is printed in this repository. The panel forces a change; the
  console and ssh only warn.
* The console asks for a login. The bench and a person at the console log in
  as root before they type a command; the boot's ready line still comes before
  the prompt.
* The image carries ``getty``, ``login`` and ``cryptpw`` (BusyBox) and
  ``crypt()`` with its SHA-256 form in the C library, which dropbear already
  needed for its password login.
* Physical access still wins: the boot loader starts anything. The password
  keeps a shell from a passer-by on the console, not from someone who can
  replace the system.
* There is no second account to give away, nor any account with fewer rights.
  A need for one would be a new decision.
