.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

========================
Flashing and trial boots
========================

This page takes you from a stick on the desk to a kernel of your own running on
it: first started from RAM, then written to bank 0 and started once as a trial,
with whatever bank 1 holds as the way back: the shipped firmware on a stick as
shipped. Once this system runs, it updates itself into the other bank
(``updating.rst``); this page remains for the first installation and for a
recovery. Everything goes through the
bench tool in ``tools/bench``, started from the top-level ``Makefile``. The
tool's README, ``tools/bench/README.rst``, is the reference for its commands,
options, settings and refusals. This page does not repeat them; it says where
it relies on them.

Read "Rules that keep the board safe" at the end before the first write. In
short: the bench writes bank 0 only. The boot loader, the configuration store
and bank 1 it never writes, and it changes the loader's environment only through
the loader's own ``saveenv``.

What you need
=============

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Part
     - What it must be
   * - The stick
     - A DFP-34X-2C3 in the SFP cage of the image host, with the cage linking at
       2500BASE-X (see ``board.rst``).
   * - The image host
     - The machine whose cage the stick sits in. It serves the image by TFTP.
       It needs Linux with ``ss`` and ``nft``, ``sudo`` without a password for
       the commands the tool runs, and Python 3.8 or later for the server. It
       may be the machine you work on.
   * - The console bridge
     - A UART-over-network bridge that gives the stick's UART as one raw TCP
       socket, set to 115200 baud, 8 data bits, no parity, 1 stop bit.
   * - The tool host
     - The machine where you run ``make``: macOS or Linux, Python 3.11 or
       later, ``tmux``, and ``ssh`` if the image host is another machine or if
       the board is to be restarted over the network. Building the kernel needs
       a MIPS cross compiler (``mips-linux-gnu-gcc`` on Debian, or name yours
       with ``CROSS_COMPILE=``) and ``mkimage``.
   * - A person
     - For a power cycle: the stick has no power switch.

Nothing is installed besides the repository. The tool is written in Python and
uses only its standard library, and it copies its own small TFTP server from the
repository to the image host at every run, never trusting an earlier copy. From
the ``tools`` directory, ``python3 -m bench selftest`` runs its unit tests,
which need no board and no network.

The addresses of the examples are the ranges that RFC 5737 reserves for
documentation. They are also the tool's defaults.

.. list-table::
   :header-rows: 1
   :widths: 56 44

   * - Where
     - Address
   * - The image host, on the cage's interface (``host.interface``,
       ``host.bind``)
     - ``192.0.2.1``, a /30
   * - The loader: ``ipaddr``
     - ``192.0.2.2``
   * - The loader: ``serverip``
     - ``192.0.2.1``
   * - The loader: ``netmask``
     - ``255.255.255.252``
   * - The board under Linux, tried second for ssh (``board.data_address``)
     - ``192.0.2.2``
   * - The board under Linux, tried first for ssh (``board.control_address``)
     - ``198.51.100.2``, with ``198.51.100.1`` on the image host

The last two rows matter only when the tool has to reach a running kernel with
ssh, to restart it or to read ``/proc/cmdline``. The control address answers
whether or not the image has put the port in a bridge; the data address may stop
answering once it has. Both depend on the image.

Set up the bench
================

You do this once.

1. Start the console client in a tmux pane on the default tmux server, and
   mirror its output into a transcript file. The directory of the transcript
   has to exist; the tool does not create it. In the commands, TOOLS_DIRECTORY
   is the ``tools`` directory of the repository, BRIDGE the host name of the
   console bridge and TRANSCRIPT the transcript file (``console.transcript``
   names it; the default is a per-user file named after the session)::

       mkdir -p "$(dirname TRANSCRIPT)"
       tmux new-session -d -s uart-wifi-1 -c TOOLS_DIRECTORY \
           'python3 -m bench --set console.bridge_host=BRIDGE console client'
       tmux set-option -t uart-wifi-1 remain-on-exit on
       tmux set-option -t uart-wifi-1 history-limit 200000
       tmux pipe-pane -t uart-wifi-1 'cat >> TRANSCRIPT'

   Run ``pipe-pane`` without ``-o``: with ``-o`` a second run switches the
   logging off. A person can watch and type in the same pane with ``tmux attach
   -t uart-wifi-1``. The client has no exit key, because every key goes to the
   board, Ctrl-C and Ctrl-] included; leave by detaching with ``C-b d``.

