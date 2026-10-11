.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

================
docs/handbook
================

The device maker's handbook: what the stick is, how this system runs on it,
and how to build it, put it on a stick and update it.

The stick and the system
    ``board.rst``
        The DFP-34X-2C3 board and its interfaces.
    ``system.rst``
        How the system works: its parts, its start, which program owns what,
        how a change of the settings reaches the hardware, and what is kept
        where.
    ``boot-contract.rst``
        The flash layout, the image formats, how a bank is selected, the
        watchdog.

Building and installing
    ``building.rst``
        Building the two flash images: what each is, its budget and its
        margin, the root filesystem and the boot script, how to build, check
        and reproduce them, and how to put them on the board.
    ``updating.rst``
        Updating the system into the other bank from the panel or a shell, the
        trial start that keeps it, what a failure or a power loss costs, and the
        way in through the shipped firmware's web page.
    ``flashing.rst``
        The bench: a RAM boot, writing bank 0, one-shot trials, recovery.
    ``toolchain.rst``
        The cross toolchain: compiling for the core, what a program must not
        do, checking one with the instruction-set gate, and running the test
        programs.

Developing the panel
    ``panel-design.rst``
        The panel's look: the sizes and colours of text, values and their
        labels, states, colour, space, edges, actions and forms, the same on
        every page.

Still to be written: copying the whole flash and writing it back, recovery
over the serial console in one place, and optical and rogue-ONU safety
derived from ITU-T G.984.3 and from measurements on the board.
