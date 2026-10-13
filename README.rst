.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

.. raw:: html

   <h1 align="center">open-dfp-34x-2c</h1>
   <p align="center">
     Open firmware for the ODI DFP-34X-2C GPON stick, built from source.
   </p>
   <p align="center">
     <img alt="Status: test builds"
          src="https://img.shields.io/badge/status-test%20builds-orange">
     <img alt="Licence: GPL-2.0-only"
          src="https://img.shields.io/badge/licence-GPL--2.0--only-blue">
   </p>
   <p align="center">
     <a href="#what-it-does">What it does</a> ·
     <a href="#status">Status</a> ·
     <a href="#before-you-install">Before you install</a> ·
     <a href="#where-to-start">Where to start</a> ·
     <a href="#building">Building</a> ·
     <a href="#contributing">Contributing</a>
   </p>

The ODI DFP-34X-2C is a GPON ONU in an SFP module. It plugs into the SFP cage
of a router or a switch and takes the place of the operator's ONT on a fibre
line. This project gives it an open system, built from source, in place of
the firmware it ships with: a Linux kernel with drivers for the stick's
Realtek RTL9601D chip, an OMCI daemon that talks to the OLT, and a small set
of programs that set the stick up, update it and show its state.

The DFP-34X-2C2 and the DFP-34X-2C3 are this same stick, the 2C2 with a UPC
optical connector and the 2C3 with an APC one. One image is for both.

::

     your router                     the stick                     the operator
    ┌────────────┐               ┌────────────────┐               ┌────────────┐
    │            │   host lane   │   DFP-34X-2C   │     fibre     │            │
    │  SFP cage  ├───────────────┤    running     ├───────────────┤    OLT     │
    │            │ 2.5 or 1 Gb/s │  this system   │      GPON     │            │
    └────────────┘               └────────────────┘               └────────────┘

..

    ⚠️ **Test builds, for development only.** The system works on a lab OLT,
    but it needs more testing before everyday use. Read `Status`_ and
    `Before you install`_ before you put it on a stick.

What it does
============

* **Connects to a GPON line.** It brings the link up (ITU-T G.984), answers
  the OLT over OMCI (ITU-T G.988), and carries the traffic the OLT sets up,
  with its GEM ports, VLAN rules and multicast, in the switch's hardware.
* **Can hold PPPoE itself.** With PPPoE accounts in its settings it holds up
  to seven sessions and hands each session's IPv4 address and IPv6 prefix
  down to the router behind it. The rest of what the OLT sets up, such as
  television and telephone, stays bridged.
* **2.5 or 1 Gb/s to the router**, found by itself: 2500BASE-X, or 1000BASE-X
  with or without auto-negotiation.
* **Is managed from the router's side**: ssh, and a web panel with an API
  over HTTPS, on the stick's addresses on that side only.
* **Updates safely.** An update goes into the other flash bank and starts on
  trial; the stick keeps it once it has come up, and goes back to the old
  one otherwise. A change of the settings is tried the same way and undone
  after 90 seconds unless the panel confirms it.

How the parts fit together is in `docs/handbook/system.rst
<docs/handbook/system.rst>`__.

Status
======

**It works on a lab OLT, and it needs more testing.** The system runs on a
DFP-34X-2C3 against a laboratory OLT, where 24 runs of the device tests are
recorded (`docs/testing/ <docs/testing/README.rst>`__). The latest, of
2026-10-12, is of the image as it is now.

| 🟢 **Tested**: passed the recorded device tests on the lab OLT.
| 🟡 **Bench**: works on the developer's stick; no recorded test run yet.
| ⚪ **Not on a stick yet**: tested on a build machine only, or not tried.