2. Write a settings file for the bench in TOML. The keys and their defaults are
   in the README of the tool. At least the bridge and the image host must be
   named::

       [console]
       bridge_host = "bridge-host.example"

       [host]
       ssh = "image-host.example"
       interface = "eth1"
       bind = "192.0.2.1"

   ``host.ssh`` is ``local`` when the image host is the machine you work on.
   ``host.interface`` is the cage's network interface on the image host, and
   ``host.bind`` its address, which has to be the one the loader's ``serverip``
   names. Give the file to ``make`` by its absolute path, because ``make`` runs
   the tool from the ``tools`` directory::

       export BENCH_CONFIG=/path/to/bench.toml

3. Give the cage's interface on the image host its address, ``192.0.2.1/30``,
   in the way that host manages its network. The tool never changes that
   interface or its address.

4. Check the console and the image host::

       cd tools
       python3 -m bench console check
       python3 -m bench serve check

   ``console check`` says whether the console is usable and what the board
   shows: a loader prompt, an autoboot countdown, a reset in progress, a kernel
   starting, a login prompt, or something else. ``serve check`` reports the
   served file, the server and the firewall allowance. Before the first run it
   exits with status 1 and says what is missing; ``make ramboot`` and ``make
   flash`` prepare all three themselves.

The commands ``python3 -m bench ...`` on the rest of this page are run from the
``tools`` directory.

Prepare the loader
==================

The tool needs five things in the loader's saved environment. A person types
them once, at the loader prompt. The tool never sets them, because its guard
refuses those lines, and it stops with the missing variable's name instead.

.. list-table::
   :header-rows: 1
   :widths: 14 10 30 46

   * - Name
     - Kind
     - Content
     - Used for
   * - ``ipaddr``
     - variable
     - ``192.0.2.2``
     - The loader's own address on the link to the image host.
   * - ``serverip``
     - variable
     - ``192.0.2.1``
     - The TFTP server, which is the image host's address on the cage's
       interface.
   * - ``netmask``
     - variable
     - ``255.255.255.252``
     - The netmask of that link.
   * - ``sdsup``
     - macro
     - The lane bring-up, below.
     - Brings the link to the host up. The loader leaves it off.
   * - ``ramboot``
     - macro
     - The four commands below.
     - Fetches the kernel by TFTP and starts it from RAM.

The three variables must equal the settings ``board.loader_ipaddr``,
``board.loader_serverip`` and ``board.loader_netmask``, and ``serverip`` must be
the address the server binds (``host.bind``). They are saved: a setting made
without ``saveenv`` is gone after the next reset. The shipped values sit on
another network, so that a first transfer without them goes to a server that is
not there, and hangs.

A RAM boot needs all five. Writing the flash by TFTP needs ``ipaddr``,
``serverip``, ``netmask`` and ``sdsup``, and writing it by YMODEM none of them.

The loader's own environment already holds the variables that the tool reads.
Nobody creates or changes them by hand; the page ``boot-contract.rst`` says
what each of them means.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Name
     - What the tool does with it
   * - ``sw_commit``
     - Reads it: it is the fall-back bank. Sets it to 0 only with ``--commit``.
   * - ``sw_tryactive``
     - Must read 2 before a flash. Sets it to 0 to arm a trial and to 2 to
       cancel one, and never to 1.
   * - ``bootdelay``
     - Reads it. Sets it, only when asked through ``console type``, to a
       whole number of seconds from 5 to 600: the break-in is made for a
       countdown of five ticks.
   * - ``bootcmd`` and the macros behind it
     - Reads their text and checks the whole chain before it types ``boot``.
   * - ``b0``
     - May run it.
   * - ``fl_*``, ``img0_kernel``, ``img1_kernel``
     - Compares them with its own table before any write, and stops with the
       name of the first one that differs.
   * - ``bootargs_base``, ``mtdparts0``
     - Named by ``ramboot``; read when the macro is checked.

The macro sdsup
---------------

``sdsup`` has to leave the SerDes lane to the host in 2500BASE-X with the link
up. The loader leaves the lane off, with the mode field of
``RTL9602C_SERDES_CTRL`` at ``0x1f``, so a transfer without the macro prints
``T`` marks and nothing arrives. The macro replays, with memory writes, the
bring-up that the kernel's switch driver performs, step by step in the same
order. The kernel page ``linux/Documentation/networking/dsa/rtl9602c.rst``
describes the bring-up in its section "The SerDes lane to the host".

