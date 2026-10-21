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

   pond [-l http[s]://ADDRESS:PORT]... [-k KEYFILE] [-c OWNFILE] [-t TOKENFILE]
        [-w WEBDIR] [-u USER]
   pond -k KEYFILE [-c OWNFILE] -F ADDRESS

Each ``-l`` adds an address to listen on, up to four; with none, the daemon
listens on port 8080 of every address. An address it cannot listen on is
reported and left out, and the daemon ends only when it listens on none. An
``https://`` address needs the key of ``-k`` (see "HTTPS"); ``-c`` names the
file of an own certificate, served in place of the daemon's own while it is
there; ``-F`` prints the fingerprint of the certificate of an address, and the
end of its validity, and ends. On the stick ``onu-run`` starts it on the management address, and
on the address of the host link if there is one, each over HTTPS on port 443
and over HTTP on port 80. ``-u`` makes a daemon started as root run as the
account ``USER``, with its groups, keeping of root's powers only the one to
listen on a port below 1024, for its listeners and those of a management pair
that comes later. ``-t`` names the file of the token of scripts (see "The token
of scripts"); without it no token is taken.

What pond changes on the stick, it asks onu-act to do (``onu-utils``, ADR
0011) rather than run the program itself: the SFP lane, the management pair,
the confirmation of the changes on trial, the name, the product name, the
identity and a new registration, the updates' status and trial, the restart,
the keys of the ssh server and of the panel ("The servers' keys"),
and the settings of the data path, which pond reads without the accounts'
secrets and changes on trial, root's password, which onu-config checks and
keeps without pond seeing its hash, and the upload and download of an
update, which stream through a pipe whose other end pond passes to onu-act.
pond runs no program of the stick itself.

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
with (ADR 0009, ADR 0011). ubusd's access list, ``acl.json``, which the image
installs as ``/usr/share/acl.d/pond.json``, lets it call the methods of
onu-act it names and read the state of omcid, of netifd's interfaces and of
dhcp6d, and nothing else; a new password, an update written, a start of
the other bank and every change of the servers' keys need root's password
with the call as well (onu-act checks it), so that pond, taken over, cannot
give itself root, nor a key to log in with; the
boot script gives it a directory of ``/run`` of its own for its log and the
token of scripts, and its group the two copies of the boot loader's
environment to read. Every other value it reads, any user may.

Signing in
==========

One user, ``root``, the stick's one account: the console's login and the ssh
server take the same password (``docs/adr/0009-one-account-root.rst``). Its
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
A change of password ends every other session. pond keeps the sessions in
memory alone, each with the address its sign-in came from, which the panel
lists (``/api/auth/sessions``), and a restart of the daemon ends them all.

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

An own certificate, one that an authority signed, takes the place of those
certificates on every address while the file of ``-c`` holds it and its key
is its own (``src/cert.h``): the file onu-config writes (``onu-config tls
own``, from the store), the certificate's P-256 key in 64 hexadecimal digits
on its first line, as the file of ``-k``, and on its second the certificate
and the CA certificates after it, DER one after another, in base64, at most
8 KiB of DER and eight certificates. Mongoose's TLS signs with a P-256 key
alone, so the certificate's key must be one; the CA certificates may be of
any kind, as only the browser checks them. The daemon sends the certificate
with its chain, each an entry of the handshake's Certificate message, and a
browser that trusts the authority's root then takes it without a warning,
for the names it carries: the address the panel is opened at must be among
its subject's alternative names (``tls`` says, for each address, whether
it is). Without the CA certificates between, a browser that does not have
them already cannot make the path to the root, and warns. A file whose key
is not its certificate's is said in the log and left out; so is a file that
is not one. ``-F`` prints the own certificate's fingerprint while there is
one.

The key and the own certificate change while the daemon runs (``/api/tls``,
below): once onu-config has written the files, the daemon reads them again,
and the connections from then on get the new certificate; the connection
that asked keeps the one it has, and its answer, and a browser warns once
about a certificate it was not shown before, unless it trusts its
authority.

Mongoose's TLS, of version 7.16, is changed in four places, each marked in
``third_party/mongoose/mongoose.c``: ChaCha20 takes the byte order from the
compiler, as the C library defines ``LITTLE_ENDIAN`` on the big-endian stick
too, where every record came out wrong; the signature of its
CertificateVerify message is encoded as DER asks, without the leading zeros
that made OpenSSL, and BoringSSL with it, refuse about one handshake in 128;
a malformed key ends the connection rather than leaving it a key of the
wrong bytes; and the Certificate message carries each certificate of a
chain as an entry of its own, where it sent the chain as one.

Routes
======

``/api/health`` answers anyone. ``POST /api/auth/login`` takes ``{"user",
"secret"}``; ``/api/auth/session`` says who is signed in and whether the
password must change; ``POST /api/auth/logout`` ends the session; ``POST
/api/auth/password`` takes ``{"current", "next"}``. ``/api/auth/sessions``
lists the sessions open, the oldest first, none with its id: ``{"now_s",
"max", "idle_timeout_s", "max_age_s", "sessions": [{"since_s", "seen_s",
"from", "this"}]}``, when each was opened, its last request and the address
its sign-in came from, by the stick's clock, ``this`` for the one that asks;
``POST /api/auth/sessions/others``, with root's password in
``X-Pond-Password`` as the updates take it, ends every one but the one that
asks, all of them for a request with the token, and answers ``{"ok": true,
"ended": N}``: whoever holds a session, a browser left open somewhere,
cannot shut the owner out without the password. ``/api/schema``,
``/api/get/ID``, ``/api/getm?ids=A,B`` (slow values left out) and ``POST
/api/set/ID`` read and change values; ``POST /api/system/restart`` restarts the
stick a second after it answers.

A value is written straight into the send buffer of the connection that asked
for it, which grows with the reply and goes back once the reply has gone: the
daemon keeps no buffer of its own for values, and a large one costs memory
only while it is sent. A value is at most 1.25 MiB of JSON, which the largest
settings and the most leases fit with room to spare (``src/value.h``); one
that would be larger is
not served cut short, but answered with an error that says so (500, and in
``/api/getm`` an object ``{"error": "too large", "detail"}`` in its place), and
the daemon's log says so too.

The updates go through ``onu-update`` (``docs/handbook/updating.rst``). The
value ``firmware`` is its status: which bank runs and which starts next, a trial
under way or one that failed, each bank's state, version, time and sizes, and an
upload under way. ``POST /api/firmware/upload`` takes an update file as the
body, with a Content-Length of at most 4 MiB: once its headers are in, the
connection leaves the HTTP parser and the body streams into ``onu-update write
-``, which onu-act runs on the reading end of a pipe pond passes it, the connection pausing while the pipe is full; the answer comes once the
bank was written and read back. One upload runs at a time. ``GET
/api/firmware/download`` streams the running bank as an update file, in chunks,
named after the product name (``product``, below) and the bank's version:
``buchos-gpon-<version>.bin`` while the name is the default.
``POST /api/firmware/try`` arms a start of the bank in the body, ``0`` or ``1``,
or of the other bank with an empty body, and restarts. ``POST
/api/firmware/return`` makes the shipped firmware of the bank in the body, or
of the other bank, the one the boot loader starts from now on, and restarts:
the way back, which ``onu-update return`` refuses for a bank that does not
hold the shipped firmware whole. ``GET /api/firmware/backup`` streams a copy of
the whole flash, every partition in its order on the chip, to keep before an
update writes over the shipped firmware; it holds the unit's credentials, and
is named the same way, ``buchos-gpon-flash-<version>.bin`` by default. The
upload, the start, the way back and the copy need root's password as well as a
session, in the header ``X-Pond-Password``, percent-encoded: 403 for a wrong
one, which counts as a wrong sign-in, 429 while sign-ins are stopped.

The servers' keys
-----------------

The ssh server's host key, the public keys root logs in to it with, whether
it takes them alone, the panel's key and an own certificate change through
these routes (``src/keys.c``), each a POST with root's password in
``X-Pond-Password``, as the updates take it, besides a session or the token:
a key to log in with gives root, and the servers' keys are what a person
checks a connection by. onu-act has onu-config do the change (``onu-config
ssh``, ``ssh-keys`` and ``tls``, of ``onu-utils``), which checks the
password again and keeps the change in the configuration store before it is
in force, and the answer comes when it has ended:

``POST /api/ssh/host-key``, no body
    A new host key; the ssh server starts again with it, and its sessions go
    on. ``{"ok": true, "host_key": {"type", "fingerprint"}}``, the
    fingerprint ``SHA256:...`` as an ssh client shows it, which changes, so
    that a client that knew the old key refuses to go on until its user
    takes the new one.
``POST /api/ssh/keys``, the body a public key, one line ``TYPE BASE64 [COMMENT]``
    The key added. ``{"ok": true, "key": {"type", "bits", "fingerprint",
    "comment", "source"}}``, the source ``panel``. onu-config takes ed25519,
    ECDSA of nistp256, nistp384 and nistp521 and RSA of 2048 to 4096 bits,
    eight keys at most, and says why not: a type, a size, a key kept already,
    a ninth, a store with no room.
``POST /api/ssh/keys/remove``, the body a key's fingerprint
    The key removed; refused for the last key while the server takes keys
    alone. ``{"ok": true}``.
``POST /api/ssh/keys-only``, the body ``on`` or ``off``
    Whether the ssh server takes keys alone, and no password; refused on
    while no key is kept. The server starts again when it changes.
    ``{"ok": true}``.
``POST /api/tls/key``, no body
    A new key for the panel, which the daemon makes new certificates from;
    while an own certificate is in force, they wait behind it.
``POST /api/tls/certificate``, the body ``{"certificate", "key"}``
    An own certificate: ``certificate`` the server's certificate and the CA
    certificates after it, each a PEM block ``BEGIN CERTIFICATE``, the
    server's first; ``key`` its private key, unencrypted, ``BEGIN EC
    PRIVATE KEY`` (SEC1) or ``BEGIN PRIVATE KEY`` (PKCS#8). The daemon checks
    it before anything is kept: the text, the chain of at most eight
    certificates and 8 KiB, a key of ECDSA P-256, and that the key is the
    certificate's; it refuses an RSA key, another curve, an encrypted key,
    and says why. An expired certificate, or one that does not name the
    address, is taken: ``tls`` shows its end and, for each address, whether
    it names it, for the panel to warn of.
``POST /api/tls/self-signed``, no body
    Back to the certificates of the panel's key.

The three of ``/api/tls`` answer ``{"ok": true, "tls": ...}``, the value
``tls`` as it is once the daemon has read the new files, with the new
fingerprints. Each answers 400 with why for a body it does not take or a
change onu-config refused (its first complaint, the cause), 403 for root's
password missing or wrong, 409 while the same change runs, 413 for a body of
more than 32 KiB, 423 and 429 as the updates do.

``/api/set/ID`` takes ``KEY=VALUE`` lines:

* ``settings``: the settings of onu-apply (see ``onu-utils``), the keys of
  ``/run/onu.conf`` (``SESSIONn_GEM``, ``_VLAN``, ``_MAC`` and ``_FAMILY``
  among them) and the accounts, ``PPPOEn_USER`` and ``PPPOEn_SECRET`` for the
  sessions 1 to 7. pond knows none of their rules, nor reads or writes their
  files, which are onu-config's (ADR 0011): ``onu-config change``, which
  onu-act runs for pond, judges the change over the settings in force, says
  why not, which pond answers with, or writes the settings and the accounts
  that changed aside, in ``/run/onu-pending``, for root only; pond then has
  onu-act start ``onu-apply try``, which puts them in force, and does not
  wait for it: the change is on
  trial, and the stick goes back to the settings of before unless it is
  confirmed (``changes``, ADR 0011), which saves them in the store; it
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
  networks behind the stick, and ``fixed``, their devices' fixed addresses;
  ``forwards``, ``dmz``, the NAT; ``rules`` and ``fw``, the firewall) are in
  the reply only from
  software that has them. Without them the panel says "Not on the stick yet"
  on those pages and saves nothing there.
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
  carries IPv4 hands its address to the router; ``USER=`` removes it, with
  the local networks that go out by it, and ``PASS=on`` takes those networks
  away too, with their fixed addresses, as the session keeps no address on
  the stick for them to go out behind. The panel names no mode: without
  accounts the router dials through the stick.
* ``ipoe``: a connection that takes its address as a router does: ``ID``
  (none for the first free one), ``KIND`` (``static``, an address typed in,
  or ``dhcp``, one from the operator's DHCP server; empty removes it, with
  the local networks that go out by it), ``VLAN``, ``GEM``, ``MAC``,
  ``FAMILY``, and for a static one ``ADDRESS`` (with its prefix length),
  ``GATEWAY``, ``DNS`` (apart by blanks or commas), ``ADDRESS6``,
  ``GATEWAY6`` and ``PREFIX6``, which one by DHCP has none of. Sessions and
  these connections are seven at most together. A read gives each, as
  ``ipoe`` beside the sessions, with what ``onu-apply state`` says it brought
  up: its interfaces, the addresses in effect, the gateway, the name servers
  and the seconds left of a DHCP lease.
* ``lan``: a local network the stick routes out by a connection, ``CONN``
  (``pppoe:N`` or ``ipoe:N``; empty removes the network with its fixed
  addresses), ``ID``, ``NAME``, ``VLAN``, ``ADDRESS``, ``DHCP``,
  ``POOL_FROM``, ``POOL_TO``, ``LEASE``, ``IPV6`` and ``IPV6_SUBNET``; and
  ``fixed``, a device's fixed address in one, ``LAN``, ``MAC``, ``IP`` (empty
  removes it, with the forwards to the device and the DMZ that is the device,
  which go to that address) and ``NAME``. The devices that have an address
  from a network's DHCP server are not among the settings but a value of
  their own, ``leases``, read only: each with its network, MAC, address, the
  name it gave and the seconds its lease has left, of udhcpd's lease file of
  each network set up. The panel polls the settings every 5 s, and reads
  the leases only on the pages that show them, when such a page opens and
  every 30 s while it is open: 512 devices in each of eight networks, each
  with a name that needs escaping, come to 836 KiB of JSON, beside 81 KiB of
  the largest settings.