.. list-table::
   :header-rows: 1
   :widths: 42 20 38

   * - Feature
     - Status
     - Notes
   * - GPON link up to operation (O5), OMCI, MIB upload, alarms, optics
       diagnostics
     - 🟢 Tested
     -
   * - The OLT's bridge: GEM ports, T-CONTs, VLAN translation, push and pop,
       priority
     - 🟢 Tested
     - GEM ports and T-CONTs on the current image. The translation, push and
       pop and priority cases send frames from a bench's ports and were last
       recorded on 2026-10-07, on an earlier image
   * - IGMP snooping, ACL, port mirroring, shaping, storm control
     - 🟢 Tested
     - Recorded on 2026-10-07, on an earlier image; the current image ran
       the kernel's selftests of the devlink parameters and the tc offload,
       not these cases
   * - The host lane, 2.5 or 1 Gb/s, found by itself
     - 🟢 Tested
     - 2500BASE-X, 1000BASE-X with and without auto-negotiation (2026-10-07);
       the current image's link came up at 2500 Mb/s
   * - The management VLAN kept off the PON
     - 🟢 Tested
     - Both ways, also after a PPPoE session (2026-10-07, an earlier
       image); the run on the current image reaches the stick through it
   * - Update into the other bank, start on trial, keep it
     - 🟢 Tested
     - Into both banks over ssh, and back, on the current image; from the
       panel on 2026-10-07
   * - Back to the old bank after a failed start
     - 🟡 Bench
     - Measured: a kernel panic, a damaged kernel, power lost during a write
   * - PPPoE on the stick, with the address and IPv6 prefix handed to the
       router
     - 🟢 Tested
     - Two sessions at once. 911 Mb/s up and 920 down together (TCP, measured
       by hand; ``docs/handbook/system.rst``)
   * - Programs started again when they stop
     - 🟢 Tested
     - Each of 14 programs killed with SIGKILL: all back within 6 s, the
       sessions in 5 s
   * - A change of the settings tried, and undone without a confirmation
     - 🟡 Bench
     - By hand; no device test yet
   * - The web panel and its API over HTTPS
     - 🟢 Tested
     - The API and the delivery of the page. What the panel shows in a
       browser is checked by hand
   * - Settings kept beside the shipped firmware's files
     - 🟢 Tested
     - A save leaves the shipped firmware's 13 files as they were. Eight
       builds of the shipped firmware, started from the other bank, found
       their own files and left this system's alone (by hand)
   * - Install through the shipped firmware's web page
     - ⚪ Build machine
     - No install on a real stick recorded yet

..

    ℹ️ **The latest run is of the current image, without the data path.**
    On 2026-10-12 32 of 48 cases passed on the image as it is now, run from
    the flash, and none failed. The 16 it skipped send frames from the ports
    of a bench, one behind the OLT and one at the far end of the stick's host
    link, and the lab as it is has no such ports. They were last recorded on
    2026-10-07, on an image of before procd, netifd, the panel over HTTPS and
    the new store of the settings, and have not run on this one
    (`tests/device/README.rst <tests/device/README.rst>`__).

Not done yet
------------

* **No release.** You build the images yourself (`Building`_).
* **Only one laser-driver IC is known.** A DFP-34X-2C may have been made
  with one of several laser-driver ICs. The optics support is written for the
  one in the developer's stick, which the kernel's log calls GN25L95; that
  name has not been checked against the chip's marking. The driver tells a
  stick by the name and part number in its header, not by the IC in it, so a
  stick that carries another IC under that header is taken for this one. No
  other IC has been tried.
* If the store of the settings cannot be read, root's password is not known:
  the panel refuses every sign-in, ssh takes keys only, and only the serial
  console, inside the stick, takes ``admin``.
* The stick has no packet filter. Its servers listen only on its addresses
  on the router's side; whether a session's peer can reach them has not
  been tested.
* An update file carries a checksum against damage, but no signature
  (`ADR 0017 <docs/adr/0017-update-files-carry-no-signature.rst>`__).
* The other known gaps are kept as the repository's issues.

Other sticks
============

Only the ODI DFP-34X-2C has run this system so far. Other sticks built on the
same chip are not supported yet, but most of this repository is not tied to
one stick: the chip's kernel support and drivers, omcid, the stick's programs,
the panel and the build are shared. A new stick needs mainly its own board
files:

