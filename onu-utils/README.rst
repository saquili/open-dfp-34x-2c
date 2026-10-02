.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========
onu-utils
=========

The small programs of the ONU.

``ponid``
   prints the identity of the unit (serial number, vendor ID, version,
   equipment ID, PLOAM password and the values of the ONU-G and ONU2-G that
   G.988 has the ONU report) from its factory configuration, the JFFS2 file
   system in the partition ``config``. At boot it fills ``/run/onu-identity``,
   which omcid reads, and gives the serial number to the PON driver. Its output
   holds the PLOAM password, a credential of the line: keep it private.

Building and testing::

   make           # build/ponid
   make check     # the tests

The programs need zlib.