* ``forward``: a port, or a range of ports, forwarded from the internet to a
  device of a local network: ``ID`` (none for the first free one, of 96),
  ``CONN``, the connection it comes in by (empty removes it), ``NAME``,
  ``PROTO`` (``tcp``, ``udp`` or ``both``), ``EXT_FROM`` and ``EXT_TO``, the
  device by ``LAN`` and ``MAC``, ``INT_PORT``, its port (a range goes to as
  many from it, the same or others; the ranges moved to others move 1024
  ports at most, a range of both protocols twice), and ``ON`` (``off`` keeps
  it, not in force); and ``dmz``, a
  connection's DMZ, the device that takes what no forward takes, ``CONN``,
  ``LAN`` and ``MAC`` (empty removes it). The device keeps its address: the
  one its lease gives it becomes its fixed address in the same write when it
  has none. A read gives them as ``forwards`` and ``dmz``; a network removed
  takes its forwards and its DMZ with it, and so does a device's fixed
  address taken away, the device's. They take no entry of the switch's
  ACL table: netfilter translates a connection's first packet, and its
  flowtable then offers the flow to the switch as any other.
* ``rule``: a rule of the firewall, the first that traffic matches deciding:
  ``ID`` (none adds one at the end, with the ID after the largest),
  ``NAME``, ``ACTION`` (``allow`` or ``block``; empty removes the rule),
  ``FROM`` and ``TO`` (``internet``, ``internet:ADDR``, ``lans``, ``lan:N``
  or ``dev:N:MAC``), ``PROTO`` (``any``, ``tcp``, ``udp``, ``both`` or
  ``icmp``), ``PORTS`` (a port or ``A-B``), ``FAMILY`` (``any``, ``ipv4``
  or ``ipv6``) and ``ON``; ``MOVE=up`` or ``MOVE=down`` with ``ID`` moves it
  one place. A rule keeps its ID wherever it moves; 96 at most. A device a
  rule goes to keeps its address, as a forward's does; a network removed
  takes the rules that name it or a device of it, and a fixed address taken
  away the rules that go to its device. ``fw``: what the firewall does where
  no rule decides, ``OUT`` to the internet and ``BETWEEN`` the networks,
  and whether the stick answers a ping, ``PING_IN`` from the internet (by
  the sessions and the IPoE connections) and ``PING_LANS`` from the local
  networks and the router a session hands its address to, each ``allow``
  or ``block``, the pings ``block`` by default; on the management VLAN the
  stick answers a ping always, and its servers, this daemon among them,
  answer there alone (``onu-utils/README.rst``, ``onu-fw``). A read gives
  them as ``rules``, in their order, and ``fw``, with ``ping_in`` and
  ``ping_lans``. The rules are the switch's ACL's, the defaults netfilter's;
  a rule that allows opens netfilter's default for IPv4 as well (ADR 0019).
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
* ``product``: the name the panel shows for the system, ``PRODUCT_NAME``: 1 to
  20 characters of ASCII, letters, digits, blanks, hyphens and dots, a letter
  or a digit first and last (``src/providers.h`` says why), or empty for the
  default, ``BuchOS``, the author's own name for the system. It goes to
  ``onu-config system``, which keeps it as ``SYS_PRODUCT_NAME`` and puts it in
  ``/run/onu-system``. A read gives the name in force from that file, any
  user may read it, and ``BuchOS`` without one, or for one that is no name.
  The files pond hands out begin with its slug: its letters in lower case and
  its digits, each run of the other characters one hyphen (``Fibre Box 2.0``
  gives ``fibre-box-2-0-gpon-<version>.bin``). It labels the panel and its
  downloads alone: the host name, the identity towards the OLT and what the
  DHCP clients send do not change with it.
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
password, only whether one is set; ``omci_frames``, the conversation with
the OLT as omcid keeps it (method ``frames``): its last frames, each with the
time it passed and what omcid did with it, and with ``?since=N`` in the
address only those from number N on, so that a reader that follows the
conversation comes back for what is new; ``tls``, whether the daemon serves HTTPS,
the own certificate or null (its subject and issuer as words, its SHA-256,
the start and end of its validity, the certificates of its chain and its
alternative names), and, for each address it serves over HTTPS, the port, the
certificate's SHA-256 as browsers show it, the start and end of its validity
and whether it names the address (``named``, always for the daemon's own);
``ssh``, the ssh server's host key (its type and fingerprint, or null), the
public keys root logs in with (``keys``: type, bits, fingerprint, comment and
``source``, ``panel`` for those of the store, which the routes below change,
``image`` for those of the image's ``/root/.ssh/authorized_keys``, which the
server takes too and nothing on the stick changes, the type ``unknown`` for
a line of that file the store would not take), whether it takes them alone
(``keys_only``) and how many of the store's it may keep (``keys_max``), as
``onu-config ssh show`` says them through onu-act, which alone reads them;
``clock``, the time
the stick's clock reads and what timed says of it in ``/run/timed/state``
(``timed/README.rst``): the source in use, ``ptp``, ``ntp``, ``olt`` or
``none``, the state (``locked``, ``syncing``, ``set``, ``holdover`` or
``unsynchronized``), the offset of the last sample, the correction of the
clock's frequency in parts per billion, and for each source it asks
what it last gave, null for one it does not ask; without the file, as before
timed starts, the source and the state are null.

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
front of it, and the dev server stops without it). ``npm run build:demo``
writes ``web/dist-demo``, the panel with the same mock stick answering it in
the browser, for a static host (``POND_DEMO_BASE`` is the path it is served
from); ``.github/workflows/demo.yml`` puts it on the repository's GitHub Pages
site. Nothing of the mock reaches ``web/dist``, the panel the stick serves.

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
``POND_ONU_MGMT``, ``POND_HOSTNAME``), and those of ``omci`` and
``omci_frames`` read omcid's answers from ``run/ubus/omcid.mib`` and
``run/ubus/omcid.frames``.
The tests of the sign-in run onu-config, behind a stand-in that keeps a new
password, and check the default password, the stop after wrong passwords,
the end of sessions, their list and the end of the others, a change of
password and a hash of the older form that a sign-in replaces; onu-utils
tests the hashes themselves.

