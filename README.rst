.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

.. raw:: html

   <h1 align="center">open-dfp-34x-2c</h1>
   <p align="center">
     Open firmware for the ODI DFP-34X-2C GPON stick, built from source
     and written with LLMs.
   </p>
   <p align="center">
     <img alt="Status: test builds"
          src="https://img.shields.io/badge/status-test%20builds-orange">
     <img alt="Licence: GPL-2.0-only"
          src="https://img.shields.io/badge/licence-GPL--2.0--only-blue">
   </p>
   <p align="center">
     <b><a href="https://saquili.github.io/open-dfp-34x-2c/">Try the panel
     in your browser</a></b>: a demo with a mock stick, no stick needed, on
     this repository's GitHub Pages site
   </p>
   <p align="center">
     <a href="#what-it-does">What it does</a> ·
     <a href="#how-it-was-made">How it was made</a> ·
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
optical connector and the 2C3 with an APC one. One image is for both, but so
far it has run on the 2C3 only: on a 2C2 it has not been tried yet.

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
* **Can be the router itself.** Its own connections, PPPoE sessions and IPoE
  ones with an address from DHCP or typed in, seven together; up to eight
  networks on the host link, each with a DHCP server (512 fixed addresses in
  all), NAT behind a connection's address, 96 port forwards and a DMZ; and a
  firewall of 96 rules. The switch's engine carries the flows, and its ACL
  judges the firewall's rules (`ADR 0019
  <docs/adr/0019-the-firewalls-rules-are-the-switchs-acl.rst>`__).
* **2.5 or 1 Gb/s to the router**: 2500BASE-X or 1000BASE-X, each with or
  without auto-negotiation. It finds three of them by itself, all but
  2500BASE-X with auto-negotiation (1000BASE-X without it since a fix that
  came after the first release); any of the four can be set on the panel.
* **Is managed from the router's side**: ssh, and a web panel with an API
  over HTTPS, on VLAN 4094 of the host link, at the address the shipped
  firmware's page had. A demo of the panel, with a mock stick, runs in a
  browser on this repository's GitHub Pages site,
  `saquili.github.io/open-dfp-34x-2c
  <https://saquili.github.io/open-dfp-34x-2c/>`__. The panel calls this
  system BuchOS, the author's own name for it; another name can be set
  under System, Device, "Product name", and the panel's title and the
  names of the files it hands out follow it.
* **Keeps time from the host**: PTP with the switch's hardware timestamps,
  or NTP, or the OLT's time of day and the seconds of its frames; and it is
  the PTP master of the host link while it hears no better clock (`timed
  <timed/README.rst>`__).
* **Updates safely.** An update goes into the other flash bank and starts on
  trial; the stick keeps it once it has come up, and goes back to the old
  one otherwise. Until the first update the other bank keeps the shipped
  firmware, and the panel goes back to it with one button. A change of the
  settings is tried the same way and undone after 90 seconds unless the
  panel confirms it.

How the parts fit together is in `docs/handbook/system.rst
<docs/handbook/system.rst>`__.

How it was made
===============

This project was written using large language models (LLMs): the drivers, the
programs, the tests and the documentation. The commits say so in their
``Co-Authored-By:`` lines.

So that nothing here has to be taken on trust, the work is checked: the
kernel's coding style and checkpatch over every commit of the kernel series,
tests for each program on the host, device tests on the stick (`docs/testing/
<docs/testing/README.rst>`__), and every fact about the hardware written down
together with how it was measured (`docs/policy/provenance.rst
<docs/policy/provenance.rst>`__).

If you know the kernel, DSA, GPON or OMCI, please have a look: you will find
what the tests did not, and every review is welcome.

Status
======