The offsets below are in the switch's register file, which starts at physical
address ``0x1b000000``. At the loader's prompt an offset is reached at
``0xbb000000`` plus the offset (the uncached segment). A memory write, ``mw.l``,
writes a whole 32-bit word, so a step that changes single bits writes the
register's previous word with those bits changed. Before the first step
``RTL9602C_SERDES_CTRL`` reads ``0x0004801f``; read the other registers with
the loader's ``md.l`` before you compose the macro.

.. list-table::
   :header-rows: 1
   :widths: 6 24 40 30

   * - Step
     - Register
     - Change
     - Purpose
   * - 1
     - ``RTL9602C_SERDES_CTRL`` (``0x000214``)
     - Set CLK_STOP, bit 13.
     - Stop the lane's clock before its analog settings are touched.
   * - 2
     - ``RTL9602C_SERDES_CTRL``
     - Clear PCIE, bit 15. Set the mode field, bits 4:0, to ``0x1f``.
     - Park the lane.
   * - 3
     - Seven analog words, in the next table
     - Write them in the order given.
     - The lane's analog settings, written while it is parked.
   * - 4
     - ``RTL9602C_SERDES_CTRL``
     - Clear PCIE, bit 15, CLK_STOP, bit 13, and PHY_MODE, bit 8. Set the mode
       field to 22 (2500BASE-X).
     - Restart the lane in that mode, with its clock running.
   * - 5
     - ``RTL9602C_SERDES_FIBER_CTRL`` (``0x021c00``)
     - Clear PDOWN, bit 11.
     - Take the fibre PHY out of power-down.
   * - 6
     - ``RTL9602C_PHY_CTRL`` (``0x000110``)
     - Set PATCH_DONE, bit 0.
     - Set the PHY's patch-done flag.
   * - 7
     - ``RTL9602C_SERDES_RESET`` (``0x021500``)
     - Write ``0x10``, then ``0x30``, then ``0x00``.
     - A reset pulse for the lane.
   * - 8
     - ``RTL9602C_SHAPE_2G5_CFG`` (``0x02d8b8``)
     - Set EN, bit 19, and the rate field, bits 18:0, to ``0x3ffff``. The word
       reads ``0x000bffff`` afterwards; a chip the driver has not touched reads
       the same.
     - The 2.5 Gb/s override: port 0 carries 2.5 Gb/s as a 1000 Mb/s port with
       it.
   * - 9
     - ``RTL9602C_SERDES_AN_CTRL`` (``0x021808``)
     - Write ``0x71e0``, as the last write.
     - Turn in-band auto-negotiation off on the SFP lane. Without it the lane
       transmits and its receiver never synchronises.
   * - 10
     - none
     - The driver remembers the mode as the programmed one.
     - Bookkeeping of the driver; a macro has nothing to do.

The analog words of step 3:

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - Write
     - Offset
     - Value
   * - 1
     - ``0x021700``
     - ``0x1800``
   * - 2
     - ``0x02170c``
     - ``0x7b01``
   * - 3
     - ``0x021718``
     - ``0x0168``
   * - 4
     - ``0x0215b8``
     - ``0x98c5``
   * - 5
     - ``0x0215b4``
     - ``0x1764``
   * - 6
     - ``0x0215bc``
     - ``0x400f``
   * - 7
     - ``0x0215e4``
     - ``0xfc00``

The order matters. The analog words are written while the lane is parked, and
the last write is the one that makes the receiver synchronise: on an
unconfigured lane ``RTL9602C_SERDES_AN_CTRL`` reads ``0x70e0``, and with a
working link ``0x71e0``, the only difference in the SerDes registers. After a
correct bring-up the receiver synchronises in about 20 ms, bit 17 of
``RTL9602C_SERDES_CTRL`` says so, and its mode field reads 22. The host sees a
link as soon as this side transmits, so the host's link light proves nothing;
the proof is a TFTP transfer that prints ``#`` marks.

The lane may drop again after a long ``sf write``, so the tool runs the macro
before each transfer of a flash image. It reads the macro from the board before
it runs it and checks the text: no loops, no conditionals, no quotes, and stores
only into the register window (see "What the tool refuses, and why" in its
README). Touch only the registers in the tables. A read in the switch's window
can stall the CPU where a block has not been enabled, as the PON MAC has not
been, and only a power cycle recovers it.

The exact text of ``sdsup`` for a given board is kept in that board's saved
environment. A ready-made text is not part of this edition.

The macro ramboot
-----------------

