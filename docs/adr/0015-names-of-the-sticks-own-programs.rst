.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

================================================================
ADR 0015: The names of the stick's own programs say what they do
================================================================

:Status: Accepted, amended (see "Amendments")
:Date: 2026-10-10
:Decided-by: the owner, 2026-10-10

.. note::

   In plain words: a program of ours is named after what it does, and a
   person sees only the few that are for a person; the rest are parts of the
   others, out of the way. The programs taken from OpenWrt keep their names.

Context
=======

The owner could not tell from the names of the stick's own programs what
each does. ``onu-mode`` was named after the modes the stick had (bridge,
router, passthrough), which nobody chooses any more; what it does is turn the
settings into what the stick runs (ADR 0014). ``onucfg`` and ``onu-store``
are two names for one thing, the stick's settings. And every helper that
only other programs call, such as ``onu-net`` or ``onu-run``, lay beside the
programs a person runs, so that a person met a dozen ``onu-`` names with no
way to tell the ones meant for them.

Decision
========

* A program of this tree is named after what it does. ``onu-mode`` is
  ``onu-apply``: it applies the settings, and ``onu-apply reload`` applies
  them again after a change. ``onucfg`` and ``onu-store`` become one,
  ``onu-config``: the settings, their store in the flash and their rules.
* The programs a person runs at the stick are in ``/usr/sbin`` and
  ``/usr/bin``: ``onu-config``, ``onu-apply``, ``onu-update``, ``onu-svc``,
  ``pon`` and ``passwd``. The ones only other programs run are in
  ``/usr/lib/onu``, where no command line looks: ``onu-net``, ``onu-run``,
  ``onu-mgmt``, ``onu-lane``, ``onu-trial``, ``ponid`` and ``dhcp-wan``.
* The programs taken from OpenWrt and elsewhere keep their names (procd,
  netifd, ubusd, dropbear, BusyBox's), as a fork keeps the name of what it
  forks. The daemons of this tree keep theirs too: omcid, pppoed, dhcp6d and
  pond say what they are.
* The files of the running state keep their names where they name a state
  rather than a program: ``/run/onu-mode`` holds the stick's mode.

Records before this one name the programs as they were called then; the old
names are, for each:

============================  ====================
Then                          Now
============================  ====================
``onu-mode``                  ``onu-apply``
``onu-mode apply``            ``onu-apply reload``
``onucfg``, ``onu-store``     ``onu-config``
============================  ====================

Consequences
============

* A name now says what its program is for, and a person at the stick sees
  six programs of this tree, not thirteen.
* The renames change no behaviour; each is made in the components that name
  the program, with their tests.
* A program in ``/usr/lib/onu`` is called by its whole path, or by a caller
  that names the directory, which its tests set to their stand-ins.

Amendments
==========

**2026-10-12, the owner: the programs as they stand.**

A person at the stick runs seven programs of this tree, not six. ``onu-nl``,
which makes the requests of ``tc``, ``devlink`` and ``ip`` that the scripts
need (ADR 0020), is in ``/usr/sbin`` beside ``onu-config``, ``onu-apply``,
``onu-update``, ``onu-svc`` and ``pon``, and ``passwd`` is in ``/usr/bin``.

The programs that only other programs run, in ``/usr/lib/onu``, are now
``onu-net``, ``onu-run``, ``onu-mgmt``, ``onu-lane``, ``onu-trial``,
``onu-change``, ``onu-act``, ``onu-drop``, ``onu-call``, ``onu-config.sh``
and ``ponid``. ``onu-act``, ``onu-drop`` and ``onu-call`` came with ADR 0014,
and ``dhcp-wan`` went with the bench's router mode. ``onu-utils/README.rst``
says what each is for.
