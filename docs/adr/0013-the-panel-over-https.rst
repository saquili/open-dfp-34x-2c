.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=============================================================
ADR 0013: Serve the panel over HTTPS with the stick's own key
=============================================================

:Status: Accepted
:Date: 2026-10-11
:Deciders: the maintainer

Summary
=======

The panel is encrypted. The stick generates its own key at its first start
and keeps it, signs its own certificate with it, and redirects anyone who
opens the panel without encryption to the encrypted address. The browser
warns once, because no authority signed the certificate; the panel and the
console show its fingerprint to compare.

Context
=======

The panel takes root's password, the stick's one account (ADR 0009). Over
plain HTTP the password would cross the host link in the clear, unless a
proxy in front of the stick encrypted it, which a bench may have and an
owner does not.

Options considered
==================

The status daemon is built on Mongoose, which offers TLS through mbedTLS,
OpenSSL or wolfSSL, or through a TLS 1.3 implementation of its own:

Mongoose's own TLS 1.3
    Measured for the stick, about 22 KiB more in the system partition,
    compressed. Chosen.
mbedTLS
    An estimated 100 to 200 KiB, of the 743,747 bytes (about 726 KiB) left
    in the partition for the root file system on 2026-10-11.
OpenSSL and wolfSSL
    Under the Apache License 2.0 and the GPL version 3 respectively, neither
    of which GPL-2.0-only code can take (ADR 0002).

Decision
========

* The status daemon serves the panel and its API over HTTPS on port 443,
  with Mongoose's own TLS 1.3. On port 80 it only redirects the browser to
  the same path over HTTPS, before it reads a request's body;
  ``/api/health`` still answers over HTTP, for the trial of a new firmware
  (``onu-trial``). No Strict-Transport-Security header is sent: with a
  certificate no authority signed, a browser would then not let its user
  proceed.
* The key is a P-256 scalar that ``onu-config tls`` generates at the first
  start from ``/dev/urandom`` and keeps in the configuration store
  (``TLS_KEY``), outside every setting a person sees or saves; the boot
  script gives the status daemon a copy that only it can read.
* From that key the status daemon makes a self-signed certificate for each
  address it answers on, with the address as the subject alternative name.
  The signature is deterministic, and validity follows a calendar of
  two-year periods from 2026-01-01, each certificate valid for 825 days: the
  same key and address give the same certificate, and the same fingerprint,
  for two years, even on a stick whose clock is not set until ntpd sets it.
  The panel shows the fingerprint for the address it was opened at, and the
  boot script prints the one for the management address on the console.
* Three faults found in Mongoose's TLS are fixed in its copy in this tree,
  each marked there: on the big-endian stick ChaCha20 took the little-endian
  path, because the C library defines ``LITTLE_ENDIAN`` everywhere, so no
  record could be read; the signature of its CertificateVerify was not DER,
  which made OpenSSL refuse 4 of 400 handshakes (none of 1,000 after the
  fix); and a malformed key was used as a key of the wrong bytes.
* The boot script lets every account listen from port 80 upwards, so that
  the status daemon, which runs as an account of its own (ADR 0011), can
  listen on 80 and 443; the stick runs no program of a user's own.

Consequences
============

* A browser warns at the first visit to each address of the stick, and at
  the start of each two-year period, until its user accepts the
  certificate; a person who wants no warning can make the browser trust it
  for its address.
* Scripts reach the API over HTTPS and accept the stick's certificate, as
  curl's ``-k`` does, or compare its fingerprint with
  ``pond -k KEYFILE -F ADDRESS`` on the stick.
* A store that loses the key, or a stick without a store, gets a new key and
  thus new certificates, which the browser warns about again.
* A handshake takes about 0.1 s of the stick's processor (92 ms measured
  from the host), and the panel's 158 KB script arrives in 0.15 s with it.
* Mongoose's TLS is young code: it speaks only TLS 1.3 with X25519,
  ChaCha20-Poly1305 and a P-256 key, which every current browser supports,
  and an update of Mongoose has to carry the three fixes over or show that
  they were made upstream.