* a device tree on top of the chip's, `rtl9602c.dtsi
  <linux/arch/mips/boot/dts/realtek/rtl9602c.dtsi>`__, with its memory and
  its flash partitions;
* support for its laser driver, if it has another one (`the optics
  <linux/Documentation/networking/device_drivers/pon/rtl9602c-optics.rst>`__);
* a Buildroot configuration and a board directory in `br2-external/
  <br2-external/README.rst>`__;
* what its boot loader expects: how it picks a bank, and where the shipped
  firmware keeps the unit's identity (`boot-contract.rst
  <docs/handbook/boot-contract.rst>`__ says how this stick does it).

If you have such a stick and want to bring it up, open an issue first:
ports are welcome.

Before you install
==================

* **Two ways in.** Through the shipped firmware's web page, with the package
  ``web-install.tar`` that the build makes, which needs no console; this way
  is tested on a build machine only, and no install on a real stick is
  recorded yet (`updating.rst <docs/handbook/updating.rst>`__). Or from the
  bench, which writes the flash from the boot loader's prompt over the
  serial console (`flashing.rst <docs/handbook/flashing.rst>`__). Either way
  the shipped firmware stays in the other bank, as the way back.
* **The first update overwrites the shipped firmware.** The panel names the
  bank that holds it and asks before it writes over it; ``onu-update write``
  on the command line does not ask. Copy the whole flash first
  (`backup.rst <docs/handbook/backup.rst>`__), and keep the copy private: it
  holds the unit's registration identity (`updating.rst
  <docs/handbook/updating.rst>`__, "The first update").
* **Builds from before 2026-10-11 erased the shipped firmware's files** in
  the partition ``config``. The system now keeps its settings in a file of
  its own beside them, and does not write theirs.
* **The password is** ``admin`` **until you set one.** There is one account,
  root, for the console, ssh and the panel. The panel allows nothing else
  until a password is set; the console and ssh only warn
  (`ADR 0012 <docs/adr/0012-one-account-root.rst>`__).
* **The panel's certificate is signed by the stick itself**, so a browser
  warns at the first visit. The console and the panel show its fingerprint
  to compare (`ADR 0016 <docs/adr/0016-the-panel-over-https.rst>`__).
* **Install only an update file you built or trust**: nothing checks who
  made it (`ADR 0017 <docs/adr/0017-update-files-carry-no-signature.rst>`__).
* **Try it on a lab OLT first.** A stick on a PON shares the fibre with every
  other ONU of its tree (`optical-safety.rst
  <docs/handbook/optical-safety.rst>`__).
* **The laser driver is known for one IC only** (`Not done yet`_). The
  driver writes the laser driver only in a module whose header reads ``ODI``
  and ``DFP-34X-2C``, and only reads any other. It cannot tell which IC such
  a stick carries. In one, every start of the PON link writes registers of
  the IC's configuration table: the automatic extinction-ratio loop goes
  off, and when the OLT asks for a lower transmit power the modulation and
  the power set point move. The driver writes no byte of the table unless it
  has read that the IC keeps register changes out of its EEPROM, and the
  image never writes the table's password bytes (only a test build's store
  entry does). The one byte the module keeps, the page select, is written
  only when it holds another page, and put back as found. What those
  registers do in another IC is not known, and neither is whether its optics
  would work or stay dark when they should (`optical-safety.rst
  <docs/handbook/optical-safety.rst>`__, "What the system does not write").

Where to start
==============

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - I want to
     - Read
   * - Try it on a stick
     - `board.rst <docs/handbook/board.rst>`__ for the stick and its
       interfaces, then `building.rst <docs/handbook/building.rst>`__,
       `updating.rst <docs/handbook/updating.rst>`__, and for a stick on a
       bench `flashing.rst <docs/handbook/flashing.rst>`__
   * - Understand how it works
     - `system.rst <docs/handbook/system.rst>`__: the parts, the start, who
       owns what; then the decision records in
       `docs/adr/ <docs/adr/README.rst>`__
   * - Work on the kernel
     - "The kernel tree" in `CONTRIBUTING.rst <CONTRIBUTING.rst>`__, the
       `SoC's page <linux/Documentation/arch/mips/rtl9602c.rst>`__ and the
       driver pages under `Documentation`_
   * - Work on the programs
     - The README of each part (`Repository map`_), and
       `toolchain.rst <docs/handbook/toolchain.rst>`__ for what a program for
       this core must not do
   * - Contribute
     - `Contributing`_

What is in this repository
==========================

Everything needed to build the system and put it on a stick:

* **the Linux kernel**, with support for the chip: its MIPS core, the
  switch, the GPON MAC, the optics and the watchdog. The kernel and the code
  call the chip by its family's name, RTL9602C;
* **the programs that run on the stick**: the OMCI daemon, the PPPoE client,
  the settings, the updates and the web panel;
* **the build**: a Buildroot tree that makes the toolchain, the images and
  the update files;
* **the tools**: the bench for the serial console and the flash, and the
  checks that every change goes through;
* **the documentation**: the hardware, the system, and why it is built the
  way it is.

Each part keeps the layout of its upstream project: the kernel tree is laid
out like Linux, the build like Buildroot. So each can be read, reused or sent
upstream on its own (`ADR 0003 <docs/adr/0003-structure-and-scope.rst>`__);
the kernel series aims at mainline Linux (`ADR 0018
<docs/adr/0018-the-kernel-series-aims-at-mainline.rst>`__).

