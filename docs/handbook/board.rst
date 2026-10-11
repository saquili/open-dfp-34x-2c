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
     - UART0, 115200 baud, 8 data bits, no parity, 1 stop bit. ``ttyS0`` in
       Linux.
   * - Power
     - From the host's cage. There is no power switch.
   * - Boot loader
     - U-Boot of the 2011.12 release, in the first 256 KiB of the flash.

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

The PON side
============

On the other side of the SoC, the GPON MAC and the PON MAC (behind switch port
2, the Linux interface ``pon``) turn Ethernet frames into the GPON frames of
ITU-T G.984.3 and back. The GPON MAC drives a separate laser-driver IC, which
sends the upstream bursts and reports the optical diagnostics of the module.
The SoC reaches that IC over its I2C master 1.

The laser driver has a hard transmit-disable input, SoC GPIO 13, active high
(M: ``linux/Documentation/networking/device_drivers/pon/rtl9602c-optics.rst``,
"Facts"). The switch driver requests it asserted at probe and releases it only
in the activation states O3 to O5, so that the stick sends no light in any
other state. Bringing the interface ``pon`` up starts the ONU's activation
towards the OLT (ITU-T G.984.3, states O1 to O5).

Nothing in the procedures of this handbook needs the PON side: starting a
kernel from RAM and writing the flash use the host side and the console only.

The console
===========

The SoC's UART0 is the console of both the boot loader and the kernel: 115200
baud, 8 data bits, no parity, 1 stop bit. It is a 16550-compatible UART, and in
Linux it is ``ttyS0``. How the UART of the stick is wired to whatever carries it
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
  ``rtl9602c-gpon.rst`` beside it, the GPON MAC; ``rtl9602c-optics.rst``, the
  optics and the laser.
* ``networking/devlink/rtl9602c.rst``: the switch driver's devlink parameters,
  among them the ONU's identity and the host lane's mode.

The other handbook pages that concern the board are ``boot-contract.rst`` and
``flashing.rst``.

Open items
==========

* Switch port 1 exists in the silicon. Nothing on this board is known to
  connect to it, which is not yet confirmed on the board either.
