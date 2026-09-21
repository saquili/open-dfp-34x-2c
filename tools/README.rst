.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=====
tools
=====

Host-side tooling for building, checking and bringing up the product. Each
subdirectory follows its own upstream conventions and is filled by its own work
item:

* ``bench/`` -- the serial-console bench: console attach, the bootloader
  driver, ramboot, and flashing with the bank guard (**WI-15**).
* ``provenance/`` -- the provenance gates that keep the tree clean (**WI-02**).
* ``equiv/`` -- the equivalence tools that prove the kernel code is unchanged
  across the clean-up steps (**WI-03**).
* ``isa-gate/`` -- the instruction-set gate that rejects any non-MIPS-I code in
  a target binary (**WI-14**).

Each subdirectory carries its own README until it is filled.