Repository map
--------------

.. list-table::
   :header-rows: 1
   :widths: 24 76

   * - Directory
     - What it holds
   * - `linux/ <linux/>`__
     - Linux 6.18.48 with this project's kernel series on top: the SoC and
       its Lexra core, the board's device tree and configuration, and the
       drivers of the switch, its CPU port, the GPON and PON MACs, the
       optics, the watchdog and the PON class
   * - `omcid/ <omcid/README.rst>`__
     - The OMCI daemon (ITU-T G.988): it answers the OLT and sets up the data
       path as the OLT asks
   * - `onu-utils/ <onu-utils/README.rst>`__
     - The stick's own programs: its settings and their store, the generator
       that applies them, its identity, its banks and updates, and the
       helpers of the start
   * - `pond/ <pond/README.rst>`__
     - The status daemon and its web panel, over HTTPS
   * - `pppoed/ <pppoed/README.rst>`__
     - The PPPoE client, one for each session
   * - `dhcp6d/ <dhcp6d/README.rst>`__
     - Hands a PPPoE session's IPv6 prefix on to the router behind the stick
   * - `netifd/ <netifd/README.rst>`__
     - OpenWrt's network interface daemon, carried as a fork
   * - `br2-external/ <br2-external/README.rst>`__
     - The distribution: a Buildroot external tree with the board's
       configuration, the packages, the root filesystem's files and the
       scripts that make the images
   * - `buildroot/ <buildroot/README.rst>`__
     - The pinned Buildroot release and the patches carried against it
   * - `board/ <board/odi/dfp-34x-2c3/readme.txt>`__
     - The board's readme, in Buildroot's form
   * - `tools/ <tools/README.rst>`__
     - Host tools: the bench, the provenance gates, the instruction-set
       gate, the size check of the images and the cut of their libraries
   * - `tests/device/ <tests/device/README.rst>`__
     - The tests that run on the stick
   * - `docs/ <docs/README.rst>`__
     - The handbook, the decision records, the provenance policy and the
       records of the device tests
   * - `.github/ <.github/workflows/README.rst>`__
     - The workflows of continuous integration and the gates they run

Documentation
=============

**The handbook**, `docs/handbook/ <docs/handbook/README.rst>`__:

* `board.rst <docs/handbook/board.rst>`__: the stick and its interfaces;
* `system.rst <docs/handbook/system.rst>`__: how the system works;
* `boot-contract.rst <docs/handbook/boot-contract.rst>`__: the flash layout,
  the image formats, how a bank is chosen, and the watchdog;
* `building.rst <docs/handbook/building.rst>`__: the images, their budgets,
  the boot script, and how to build, check and reproduce them;
* `updating.rst <docs/handbook/updating.rst>`__: updates from the panel or a
  shell, and the way in through the shipped firmware's web page;
* `backup.rst <docs/handbook/backup.rst>`__: a copy of the whole flash,
  taken before the first update, and how to write it back;
* `recovery.rst <docs/handbook/recovery.rst>`__: recovery over the serial
  console in one place;
* `optical-safety.rst <docs/handbook/optical-safety.rst>`__: the laser, the
  other subscribers of a PON, and rogue ONUs;
* `flashing.rst <docs/handbook/flashing.rst>`__: the bench, RAM boots,
  writing the flash, trial starts and recovery;
* `toolchain.rst <docs/handbook/toolchain.rst>`__: the cross toolchain and
  the instruction-set gate;
* `panel-design.rst <docs/handbook/panel-design.rst>`__: the panel's look.

**The kernel**, under ``linux/Documentation/``:

* `arch/mips/rtl9602c.rst <linux/Documentation/arch/mips/rtl9602c.rst>`__:
  the system-on-chip, its core and the board;
* `networking/pon.rst <linux/Documentation/networking/pon.rst>`__: the PON
  class;
* `networking/dsa/rtl9602c.rst <linux/Documentation/networking/dsa/rtl9602c.rst>`__:
  the switch, with its pages on
  `bridging <linux/Documentation/networking/dsa/rtl9602c-bridging.rst>`__,
  `ACL and QoS <linux/Documentation/networking/dsa/rtl9602c-acl-qos.rst>`__,
  `the L3/NAT engine <linux/Documentation/networking/dsa/rtl9602c-l3-nat.rst>`__
  and `PTP <linux/Documentation/networking/dsa/rtl9602c-ptp.rst>`__;
