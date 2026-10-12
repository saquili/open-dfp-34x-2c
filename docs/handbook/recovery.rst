.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=================================
Recovery over the serial console
=================================

When the stick answers neither ssh nor the panel, the serial console is the
way back. This page puts the ways in one place, in the order a person meets
them: where the stick is, then what to do about it. It repeats nothing that a
page of the handbook already says; it names the page.

What you need: the console of the stick (115200 baud, 8 data bits, no parity,
one stop bit, ``board.rst``, "The console"), and for anything that writes the
flash the bench of ``flashing.rst``, "What you need" and "Set up the bench".
Read "Rules that keep the board safe" on that page first.

Where the stick is
==================

.. list-table::
   :header-rows: 1
   :widths: 34 38 28

   * - The console shows
     - The stick is
     - Go to
   * - ``login:``, or a shell prompt
     - Running Linux. If ssh and the panel do not answer, the fault is the
       network or the password.
     - "The password is lost"
   * - Kernel lines, then ``Kernel panic - not syncing``
     - Restarting by itself after 10 s. A bank on trial gives way to the
       committed one.
     - "A bank does not come up"
   * - ``Hit any key to stop autoboot:`` and a count
     - At the boot loader's countdown.
     - "Stop the countdown"
   * - ``9601D#``
     - At the boot loader's prompt, with nothing started.
     - "Start a bank, or RAM, from the prompt"
   * - Nothing at all
     - Perhaps at a prompt: a console at a prompt prints nothing until a key
       arrives. Perhaps stalled.
     - Press Enter. If nothing comes, "A power cycle"

Stop the countdown
==================

The boot loader counts ``bootdelay`` ticks (5 as shipped) after every reset and
then starts the committed bank. Any byte it receives stops the count, and is
consumed: a command typed at once loses its first character
(``boot-contract.rst``, "How a bank is chosen"). To reach the countdown from a
running system restart it (``ssh root@STICK reboot``, or ``reboot`` on the
console; the watchdog then resets the chip and the loader is up about three
seconds later, ``board.rst``, "Power and restarts"), or power-cycle the stick.
Keep one typist on the console: the bench breaks into the countdown for you, and
a second client on the same bridge garbles what it types
(``flashing.rst``, "When something goes wrong").

A power cycle
=============

A power cycle is a person's act: take the stick out of the cage and put it
back, or cut the cage's power, and never while a flash write is running. The
failures that need one are a CPU stalled in a load that nothing answers and,
perhaps, a kernel that hangs early in a trial boot (``board.rst``, "Power and
restarts"; ``flashing.rst``, "Recover by a power cycle").

A bank does not come up
=======================

A system that is tried, which is every first start of a bank written by an
update, falls back by itself: a panic restarts the stick after 10 s, a kernel
the loader cannot unpack is cut off by the watchdog the loader armed, and a
system that boots but does not come up is restarted after 300 s; each time the
committed bank starts (``updating.rst``, "What a failure costs", with what was
measured). Nothing is needed from you.

One case needs the prompt: the bank that fails is the committed one itself.
Then every reset starts the same kernel again. Stop the countdown, and start
the other bank for this boot only::

    9601D# run b1

``b0`` and ``b1`` change nothing in the environment. Once the other bank runs,
``onu-update status`` says which bank is committed and which holds what, and
``onu-update commit`` keeps the running one (``updating.rst``, "Which bank
runs").

Start a bank, or RAM, from the prompt
=====================================

``run b0`` and ``run b1`` start a bank from the flash, once. To start a system
that does not depend on the flash at all, boot a RAM image with the bench
(``flashing.rst``, "Start a trial kernel from RAM"; ``building.rst``, "Putting
the images on the board")::

    make ram-image
    make ramboot IMAGE=build/image/images/ram.uImage \
        ARGS='--ready "open-dfp-34x-2c: ready"'

A RAM boot reads nothing from the banks and writes nothing to the flash. It is
the stick you do the rest with.

Neither bank starts
===================

Boot a RAM image as above, then put a system into a bank, by whichever way
suits what you hold:

* an update file, streamed into the bank you name (after a start from RAM no
  bank runs, so ``write`` needs one)::

      ssh root@STICK 'onu-update write - 0' < update.bin
      ssh root@STICK 'onu-update try 0 && reboot'

  (``updating.rst``, "Updating");
* an image of a bank, with the bench, which writes bank 0 and checks bank 1
  stays as it was (``flashing.rst``, "Write bank 0");
* a copy of the whole flash taken earlier, which also puts back the store, the
  loader's environment and bank 1 (``backup.rst``, "Writing it back"). This is
  the way for a flash whose store or environment is damaged as well.

The password is lost
====================

There is one account, root, for the console, ssh and the panel
(``docs/adr/0012-one-account-root.rst``). A password that is lost is replaced
from a root shell that does not ask for one:

1. Build a RAM image whose ``/etc/board.conf`` has ``CONSOLE_AUTOLOGIN=root``
   (``building.rst``, "Files of one bench"). Without that setting the console
   asks for the password, as the plain image's does.
2. Boot it from RAM (above). The console gives a root shell. The stick's
   store is the partition ``config``, which the boot mounted.
3. Give root a new password, the line on the input of ``onu-config password``,
   8 to 128 characters::

       onu-config password

   and check it: ``onu-config password-check`` takes a line on its input and
   exits 0 for the password in force and 1 for any other. The hash is in the
   store in the flash, so the next start of the stick, from the flash, takes
   the new password on the console, on ssh and in the panel.

(M: on the board, 2026-10-12, in a RAM boot with ``CONSOLE_AUTOLOGIN=root``:
``onu-config password`` exited 0, ``password-check`` exited 0 for the new
password and 1 for another, and the store was then put back from a copy. The
commands were typed over ssh with the bench's key, as root, which is the shell
the console gives; the restart into the flash with the new password was not
made.)

This is meant to be possible: the password protects the stick from the network,
not from whoever holds its console and can start it from RAM. The same holds for
the loader's own prompt, which has no password.

If the store cannot be read at all, the panel refuses every sign-in and ssh
takes keys only, and the console takes ``admin`` (``README.rst``, "Not done
yet"; ``building.rst``, "Known limitations"). Mend the store there. A new
password cannot be kept until it is back: put ``config`` back from a copy
(``backup.rst``, "Writing it back"). Without a copy, do not erase ``config``
to start afresh: the partition holds the unit's registration identity, which
nothing in this repository can recreate (``flashing.rst``, "Rules that keep
the board safe").

Back to the shipped firmware
============================

Until the first update, the bank this system does not run holds the shipped
firmware, in its own split: bank 1 after the bench, either bank after the web
page (``updating.rst``, "The first update"). Stop the countdown and start that
bank for this boot with ``run b0`` or ``run b1``. After the first update it is
gone; a copy of the flash taken before it brings it back (``backup.rst``).

The loader and its environment
==============================

The loader is never written by this system or by the bench. If it is damaged,
nothing at its prompt can repair it, and it is assumed that only a programmer
on the flash chip could (I: not tried; it would be settled by a damaged loader
and a programmer, and the rule keeps it from ever being tried here). Its two
copies of the environment are written by the loader's own ``saveenv`` and by
``onu-update``; what the loader does with both damaged is not recorded.
``backup.rst`` puts them back from a copy.