``ramboot`` is the RAM boot of ``boot-contract.rst`` as a macro: bring the host
link up, fetch ``uImage`` to ``0x81000000``, give the kernel bank 0's
arguments, and start the image. In order, the macro is these four commands::

    run sdsup
    tftpboot 0x81000000 uImage
    setenv bootargs ${bootargs_base} ${mtdparts0}
    bootm 0x81000000

The name ``uImage`` is the served name of the kernel (``image.kernel_name``).
Store the four commands as the one value of ``ramboot``, separated by
semicolons and quoted so that the loader keeps the semicolons and does not
expand the two variables when the macro is defined::

    setenv ramboot '<command 1>; <command 2>; <command 3>; <command 4>'

After ``saveenv``, ``printenv ramboot`` must show the four commands separated
by semicolons, with ``${bootargs_base}`` and ``${mtdparts0}`` unexpanded and
no quote or backslash in the text. If the loader stored something else, its
treatment of quotes differs from what is assumed here; this has not yet been
confirmed on the board.

Typing them
-----------

1. Attach to the console pane with ``tmux attach -t uart-wifi-1``.
2. Reach the loader prompt, ``9601D# ``. Restart a running kernel (the tool uses
   SysRq ``b`` over ssh) or power cycle the stick. When the line ``Hit any key
   to stop autoboot:`` appears you have ``bootdelay`` ticks, 5 as shipped.
   Press Enter, and nothing else: the key is consumed, and a command typed
   during the countdown loses its first character. Wait for the prompt.
3. Type ``printenv`` and see which of the five things are missing or differ. The
   output holds the board's MAC address: do not copy it into a log or a
   message.
4. Set the three variables::

       setenv ipaddr 192.0.2.2
       setenv serverip 192.0.2.1
       setenv netmask 255.255.255.252

5. Define ``sdsup`` and ``ramboot`` with ``setenv``, one value each, as
   described above.
6. Type ``saveenv``. It rewrites the environment, one of its two copies at a
   time, and ends with ``done`` and ``Valid environment: 1`` or ``2``. Do not
   interrupt it; it takes seconds.
7. Check each of the five with ``printenv <name>``.
8. Type ``reset``, break in again at the countdown, and check one of the
   variables again: a setting that was not saved is gone after a reset.
9. Detach with ``C-b d``.

Start a trial kernel from RAM
=============================

A RAM boot writes nothing to the flash. A kernel that fails costs one reset.

1. Build the kernel from the top of the repository::

       make linux

   The image is ``build/linux/arch/mips/boot/uImage``.

2. See what the tool would do, without touching the console or the image host::

       make ramboot ARGS=--dry-run

3. Run it::

       make ramboot

   ``IMAGE=<file>`` names another image. The options ``--ready``,
   ``--ready-limit`` and ``--recovery`` go into ``ARGS``; the README of the tool
   describes them.

The tool does the following, in order:

1. It checks the image: the header with its CRCs, MIPS, LZMA, the load address,
   and a size of at most 12 MiB.
2. It checks the console and takes a lock, so that no second operation types.
3. It makes the image, the server and the firewall allowance ready on the image
   host.