* the `PON MAC <linux/Documentation/networking/device_drivers/pon/rtl9602c.rst>`__,
  the `GPON MAC <linux/Documentation/networking/device_drivers/pon/rtl9602c-gpon.rst>`__
  and the `optics <linux/Documentation/networking/device_drivers/pon/rtl9602c-optics.rst>`__;
* `the CPU port <linux/Documentation/networking/device_drivers/ethernet/realtek/rtl9602c.rst>`__
  and `devlink <linux/Documentation/networking/devlink/rtl9602c.rst>`__;
* the device-tree bindings under ``linux/Documentation/devicetree/bindings/``.

**Decisions, policy and test records**, in ``docs/``:

* `adr/ <docs/adr/README.rst>`__: the architecture decision records, why the
  system is built as it is;
* `policy/provenance.rst <docs/policy/provenance.rst>`__: what may and may
  not enter the repository;
* `testing/ <docs/testing/README.rst>`__: one page per run of the device
  tests.

Building
========

The top-level ``Makefile`` holds the entry points. You need:

* for ``make`` and ``make sdk``, which run Buildroot: Linux (or the container
  that `building.rst <docs/handbook/building.rst>`__ names), the packages
  that page lists, some 9 GB of disk, and the network, to fetch the sources
  of the packages, which are checked by hash;
* for ``make linux`` alone: a MIPS cross compiler (``mips-linux-gnu-gcc`` on
  Debian, or name yours with ``CROSS_COMPILE=``) and ``mkimage``;
* for ``make buildroot``, ``make ramboot`` and ``make flash``: Python 3.11 or
  later. The last two drive the bench in ``tools/bench``, whose README
  describes its settings, its commands and the bank guard.

::

    make               # k0.uImage, r0.squashfs, update.bin and web-install.tar
                       # in build/image/images, then make image-check
    make image-check   # the sizes, the instruction-set gate and the digests
    make ram-image     # after make: build/image/images/ram.uImage, for ramboot
    make linux         # the kernel alone (build/linux/arch/mips/boot/uImage)
    make buildroot     # fetch the pinned Buildroot and patch it (build/buildroot)
    make sdk           # a relocatable cross toolchain (build/toolchain/images)
    make legal-info    # the licences of a built image (build/image/legal-info)
    make check         # the offline gates and the tools' tests
    make omcid         # after make: the OMCI daemon again, into the image
    make docs          # the pages as HTML in build/docs (needs docutils)
    make ramboot       # start a kernel from RAM over the bench
    make flash IMAGE=F TARGET=kernel|rootfs   # write bank 0 over the bench
    make clean         # remove the build products; distclean all of build/

The programs of this tree build and test on their own on the host, with
``make -C <program> check``.

Contributing
============

Fixes, tests, test reports from other OLTs and ports to other sticks are all
welcome. Read `CONTRIBUTING.rst <CONTRIBUTING.rst>`__ first: a commit touches
one part of the tree, carries a ``Signed-off-by:`` line, and passes the
provenance gates (`docs/policy/provenance.rst
<docs/policy/provenance.rst>`__), which keep out anything that is not ours
to publish. ``make check`` runs the gates and the tools' tests offline.

Licensing
=========

* Everything written for this project is ``GPL-2.0-only``; see ``COPYING``
  and ``LICENSES/``.
* The third-party code committed here keeps its licence and says so: Linux,
  in ``linux/``; OpenWrt's netifd, in ``netifd/``, under ``GPL-2.0-only``;
  and Mongoose, in ``pond/third_party``, taken under ``GPL-2.0-only``, which
  holds micro-ecc under ``BSD-2-Clause``. ``REUSE.toml`` names them.
* The packages of the image are not committed: they are fetched by hash at
  build time and keep their own licences. ``make legal-info`` collects the
  licence of every package of the image, their texts and the sources a
  release hands on, into ``build/image/legal-info``. The image carries the
  licences of the panel's packages beside the panel
  (``/usr/share/pond/www/licenses.txt``).
* Contributions follow the Developer Certificate of Origin and the
  provenance policy.

Trademarks and affiliation
==========================

"Realtek", "RTL9601D" and "RTL9602C" are names of Realtek Semiconductor Corp.
"ODI" and "DFP-34X" are names of the stick's manufacturer. This project is not
affiliated with, authorised by, or endorsed by Realtek Semiconductor Corp. or
by that manufacturer. Other product and company names that appear here may be
the trademarks of their respective owners. No third-party logos are used or
distributed.
