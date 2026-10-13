.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

====
pond
====

The status daemon of the ONU and its panel, over HTTPS on the stick. It serves
the state of the device as JSON (the system, the interfaces and their
counters, the bridge, the GEM ports and T-CONTs, the tc filters, the ONU as
the PON driver reports it, the optics, the flash with its two firmware
banks), changes the stick's settings, and serves the panel, a single-page
app in ``web/``, as static files.

::

   pond [-l http[s]://ADDRESS:PORT]... [-k KEYFILE] [-t TOKENFILE] [-w WEBDIR]
        [-u USER]
   pond -k KEYFILE -F ADDRESS

Each ``-l`` adds an address to listen on, up to four; with none, the daemon
listens on port 8080 of every address. An address it cannot listen on is
reported and left out, and the daemon ends only when it listens on none. An
``https://`` address needs the key of ``-k`` (see "HTTPS"); ``-F`` prints the
fingerprint of the certificate of an address, and the end of its validity,
and ends. On the stick ``onu-run`` starts it on the management address, and
on the address of the host link if there is one, each over HTTPS on port 443
and over HTTP on port 80. ``-u`` makes a daemon started as root run as the
account ``USER``, with its groups, keeping of root's powers only the one to
listen on a port below 1024, for its listeners and those of a management pair
that comes later. ``-t`` names the file of the token of scripts (see "The token
of scripts"); without it no token is taken.

What pond changes on the stick, it asks onu-act to do (``onu-utils``, ADR
0014) rather than run the program itself: the SFP lane, the management pair,
the confirmation of the changes on trial, the name, the identity and a new
registration, the updates' status and trial, the restart, and the settings
of the data path, which pond reads without the accounts' secrets and changes
on trial, root's password, which onu-config checks and keeps without pond
seeing its hash, and the upload and download of an update, which stream
through a pipe whose other end pond passes to onu-act. pond runs no program
of the stick itself.

A change waits for what onu-act runs for it, seconds for a management pair
or a store's write, without holding up the daemon: pond starts the method,
serves the other requests meanwhile, and answers the change when the program
has ended (``src/later.h``), with what it said. A change of one kind runs
once at a time, and a second one gets 409; a sign-in or a change of the
password checks one password at a time, and a second one waits a second
(429), so that a wrong one is counted before the next is tried. Reads of
state, which onu-act answers at once, are waited for, at most 35 seconds,
onu-act's own limit and a margin. So are the check of root's password
before an update's upload or trial and the arming of the trial, a second or
two: the stick restarts right after a trial is armed.

On the stick pond runs as an account of its own, ``pond``, without a password
or a shell, which no one logs in with, started as root with ``-u pond``; root stays the one account to log in
with (ADR 0012, ADR 0014). ubusd's access list, ``acl.json``, which the image
installs as ``/usr/share/acl.d/pond.json``, lets it call the methods of
onu-act it names and read the state of omcid, of netifd's interfaces and of
dhcp6d, and nothing else; a new password, an update written and a start of
the other bank need root's password with the call as well (onu-act checks
it), so that pond, taken over, cannot give itself root; the
boot script gives it a directory of ``/run`` of its own for its log and the
token of scripts, and its group the two copies of the boot loader's
environment to read. Every other value it reads, any user may.

Signing in
==========

One user, ``root``, the stick's one account: the console's login and the ssh
server take the same password (``docs/adr/0012-one-account-root.rst``). Its
hash, of the form of ``crypt()`` (SHA-256, ``$5$``, made by ``onu-config
password``), is kept in the configuration store, which ``onu-config`` puts in
``/run/onu-auth`` at boot, for root alone; pond has onu-act run
``onu-config password-check`` on a password, which takes ``crypt()`` about
0.5 s on the stick, during which pond answers nothing else, and ``onu-config
password`` hash and keep a new one, and never reads the hash. Until a
password is set, it is ``admin``, and a session may do nothing but set its
own; pond asks which at the start (``onu-config password-state``) and knows
it after each sign-in and change, so that one set at the console counts from
the next sign-in. A hash of the form pond made before, PBKDF2-HMAC-SHA256
(``pbkdf2-sha256$...``), is still taken, and the first sign-in with it has
``onu-config`` keep the password in ``crypt()``'s form, which the console and
ssh take too. A sign-in opens a session: a random 128-bit id in an ``HttpOnly``,
``SameSite=Strict`` cookie (``Secure`` over HTTPS, the daemon's own or a
proxy's that says so). There are at most three; a fourth sign-in ends the one
idle the longest, and a
session ends after 15 minutes without a request or 12 hours in all. Five wrong
passwords from one address stop sign-ins from that address for 30 seconds,
and twenty from all addresses together stop every sign-in as long (429, with
``Retry-After``); behind a proxy every sign-in comes from the proxy's address.
A change of password ends every other session.

A sign-in, and a change made in a session, must carry the header ``X-Pond``,
which a page of another site cannot add, so a forged form neither signs the
browser in nor changes anything.

The token of scripts
--------------------

A script that cannot sign in as a person does may present a bearer token
instead of a session. The stick has none until root makes one, on the stick
itself (ssh, the console)::

    onu-config token new      # makes a token, keeps it, prints it
    onu-config token          # prints it again
    onu-config token off      # takes it away

The token is 64 hexadecimal digits of ``/dev/urandom`` in the configuration
store (``POND_TOKEN``), where it stays across restarts and updates, never
among the settings a person sees or saves. ``onu-config`` and the boot script
(``onu-config token install``) put a copy in the file the daemon is told to
read, ``-t TOKENFILE`` (``/run/pond/token``, for the account ``pond`` alone);
without ``-t`` the daemon has no token. The daemon reads that file at each
request that comes without a session, so a new token holds at once and one
taken away stops holding, with no restart that would end the sessions.

A script sends ``Authorization: Bearer TOKEN`` to an ``https://`` address; the
panel's certificate is the stick's own, which the fingerprint under System or
on the console tells apart. Over ``http://`` the request is sent on to HTTPS
before its body is read, but its headers have crossed the link in the clear:
the token is for HTTPS only. A request with the token needs no ``X-Pond``
header, as it has no session for a page of another site to use, and it is not
held back by the default password, since someone with root's shell made it.
It may call every route that a session may, and no more: a new update written
and a bank started need root's password in ``X-Pond-Password`` too.

HTTPS
=====

With an ``https://`` address, the daemon serves the panel and its API over
TLS 1.3, with Mongoose's own TLS, and an ``http://`` address only sends the
browser on: a request comes back as a 308 to the same host and path over
HTTPS, before its body is read, so that a password never crosses the link in
the clear. ``/api/health`` alone still answers over HTTP, for onu-trial, which
asks it whether the daemon of a new firmware runs. No
``Strict-Transport-Security`` is sent: with a certificate no authority signed,
it would keep a browser from letting its user go on.

The key, the file of ``-k``, is a scalar of the curve P-256 in 64 hexadecimal
digits, which onu-config made once on the stick and keeps in the
configuration store (``TLS_KEY``, ``onu-config tls``); the boot script gives
the daemon a copy only it reads. From it the daemon makes, for each address
it answers on, a certificate (``src/cert.c``): X.509 version 3, the common
name ``ONU`` and the address, the address as the subject's alternative name,
for TLS servers and not a CA, signed by its own key with ECDSA and SHA-256.
The signature is deterministic (RFC 6979) and nothing else in it changes from
one start to the next, so the same key and address give the same
certificate, and the same fingerprint, for as long as it is valid. The
validity follows a calendar of two-year periods from 2026-01-01: each
certificate is valid for 825 days from the start of its period, the most
that Apple's systems take, so that two periods overlap by three months, and
a clock that has not been set yet, before 2026, counts as the first period.
The panel shows the fingerprint of the address it was opened at (``tls``,
below), and the boot script prints the one of the management address on the
console, for a person to compare with the browser's.

Mongoose's TLS, of version 7.16, is changed in three places, each marked in
``third_party/mongoose/mongoose.c``: ChaCha20 takes the byte order from the
compiler, as the C library defines ``LITTLE_ENDIAN`` on the big-endian stick
too, where every record came out wrong; the signature of its
CertificateVerify message is encoded as DER asks, without the leading zeros
that made OpenSSL, and BoringSSL with it, refuse about one handshake in 128;
and a malformed key ends the connection rather than leaving it a key of the
wrong bytes.

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
-``, which onu-act runs on the reading end of a pipe pond passes it, the connection pausing while the pipe is full; the answer comes once the
bank was written and read back. One upload runs at a time. ``GET
/api/firmware/download`` streams the running bank as an update file, in chunks.
``POST /api/firmware/try`` arms a start of the bank in the body, ``0`` or ``1``,
or of the other bank with an empty body, and restarts. The upload and the
start need root's password as well as a session, in the header
``X-Pond-Password``, percent-encoded: 403 for a wrong one, which counts as a
wrong sign-in, 429 while sign-ins are stopped.

``/api/set/ID`` takes ``KEY=VALUE`` lines:

* ``settings``: the settings of onu-apply (see ``onu-utils``), the keys of
  ``/run/onu.conf`` (``SESSIONn_GEM``, ``_VLAN``, ``_MAC`` and ``_FAMILY``
  among them) and the accounts, ``PPPOEn_USER`` and ``PPPOEn_SECRET`` for the
  sessions 1 to 7. pond knows none of their rules, nor reads or writes their
  files, which are onu-config's (ADR 0014): ``onu-config change``, which
  onu-act runs for pond, judges the change over the settings in force, says
  why not, which pond answers with, or writes the settings and the accounts
  that changed aside, in ``/run/onu-pending``, for root only; pond then has
  onu-act start ``onu-apply try``, which puts them in force, and does not
  wait for it: the change is on
  trial, and the stick goes back to the settings of before unless it is
  confirmed (``changes``, ADR 0014), which saves them in the store; it
  refuses another change, with 409, until onu-apply has ended. A read gives
  the settings as ``onu-config current`` reads them,
  each session with its user and whether a secret is set, never the secret,
  and what was brought up for it: its link, its address and name servers
  from its interface of netifd (``network.interface.sesN`` on ubus), the
  delegated IPv6 prefix and the host it went to from its dhcp6d
  (``dhcp6d.sesN``), and from ``onu-apply state``, which onu-act runs for
  pond (``settings_state``), what onu-apply keeps of it (the interfaces,
  what it hands down, the gateway the router is given), the address handed
  on, and what became of the settings in the store (``store``: ``loaded``,
  ``defaults``, ``saved``, ``failed`` or ``none``), and each change on trial,
  of the settings, the SFP lane or the management pair, with when it goes
  back and the pair's new address (``trials``). pond reads no file of
  ``/run`` for it.
  The parts of the settings that come with a stick that does the routing
  (``ipoe``, the connections with a fixed address or from DHCP; ``lans``, the
  networks behind the stick with their devices; ``forwards``, ``dmz``, the
  NAT; ``rules`` and ``fw``, the firewall) are in the reply only from
  software that has them. Without them the panel says "Not on the stick yet"
  on those tabs and saves nothing there.
* ``changes``: ``CONFIRM=1`` keeps every change on trial (``onu-change``),
  a new management pair only from a request that came to its new address,
  and ``REVERT=1`` takes them all back; pond then listens on the management
  address in use alone. A confirmation when nothing is on trial any more, as
  after the changes went back, gets 409. The panel confirms them as soon as
  it reaches the stick again.
* ``session``: a PPPoE session of the stick's own: ``ID`` (none for the first
  free one), ``USER``, ``SECRET`` (empty keeps it), ``VLAN``, ``MAC``, ``GEM``,
  ``FAMILY`` (``ipv4``, ``ipv6`` or ``both``), ``PASS`` (``on`` to hand its
  address down to the router, ``off`` to keep it on the stick) and
  ``HOST_VLAN``, the VLAN of the host link it hands down in (nothing for the
  session's own VLAN, ``untagged`` for none). With an account the stick holds
  the session, and in each VLAN towards the host the first that hands down and
  carries IPv4 hands its address to the router; ``USER=`` removes it. The
  panel names no mode: without accounts the router dials through the stick.
* ``identity``: how the stick introduces itself to the OLT, ``SERIAL`` (four
  letters or digits and eight hex digits), ``VENDOR_ID``, ``EQUIPMENT_ID``,
  ``VERSION``, ``EQUIPMENT_SERIAL``, ``MODEL``, ``OMCC_VERSION``,
  ``VENDOR_PRODUCT_CODE`` and ``TM_OPTION`` (numbers, decimal or ``0x``),
  ``LOID``, the logical ONU ID, ``SW_VERSION0`` and ``SW_VERSION1``, each
  software image's version, ``PACK_VERSION``, and ``PLOAM`` (ten characters or
  twenty hex digits) and ``LOID_PASSWORD`` (up to twelve), each kept when
  empty. It goes to ``onu-config identity``, then ``onu-apply reidentify``
  registers again. A read is ``onu-config identity-current`` through
  onu-act, which never gives either password, only whether one is set: root
  alone reads ``/run/onu-identity``.
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
password, only whether one is set; ``tls``, whether the daemon serves HTTPS
and, for each address it does, the port, the certificate's SHA-256 as
browsers show it and the start and end of its validity.

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
The tests of the sign-in run onu-config, behind a stand-in that keeps a new
password, and check the default password, the stop after wrong passwords,
the end of sessions, a change of password and a hash of the older form that
a sign-in replaces; onu-utils tests the hashes themselves.

The tests of HTTPS write a certificate that OpenSSL then reads, its signature
and its fingerprint (``tests/test_cert.c``), and run the daemon with a key on
two addresses of the loopback, one over HTTPS and one over HTTP, which
``tests/test_tls.py`` reaches with Python's ssl module, OpenSSL too: the
handshake, the certificate, a hundred handshakes in a row, the cookie, the
redirect and the refusals; ``openssl`` is needed for them.

The HTTP layer is Mongoose 7.16 by Cesanta Software, in
``third_party/mongoose``, used under the GNU General Public License version 2,
one of the two licences it is offered under, and built with its own TLS,
changed as "HTTPS" says. The tests run on the build machine, little-endian:
the byte order of the stick's own is tried on the stick (``tests/device``,
``mgmt.pond-https``). It holds
micro-ecc by Kenneth MacKay, for its TLS, under the BSD 2-clause licence, whose
notice goes with the binary too (``LICENSES/preferred/BSD-2-Clause.txt``).