4. It brings the board to the loader prompt. A running kernel is restarted with
   SysRq ``b`` over ssh. If ssh cannot reach the board, the tool asks for a
   power cycle and breaks in when the board restarts (see "Recover by a power
   cycle").
5. It reads the environment and checks the five things of the previous section.
6. It types ``run ramboot`` and watches the transfer.
7. It compares what the loader reports about the image (name, creation time,
   size, addresses) with the file.
8. It waits for the kernel's banner and then for the image's ready marker.

It ends with a line that begins ``ramboot ok:``.

The kernel gets bank 0's command line, with ``root=31:5``. An image that
carries its user space runs it. An image without one looks for a root
filesystem in ``r0``, and the default ready marker, ``login:``, will not appear:
give the marker that your image prints with ``--ready``.

A good run looks like this on the console, from the restart to the kernel
(calibration lines, the loader's start-up lines, names and build-specific
values left out; the countdown is that of a board whose ``bootdelay`` is 600)::

    sysrq: Resetting
    rtl9602c: entering the reset vector; watchdog armed behind it

    9601D
    PRELOADER <name> <version>
    ...
    U-Boot 2011.12.NA (<build date>)
    ...
    DRAM: 32 MB
    ...
    Hit any key to stop autoboot: 600 599 598 597 596 595 594  0
    9601D#
    (more prompt lines: the break-in presses Enter many times)
    9601D# run ramboot
    ...
    TFTP from server 192.0.2.1; our IP address is 192.0.2.2
    Filename 'uImage'.
    Load address: 0x81000000
    Loading: *#############################################################
             ###########
    done
    Bytes transferred = 3988036 (3cda44 hex)
    ## Booting kernel from Legacy Image at 81000000 ...
       Image Name:   Linux-<release>
       Image Type:   MIPS Linux Kernel Image (lzma compressed)
       Data Size:    <n> Bytes = <size>
       Load Address: 80100000
       Entry Point:  <address>
       Verifying Checksum ... OK
       Uncompressing Kernel Image ... OK

    Starting kernel ...

    [    0.000000] Linux version <release> (...)

Write bank 0
============

``k0`` takes a legacy U-Boot image of at most 1900544 bytes, and ``r0`` a
SquashFS image of at most 2031616 bytes; ``boot-contract.rst`` describes both,
and how a board gets bank 0's split before its first write.
The tool checks the file against its target before it touches the board.

1. See the plan::

       make flash IMAGE=<file> TARGET=kernel ARGS=--dry-run

   It prints the image's size and the lines the tool would type, each already
   approved by the guard.

2. Write the kernel to ``k0``, or the root filesystem to ``r0``::

       make flash IMAGE=<file> TARGET=kernel
       make flash IMAGE=<file> TARGET=rootfs

   ``BANK=0`` is the default and the only value accepted. ``BANK=1``, or a
   target that names a protected partition, is refused before any work, with
   exit status 3.

The tool does the following, in order:

1. It checks the file for its partition and takes its CRC32.
2. For TFTP, it makes the image and the server ready on the image host.
3. It reaches the loader prompt as for a RAM boot.
4. It reads the environment. It stops if a layout variable differs from its
   table or if ``sw_tryactive`` does not read 2.
5. It records the CRC32 of bank 1 through the loader's window.
6. It brings the image into RAM at ``0x80000000``, by TFTP with ``sdsup`` run
   first or by YMODEM, and compares the CRC32 of the RAM copy with the file's.
7. It stops the watchdog's count with ``mw.l b8003268 0``. With YMODEM this
   comes before the transfer.
8. It erases the partition and writes it.
9. It reads the flash back through the window and compares its CRC32 with the
   file's.
10. It compares the CRC32 of bank 1 with the one it recorded.
11. Only then, and only if asked, it sets the flags and boots (see the next
    section).

It ends with a line that begins ``flash ok:`` and names the partition, the
length and the CRC32. Without ``--arm`` the board stays at the loader prompt.

The lines it types for the erase, the write and the check have this form (the
sizes are those of an example image of 390170 bytes, ``5f41a`` in hexadecimal,
and the percentages in between are left out)::

    9601D# sf erase 80000 +1d0000
    Erasing 1900544 B from 00080000... 100% ~ 0024ffff/1900544 B
    9601D# sf write 80000000 80000 5f41a
    Writing 390170 B from 80000000 to 00080000... 100% ~ 000df419/390170 B
    9601D# crc32 94080000 5f41a
    CRC32 for 94080000 ... <end> ==> <crc>

A write is finished only when the line with ``100% ~`` has appeared; the tool
waits for it, and treats 30 seconds without a new byte as a stall. If it
stalls, the tool stops, says that the partition is invalid and arms nothing.

Do not touch the console while a tool command runs. Do not reset, restart or
power-cycle the board during ``sf erase`` or ``sf write``. If you must stop the
tool, press Ctrl-C once, or send it SIGTERM: it lets the running command
finish, waits for the prompt, releases the console and exits. Never kill it
with SIGKILL; the README of the tool says how to restore the pane if that
happens.

``--via ymodem`` sends the image over the serial line instead. It is meant for
a serial device that nothing else holds (``console.serial_device``). Through a
bridge it is refused unless ``--ymodem-over-bridge`` is given and
``console.bridge_host`` names the bridge, because that has not been tried and
the bridge is known to drop bytes.

Arm a one-shot trial
====================

A trial boots the freshly written bank once and, if it fails, falls back to the
committed bank. ``--arm`` writes the image, verifies it, and then arms the
trial and boots it in the same run::

    make flash IMAGE=<file> TARGET=kernel ARGS=--arm

Look at the fall-back first. It is the committed bank, ``sw_commit``, and it
decides what a failure costs.

* With ``sw_commit`` at 1, a trial that fails costs one reset, and the board
  comes back on the shipped firmware in bank 1.
* With ``sw_commit`` at 0, the committed bank is bank 0, the bank that has just
  failed. The failed kernel boots again at every reset, and the way back is the
  loader prompt and ``run b1``, which boots bank 1 once and changes nothing.
  The tool refuses to arm in this case, before it has written anything, unless
  you add ``--accept-same-bank-fallback``::

      make flash IMAGE=<file> TARGET=kernel \
          ARGS='--arm --accept-same-bank-fallback'

With the console at the loader prompt, ``python3 -m bench console type
'printenv sw_commit'`` prints the committed bank. The tool states the fall-back
bank when it arms.

A board whose countdown was set to 600 seconds waits that long after every
reset, where a shipped board counts 5. It gets its countdown back to 5 seconds
with::

    python3 -m bench console type --restart 'setenv bootdelay 5' saveenv

``--restart`` restarts a running board and breaks in first, and the two lines
are judged in one session, so the ``saveenv`` follows the ``setenv`` it saves.

Arming happens only after the flash was read back as the file, bank 1 was
checked, and ``sw_tryactive`` read 2 at the start. The tool then sets
``sw_tryactive`` to 0 (a trial of bank 0), saves the environment, checks the
value, and types ``boot``. The loader runs ``bootcmd``, which sees the pending
trial and spends it: ``sw_tryactive`` goes back to 2 and ``sw_active`` to 0,
the environment is saved, the watchdog is armed, and bank 0 boots from the
loader's window. The flag is therefore gone before the kernel runs, and the
next reset takes the committed bank. Before the header of bank 0 the console
shows::

    ...
    Saving Environment to SPI Flash...
    ...
    Valid environment: 2
    ## Booting kernel from Legacy Image at 94080000 ...

The tool waits for that header and compares its fields with the file: the name,
the creation time, the size, and the address ``94080000``. It then waits for
the kernel's banner and the ready marker, and, when ssh reaches the board,
checks that ``/proc/cmdline`` holds ``root=31:5``. If the trial does not start,
it tries to cancel the flag it set (``sw_tryactive`` back to 2) and says so; if
it cannot, check ``sw_tryactive`` at the loader prompt yourself.

``--commit`` makes bank 0 the committed bank, ``sw_commit`` at 0, and saves it.
Use it only when you want every reset to boot bank 0, and only on purpose: it
removes the way back that a trial gives.

The trial's watchdog does not cover every failure. The kernel clears the count
early in its start and arms its own at the probe of the watchdog driver, so a
kernel that hangs between the two, or a user space that never opens the
watchdog, is expected to recover only by a power cycle (see the watchdog in
``boot-contract.rst``).

Check that bank 1 is untouched
==============================

The tool records the CRC32 of both partitions of bank 1 through the loader's
window before it writes, and again before it arms the trial, and fails if they
differ. You can read them yourself. With the console at the loader prompt::

    python3 -m bench console type 'crc32 94440000 14c000'
    python3 -m bench console type 'crc32 9458c000 274000'

The first is the loader's ``k1``, the second its ``r1``, in the shipped split;
together they cover the bank. From Linux, ``k1`` and ``r1`` are
``/dev/mtd6ro`` and ``/dev/mtd7ro``, in this system's split. Give the read an
explicit block count, because without it the read does not see the end of the
file and hangs::

    dd if=/dev/mtd6ro bs=4096 count=464 | md5sum
    dd if=/dev/mtd7ro bs=4096 count=496 | md5sum

The first is ``k1`` and the second ``r1``. Take both values once, before the
first write, and keep them: the tool proves that a run left bank 1 as it found
it, and your own record proves that bank 1 is still what it was. On a stick as
shipped that is the shipped firmware, which the first update of this system
overwrites (``updating.rst``).

How long each step takes
========================

.. list-table::
   :header-rows: 1
   :widths: 56 44

   * - Step
     - Time
   * - Pushing an image of 0.8 to 1.2 MB to the image host
     - About 1 s.
   * - A restart through the reset vector, to the loader's countdown
     - 1.81 s.
   * - TFTP transfer and start of an image by the loader
     - About 4 s for images of 0.8 to 1.2 MB.
   * - The kernel's banner to user space
     - About 2 s.
   * - A whole ``make ramboot``
     - 13 s from the loader prompt and 18 s through a restart whose first
       transfer worked, not counting the image host's preparation (measured with
       the tool). A script of the same steps took 25 s and 32 s, and 63 s when
       the first transfer failed and the watchdog's reset intervened.
   * - Waiting for the watchdog's reset after a failed first transfer
     - About 46 s after the restart.
   * - YMODEM at 115200 baud
     - 123 s for 1239420 bytes, so about 200 s for the largest ``r0`` image.
   * - ``sf erase`` of a partition of bank 0, ``sf write``, ``saveenv``,
       ``crc32`` through the window
     - Not recorded. The tool waits for the loader's progress lines, treats 30
       s without a new byte as a stall, and allows 15 s for ``saveenv`` and 30
       s for ``crc32``.
   * - Reading ``k1`` from Linux and hashing it
     - 0.74 s; ``r1`` is twice the size and takes about twice as long.

