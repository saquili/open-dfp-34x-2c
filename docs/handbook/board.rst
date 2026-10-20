.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=====================
The DFP-34X-2C3 board
=====================

The ODI DFP-34X-2C3 is a GPON optical network unit (ONU) in the form of an SFP
module. It plugs into the SFP cage of a host, such as a router or a switch,
takes its power from the cage and exchanges Ethernet frames with the host over
the cage's data pins. On its other side it has the fibre that leads to the
operator's optical line terminal (OLT). Inside are one system-on-chip, a little
memory and flash, a laser-driver IC and a console UART.

This page says what each of these is for when the stick lies on a developer's
desk. It does not repeat the kernel's description of the hardware; the kernel
pages named under `Where to read next`_ do that. The flash layout and the way
the boot loader starts an image are on the page ``boot-contract.rst``, and the
procedures for starting a trial kernel and for writing the flash are on
``flashing.rst``, both in this directory.

At a glance
===========

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Part
     - What it is
   * - Board
     - ODI DFP-34X-2C3, a GPON ONU in an SFP module.
   * - System-on-chip
     - Realtek RTL9602C. One Lexra LX5281 core (a big-endian MIPS I design),
       an Ethernet switch with four ports and, behind the switch's PON port,
       the GPON MAC and the PON MAC.
   * - Memory
     - 32 MiB of DRAM, one bank at physical address 0 (DDR2, as the boot
       loader reports it).
   * - Flash
     - 8 MiB of SPI NOR (JEDEC ID C2 20 17) on the SoC's SPI controller, also
       mapped for reading at physical address ``0x14000000``.
   * - Host side
     - The SFP edge connector: a SerDes lane of the switch (port 0) running
       2500BASE-X or 1000BASE-X, as the host's cage does. Linux interface
       ``sfp``.
   * - PON side
     - The GPON MAC and a laser-driver IC on an I2C bus (switch port 2). Linux
       interface ``pon``.
   * - Console
     - UART0, 3.3 V logic, 115200 baud, 8 data bits, no parity, 1 stop bit.
       ``ttyS0`` in Linux.
   * - Power
     - From the host's cage. There is no power switch.
   * - Boot loader
     - U-Boot of the 2011.12 release, in the first 256 KiB of the flash.

Block diagram
=============

::

    host's SFP cage
         |    power, SerDes lane, signals
         |
    port 0 (sfp)
         |
    switch core ---- port 3 (CPU) ---- DMA ---- LX5281 CPU ---- DRAM, 32 MiB
         |                                          |   |
    port 2 (pon)                                    |   +-- SPI -- flash, 8 MiB
         |                                          |
    PON MAC, GPON MAC                               +---- UART0 -- console pads
         |
         +---- burst enable ------------------------------+
                                                          |
    CPU -- I2C master 1 -- SCL, SDA ----------------+     |
    CPU -- GPIO 13 ------- TX_DISABLE ---------+    |     |
                                               |    |     |
                                               v    v     v
                                      laser-driver IC (GN25L95) ---- fibre
                                      transmitter and receiver

