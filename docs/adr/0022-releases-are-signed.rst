.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===================================================================
ADR 0022: Sign each release, and say what a file is signed with
===================================================================

:Status: Accepted
:Date: 2026-10-22
:Deciders: the maintainer
:Supersedes: ADR 0014

Summary
=======

The release workflow signs the update file of each release with an Ed25519
key that only it holds. The stick checks the signature with the keys its
image trusts, and says what a file, and each bank, is signed with; the panel
shows that beside each system, with the system's SHA-256, the digest a
release lists. A file without a signature, such as an owner's own build,
still installs, after a warning; a signature that does not match its file is
refused.

Context
=======

ADR 0014 decided that update files carry no signature: the stick installs
any file that is intact and made for its board, from anyone who knows root's
password. A person who wanted to know whether a file was a release compared
its SHA-256 with the release's ``SHA256SUMS`` by hand; the panel showed a
system's version and a time, but nothing that told a release from any other
build of the same source.

The owner asked, on 2026-10-22, for the SHA-256 and the signature of each
system on the panel's page of the software: the SHA-256 of the update file,
so that it compares with the release at a glance, and a signature made by the
release workflow, which the panel shows, with a warning for a file that has
none.

An ``onu-update`` of before this decision refuses a header whose flags or
reserved bytes are not zero, and a file that goes on after its checksum, so
a signature has no room there. It takes a root filesystem part longer than
the SquashFS image in it, and writes all of it into the bank.

Decision
========

* The signature is a block of 128 bytes right after the SquashFS image,
  inside the root filesystem's part, which the header's length counts
  (``onu-utils/src/update.h``): the magic ``ONUS``, a format version, the
  SHA-256 of the raw public key, and the Ed25519 signature (RFC 8032) of the
  text ``open-dfp-34x-2c update`` with its NUL, followed by the SHA-256 of
  the header, the kernel image and the SquashFS image. An older
  ``onu-update`` installs a signed file as it is; the bank keeps the block
  behind the root filesystem, where the kernel does not look, so that the
  bank read back is the signed file, byte for byte.
* The key's private half is the repository's secret ``RELEASE_SIGNING_KEY``,
  which the workflow's last job alone reads, for the time of one step, and
  a copy the maintainer keeps away from any computer that builds. Its
  public half is in ``br2-external/board/dfp-34x-2c3/overlay/etc/
  onu-update.keys``, which the image carries.
* The release workflow (``.github/workflows/release.yml``) signs
  ``update.bin`` once the two builds compared equal, with
  ``br2-external/board/dfp-34x-2c3/signupdate.py``, which leaves the
  signature itself to OpenSSL; it checks the signed file with the image's
  keys, packs ``web-install.tar`` again with the signed root filesystem, so
  that a bank the shipped firmware's page writes is signed too, and lists
  the digests anew. ``r0.squashfs`` stays the image the build made. A
  release is not made without the key.
* ``onu-update`` checks a signature with the keys of ``/etc/onu-update.keys``
  and Monocypher 4.0.2: ``check``, ``write``, ``read`` and ``digest`` say
  ``SIGNATURE=valid`` with the key's name, ``none``, ``unknown`` for a key the
  image does not trust, or ``bad``, and the file's SHA-256. ``check`` and
  ``write`` refuse a bad signature; every other file is written as before.
  The boot script has each bank read once after a start, in the background,
  and the status gives the readings.
* The panel shows each system's SHA-256 and signature. Before an upload, the
  browser reads the file, its version, its SHA-256 and its signature, which
  it checks with the stick's keys, and the confirmation says what it found:
  a file with no signature, or one of a key the stick does not trust, is
  written only after a warning, and one with a bad signature is not sent.
* Writing an update and starting the other bank still need root's password,
  as ADR 0014 had it.

Consequences
============

* The stick still belongs to its owner: whoever knows root's password can
  install any system, and an own build installs as a release does. The
  signature tells, it does not forbid.
* A release changed after it was signed is refused, even with a checksum
  made anew to match the change.
* The key has to be kept. To change it, a release whose image trusts the new
  key is signed with the old one, and the releases after it with the new
  one; a stick trusts the keys of the system it runs. A key that leaks lets
  anyone make files that a stick calls releases until it runs an image
  without that key.
* Each bank is read once after a start, some 3 s at the lowest priority,
  and after each write; until then the panel says that it is reading.
* ``onu-update`` carries Monocypher's Ed25519 check and SHA-512, some 21 KB.
* A bank that a release workflow of before this decision wrote, or that the
  bench wrote from ``r0.squashfs``, has no signature: the panel says so.