**It works on a lab OLT, and it needs more testing.** The system runs on a
DFP-34X-2C3 against a laboratory OLT, where 24 runs of the device tests are
recorded (`docs/testing/ <docs/testing/README.rst>`__). The latest, of
2026-10-12, is of an earlier image: PTP and the stick's clock, the router, the
firewall, the laser driver of its own, the address on VLAN 4094, the first
start's import from the shipped firmware and the way back to it came after
it, and have been tried by hand. On 2026-10-20 the files of the first release
were installed on a stick through the shipped firmware's page, by hand
(`install-guide.rst <docs/handbook/install-guide.rst>`__).

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
     - 2500BASE-X, and 1000BASE-X with auto-negotiation (2026-10-07); the
       current image's link came up at 2500 Mb/s. 1000BASE-X without
       auto-negotiation was not found on 2026-10-20; the driver's fix found
       it 10.4 and 11.3 s after the router's change, and 2500BASE-X again
       12.7 to 14.1 s after the change back (by hand, a test image,
       2026-10-21). On sticks whose host identity page was changed (`Before
       you install`_)
   * - The host lane's mode set by hand: 2500BASE-X and 1000BASE-X, each
       with and without auto-negotiation
     - 🟡 Bench
     - Each of the four against a host in the same mode: the link up and
       traffic both ways (by hand, 2026-10-20)
   * - The management VLAN kept off the PON
     - 🟢 Tested
     - Both ways, also after a PPPoE session (2026-10-07, an earlier
       image); the run of 2026-10-12 reaches the stick through it. The panel
       on VLAN 4094 at the shipped firmware's address: by hand, 2026-10-20
   * - Update into the other bank, start on trial, keep it
     - 🟢 Tested
     - Into both banks over ssh, and back, on the image of 2026-10-12; from
       the panel on 2026-10-07. The first release's ``update.bin`` on
       2026-10-20, kept after its trial (by hand)
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
   * - The stick as the router: IPoE connections, local networks, DHCP,
       NAT, port forwards, DMZ
     - 🟡 Bench
     - On the developer's stick, an IPoE connection among them (2026-10-19);
       129 flows at once carried by the switch's engine, of the 4096 it can
       hold (`rtl9602c-l3-nat.rst
       <linux/Documentation/networking/dsa/rtl9602c-l3-nat.rst>`__); no
       recorded test run yet
   * - The firewall's rules in the switch
     - 🟡 Bench
     - Blocks, allows and their answers measured by hand on 2026-10-19
       (``docs/handbook/system.rst``, "The firewall, as measured")
   * - IPv6 routed to the local networks
     - ⚪ Not on a stick yet
     - The sessions' IPv6 is handed to the router behind the stick. The
       panel takes a network's IPv6 setting, but the stick does not apply it
       yet (``onu-apply``)
   * - The PTP hardware clock, its pulse once a second, its receive stamps
     - 🟢 Tested
     - ``system.phc``, 2026-10-12
   * - The stick's clock (``timed``): PTP with the switch's receive and
       transmit stamps, the PHC's servo and the system clock on the PHC; the
       OLT's time of day and seconds; PTP master on the host link; NTP
     - 🟡 Bench
     - By hand (`timed <timed/README.rst>`__): the PHC within 1 to 3 us of a
       master that stamps in software, the system clock within 1 us of the
       PHC, the PHC within 15 us of the OLT's frames over 8 minutes. Not yet
       against a master that stamps in hardware; two assumptions of the
       driver are open (`rtl9602c-ptp.rst
       <linux/Documentation/networking/dsa/rtl9602c-ptp.rst>`__, "Open
       questions")
   * - The laser driver as a driver of its own (GN25L95), with its hwmon
       device
     - 🟡 Bench
     - The release's image reached O5 with it on 2026-10-20; its
       diagnostics are not in a recorded run yet
   * - The web panel and its API over HTTPS
     - 🟢 Tested
     - The API and the delivery of the page. What the panel shows in a
       browser is checked by hand
   * - Settings kept beside the shipped firmware's files
     - 🟢 Tested
     - A save leaves the shipped firmware's 13 files as they were. Eight
       builds of the shipped firmware, started from the other bank, found
       their own files and left this system's alone (by hand)
   * - Install through the shipped firmware's web page, with the first
       start's import of the identity, the administrator's password, the LAN
       address and the MAC address
     - 🟡 Bench
     - On 2026-10-19, from two builds of the shipped firmware, one of them on a
       copy of a stick as sold; on 2026-10-20 with the files of the first
       release, which reached O5 in about 10 s, kept its bank and took the
       administrator's password (by hand, ``updating.rst``)
   * - The panel's way back to the shipped firmware
     - 🟡 Bench
     - On 2026-10-20 with the first release: the shipped firmware started
       with its own settings, its page at 192.168.1.1 without a VLAN tag
       (by hand)
   * - The panel's copy of the whole flash
     - ⚪ Not on a stick yet
     - Tested on the build machine (``onu-utils/tests/test_onu_update.sh``).
       The copy over ssh was taken on a stick on 2026-10-11, and written back
       from a RAM boot on 2026-10-12 (``docs/handbook/backup.rst``)
   * - Releases built by the release workflow, twice and compared, with a
       pinned toolchain release
     - 🟡 Bench
     - The first release, v2026.10.20-1522, built so and published on
       2026-10-20 after its files were tried on two sticks by hand:
       ``web-install.tar`` through the shipped firmware's page, and
       ``update.bin`` on a stick that ran this system, which reached O5 in
       about 10 s and kept the new bank after its trial (`Releases`_)

..

    ℹ️ **The latest recorded run is of an earlier image, without the data
    path.** On 2026-10-12 32 of 48 cases passed on the image of that day, run
    from the flash, and none failed. The 16 it skipped send frames from the
    ports of a bench, one behind the OLT and one at the far end of the stick's
    host link, and the lab as it is has no such ports. They were last recorded
    on 2026-10-07, on an image of before procd, netifd, the panel over HTTPS and
    the new store of the settings, and have not run on this one
    (`tests/device/README.rst <tests/device/README.rst>`__).

Not done yet
------------

* **Releases are test builds.** Each is made by this repository's release
  workflow and tested before it is published (`Releases`_), but only on two
  sticks against the lab OLT.
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
* **The guard of the stick's own servers is not tried on a stick yet.** ssh,
  the panel and the stick's other programs answer on the management VLAN
  alone, in every mode, a router's too: a table of netfilter of the stick's
  own drops what comes in by any other interface but what one of its
  programs takes there, and forwards nothing to the management VLAN or from
  it (`onu-utils/README.rst <onu-utils/README.rst>`__, ``onu-fw``). It is
  tested between network namespaces (T), not yet from a session's far end
  on a stick.
* The stick has no clock that runs while it is off. Until a time source
  answers (NTP or PTP from the host, or the OLT's time of day), its clock
  starts at 1970.
* An update file carries a checksum against damage, but no signature
  (`ADR 0014 <docs/adr/0014-update-files-carry-no-signature.rst>`__);
  check a release's files against its ``SHA256SUMS``.
* Gaps found from here on are kept as the repository's issues.

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
  <linux/Documentation/networking/device_drivers/pon/gn25l95.rst>`__);
* a Buildroot configuration and a board directory in `br2-external/
  <br2-external/README.rst>`__;
* what its boot loader expects: how it picks a bank, and where the shipped
  firmware keeps the unit's identity (`boot-contract.rst
  <docs/handbook/boot-contract.rst>`__ says how this stick does it).

If you have such a stick and want to bring it up, open an issue first:
ports are welcome.

Before you install
==================

* **Follow the guide.** `install-guide.rst
  <docs/handbook/install-guide.rst>`__ takes a stick through the install, the
  first sign-in, an update and the ways back, step by step, with what to do
  when a step fails.
* **Take great care, and keep a backup.** The install is not expected to
  remove data or settings: the shipped firmware stays in its bank with its
  settings ("Two ways in", below). But these are test builds, tried on few
  sticks. Before the install, write down what the shipped firmware's pages
  show and you would need again: the line's identity (the GPON serial number,
  and the PLOAM password or LOID if your line uses one), the MAC address and
  any setting you changed. After it, copy the whole flash before anything
  else (below).
* **Take the files from a release** (`Releases`_), and check them against
  its ``SHA256SUMS`` before you install them.
* **The sticks tested here do not carry the host's identity page as
  shipped.** As shipped, the page that the host reads over the cage's
  two-wire bus declares RX_LOS, and 1000BASE-LX at 1300 MBd. A Linux host
  then keeps the link down while that line is high, which left it down after
  warm restarts of the host, and drops the link, the stick's only way in,
  whenever the fibre is dark; and it runs the lane at 1 Gb/s. On the sticks
  tested the page was written from the host, at the owner's risk: the claim
  of RX_LOS taken away, 2500 MBd declared with no compliance code, and the
  page's check bytes with them. That is what lets a Linux host bring the
  2.5 Gb/s link up every time and keep it while the fibre is dark; RX_LOS
  stays a line the host may read on its own. This system cannot change that
  page. A page changed so makes Linux 6.4 to 6.7 find no mode for the lane;
  only 6.12 and 6.18 were tried (`install-guide.rst
  <docs/handbook/install-guide.rst>`__, "The host's view of the stick").
* **Two ways in.** Through the shipped firmware's web page, with the package
  ``web-install.tar`` of a release, which needs no console
  (`install-guide.rst <docs/handbook/install-guide.rst>`__, `updating.rst
  <docs/handbook/updating.rst>`__). Or from the bench, which
  writes the flash from the boot loader's prompt over the serial console
  (`flashing.rst <docs/handbook/flashing.rst>`__). Either way the shipped
  firmware stays in the other bank, as the way back, with its settings.
* **The panel is on VLAN 4094**, at the address where the shipped firmware's
  page was, ``192.168.1.1/24`` unless it was changed there (`Reaching the
  panel`_). The host link itself has no address of the stick's: the OLT's
  services may put it in a bridge.
* **Copy the whole flash right after the install**, before the first
  ``update.bin``. Take it over ssh and compare its SHA-256 digest with the
  stick's, the way that was tried on a stick (`backup.rst
  <docs/handbook/backup.rst>`__); the panel's button "Copy the whole flash"
  makes the same copy, but has not been tried on a stick yet. Keep the copy
  private: it holds the unit's registration identity. The install has
  already written over the other bank, so the copy does not hold what that
  bank held before (`install-guide.rst <docs/handbook/install-guide.rst>`__,
  step 6).
* **The way back.** While the other bank holds the shipped firmware, the
  same page has "Go back to the shipped firmware (system A)", or B, which
  makes it the bank that starts, with its own settings.
* **The first update overwrites the shipped firmware.** The panel names the
  bank that holds it and asks before it writes over it; ``onu-update write``
  on the command line does not ask (`updating.rst
  <docs/handbook/updating.rst>`__, "The first update"). Going back to the
  shipped firmware after it takes the copy of the whole flash, the serial
  console, a host port at 2500BASE-X and a RAM image built from the sources,
  which no release carries (`backup.rst <docs/handbook/backup.rst>`__,
  "Writing it back"; `recovery.rst <docs/handbook/recovery.rst>`__).
* **Builds from before 2026-10-11 erased the shipped firmware's files** in
  the partition ``config``. The system now keeps its settings in a file of
  its own beside them, and does not write theirs.
* **The password is the shipped firmware's.** There is one account, root,
  for the console, ssh and the panel. The first start takes the password of
  the shipped firmware's administrator for it. Where that was still the
  default, ``admin``, the password stays ``admin``: the panel then allows
  nothing until a password of your own is set, and ssh takes keys only
  (`ADR 0009 <docs/adr/0009-one-account-root.rst>`__).
* **The panel's certificate is signed by the stick itself**, so a browser
  warns at the first visit. The console and the panel show its fingerprint
  to compare (`ADR 0013 <docs/adr/0013-the-panel-over-https.rst>`__).
* **Install only an update file you built or trust**: nothing checks who
  made it (`ADR 0014 <docs/adr/0014-update-files-carry-no-signature.rst>`__).
* **Try it on a lab OLT first.** A stick on a PON shares the fibre with every
  other ONU of its tree (`optical-safety.rst
  <docs/handbook/optical-safety.rst>`__).
* **The laser driver is known for one IC only** (`Not done yet`_). The
  driver writes the laser driver only in a module whose header reads ``ODI``
  and ``DFP-34X-2C``, and only reads any other. It cannot tell which IC such
  a stick carries. In one, every start of the PON link writes registers of
  the IC's configuration table: it puts back what the IC loads at
  power-up, the automatic extinction-ratio loop on, and when the OLT asks
  for a lower transmit power the power set point moves. The driver writes
  no byte of the table unless it has read that the IC keeps register
  changes out of its EEPROM, and the image never writes the table's
  password bytes (only a test build's store entry does). The one byte the
  module keeps, the page select, is written
  only when it holds another page, and put back as found. What those
  registers do in another IC is not known, and neither is whether its optics
  would work or stay dark when they should (`optical-safety.rst
  <docs/handbook/optical-safety.rst>`__, "What the system does not write").

Reaching the panel
------------------

The stick answers on VLAN 4094 of its host link, which its switch keeps away
from the PON, at the address the shipped firmware's page had. Give the
router's SFP port that VLAN and an address of the same network, then open
``https://192.168.1.1/`` (or the address you had given the shipped
firmware) and sign in as ``root``. On a Linux router whose SFP port is
``eth1``::

    ip link add link eth1 name eth1.4094 type vlan id 4094
    ip address add 192.168.1.2/24 dev eth1.4094
    ip link set eth1.4094 up

On RouterOS::

    /interface vlan add interface=sfp1 vlan-id=4094 name=onu-mgmt
    /ip address add address=192.168.1.2/24 interface=onu-mgmt

If the router's own network uses the same addresses, the two overlap:
give the shipped firmware another address before the install, or give the
router the VLAN's address only until the panel has moved the stick to a
network of its own. The panel can move the stick to another VLAN or address;
it keeps the old pair until the new one is confirmed from a panel opened at
it.

Releases
--------

Releases are on the repository's Releases page. Each is built by the
workflow ``.github/workflows/release.yml`` from the commit it names, once the
gates (``.github/workflows/gates.yml``) have passed on that commit, twice,
and the two builds must be identical; nothing built elsewhere goes into a
release. A release is published only after its own files were tested on two
sticks against the lab OLT, and then as a full release, which the page shows
as the latest. Its version is the time of its commit in UTC, as
``2026.10.20-0130``, and its tag is that version with a ``v`` in front. The
releases whose names begin with ``toolchain-`` hold the cross toolchain that
the images are built with, not the stick's system.

A release holds:

* ``web-install.tar``: the package for the shipped firmware's web page;
* ``update.bin``: an update for a stick that runs this system, from its
  panel;
* ``k0.uImage`` and ``r0.squashfs``: the two images, for the bench;
* the sources and licences of everything in the images;
* ``SHA256SUMS``: the digests of all of them.

Where to start
==============

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - I want to
     - Read
   * - Install it on a stick
     - `install-guide.rst <docs/handbook/install-guide.rst>`__, after
       `Before you install`_
   * - See the panel without a stick
     - The demo on this repository's GitHub Pages site, with a mock stick,
       `saquili.github.io/open-dfp-34x-2c
       <https://saquili.github.io/open-dfp-34x-2c/>`__
       (``.github/workflows/demo.yml``)
   * - Try a build of your own on a stick
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
upstream on its own (`ADR 0003 <docs/adr/0003-structure-and-scope.rst>`__).
The kernel series keeps to mainline Linux's rules, so that anyone can take it
there, but this project does not submit it (`ADR 0015
<docs/adr/0015-the-kernel-series-keeps-mainline-rules-without-a-submission.rst>`__).

Repository map
--------------

.. list-table::
   :header-rows: 1
   :widths: 24 76

   * - Directory
     - What it holds
   * - `linux/ <linux/>`__
     - Linux 6.18.57 with this project's kernel series on top: the SoC and
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
   * - `timed/ <timed/README.rst>`__
     - The stick's clock: NTP or PTP from the host, or the OLT's time of day
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
* `install-guide.rst <docs/handbook/install-guide.rst>`__: the install,
  the panel, an update and the ways back, step by step;
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
  and the `optics <linux/Documentation/networking/device_drivers/pon/gn25l95.rst>`__;
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
  of the packages and the toolchain's release, which are checked by hash;
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
    make buildroot     # fetch and patch the pinned Buildroot (build/buildroot)
    make sdk           # a relocatable cross toolchain (build/toolchain/images)
    make legal-info    # the licences of a built image (build/image/legal-info)
    make check         # the offline gates and the tools' tests
    make omcid         # after make: the OMCI daemon again, into the image
    make docs          # the pages as HTML in build/docs (needs docutils)
    make ramboot       # start a kernel from RAM over the bench
    make flash IMAGE=F TARGET=kernel|rootfs   # write bank 0 over the bench
    make clean         # remove the build products; distclean all of build/

``make`` builds the images with the cross toolchain of the release that
``br2-external/toolchain/PIN`` names, checked against the pin, and builds no
compiler. ``make TOOLCHAIN=source``, or a pin that names no release, has it
build a toolchain of its own from source first (`br2-external/README.rst
<br2-external/README.rst>`__, "The toolchain of a release").

The programs of this tree build and test on their own on the host, with
``make -C <program> check``. A build of your own installs as a release does,
but only the release workflow makes releases (`Releases`_).

Contributing
============

Fixes, tests, test reports from other OLTs and ports to other sticks are all
welcome. Read `CONTRIBUTING.rst <CONTRIBUTING.rst>`__ first: a commit touches
one part of the tree, carries a ``Signed-off-by:`` line, and passes the
provenance gates (`docs/policy/provenance.rst
<docs/policy/provenance.rst>`__), which keep out anything that is not ours
to publish. ``make check`` runs the gates and the tools' tests offline.
Report a security weakness privately, as `SECURITY.md <SECURITY.md>`__
says.

The system was built for one use, its maintainer's own stick and line, and it
is kept as time allows: pull requests and reports from other lines are
welcome, with no promise of when they are answered.

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
