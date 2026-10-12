.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=====================================================================
ADR 0020: The flash's image has no iproute2 and the scripts run no ip
=====================================================================

:Status: Accepted
:Date: 2026-10-12
:Decided-by: the owner, 2026-10-11 and 2026-10-12
:Amends: ADR 0014

.. note::

   In plain words: the tools the scripts used to ask the kernel for a link,
   a filter or a switch rule are replaced by one small program of ours that
   makes the same requests. The big ``ip``, ``tc`` and ``devlink`` are left
   out of the flash's image and kept in the image for the bench.

Context
=======

ADR 0014 said that most of the image's uses of ``ip`` go with
``onu-mode``'s and that iproute2 would leave the image in a step of its own.
What stayed was iproute2, with ``ip``, ``tc`` and ``devlink``, for requests
that a few lines of netlink make.

Decision
========

* **iproute2 leaves the flash's image (2026-10-11).** BusyBox's ``ip``, built
  in already, does what the scripts ask of ``ip``. ``onu-nl`` of onu-utils,
  some 7 KB compressed, makes the requests of ``tc`` and ``devlink`` that they
  made: the clsact qdisc, the switch's rules of the management VLAN, the
  feeds of the sessions' GEM ports, the removal of omcid's bridge rules, and
  the parameters of the switch and of the PON driver.
* **The RAM image keeps iproute2 for diagnosis**, its ``ip`` as ``ip-full``,
  so that the scripts run BusyBox's ``ip`` there as they do from the flash.
* **The scripts run no ``ip`` at all (2026-10-12).** What was left of it,
  fourteen requests on links (the PON up and down, a session's GEM and VLAN
  interfaces made, taken away and given their MTU and MAC address, and the
  boot script's three), is ``onu-nl link``, as the requests ``ip`` makes.
  BusyBox's ``ip`` stays built in for a person at the console, and is the
  first applet to go when the image is slimmed.

Consequences
============

* The root file system went from 1,825,101 to 1,287,869 bytes
  (2026-10-11). ``docs/handbook/building.rst``, "Sizes and budgets", has what
  the partition leaves today.
* A request a script makes of the kernel is in ``onu-nl``, in C, with unit
  tests of the messages it builds and of the program (``onu-utils/tests``).
* A person at the stick has BusyBox's ``ip`` and no ``tc`` or ``devlink``.
  The RAM image has all three, and ``devtest`` can put them on a stick that
  has none (``tests/device/README.rst``).
