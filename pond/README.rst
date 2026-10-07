.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

====
pond
====

The HTTP daemon of the ONU and its panel. It serves the state of the device as
JSON (the system, the interfaces and their counters, the bridge, the GEM ports
and T-CONTs, the tc filters, the ONU as the PON driver reports it, the optics,
the flash with its two firmware banks), changes the stick's settings, and
serves the panel, a single-page app in ``web/``, as static files.

::

   pond [-l http://ADDRESS:PORT]... [-t TOKENFILE] [-w WEBDIR]

Each ``-l`` adds an address to listen on, up to four; with none, the daemon
listens on port 8080 of every address. An address it cannot listen on is
reported and left out, and the daemon ends only when it listens on none.

Signing in
==========

One user, ``admin``. Its password is kept as a PBKDF2-HMAC-SHA256 hash
(20000 iterations over a random 16-byte salt, on Mongoose's SHA-256; about
0.55 s on the stick, during which pond answers nothing else) in the
configuration store, which ``onu-store`` puts in ``/run/onu-auth`` at boot.
Until a password is set, it is ``admin``, and a session may do nothing but set
its own. A sign-in opens a session: a random 128-bit id in an ``HttpOnly``,
``SameSite=Strict`` cookie (``Secure`` behind a proxy that says HTTPS). There
are at most three; a fourth sign-in ends the one idle the longest, and a
session ends after 15 minutes without a request or 12 hours in all. Five wrong
passwords in a row stop sign-ins for 30 seconds (429, with ``Retry-After``). A
change of password ends every other session.

A change made in a session must carry the header ``X-Pond``, which a page of
another site cannot add, so a forged form changes nothing. Scripts may present
the bearer token of the token file (``/etc/pond.token`` by default) instead of
a session.

Routes
======

``/api/health`` answers anyone. ``POST /api/auth/login`` takes ``{"user",
"secret"}``; ``/api/auth/session`` says who is signed in and whether the
password must change; ``POST /api/auth/logout`` ends the session; ``POST
/api/auth/password`` takes ``{"current", "next"}``. ``/api/schema``,
``/api/get/ID``, ``/api/getm?ids=A,B`` (slow values left out) and ``POST
/api/set/ID`` read and change values; ``POST /api/system/restart`` restarts the
stick a second after it answers.

``/api/set/ID`` takes ``KEY=VALUE`` lines:

* ``settings``: the settings of onu-mode (see ``onu-utils``), the keys of
  ``/run/onu.conf`` (``SESSIONn_GEM``, ``_VLAN``, ``_MAC`` and ``_FAMILY``
  among them) and the accounts, ``PPPOEn_USER`` and ``PPPOEn_SECRET`` for the
  sessions 1 to 7. pond checks every value and the settings as a whole, writes
  ``/run/onu.conf`` and each account's ``/run/onu-pppoeN`` for root only, then
  starts ``onu-mode apply``, which also saves them in the store; it refuses
  another change, with 409, until that has ended. A read gives the settings,
  each session with its user and whether a secret is set, never the secret,
  and what onu-mode and dhcp6d brought up for it (``/run/session/N``: the
  interfaces, the addresses, the delegated IPv6 prefix and the host it went
  to), the address handed on, and what became of the settings in the store (``store``: ``loaded``,
  ``defaults``, ``saved``, ``failed`` or ``none``).
* ``session``: a PPPoE session of the stick's own: ``ID`` (none for the first
  free one), ``USER``, ``SECRET`` (empty keeps it), ``VLAN``, ``MAC``, ``GEM``
  and ``FAMILY`` (``ipv4``, ``ipv6`` or ``both``). With an account the stick
  holds the session, and the first that carries IPv4 hands its address to the
  router; ``USER=`` removes it. The panel names no mode: without accounts the
  router dials through the stick.
* ``link``: how the router gets the address, ``VLAN`` and ``GATEWAY``.
* ``identity``: how the stick introduces itself to the OLT, ``SERIAL`` (four
  letters or digits and eight hex digits), ``VENDOR_ID``, ``EQUIPMENT_ID``,
  ``VERSION``, ``EQUIPMENT_SERIAL``, ``MODEL``, ``OMCC_VERSION`` and ``PLOAM``
  (ten characters or twenty hex digits; empty keeps it). It goes to
  ``onu-store identity``, then ``onu-mode reidentify`` registers again. A read
  never gives the PLOAM password, only whether one is set.

The panel
=========

Every path outside ``/api`` is the panel, from the web directory
(``/usr/share/pond/www`` by default): ``/assets/`` holds the files of a build,
named by their hash and cached for good; any other path is ``index.html``, so
that the app's own paths work on a reload. The files are stored gzip-compressed
only, and pond hands them as they are to a browser that accepts gzip. The page
is served with a content security policy that lets it load and talk to pond
only.

``web/`` is TypeScript on Preact, with TanStack Router and Query, react-hook-form
and zod/mini, Tailwind and daisyUI; ``npm run build`` writes ``web/dist``.
``npm run dev:mock`` runs it against a mock stick inside the dev server, and
``npm run dev`` against a stick (``POND_TARGET``, the router's proxy by
default).

Building and testing::

   make           # build/pond
   make check     # the tests

pond reads netlink with libmnl and the kernel's headers, among them
``linux/pon.h`` of this repository's kernel; for a build on the host, point
CPPFLAGS at an installation of those headers (``make headers_install`` in
``linux/``). The tests run on the files of ``tests/fixtures``, which stand in
for ``/proc`` and ``/sys``, and on netlink dumps they build; the two copies of
the boot loader's environment are made by ``tests/make-env.py``. The tests of
the settings write under the same tree and start stand-ins for onu-mode and
onu-store, which ``POND_APPLY``, ``POND_REIDENTIFY`` and ``POND_STORE`` name.
The tests of the sign-in check PBKDF2 against RFC 7914's vectors, the stored
form, the stop after wrong passwords, the end of sessions and a change of
password.

The HTTP layer is Mongoose 7.16 by Cesanta Software, in
``third_party/mongoose``, used under the GNU General Public License version 2,
one of the two licences it is offered under, and built as it comes.
