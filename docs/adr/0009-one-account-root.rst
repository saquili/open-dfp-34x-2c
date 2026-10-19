.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===================================================================
ADR 0009: Use one account, root, for the console, ssh and the panel
===================================================================

:Status: Accepted
:Date: 2026-10-19
:Deciders: the maintainer

Summary
=======

The stick has one user, root, and one password, which the serial console,
ssh and the web panel all ask for; a change in any of them changes it in
all three. At the first start the password is taken from the shipped
firmware's administrator account, so the owner signs in with the password
they already know. Where that is still the factory default, ``admin``, the
panel does nothing until it is changed, and ssh takes keys only.

Context
=======

A stick can be reached in three ways: the serial console inside its case,
ssh and the web panel. A unit in its owner's hands needs a way in that does
not depend on anyone else's key, and a console that does not hand a shell to
whoever opens the case. The stick is managed by one person, its owner, so
there are no users to separate.

The shipped firmware has an administrator account of its own, whose
password its owner may have changed on its web page; its files on the
``config`` partition keep that password. Its factory default is ``admin``,
which is printed in this repository as well.

Decision
========

The stick has one account, ``root``, with one password.

* The configuration store keeps the password's hash in the form of
  ``crypt()``: SHA-256 (``$5$``), 20,000 rounds and a salt of 16 random
  characters, which costs about half a second to check on the stick.
  ``onu-config password`` hashes a new password with BusyBox's ``cryptpw``
  and stores it; at every boot ``onu-config load`` writes root's line of
  ``/run/shadow``, which ``/etc/shadow`` on the read-only root file system
  points to.
* The console runs ``getty`` and BusyBox's ``login``; dropbear takes the
  password as well as keys; the panel's user is ``root``. All three check
  the hash with the C library's ``crypt()``, so there is one implementation.
  The panel also accepts a PBKDF2 hash in the store and replaces it with
  ``crypt()``'s form at the first sign-in.
* ``passwd`` on the console or over ssh and the panel's password change all
  go through ``onu-config password``.
* **The first password is the shipped firmware's.** At the first start,
  while the store holds no password, root's password becomes that of the
  shipped firmware's administrator, which ``ponid`` reads from the shipped
  firmware's files. The factory default ``admin`` is not taken over as a
  password of its own.
* **Until a password is set, it is** ``admin`` **on all three**, and the
  panel allows nothing but setting one; root's shell says so at every login.
  A password cleared later, to bring the default back, stays cleared.
* **While the default is in force, ssh takes keys only**, as it does while
  the password is not known, since ``admin`` is printed in this repository.
  Setting a password ends the ssh server, and procd starts it again with
  passwords allowed. The console still takes ``admin``, which needs the case
  opened.
* A forgotten password is cleared from the boot loader's console, by a RAM
  boot into a shell (``rdinit=/bin/sh``) and ``onu-config unset AUTH_HASH``,
  which brings the default back (``docs/handbook/building.rst``).
* A bench may give the console a shell without the password
  (``CONSOLE_AUTOLOGIN=root``, ADR 0010); an image without the bench's file
  asks for it. The panel's own process runs as an account of its own, with
  which no one logs in (ADR 0011).

Consequences
============

* Whoever reaches the stick's addresses or its console and knows the
  password can do everything. While the default is in force the panel
  forces a change, ssh refuses passwords, and the console warns.
* The console asks for a login. The bench and a person at the console log
  in as root before typing a command; the boot's ready line still comes
  before the prompt.
* The image carries ``getty``, ``login`` and ``cryptpw`` (BusyBox) and the
  SHA-256 form of ``crypt()`` in the C library, which dropbear needs for
  password logins anyway.
* Physical access still wins: the boot loader starts anything. The password
  keeps a passer-by from a shell on the console, not someone who can replace
  the system.
* There is no second account to hand out, and no account with fewer rights.
  A need for one would be a new decision.
