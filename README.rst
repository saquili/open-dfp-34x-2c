.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===============
open-dfp-34x-2c
===============

A from-source Linux distribution for one product: the ODI DFP-34X-2C2 and
DFP-34X-2C3 GPON SFP ONU sticks, built on the Realtek RTL9601D system-on-chip
(the kernel and the code call the chip's family RTL9602C). The stick plugs into
the SFP cage of a router or a switch and takes the place of an operator's ONT
on a GPON line. Other products built on the same chip are not supported.

**Test builds, for development only.** The system runs, but it is a base to
develop on, not firmware for everyday use. Read `Status`_ and
`Before you install`_ before you put it on a stick.

.. contents:: On this page
   :local:
   :depth: 1

Scope
=====

This repository holds what a device maker ships to build the product from
source:

* the cross toolchain (through the distribution build system);
* the Linux kernel, with the board and SoC support this device needs;
* the userspace: the OMCI daemon, the stick's own programs, and the panel;
* the image and flash tooling; and
* the hardware documentation.

The **structure** follows mainline Linux conventions -- each part is laid out
the way its own upstream community lays it out, so it stays recognisable and
liftable. The **scope** is the whole product, not one kernel tree. See
``docs/adr/`` for the reasoning and ``docs/policy/provenance.rst`` for what may
and may not enter.

Status
======

The system runs on the bench's DFP-34X-2C3 against a laboratory OLT. It has
also run on one operator's line, where no test is recorded.

What it does
------------

* It brings the GPON link up to operation (ITU-T G.984.3, O5), answers the
  OLT's OMCI (ITU-T G.988), and carries what the OLT provisions as a bridge
  in the switch's hardware: VLAN rules, the GEM ports, multicast snooping.
* With PPPoE accounts in its settings, it holds up to seven PPPoE sessions
  itself and hands a session's IPv4 address, and its delegated IPv6 prefix,
  down to the router behind it, while the OLT's other services stay bridged.
* It finds the host's lane: 2500BASE-X, or 1000BASE-X with or without in-band
  negotiation.
* It is managed over ssh and over a panel and API on HTTPS, on addresses of
  the host side only, one of them on a VLAN that the switch keeps off the PON
  both ways; the OLT manages the data path over OMCI.
* It updates itself into its other flash bank and keeps the new system only
  once it has come up; it starts again from the old one otherwise. A change of
  the settings is applied on trial and undone unless the panel confirms it.
* procd starts every program again when it ends; netifd holds the interfaces
  of the host side and the sessions; one table holds the settings and their
  rules (``docs/handbook/system.rst``).

How each part is known
----------------------

**Recorded** means a run of the device tests that ``docs/testing/`` keeps;
**bench** means tried on the bench's stick as a decision record says, with no
recorded run; **host** means tested on a build host only.

.. list-table::
   :header-rows: 1
   :widths: 46 18 36

   * - What
     - Known by
     - Where
   * - The GPON link to O5, the OMCI channel, the MIB upload, the OLT's
       bridge with its VLAN rules, ACL, mirroring, shaping, storm control and
       multicast snooping
     - Recorded
     - 23 runs, the latest on 2026-10-07
   * - The host lane's search of 2500BASE-X and 1000BASE-X
     - Recorded
     - ``net.host-link-search``, 2026-10-07
   * - A start from the flash, after the system wrote itself into both banks
       with its own update path and kept the tried bank
     - Recorded
     - The run of 2026-10-07
   * - Two PPPoE sessions at once, their throughput (some 900 Mb/s each way),
       an IPv6 prefix handed to the host, each program killed during a session
     - Bench
     - ADR 0014
   * - procd's supervision; omcid going on from its MIB after a start again
     - Bench
     - ADR 0013, ADR 0014
   * - The panel over HTTPS, and the one account, root
     - Bench and host
     - ADR 0016, ADR 0012
   * - The settings kept in the shipped firmware's partition ``config``,
       beside its files; eight packages of the shipped firmware started from
       the other bank, finding their files and leaving this system's alone
     - Bench
     - ADR 0014, step 9
   * - The way in through the shipped firmware's web page
     - Host
     - ``docs/handbook/updating.rst``

**The image that main builds today has no recorded run.** Every record is of
2026-10-07 or before, of an image whose programs were linked statically, before
procd, netifd, the panel's HTTPS and the store in ``config``. Some cases of the
suite still expect that image (``tests/device/README.rst``); a recorded run on
the current image is the next test.

What is not done, or not tried
------------------------------

* Only the bench's DFP-34X-2C3 has run it. The DFP-34X-2C2 has not been tried,
  and the build makes images for the DFP-34X-2C3 only.
* There is no release: the images are built from source (`Building`_).
* A change of the settings takes every PPPoE session down and up again, not
  only the one that changed.
* If the configuration store cannot be read, root's password is not known:
  the panel refuses every sign-in, ssh takes keys only, and only the serial
  console, inside the stick, takes ``admin``.
* The stick has no packet filter: its servers keep off the PON side by the
  addresses they listen on. Whether they can be reached from a session's peer
  has not been tested.
* The update file carries a checksum against damage and no signature.
* The handbook has no page yet on copying the whole flash and writing it back,
  on recovery over the serial console in one place, or on optical safety.
* The other known gaps of the code are kept as the repository's issues.

Before you install
==================

* A stick as shipped takes this system in one of two ways:

  - through its shipped firmware's web page, with the package
    ``web-install.tar`` that the build makes, which needs no console. This way
    has tests on a build host only: an installation on a real stick has not
    been recorded yet (``docs/handbook/updating.rst``);
  - through the bench, which writes the flash from the boot loader's prompt,
    over the console and the SFP lane (``docs/handbook/flashing.rst``).

  Either way the shipped firmware stays in the other bank, as the way back.
* **The first update from the running system overwrites the shipped
  firmware.** The panel names the bank that holds it and asks before it
  writes over it; ``onu-update write`` does not ask. Keep a copy of the
  whole flash before it. The copy holds the unit's
  registration identity: keep it private (``docs/handbook/updating.rst``, "The
  first update").
* This system keeps its settings in a file of its own in the partition
  ``config``, beside the shipped firmware's files, which it does not write. A
  stick that ran a build of before 2026-10-11 had that partition taken whole
  by the store of then, and the shipped firmware's files there are gone.
* There is one account, root, whose password is ``admin`` until one is set.
  The panel allows nothing but setting one; the console and ssh only warn
  (``docs/adr/0012-one-account-root.rst``).
* The panel's certificate is signed by the stick itself, and a browser warns
  at the first visit; the console and the panel show its fingerprint to compare
  (``docs/adr/0016-the-panel-over-https.rst``).
* Install only an update file you built or trust: nothing checks who made it.
* A stick on a PON shares the fibre with every ONU of its tree. Try a build on a
  laboratory OLT before an operator's line.

Where to start
==============

To try it on a stick
    `docs/handbook/board.rst <docs/handbook/board.rst>`__ for the stick and its
    interfaces, then `building.rst <docs/handbook/building.rst>`__,
    `updating.rst <docs/handbook/updating.rst>`__ and, for a stick on a bench,
    `flashing.rst <docs/handbook/flashing.rst>`__.
To understand the system
    `docs/handbook/system.rst <docs/handbook/system.rst>`__: the parts, the
    start, who owns what, and how a change of the settings reaches the
    hardware; then the decision records in `docs/adr/ <docs/adr/README.rst>`__.
To work on the kernel
    "The kernel tree" in `CONTRIBUTING.rst <CONTRIBUTING.rst>`__, then
    `the SoC's page <linux/Documentation/arch/mips/rtl9602c.rst>`__ and the
    driver pages under `Documentation`_.
To work on the programs
    The README of the component (`Repository map`_), and
    `docs/handbook/toolchain.rst <docs/handbook/toolchain.rst>`__ for what a
    program for this core must not do.
To contribute
    `CONTRIBUTING.rst <CONTRIBUTING.rst>`__ and
    `docs/policy/provenance.rst <docs/policy/provenance.rst>`__: what may enter
    the repository, how a commit is written, and how to test a change.

Repository map
==============

`linux/ <linux/>`__
    Linux 6.18.48 with the kernel series of this project on top: the SoC and its
    Lexra core, the board's device tree and configuration, and the drivers of
    the switch, its CPU port, the GPON and PON MACs, the optics, the watchdog
    and the PON class.
`omcid/ <omcid/README.rst>`__
    The OMCI daemon (ITU-T G.988): it answers the OLT and programs the data path
    from what the OLT configures.
`onu-utils/ <onu-utils/README.rst>`__
    The stick's own programs: its settings and their store, the generator that
    applies them, its identity, its banks and updates, and the helpers of the
    boot.
`pond/ <pond/README.rst>`__
    The status daemon and its panel, over HTTPS: the state of the stick, its
    settings and its updates.
`pppoed/ <pppoed/README.rst>`__
    The PPPoE client, which netifd runs for each session.
`dhcp6d/ <dhcp6d/README.rst>`__
    The IPv6 prefix of a PPPoE session, delegated to the host behind the stick.
`netifd/ <netifd/README.rst>`__
    The network interface daemon of OpenWrt, carried as a fork.
`br2-external/ <br2-external/README.rst>`__
    The distribution: a Buildroot external tree with the board's configuration,
    the packages (procd, ubus and the programs above among them), the root
    filesystem's files and the scripts that make the images.
`buildroot/ <buildroot/README.rst>`__
    The pin of the build system and the patches carried against it.
`board/ <board/odi/dfp-34x-2c3/readme.txt>`__
    The board's readme, in Buildroot's form: what the stick is, and how its
    images are built and started.
`tools/ <tools/README.rst>`__
    Host tools: the bench for RAM boots and flashing, the provenance gates, the
    instruction-set gate, the size check of the images and the cut of their
    libraries.
`tests/device/ <tests/device/README.rst>`__
    The tests that run on the stick.
`docs/ <docs/README.rst>`__
    The handbook, the architecture decision records, the provenance policy and
    the records of the device tests.
`.github/ <.github/workflows/README.rst>`__
    The continuous-integration workflows and the gates they run.

Documentation
=============

The handbook, `docs/handbook/ <docs/handbook/README.rst>`__:

* `board.rst <docs/handbook/board.rst>`__ -- the stick and its interfaces;
* `system.rst <docs/handbook/system.rst>`__ -- how the system works;
* `boot-contract.rst <docs/handbook/boot-contract.rst>`__ -- the flash layout,
  the image formats, how a bank is chosen and the watchdog;
* `building.rst <docs/handbook/building.rst>`__ -- the images, their budgets,
  the boot script, and how to build, check and reproduce them;
* `updating.rst <docs/handbook/updating.rst>`__ -- updates into the other bank,
  from the panel or a shell, and the way in through the shipped firmware's
  web page;
* `flashing.rst <docs/handbook/flashing.rst>`__ -- the bench: RAM boots,
  writing the flash, trial starts and recovery;
* `toolchain.rst <docs/handbook/toolchain.rst>`__ -- the cross toolchain and the
  instruction-set gate;
* `panel-design.rst <docs/handbook/panel-design.rst>`__ -- the panel's look.

The kernel's pages, under ``linux/Documentation/``:

* `arch/mips/rtl9602c.rst <linux/Documentation/arch/mips/rtl9602c.rst>`__ --
  the system-on-chip, its core and the board;
* `networking/pon.rst <linux/Documentation/networking/pon.rst>`__ -- the PON
  class;
* `networking/dsa/rtl9602c.rst <linux/Documentation/networking/dsa/rtl9602c.rst>`__
  -- the switch, with its pages on
  `bridging <linux/Documentation/networking/dsa/rtl9602c-bridging.rst>`__,
  `ACL and QoS <linux/Documentation/networking/dsa/rtl9602c-acl-qos.rst>`__,
  `the L3/NAT engine <linux/Documentation/networking/dsa/rtl9602c-l3-nat.rst>`__
  and `PTP <linux/Documentation/networking/dsa/rtl9602c-ptp.rst>`__;
* `networking/device_drivers/pon/ <linux/Documentation/networking/device_drivers/pon/index.rst>`__
  -- the `PON MAC <linux/Documentation/networking/device_drivers/pon/rtl9602c.rst>`__,
  the `GPON MAC <linux/Documentation/networking/device_drivers/pon/rtl9602c-gpon.rst>`__
  and the `optics <linux/Documentation/networking/device_drivers/pon/rtl9602c-optics.rst>`__;
* `the CPU port <linux/Documentation/networking/device_drivers/ethernet/realtek/rtl9602c.rst>`__
  and `devlink <linux/Documentation/networking/devlink/rtl9602c.rst>`__;
* the device-tree bindings under ``linux/Documentation/devicetree/bindings/``.

The rest of ``docs/``:

* `adr/ <docs/adr/README.rst>`__ -- the architecture decision records: why the
  repository, the build, the libraries, the accounts, the supervision, the
  control plane and the panel are as they are;
* `policy/provenance.rst <docs/policy/provenance.rst>`__ -- what may and may not
  enter the repository;
* `testing/ <docs/testing/README.rst>`__ -- one page per run of the device
  tests.

Building
========

The top-level ``Makefile`` holds the entry points. ``make`` and ``make sdk``
run Buildroot, which runs on Linux (on another system, in the container that
``docs/handbook/building.rst`` names); they need the packages that page names,
some 9 GB of disk, and the network, to fetch the sources of their packages,
which they check by hash. ``make linux`` needs a MIPS cross compiler
(``mips-linux-gnu-gcc`` on Debian, or name yours with ``CROSS_COMPILE=``) and
``mkimage``. ``make buildroot``, ``make ramboot`` and ``make flash`` need
Python 3.11 or later; the last two drive the bench in ``tools/bench``, whose
README describes the settings, the commands and the bank guard::

    make               # k0.uImage, r0.squashfs, update.bin and web-install.tar
                       # in build/image/images, then make image-check
    make image-check   # the sizes, the instruction-set gate and the digests
    make ram-image     # after make: build/image/images/ram.uImage, for ramboot
    make linux         # the kernel alone (build/linux/arch/mips/boot/uImage)
    make buildroot     # fetch the pinned Buildroot and patch it (build/buildroot)
    make sdk           # a relocatable cross toolchain (build/toolchain/images)
    make legal-info    # the licences of a built image (build/image/legal-info)
    make check         # the offline gates and the tools' tests
    make ramboot       # start a kernel from RAM over the bench
    make flash IMAGE=F TARGET=kernel|rootfs   # write bank 0 over the bench

``make omcid``, ``make docs``, ``make clean`` and ``make distclean`` are not
implemented yet; each says what to do instead. The programs of this tree build
and test on their own on the host, with ``make -C <program> check``
(``CONTRIBUTING.rst``).

Licensing
=========

Everything authored in this repository is ``GPL-2.0-only``; see ``COPYING`` and
``LICENSES/``. The third-party code committed here keeps its licence and says
so: the Linux release in ``linux/``; OpenWrt's netifd, in ``netifd/``, under
``GPL-2.0-only``; and Mongoose, in ``pond/third_party``, taken under
``GPL-2.0-only``, which holds micro-ecc under ``BSD-2-Clause``. ``REUSE.toml``
names them. Third-party packages are not committed: they are fetched by hash
at build time and keep their own licences. ``make legal-info`` collects the
licence of every package of the image, their texts and the sources a release
hands on, into ``build/image/legal-info``, and the image carries the licences
of the panel's packages beside the panel
(``/usr/share/pond/www/licenses.txt``). Contributions follow the Developer
Certificate of Origin and the provenance policy; see ``CONTRIBUTING.rst`` and
``docs/policy/provenance.rst``.

Trademarks and affiliation
==========================

"Realtek", "RTL9601D" and "RTL9602C" are names of Realtek Semiconductor Corp.
"ODI" and "DFP-34X" are names of the stick's manufacturer. This project is not
affiliated with, authorised by, or endorsed by Realtek Semiconductor Corp. or
by that manufacturer. Other product and company names that appear here may be
the trademarks of their respective owners. No third-party logos are used or
distributed.