When something goes wrong
=========================

.. list-table::
   :header-rows: 1
   :widths: 30 38 32

   * - The console shows
     - Cause
     - What to do
   * - ``Loading: *`` followed only by ``T`` marks, one per timeout, and no
       ``#``
     - The transfer does not start or does not go on. The lane is off:
       ``sdsup`` is missing, wrong, or its effect was lost. Or the image host
       does not answer: the cable or the cage, the server not running, UDP port
       69 blocked. Or, right after a restart through the reset vector, the
       first transfer failed for a reason that is not established.
     - ``python3 -m bench serve check``. The tool ends the attempt after two
       ``T`` marks, presses Ctrl-C and recovers once (see "Recover by a power
       cycle"). Run it again.
   * - ``TFTP error: 'no such file' (1)``, ``Not retrying...``, then ``Wrong
       Image Format for bootm command`` and ``ERROR: can't get kernel image!``
     - The served file is not there under the name that ``ramboot`` asks for.
       The served directory is in a temporary directory by default, which a
       reboot of the image host clears. Or the name in the macro is not
       ``image.kernel_name``.
     - Run the tool again: it pushes the file at every run. Compare
       ``printenv ramboot``.
   * - The ``TFTP from server`` line, and no ``Loading: *`` after it; the prompt
       does not return
     - The transfer never starts: the loader asks another server, because
       ``serverip`` was not saved or is wrong.
     - Compare ``printenv ipaddr serverip netmask`` with the settings.
   * - ``CRC32 for 80000000 ... ==>`` a value that is not the file's
     - Bytes were lost or damaged on the way. The tool reports both values and
       has written nothing.
     - Run it again. The tool erases only after the RAM copy matches.
   * - ``Unknown command 'un' - try 'help'``, or another word that is the start
       of your line with a character missing
     - A key was lost. The first character typed into a countdown is consumed.
       A stray byte from a second client on the bridge looks similar (for
       example ``printenv`` arriving as ``xprintenv``).
     - Do not type while a tool runs. Keep one client on the bridge port.
   * - ``Unknown command 'ramboot' - try 'help'``
     - A macro typed without ``run``.
     - Type ``run ramboot``.
   * - Nothing after ``Starting kernel ...``
     - The kernel stopped before its console came up: a wrong load address, an
       image that is too large, or an early hang.
     - Check the image header (see ``boot-contract.rst``). Power cycle if the
       board does not come back.
   * - ``Booting kernel from Legacy Image at 94080000`` where ``81000000`` was
       expected, or an image name and creation time that are not your file's
     - The wrong kernel started: after a failed transfer, a ``boot`` or the end
       of the countdown started the kernel in the flash instead of the image
       that was sent.
     - The tool treats this as a failure. Find out why the transfer failed
       first.
   * - ``login:`` where a loader prompt was expected
     - Linux booted: the countdown ran out before the break-in, or the board
       was never restarted.
     - The tool never types at a login prompt. It restarts a running board over
       ssh, or asks for a power cycle.
   * - ``PRELOADER`` again, which nothing asked for
     - The board reset by itself, which the watchdog's count does about 46 s
       after a restart through the reset vector.
     - Expected after a failed first transfer; the tool breaks in again. In the
       middle of a flash it means that the count was not stopped, and the
       partition is invalid.
   * - Nothing printed at all
     - A console at a prompt prints nothing until a key arrives. Or the pane or
       the bridge is down.
     - ``python3 -m bench console check``. Marker lines such as ``console:
       reconnecting`` show that the bridge dropped the connection; the client
       reconnects by itself.

Recover by a power cycle
========================

When the board runs no kernel that ssh reaches and is not at the loader prompt,
the tool asks for a power cycle: it prints ``ssh cannot reach the board.
Power-cycle it now; the tool will break in when it restarts.`` and watches the
transcript. From the moment the first-stage banner, ``PRELOADER``, appears it
sends Enter every 100 ms until the loader prompt is verified, or 30 seconds
have passed.

1. Take the stick out of the cage and put it back, or cut the cage's power.
2. Leave the console alone. The tool's catcher breaks into the countdown.
3. If the countdown ran out first, the committed bank booted. Run the command
   again.

The window is ``bootdelay`` ticks. When it closes, the committed bank boots.
A power cycle is also the way out of the failures that nothing else recovers.

Rules that keep the board safe
==============================

* **The bench never writes bank 1.** It holds the way back to a board that
  works when bank 0 does not boot: the shipped firmware, or this system once it
  has updated itself. The tool checks, before and after every write, that bank 1
  reads back unchanged.
* **The boot loader is never written.** The CPU's reset vector runs it. If it
  were damaged, nothing could start, and nothing at the loader's prompt could
  repair it; it is assumed that only a programmer on the flash chip could. This
  has never been tried, and the rule keeps it that way.
* **The tool changes the environment only through the loader's own save
  command, and only two variables.** It sets ``sw_tryactive`` (0 to arm a
  trial, 2 to cancel one) and, when asked with ``--commit``, ``sw_commit`` (0).
  No other variable is set, and the partitions ``env`` and ``env2`` are never
  erased or written directly. These variables decide which bank boots, and the
  layout variables decide where the loader's macros write. By hand, change
  only what "Prepare the loader" lists.
* **The configuration is never written.** The partition ``config`` holds the
  device's configuration, including its registration identity, and nothing in
  this repository can recreate it.
* **The flash windows are never written.** A store into the memory-mapped
  window is refused in every spelling. The window is for reading.
* **A write is never interrupted.** Nothing is typed, reset or power-cycled
  while ``loady``, ``sf erase`` or ``sf write`` run: the loader is not reading
  the console then, and a byte is lost or corrupts the line.
* **A trial is armed only after the flash read back as the file,** after bank 1
  was checked, and never for bank 1.
* **Only the macros the tool knows are run.** ``run`` is allowed for
  ``ramboot``, ``sdsup`` and ``b0`` only, and ``boot`` only after the boot chain
  has been read and checked. The environment also holds macros that erase and
  write the flash; the tool never runs them, and neither should you.
* **One typist at a time.** Never open a second connection to the bridge port,
  not even to look, and never send BREAK. The tool takes a lock, so a second
  tool command refuses at once.
* **Keep no copy of the output of** ``printenv``. It holds the board's MAC
  address. The tool redacts its own logs; do the same with yours.
* **Read only what you know.** Some addresses stall the CPU on a load, and
  only a power cycle recovers it. Use the memory map in
  ``linux/Documentation/arch/mips/rtl9602c.rst``.

The tool enforces most of these; its README, in "What the tool refuses, and
why", lists the refusals. They are written here because they are also the rules
for what you type by hand.

Open items
==========

None of the following has been run end to end on the board yet. The README of
the tool lists the full set in "What is checked only on the board".

* The kernel of this tree has been started from RAM on the board. Starting it
  from the flash, from bank 0 with its root filesystem in ``r0``, has not been
  tested yet; the first trial boot of a flashed kernel is that test.
* Flashing by TFTP into ``k0`` and ``r0``: that the lane survives the erase and
  the write, and the durations of ``sf erase``, ``sf write``, ``saveenv`` and
  ``crc32``. The tool's own TFTP server, bound to one address and serving one
  directory, has not been run against the loader.
* That ``crc32`` read through the flash window equals the CRC32 of the file,
  for the RAM copy and for the flash. It has been seen to match once.
* That a typed ``reset`` heals a failed first transfer as the watchdog's reset
  does (``--recovery reset``, expected to save about 35 seconds). The recovery
  that is proven is to wait for the watchdog's reset, about 46 seconds after
  the restart.
* That ``mw.l b8003268 0`` at the prompt stops the watchdog's count after a
  restart of the current kernel. The measurement behind it was made with a
  different kernel build; a flash that starts after a restart is cut off by the
  chip reset if the write does not stop the count.
* That ``boot`` typed at the prompt consumes a pending trial exactly as the end
  of the countdown does. The countdown form is the measured one.
* That the loader stores the text of a macro as typed, with its semicolons and
  without the quotes.
* That the countdown tick is one second.
* That a trial boot is not reset by the watchdog when the kernel hangs early
  (see ``boot-contract.rst``).
* YMODEM through the bridge, and the bridge's rule for a second client.
* tmux older than 3.7c, and keys typed while the bridge drops its connection.
