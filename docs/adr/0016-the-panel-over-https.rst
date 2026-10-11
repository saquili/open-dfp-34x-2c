.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========================================================
ADR 0016: The panel answers over HTTPS with its own key
=========================================================

:Status: Accepted
:Date: 2026-10-11
:Decided-by: the owner, 2026-10-11

.. note::

   In plain words: the panel is encrypted. The stick makes its own key the
   first time it starts and keeps it, signs its own certificate with it, and
   sends anyone who opens the panel without encryption to the encrypted
   address. The browser warns once, since no authority signed the
   certificate; the panel and the console show its fingerprint to compare.

Context
=======

The panel took root's password, the one account of the stick (ADR 0012),
over plain HTTP on port 8080. Only a proxy in front of it, which a bench had
and a buyer has not, kept the password off the link in the clear.

The status daemon is built on Mongoose, which offers TLS through mbedTLS,
OpenSSL or wolfSSL, or through a TLS 1.3 of its own. Measured for the stick,
Mongoose's own adds about 22 KiB to the system partition, compressed; mbedTLS
would add an estimated 100 to 200 KiB, of the 743 KiB left. OpenSSL is under
the Apache licence 2.0 and wolfSSL under the GPL version 3, neither of which
GPL-2.0-only code can take (ADR 0002).

Decision
========

* The status daemon serves the panel and its API over HTTPS on port 443, with
  Mongoose's own TLS 1.3, and answers on port 80 only to send the browser to
  the same path over HTTPS, before it reads a request's body. ``/api/health``
  still answers over HTTP, for the trial of a new firmware (onu-trial). No
  Strict-Transport-Security is sent: with a certificate no authority signed,
  a browser would then not let its user go on.
* The key is a scalar of the curve P-256 that ``onu-config tls`` makes at the
  first start from ``/dev/urandom`` and keeps in the configuration store
  (``TLS_KEY``), out of every setting a person sees or saves; the boot script
  gives the status daemon a copy that only it reads.
* The status daemon makes, from that key, a certificate for each address it
  answers on, with the address as its subject's alternative name, signed by
  itself. Its signature is deterministic and its validity follows a calendar
  of two-year periods from 2026-01-01, each certificate valid for 825 days:
  the same key and address give the same certificate, and the same
  fingerprint, for two years, on a stick whose clock is not set until ntpd
  sets it. The panel shows the fingerprint of the address it was opened at,
  and the boot script prints the one of the management address on the
  console.
* Three faults found in Mongoose's TLS are fixed in its copy in this tree,
  each marked there: on the big-endian stick ChaCha20 took the little-endian
  path, as the C library defines ``LITTLE_ENDIAN`` everywhere, and no record
  could be read; the signature of its CertificateVerify was not DER, which
  made OpenSSL refuse 4 of 400 handshakes and, after the fix, none of 1,000;
  and a malformed key went on as a key of the wrong bytes.
* The boot script lets every account listen from port 80 up, so that the
  status daemon, which runs as an account of its own (ADR 0014), listens on
  80 and 443: the stick runs no program of a person's own.

Consequences
============

* A browser warns at the first visit to each address of the stick, and at
  the start of each two-year period, until its user accepts the certificate;
  a person who wants no warning can make their browser trust it, for its
  address.
* Scripts reach the API over HTTPS and accept the stick's certificate, as
  curl's ``-k`` does, or compare its fingerprint with ``pond -k KEYFILE -F
  ADDRESS`` on the stick.
* A store that loses the key, or a stick without a store, gets a new key and
  so new certificates, which the browser warns about again.
* A handshake takes about 0.1 s of the stick's processor (92 ms measured from
  the host side), and the panel's script of 158 KB arrives in 0.15 s with it.
* Mongoose's own TLS is young code: it speaks only TLS 1.3 with X25519,
  ChaCha20-Poly1305 and a P-256 key, which every current browser does, and an
  update of Mongoose has to carry the three fixes over or show them made.