The switch (ports 0, 2 and 3), the CPU's DMA engine, the CPU, its controllers,
the GPON MAC, the PON MAC, the I2C master and the GPIO are blocks of the SoC.
The DRAM, the flash and the laser-driver IC are separate parts, the console is
the SoC's UART0 on pads (below), and the kernel pages named under `Where to read
next`_ describe each block.

Names
=====

The stick goes by more than one name, and the names do not all agree:

* The board is the **ODI DFP-34X-2C3**. The kernel's device tree says so in its
  ``model`` string and in the compatible ``odi,dfp-34x-2c3``.
* The silicon is a **Realtek RTL9602C**. That is what the chip's own model word
  reads, and what the kernel, its drivers and the kernel documentation call it.
* The boot loader prints the chip's marketing name instead, **RTL9601D**, in its
  banner (``Board: RTL9601D``) and in its prompt (``9601D# ``). The README of
  the repository uses that name as well. On this board the two names mean the
  same chip, so a console line in this handbook that says ``9601D`` is the boot
  loader speaking.

Memory and flash
================

The 32 MiB of DRAM start at physical address 0. Two areas of about 1 MiB each
belong to the PON MAC, which keeps its packet buffers there and reaches them by
DMA: one lies at the top of the memory and the other 8 MiB below it. The kernel
reserves both and never allocates or maps them.

A memory test at the boot loader's prompt has to go through the uncached
segment (KSEG1, the physical address plus ``0xa0000000``). Through the cached
segment the data cache answers for addresses where there is no memory. Seen
through KSEG1, a word written at physical ``0x01fffff0`` reads back, and a word
written at ``0x02000000``, the first address past the memory, does not.

The flash is a Macronix part of 64 Mbit (JEDEC manufacturer code C2) on the
SoC's SPI controller. The SoC also maps the whole chip into physical memory at
``0x14000000`` to ``0x147fffff``, for reading only, and the boot loader starts
a kernel from that window in place. Linux reaches the chip through the
controller, which erases and programs it in 4 KiB sectors. The chip holds the
boot loader, its environment, the partition ``config`` and two banks, each
with a kernel and a root filesystem. Linux can write all of them but the boot
loader, whose partition the device tree marks read-only. The layout is on the
page ``boot-contract.rst``.

The host side
=============

The only electrical data interface of the stick is its SFP edge connector. It
is wired to a SerDes lane of the SoC's switch (port 0), with no PHY in between.
In Linux this port is the network interface ``sfp``. Linux finds what the
host's cage runs, and keeps the first of these that the lane's receiver
synchronises to: 2500BASE-X without in-band negotiation, 1000BASE-X with it,
and 1000BASE-X without it. The devlink parameter ``sfp_lane`` narrows the
choice (``linux/Documentation/networking/devlink/rtl9602c.rst``). The device
test ``net.host-link-search`` saw the lane follow a host that moved from
2500BASE-X to 1000BASE-X with negotiation and back (T, record
``docs/testing/2026-10-07-run23.rst``).

For a developer it is the way in and out of the board. The host sends images to
the boot loader over it, reaches the running board over it, and sends the test
traffic over it. The boot loader runs the lane at 2500BASE-X only, so the bench
needs a cage that links at 2500BASE-X; once Linux runs, a cage of 1000BASE-X
works too.

The boot loader's network interface is the same lane. The boot loader leaves
the lane switched off, so a transfer at its prompt works only after the lane
has been brought up by hand or by a macro of the saved environment. The page
``flashing.rst`` says what that macro has to do.

The SFP connector
=================

The host sees the stick through the signals of an SFP module. The stick takes
its power from the cage and its data over the SerDes lane. What the host's
driver reads of the others, with the stick in O5 on a lab OLT in a cage of a
Linux router (M: the state file of the router's SFP driver, in its debugfs, and
``ethtool -m`` of the port, 2026-10-12):

.. list-table::
   :header-rows: 1
   :widths: 24 20 56

   * - Signal
     - The host reads
     - Notes
   * - MOD_DEF0 (module present)
     - present
     - The host sees the module while it is in the cage.
   * - TX_FAULT
     - 0
     - The option word declares the signal implemented.
   * - RX_LOS
     - 0
     - Not declared by the option word as it was changed on this stick (see
       below). It read 0 throughout, with light from the OLT. The contact is
       the laser driver's own loss-of-signal output: with the fibre pulled it
       went high within a second of the PON leaving O5, and low again within a
       second of light returning (M: ``gn25l95.rst``, 2026-10-17).
   * - TX_DISABLE
     - 0, driven by the host
     - The option word declares the signal implemented. What the stick does
       with it is not recorded: the hard disable that this system uses is
       the SoC's GPIO 13, inside the module ("The PON side").
   * - RS0, RS1
     - 0, driven by the host
     - The option word declares no rate select.

The identity page the host reads over the cage's two-wire bus (A0h at
``0x50``) declares an SFP (identifier ``0x03``) with a single-mode laser of
1310 nm and 20 km on an SC connector, the name ``ODI`` (bytes 20 to 35) and
the part number ``DFP-34X-2C3`` (bytes 40 to 55), and no digital
diagnostics. It declares no Ethernet compliance code (byte 6 is 0) and a
nominal bit rate of 2500 MBd (byte 12, ``0x19``), so a Linux host picks the
lane's mode from the bit rate alone (``install-guide.rst``, "The host's view
of the stick").

The page is a memory of its own, apart from the laser driver's private EEPROM,
which the host never sees, on a bus the SoC does not reach: the SoC's SFP
slave is disabled and empty, and the stick cannot change what the host reads
(M: ``gn25l95.rst``, 2026-10-17).

**The sticks this system was tested on do not carry the page as shipped.** As
shipped, byte 65 (``0x41``), the low byte of the option word of bytes 64 and
65, read ``0x1a``: bit 4 (``SFP_OPTIONS_TX_DISABLE``), bit 3
(``SFP_OPTIONS_TX_FAULT``) and bit 1 (``SFP_OPTIONS_LOS_NORMAL``) of
``linux/include/linux/sfp.h``, RX_LOS declared. It was written to ``0x18``
from the Linux host of the cage, and the check byte CC_EXT, byte 95
(``0x5f``), the low byte of the sum of bytes 64 to 94, from ``0x3d`` to
``0x3b``. The option word now reads ``0x0018``, TX_DISABLE and TX_FAULT
alone. With RX_LOS declared, a Linux host keeps the link down while RX_LOS is
high: after a warm restart of the host its SFP driver stayed in the state
``wait_los``, while a cold power cycle sometimes brought the link up anyway;
with the change the link came up after every warm restart (M: the host's SFP
driver, warm restarts of the host before and after the change). Why the change
matters, and how to make it, is in ``install-guide.rst``, "The host's view of
the stick".

The PON side
============

On the other side of the SoC, the GPON MAC and the PON MAC (behind switch port
2, the Linux interface ``pon``) turn Ethernet frames into the GPON frames of
ITU-T G.984.3 and back. The GPON MAC drives a separate laser-driver IC, which
sends the upstream bursts and reports the optical diagnostics of the module.
The SoC reaches that IC over its I2C master 1.

The laser driver has a hard transmit-disable input, SoC GPIO 13, active high
(M: ``linux/Documentation/networking/device_drivers/pon/gn25l95.rst``,
"Facts"). The laser driver's driver, ``gn25l95``, requests it asserted at
probe, and the GPON MAC has it released only in the activation states O3 to
O5, so that the stick sends no light in any other state. Bringing the interface ``pon`` up starts the ONU's activation
towards the OLT (ITU-T G.984.3, states O1 to O5).

Nothing in the procedures of this handbook needs the PON side: starting a
kernel from RAM and writing the flash use the host side and the console only.

The console
===========

The SoC's UART0 is the console of both the boot loader and the kernel: 115200
baud, 8 data bits, no parity, 1 stop bit. It is a 16550-compatible UART, and in
Linux it is ``ttyS0``. The UART has two signals, TX and RX, and a ground, and it
runs at 3.3 V logic: an adapter for 1.8 V or 5 V is the wrong one, and a
3.3 V one carries the console of the unit that this system was developed on (I:
the unit's owner reports it so; a meter on the pad would settle it). The pads
are on the top side of the board, near the SFP edge connector, to be reached
with the stick out of its cage and powered through a breakout, since the
adapter does not power it (I: a community page about the DFP-34X-2C2, "ODI
Realtek DFP-34X-2C2" at hack-gpon.org, read on 2026-10-12, says so with
photographs; a photograph of the 2C3's board would settle it). Which pad is TX
and which RX is not recorded here: if the console stays silent, swap the two
wires at the adapter. How the UART of the stick is wired to whatever carries it
to the bench is not part of the board's description.

On a bench the UART is reached through a UART-over-network bridge that offers
one raw TCP socket per UART. Three properties of such a bridge matter:

* The baud rate is set on the bridge. A baud figure that a client prints means
  nothing.
* A BREAK cannot be sent across it.
* A console at a prompt prints nothing until a key arrives. Silence does not
  mean that the board is dead: press Enter and look for the prompt.

Power and restarts
==================

The stick takes its power from the host's cage and has no power switch, and the
board cannot switch its own supply off: the kernel's halt and power-off only
stop the CPU. What software can do is restart. ``reboot``, or SysRq ``b`` on a
running kernel, has the watchdog reset the whole chip, and the boot loader is
at its autoboot countdown about three seconds later.

A power cycle is a person's act: take the stick out of the cage and put it
back, or cut the cage's power, and never while a flash write is running. A few
failures need one, because neither a restart nor the watchdog recovers from
them. A CPU stalled in a load that nothing answers is one (the memory map in
the kernel's MIPS page says which addresses do that), and a kernel that hangs
early in a trial boot may be another (see the page ``boot-contract.rst``).

What a bench shares
===================

A bench has two parts that more than one person, or more than one tool, may be
using at the same moment.

The console bridge
    Each bridge port serves one client. A second connection either replaces the
    first or has its input merged into the UART; which of the two happens is not
    settled. Never open one "just to look". The bench tool keeps the single
    client in a tmux session, one per bridge port and named after it, and writes
    everything the board prints to a transcript file. A person can attach to
    the same pane to watch and to type; two typists at once interleave their
    keystrokes.

The image host
    The machine whose SFP cage the stick sits in. It serves the images to the
    boot loader by TFTP, from one directory, over the cage's link. The bench
    tool pushes the image and its own small TFTP server to it at every run and
    adds one firewall allowance for UDP port 69 on the cage's interface. It
    changes nothing else on that interface, because on some benches that link
    is also how the host itself is managed.

The tool's README, ``tools/bench/README.rst``, describes both in detail, and the
page ``flashing.rst`` sets them up.

Where to read next
==================

The kernel pages, under ``linux/Documentation/``, describe the hardware as the
kernel drives it:

* ``arch/mips/rtl9602c.rst``: the CPU core, the memory map, the platform code,
  the watchdog, the GPIO bank, the flash window and its partitions, the device
  tree, and the kernel's side of the boot contract.
* ``networking/dsa/rtl9602c.rst``: the switch, its four ports and the SerDes
  lane to the host.
* ``networking/pon.rst``: the PON class, the kernel's interface to the ONU
  that omcid and the stick's programs use.
* ``networking/device_drivers/pon/rtl9602c.rst``: the PON MAC;
  ``rtl9602c-gpon.rst`` beside it, the GPON MAC; ``gn25l95.rst``, the
  optics and the laser.
* ``networking/devlink/rtl9602c.rst``: the switch driver's devlink parameters,
  among them the ONU's identity and the host lane's mode.

The other handbook pages that concern the board are ``boot-contract.rst`` and
``flashing.rst``.

Open items
==========

* The console's pads: a photograph of the board, with TX, RX and ground marked,
  is still to be added; until then "The console" is all this handbook says.

* Switch port 1 exists in the silicon. Nothing on this board is known to
  connect to it, which is not yet confirmed on the board either.
