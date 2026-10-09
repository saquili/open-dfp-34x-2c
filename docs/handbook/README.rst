.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

================
docs/handbook
================

The device maker's handbook.

The pages so far:

``board.rst``
    The DFP-34X-2C3 board and its interfaces.
``boot-contract.rst``
    The flash layout, the image formats, how a bank is selected, the watchdog.
``flashing.rst``
    A RAM boot, writing the scratch bank, one-shot trials, recovery.
``updating.rst``
    Updating the system into the other bank from the panel or a shell, the
    trial start that keeps it, and what a failure or a power loss costs.
``toolchain.rst``
    The cross toolchain: compiling for the core, what a program must not do,
    checking one with the instruction-set gate, and running the test programs.
``building.rst``
    Building the two flash images: what each is, its budget and today's margin, the
    root filesystem, how to build, check and reproduce them, and how to put them
    on the board.
``panel-design.rst``
    The panel's look: the sizes and colours of text, values and their labels,
    states, colour, space, edges, actions and forms, the same on every page.

To come with **WI-20**: recovery over the serial console, bank discipline in one
place, and optical and rogue-ONU safety derived from ITU-T G.984.3 and from our
own measurements.
