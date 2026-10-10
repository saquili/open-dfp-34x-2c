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

What pond changes on the stick, it asks onu-act to do (``onu-utils``, ADR
0014) rather than run the program itself: the SFP lane, the management pair,
the confirmation of the changes on trial, the name, the identity and a new
registration, the updates' status and trial, and the restart. The settings,
the password and the upload and download of an update it still runs itself,
until the next part of step 8 of ADR 0014 moves them too.

Signing in
==========

One user, ``root``, the stick's one account: the console's login and the ssh
server take the same password (``docs/adr/0012-one-account-root.rst``). Its
hash, of the form of ``crypt()`` (SHA-256, ``$5$``, made by ``onu-config
password``), is kept in the configuration store, which ``onu-config`` puts in
``/run/onu-auth`` at boot; pond checks a password with ``crypt()`` of the C
library, about 0.5 s on the stick, during which it answers nothing else, and
has ``onu-config password`` hash and keep a new one. Until a password is set, it
is ``admin``, and a session may do nothing but set its own. A hash of the form
pond made before, PBKDF2-HMAC-SHA256 (``pbkdf2-sha256$...``), is still taken,
and the first sign-in with it has ``onu-config`` keep the password in
``crypt()``'s form, which the console and ssh take too. A sign-in opens a session: a random 128-bit id in an ``HttpOnly``,
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

The updates go through ``onu-update`` (``docs/handbook/updating.rst``). The
value ``firmware`` is its status: which bank runs and which starts next, a trial
under way or one that failed, each bank's state, version, time and sizes, and an
upload under way. ``POST /api/firmware/upload`` takes an update file as the
body, with a Content-Length of at most 4 MiB: once its headers are in, the
connection leaves the HTTP parser and the body streams into ``onu-update write
-``, the connection pausing while the pipe is full; the answer comes once the
bank was written and read back. One upload runs at a time. ``GET
/api/firmware/download`` streams the running bank as an update file, in chunks.
``POST /api/firmware/try`` arms a start of the bank in the body, ``0`` or ``1``,
or of the other bank with an empty body, and restarts.

``/api/set/ID`` takes ``KEY=VALUE`` lines:

* ``settings``: the settings of onu-apply (see ``onu-utils``), the keys of
  ``/run/onu.conf`` (``SESSIONn_GEM``, ``_VLAN``, ``_MAC`` and ``_FAMILY``
  among them) and the accounts, ``PPPOEn_USER`` and ``PPPOEn_SECRET`` for the
  sessions 1 to 7. pond knows none of their rules, which are onu-config's (ADR
  0014): ``onu-config check`` judges the change over the settings in force and
  gives them back in full, or says why not, which pond answers with; pond
  writes ``/run/onu.conf`` as onu-config gave it and each account's
  ``/run/onu-pppoeN`` for root only, then starts ``onu-apply try``: the
  change is on trial, and the stick goes back to the settings of before
  unless it is confirmed (``changes``, ADR 0014), which saves them in the
  store; it refuses another change, with 409, until onu-apply has ended. A read gives the settings as ``onu-config settings`` reads them,
  each session with its user and whether a secret is set, never the secret,
  and what was brought up for it: its link, its address and name servers
  from its interface of netifd (``network.interface.sesN`` on ubus), the
  delegated IPv6 prefix and the host it went to from its dhcp6d
  (``dhcp6d.sesN``), and from ``/run/session/N`` what onu-apply keeps of it
  (the interfaces, what it hands down, the gateway the router is given), the address handed on, and what became of the
  settings in the store (``store``: ``loaded``, ``defaults``, ``saved``,
  ``failed`` or ``none``), and each change on trial, of the settings, the SFP
  lane or the management pair, with when it goes back and the pair's new
  address (``trials``).
* ``changes``: ``CONFIRM=1`` keeps every change on trial (``onu-change``),
  a new management pair only from a request that came to its new address,
  and ``REVERT=1`` takes them all back; pond then listens on the management
  address in use alone. The panel confirms them as soon as it reaches the
  stick again.
* ``session``: a PPPoE session of the stick's own: ``ID`` (none for the first
  free one), ``USER``, ``SECRET`` (empty keeps it), ``VLAN``, ``MAC``, ``GEM``
  and ``FAMILY`` (``ipv4``, ``ipv6`` or ``both``). With an account the stick
  holds the session, and the first that carries IPv4 hands its address to the
  router; ``USER=`` removes it. The panel names no mode: without accounts the
  router dials through the stick.
* ``link``: how the router gets the address, ``VLAN``, ``GATEWAY``, the
  stick's address on the link, and ``PEER``, ``on`` to give the router the
  concentrator's address as its gateway or ``off`` to give it the stick's.
