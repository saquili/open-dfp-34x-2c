.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==========
tools/bench
==========

The serial-console bench for bring-up and flashing.

A small Python package: attach to the shared console, drive the bootloader,
load a trial kernel over the network (ramboot), and flash an image to the
scratch bank behind a **bank guard** that refuses any command naming the other
bank's or the boot partition's offsets.

Empty for now. It is populated by **WI-15**, with unit tests against a
simulated serial port.
