.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==========================================================
ADR 0012: Name the stick's own programs after what they do
==========================================================

:Status: Accepted
:Date: 2026-10-12
:Deciders: the maintainer

Summary
=======

A program of this project is named after what it does, and a person at the
stick sees only the few programs meant for a person; the rest, which only
other programs call, are kept out of the way. Programs taken from OpenWrt
and elsewhere keep their names.

Context
=======

A person at the stick should be able to tell from a program's name what it
is for, and which programs are meant for them. Names given early in the
bring-up did not: one was named after operating modes the stick no longer
has, the settings had two names, and a dozen helpers that only other
programs call lay beside the programs a person runs.

Decision
========

* A program of this tree is named after what it does: ``onu-config`` holds
  the settings, their store in the flash and their rules; ``onu-apply``
  applies them, and ``onu-apply reload`` applies them again after a change;
  ``onu-update`` writes and starts the banks.
* The programs a person runs at the stick are in ``/usr/sbin`` and
  ``/usr/bin``. Those that only other programs run are in ``/usr/lib/onu``,
  where no command line looks. ``onu-utils/README.rst`` lists both and says
  what each is for.
* Programs taken from OpenWrt and elsewhere keep their names (procd,
  netifd, ubusd, dropbear, BusyBox's applets), as a fork keeps the name of
  what it forks. The daemons of this tree keep theirs too: omcid, pppoed,
  dhcp6d, timed and pond say what they are.
* Files of the running state are named after the state they hold, not after
  a program: ``/run/onu-mode`` holds the stick's mode.

Consequences
============

* A person at the stick sees seven programs of this tree: ``onu-config``,
  ``onu-apply``, ``onu-update``, ``onu-svc``, ``onu-nl`` and ``pon`` in
  ``/usr/sbin``, and ``passwd`` in ``/usr/bin``.
* A program in ``/usr/lib/onu`` is called by its full path, or by a caller
  that names the directory, which the tests point at their stand-ins.