The tests of HTTPS write a certificate that OpenSSL then reads, its signature
and its fingerprint (``tests/test_cert.c``), and run the daemon with a key on
two addresses of the loopback, one over HTTPS and one over HTTP, which
``tests/test_tls.py`` reaches with Python's ssl module, OpenSSL too: the
handshake, the certificate, a hundred handshakes in a row, the cookie, the
redirect and the refusals; ``openssl`` is needed for them. An own
certificate is tested with those ``tests/make-certs.sh`` makes with openssl,
as an authority would, a root, an intermediate and the server's: taken and
refused (``tests/test_cert.c``), and served by the daemon, through which a
client that trusts the root alone verifies it by the address, which it
cannot without the intermediate (``tests/test_tls.py``); the routes of the
servers' keys run against a stand-in for onu-config, and those of the
sessions with a password of root's own.

The HTTP layer is Mongoose 7.16 by Cesanta Software, in
``third_party/mongoose``, used under the GNU General Public License version 2,
one of the two licences it is offered under, and built with its own TLS,
changed as "HTTPS" says. The tests run on the build machine, little-endian:
the byte order of the stick's own is tried on the stick (``tests/device``,
``mgmt.pond-https``). It holds
micro-ecc by Kenneth MacKay, for its TLS, under the BSD 2-clause licence, whose
notice goes with the binary too (``LICENSES/preferred/BSD-2-Clause.txt``).
