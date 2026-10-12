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
    ``run-files.rst``
        The files of ``/run``: which program writes each, which reads it, and
        how far a reader may count on it.
    ``optical-safety.rst``
        The laser and the other subscribers of a PON: how the stick is kept
        dark, what it does not write to the laser driver, what it cannot do,
        and what a person does.

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
    ``backup.rst``
        A copy of the whole flash, taken before the first update: what it
        holds, how to take and check it, and how to write it back.
    ``recovery.rst``
        Recovery over the serial console in one place: where the stick is,
        the loader's countdown and prompt, a bank that does not come up, a
        lost password, the way back to the shipped firmware.
    ``toolchain.rst``
        The cross toolchain: compiling for the core, what a program must not
        do, checking one with the instruction-set gate, and running the test
        programs.

Developing the kernel
    ``kernel-series.rst``
        The kernel series and the kernel under it: its layers, and every file
        of the kernel's own that it changes, with the reason.

Developing the panel
    ``panel-design.rst``
        The panel's look: the sizes and colours of text, values and their
        labels, states, colour, space, edges, actions and forms, the same on
        every page.
