.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

====
pond
====

The HTTP status daemon of the ONU. It serves the state of the device as JSON:
the system, the interfaces and their counters, the bridge, the GEM ports and
T-CONTs, the tc filters, the ONU as the PON driver reports it, the optics and
the flash with its two firmware banks.

::

   pond [-l http://ADDRESS:PORT] [-t TOKENFILE]

``/api/health`` answers without a token. ``/api/schema``, ``/api/get/ID`` and
``/api/getm?ids=A,B`` need the bearer token of the token file
(``/etc/pond.token`` by default); without the file the API stays locked.

Building and testing::

   make           # build/pond
   make check     # the tests

pond reads netlink with libmnl and the kernel's headers, among them
``linux/pon.h`` of this repository's kernel; for a build on the host, point
CPPFLAGS at an installation of those headers (``make headers_install`` in
``linux/``). The tests run on the files of ``tests/fixtures``, which stand in
for ``/proc`` and ``/sys``, and on netlink dumps they build; the two copies of
the boot loader's environment are made by ``tests/make-env.py``.

The HTTP layer is Mongoose 7.16 by Cesanta Software, in
``third_party/mongoose``, used under the GNU General Public License version 2,
one of the two licences it is offered under, and built as it comes.
