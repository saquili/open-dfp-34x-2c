.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===============================================
ADR 0017: Leave iproute2 out of the flash image
===============================================

:Status: Accepted
:Date: 2026-10-12
:Deciders: the maintainer

Summary
=======

The scripts ask the kernel for links, filters and switch rules through one
small program of ours instead of iproute2. The full ``ip``, ``tc`` and
``devlink`` are left out of the flash image and kept in the RAM image for
diagnosis.

Context
=======

netifd sets the stick's addresses and routes (ADR 0011). What the scripts
still need from the kernel, links, a qdisc, the switch's rules and devlink
parameters, is a few lines of netlink each, while iproute2, with ``ip``,
``tc`` and ``devlink``, costs over half a megabyte of a root file system that
has to fit its partition.

Decision
========

* **The flash image has no iproute2.** ``onu-nl`` of onu-utils, some 7 KB
  compressed, makes the requests the scripts need of ``tc`` and
  ``devlink``: the clsact qdisc, the switch's rules for the management VLAN,
  the feeds of the sessions' GEM ports, the removal of omcid's bridge rules,
  and the parameters of the switch and of the PON driver.
* **The RAM image keeps iproute2 for diagnosis**, with its ``ip`` installed
  as ``ip-full``, so that the scripts run BusyBox's ``ip`` there as they do
  from the flash.
* **The scripts run no ``ip``.** Their requests on links (the PON up and
  down; a session's GEM and VLAN interfaces created, removed and given their
  MTU and MAC address; and those of the boot script) are made with
  ``onu-nl link``. BusyBox's ``ip`` is built in for a person at the console,
  and is the first applet to go when the image is slimmed.

Consequences
============

* Leaving iproute2 out took the root file system from 1,825,101 to
  1,287,869 bytes (2026-10-11). ``docs/handbook/building.rst``, "Sizes and
  budgets", gives what the partition has left today.
* Every request a script makes of the kernel is in ``onu-nl``, in C, with
  unit tests of the messages it builds and of the program
  (``onu-utils/tests``).
* A person at the stick has BusyBox's ``ip`` and no ``tc`` or ``devlink``.
  The RAM image has all three, and ``devtest`` can put them on a stick that
  has none (``tests/device/README.rst``).
