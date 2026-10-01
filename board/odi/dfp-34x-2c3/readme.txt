SPDX-License-Identifier: GPL-2.0-only
SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

ODI DFP-34X-2C3
***************

Intro
=====

The DFP-34X-2C3 is a GPON optical network unit (ONU) in an SFP module. It has
a Realtek RTL9602C system-on-chip (one Lexra LX5281 core, big-endian MIPS I),
32 MiB of DRAM and 8 MiB of SPI NOR flash. The host sees it through the SFP
edge connector at 2500BASE-X. The console is a UART at 115200 8N1. The names
the board goes by are explained in docs/handbook/board.rst.

How to build
============

Build the kernel image from the top of the repository:

  $ make linux

The result is build/linux/arch/mips/boot/uImage. It needs a MIPS cross compiler
(mips-linux-gnu-gcc, or name yours with CROSS_COMPILE=) and mkimage.

How to boot a trial kernel
==========================

Nothing is written to the flash. The boot loader fetches the image over TFTP,
into RAM, and starts it:

  $ make ramboot

IMAGE=<file> names another image. The bench in tools/bench drives the board
through its serial console, so the console bridge, the image host and the
boot loader's saved environment have to be set up once first; see
docs/handbook/flashing.rst.

How to flash bank 0
===================

  $ make flash IMAGE=<file> TARGET=kernel
  $ make flash IMAGE=<file> TARGET=rootfs

TARGET=kernel writes k0 with a uImage, TARGET=rootfs writes r0 with a SquashFS
image. The flash is read back and compared by CRC32, and bank 1 is checked to
be unchanged. ARGS=--arm arms a one-shot trial of bank 0 after the write and
boots it; read the note on the fall-back in flashing.rst first.

The banks
=========

Only bank 0 (k0 and r0) is ever written; bank 1 (k1 and r1) holds the shipped
firmware, the fall-back. The boot loader, its environment and the configuration
store are never written either, apart from the loader's own saveenv of the two
variables that choose the bank.

Documentation
=============

  docs/handbook/board.rst          the board and its interfaces
  docs/handbook/boot-contract.rst  flash layout, image format, banks, watchdog
  docs/handbook/flashing.rst       RAM boot, flashing, trials, recovery
  tools/bench/README.rst           the bench tool, its settings and refusals