* ``identity``: how the stick introduces itself to the OLT, ``SERIAL`` (four
  letters or digits and eight hex digits), ``VENDOR_ID``, ``EQUIPMENT_ID``,
  ``VERSION``, ``EQUIPMENT_SERIAL``, ``MODEL``, ``OMCC_VERSION``,
  ``VENDOR_PRODUCT_CODE`` and ``TM_OPTION`` (numbers, decimal or ``0x``),
  ``LOID``, the logical ONU ID, ``SW_VERSION0`` and ``SW_VERSION1``, each
  software image's version, ``PACK_VERSION``, and ``PLOAM`` (ten characters or
  twenty hex digits) and ``LOID_PASSWORD`` (up to twelve), each kept when
  empty. It goes to ``onu-config identity``, then ``onu-apply reidentify``
  registers again. A read never gives either password, only whether one is
  set.
* ``hostname``: the stick's name, ``HOSTNAME``, a name of RFC 1123; it goes to
  ``onu-config system`` and to the kernel.
* ``sfp_lane``: the SFP lane's setting through ``onu-lane``: ``MODE``
  (``auto``, ``2500basex``, ``1000basex-an`` or ``1000basex``) on trial, which
  ends by itself after 90 s, ``KEEP=1`` or ``REVERT=1``. A read gives the
  setting, the one the lane runs in and a trial.
* ``mgmt``: the management VLAN and address through ``onu-mgmt``: ``VLAN`` and
  ``ADDRESS`` add a new pair beside the old one, and pond listens on it too;
  ``COMMIT=1``, taken only from a request that came to the new address, makes
  it the pair in use and stops the old address; ``CANCEL=1`` takes it away. A
  read gives the pair and one that waits.

Among the values read only: ``cpu``, the processor's clock ticks in each state
(a share of busy time is the difference of two reads), its load and model;
``omci``, the managed entities omcid holds for the classes that describe the
unit and its services, every attribute of each in hexadecimal and the rows of
its tables, and omcid's counts of the frames: omcid's answer on ubus (method
``mib`` of the object ``omcid``), which never gives the ONU-G's logical
password, only whether one is set.

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
and zod/mini, Tailwind and daisyUI; ``npm run build`` writes ``web/dist``, with
``licenses.txt``: the licence of every package whose code the panel holds, as
the build finds them in the bundle and in the stylesheet's imports
(``web/licences.ts``). A package without a licence file, or under a licence
other than MIT, ISC, BSD-2-Clause, BSD-3-Clause and 0BSD, stops the build.
``npm run dev:mock`` runs it against a mock stick inside the dev server, and
``npm run dev`` against a stick (``POND_TARGET`` names it, or the proxy in
front of it, and the dev server stops without it).

Building and testing::

   make           # build/pond
   make check     # the tests

pond reads netlink with libmnl and the kernel's headers, among them
``linux/pon.h`` of this repository's kernel; for a build on the host, point
CPPFLAGS at an installation of those headers (``make headers_install`` in
``linux/``). The tests run on the files of ``tests/fixtures``, which stand in
for ``/proc`` and ``/sys``, and on netlink dumps they build; the two copies of
the boot loader's environment are made by ``tests/make-env.py``. The tests of
the settings write under the same tree and run onu-config, built from
``../onu-utils`` (``ONU_UTILS``) and named by ``POND_ONU_CONFIG``, and a
stand-in for onu-apply, which ``POND_APPLY`` names. What pond has onu-act do
(``src/bus.h``) a build without libubus, the tests', runs itself from
onu-act's table, which it builds from ``../onu-utils``, each program from
``$POND_<PROGRAM>`` when set: those of the identity and the stick's own
settings start stand-ins for onu-config, onu-apply, onu-lane, onu-mgmt and
hostname (``POND_ONU_CONFIG``, ``POND_ONU_APPLY``, ``POND_ONU_LANE``,
``POND_ONU_MGMT``, ``POND_HOSTNAME``), and those of ``omci`` read omcid's
answer from ``run/ubus/omcid.mib``.
The tests of the sign-in check PBKDF2 against RFC 7914's vectors, the stored
form, the stop after wrong passwords, the end of sessions and a change of
password.

The HTTP layer is Mongoose 7.16 by Cesanta Software, in
``third_party/mongoose``, used under the GNU General Public License version 2,
one of the two licences it is offered under, and built as it comes. It holds
micro-ecc by Kenneth MacKay, for its TLS, under the BSD 2-clause licence, whose
notice goes with the binary too (``LICENSES/preferred/BSD-2-Clause.txt``).
