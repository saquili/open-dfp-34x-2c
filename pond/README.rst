.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

====
pond
====

The HTTP status daemon of the ONU. It serves the state of the device as JSON:
the system, the interfaces and their counters, the bridge, the GEM ports and
T-CONTs, the tc filters, the ONU as the PON driver reports it, the optics and
the flash with its two firmware banks. It also changes the settings of the
stick's mode, bridge or router.

::

   pond [-l http://ADDRESS:PORT]... [-t TOKENFILE] [-p PAGE]

Each ``-l`` adds an address to listen on, up to four; with none, the daemon
listens on port 8080 of every address. An address it cannot listen on is
reported and left out, and the daemon ends only when it listens on none.

``/api/health`` answers without a token. ``/api/schema``, ``/api/get/ID``,
``/api/getm?ids=A,B`` and ``/api/set/ID`` need the bearer token of the token
file (``/etc/pond.token`` by default); without the file the API stays locked.

``/api/set/ID`` is a POST whose body is the new value. ``settings`` is the one
value it changes: the settings of onu-mode (see ``onu-utils``), whose body is
``KEY=value`` lines, the keys of ``/run/onu.conf`` and ``PPPOE_USER`` and
``PPPOE_SECRET``. pond checks every value and the settings as a whole, writes
``/run/onu.conf`` and, for PPPoE, ``/run/onu-pppoe`` for root only, then starts
``onu-mode apply``; it refuses another change, with 409, until that has ended.
A read of ``settings`` gives the settings, the PPPoE user and whether a secret
is set, never the secret, what onu-mode brought up, and what became of the
settings in the configuration store (``store``: ``loaded``, ``defaults``,
``saved``, ``failed`` or ``none``, as ``onu-store`` records it). ``onu-mode
apply`` saves them there, so they outlive a restart.

``/`` is the status page, ``www/index.html``, installed as
``/usr/share/pond/index.html`` (``-p`` names another). It asks for the token,
keeps it for the session or, if asked to, in the browser, and refreshes every
five seconds. It leads with the state of the line in words: the fibre (PON
state, received and transmitted light in dBm, alarms), the internet (the
session and, in passthrough mode, the address handed to the router), the link
to the router, and the device (build, bank, uptime, whether the settings are
saved in flash). Under them, the traffic of the PON port from the switch's own
counters, with ten minutes of history and how many frames of it the processor
sees; then a form for the settings, grouped by what they are for; then, folded,
every value of the API as a table, the slow ones read on request. It needs
nothing from outside the device, and the daemon serves it with a content
security policy that lets it talk to the daemon only.

Building and testing::

   make           # build/pond
   make check     # the tests

pond reads netlink with libmnl and the kernel's headers, among them
``linux/pon.h`` of this repository's kernel; for a build on the host, point
CPPFLAGS at an installation of those headers (``make headers_install`` in
``linux/``). The tests run on the files of ``tests/fixtures``, which stand in
for ``/proc`` and ``/sys``, and on netlink dumps they build; the two copies of
the boot loader's environment are made by ``tests/make-env.py``. The tests of
the settings write under the same tree and start a stand-in for onu-mode,
which ``POND_APPLY`` names.

The HTTP layer is Mongoose 7.16 by Cesanta Software, in
``third_party/mongoose``, used under the GNU General Public License version 2,
one of the two licences it is offered under, and built as it comes.
