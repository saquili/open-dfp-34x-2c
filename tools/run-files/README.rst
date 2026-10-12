.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=================
tools/run-files
=================

A check that the handbook page of ``/run`` and the programs agree on which
files the programs of the stick keep there.

The programs of the stick tell each other things in files of ``/run``, and
``docs/handbook/run-files.rst`` says which, who writes each, who reads it and
how far a reader may count on it. A file added in a script and not on the page
is a file whose format only its writer knows; a file left on the page after
its program is gone sends a reader to look for what is not there.

Usage
=====

::

    tools/run-files/run-files [--page FILE] [--top DIR] [--list]

It reads the scripts, the C sources and the boot script of the tree (the
directories are the list ``SOURCES`` in the tool; a ``tests`` directory is left
out, which names stand-ins of its own) for the paths of ``/run`` they name:

* the literal ``/run/NAME``, in C and in shell, and ``$RUN/NAME`` and
  ``${RUN}/NAME`` of the scripts, with one level below a directory
  (``/run/omcid/mib``);
* the files of omcid's directory, which its sources name by the macro
  ``OMCI_RUN_DIR "/NAME"``.

A path is compared by its name up to the first character a program fills in:
``/run/onu-pppoe%d``, ``$RUN/onu-pppoe$n``, ``/run/onu-pppoe1`` and the page's
``/run/onu-pppoeN`` are one path, and so are ``/run/udhcpd-$lan.conf`` and the
page's ``/run/udhcpd-lanN.conf``. A file below a directory that the page names
with a slash (``/run/onu-trial.d/``) is named by it.

It fails on

* a path a program names that the page does not, and
* a path the page names that no program does.

``--list`` prints the paths the sources name, with a star on those a program
fills in the rest of, and exits.

Exit status is 0 when they agree, 1 on a finding, which is named on the
standard error, and 2 when a file cannot be read. ``make check`` runs the
tool, and its tests (``tests/run``) on trees and pages they make, and on this
repository's own.
