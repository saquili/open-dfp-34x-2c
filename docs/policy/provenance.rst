.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=================
Provenance policy
=================

:Status: adopted

This is the rule for what may and may not enter open-dfp-34x-2c, and how the
things that may enter are written. It is normative: the categories below are
enforced by the gates named at the end, and each gate ships with a
seeded-violation test that makes it fail.

Scope
=====

This policy applies to every file, every commit message, every generated
artefact and every release asset of open-dfp-34x-2c.

What may enter
==============

#. Code written for this project.
#. Upstream open-source code under GPL-2.0-compatible licences, unchanged or
   patched, with its origin stated -- the kernel, the distribution build
   system and the packages fetched by hash.
#. **Hardware facts** -- addresses, offsets, bit positions, values, required
   sequences, timings, observed behaviour -- stated in our own words, each
   carrying a way to verify it (see `How hardware facts are written`_).
#. Public standards and their terminology: ITU-T G.984.x, ITU-T G.988,
   IEEE 802.3, IEEE 802.1Q, IEEE 1588, SFF-8472, SFF-8431, JEDEC.
#. Publicly downloadable datasheets of third-party parts, cited by title and
   revision, and only after their public availability has been confirmed.
#. Our own measurements -- procedures and results -- and test vectors we
   generate from a standard or capture from our own stack.

What must not enter
===================

The list below describes categories, so that they can be recognised and
refused. It deliberately contains no concrete instance of any of them.

#. Code, pseudo-code, call graphs, traces or logs of another party's
   software, whether taken from its source or recovered from its binaries;
   the names of the tools that recover them, cited as a source.
#. Another party's binaries, firmware images, or dumps of any kind -- flash,
   EEPROM, or register snapshots taken while other firmware was running --
   and calibration files taken from another implementation.
#. Another party's function, symbol, module, file, type and enumeration
   names; its code addresses; its source paths; its internal code names.
#. Text, tables, enumerations or data copied from documents or software kits
   that their owners have not published -- register and field names,
   management-entity tables and attribute strings among them -- and
   datasheets that their publishers have not released.
#. Narrative about another party's implementation -- statements of the form
   "their driver does X", or comparisons with its behaviour -- and
   investigation diaries.
#. Unit identifiers and secrets: device serial numbers, GPON serial numbers,
   registration passwords, MAC addresses of real equipment, the lab's IP
   addresses and host names, and captures of an operator's OLT.
#. Internal references: issue numbers of a private tracker, paths on the
   maintainer's machines or in the lab, and the names of the lab's own
   repositories.

Grey zone, decided
------------------

Interoperability identifiers imposed by the bootloader -- partition offsets and
the environment-variable names our tooling must set -- may be stated as
interface facts, without describing the bootloader's internals. Prefer our own
configuration (device-tree partitions) wherever it removes the need to state
them at all.

The firmware a stick ships with -- the shipped firmware -- stays in its other
bank as the way back, and its web page is one way in. What this system must
know to live beside it may be stated the same way, as interface facts: the
format of the update its web page takes, the partitions and files it owns and
this system leaves alone, and how it splits a bank. Its behaviour at that
interface -- that it keeps to its own files, that it installs such a package
-- may be stated as a measured or tested fact, with its tag. How it works
inside, its versions and builds by name, and how it behaves where this system
does not depend on it, are not stated.

The names of the shipped firmware's files, and of the keys in them, that this
system reads for compatibility may be named in code and documentation: the
files ``lastgood.xml`` and ``lastgood_hs.xml``, and keys such as ``GPON_SN``,
``SUSER_PASSWORD`` and ``ELAN_MAC_ADDR``. Importing the owner's identity,
password and MAC address from the shipped firmware needs them. Its versions
and builds are still not named. (Decided by the maintainer on 2026-10-20.)

How hardware facts are written
==============================

* In code, a comment says what the hardware does and why the code does this;
  it never says where the knowledge came from.
* In documentation, a fact carries a **tag** that names how it can be checked
  again. Only these tags are admissible:

  ===  =====================================================================
  Tag  Meaning
  ===  =====================================================================
  M    Measured on the board. Names the selftest, or the documented command
       sequence, taken through the driver's own interfaces.
  T    Tested by function: the driver depends on it and a named test fails if
       it is wrong. Names the test.
  P    Public standard. Names the document and clause.
  I    Inferred, not verified. Names what would confirm or refute it, and is
       never load-bearing without saying so.
  ===  =====================================================================

* Facts the driver does not use, and that nobody measured, are omitted rather
  than imported. The documentation describes what this project uses and has
  verified -- not an exhaustive register map.

Register and identifier naming
==============================

* Every register and field macro is named by us, systematically and from
  function: ``RTL9602C_<BLOCK>_<REG>`` and ``..._<REG>_<FIELD>``, with block
  mnemonics we choose, and with standards terms wherever a register implements
  a standard concept (GTC, BWMAP, PLOAM, GEM, T-CONT, Alloc-ID, ONU-ID, EqD;
  the IEEE and SFF names). The aim is not to avoid every coincidence with a
  generic word, but to guarantee that every name is our own and none was
  transcribed from another party's material.
* Only the registers the driver uses are defined; unused macros are removed.
* Renaming or removing a macro must not change the object code -- that is how
  the naming is proven correct.
* Debug, log and devlink strings use the same names.

Documents and standards
=======================

Standards are cited by number and clause. A third-party datasheet is cited only
when it is publicly downloadable, by title and revision. A datasheet is not
cited before its publisher has released it to the public.

Unit identifiers, credentials, captures
=======================================

The GPON serial number and the registration password come from a per-device
configuration written at install time; they never come from the repository.
Examples use ranges reserved for documentation:

* MAC addresses from the IEEE documentation range (see RFC 7042).
* IPv4 addresses from the ranges RFC 5737 reserves for documentation:
  192.0.2.0/24, 198.51.100.0/24 and 203.0.113.0/24.

Captures used in tests are synthetic -- built from the standard -- or recorded
from our own stack against the lab's OLT, with serial numbers scrubbed.

The lab and this repository
===========================

The hardware is measured and tested in the project's local lab. What goes
from there into this repository takes one way:

#. **Fact sheets** (in the lab): what the driver needs is written down -- one
   fact sheet per hardware block, covering every register, field, value and
   sequence the driver uses, in neutral words, each with its verification
   tag. A fact sheet must pass the gates below before it leaves the lab.
#. **Authoring** (in this repository): comments, commit messages and
   documentation are written from the fact sheets, the public standards and
   our own code only. Notes, logs and drafts kept for other purposes are not
   copied in.
#. **Checks**: before every push, the maintainer runs every gate on their
   machine, with the list and the path patterns kept in the lab; continuous
   integration runs every other gate on every change.

Gates
=====

Each gate ships with a seeded-violation test that makes it fail on purpose. A
list kept in the project's local lab, and the patterns of the lab's own paths,
are not part of this repository: they are checked on the maintainer's machine
before every push. Continuous integration, which does not have them, runs
every other gate.

* **Hashed deny-list.** Every identifier-like token of every tracked text file
  and commit message is hashed and checked against the list kept in the lab.
  Tokens present in pristine upstream, in the standards' vocabularies, or in a
  dictionary are left off it. This repository holds the gate and its tests,
  never the list.
* **Forbidden words.** Plain regular expressions for the vocabulary and the
  tool names of recovering code from binaries, for comparative narrative about
  another party, for references to unpublished material, for paths on a
  person's own machine, and for code addresses outside an allow-list of
  documented memory-map constants; and, with the patterns kept in the lab,
  for the lab's own paths.
* **Unit identifiers.** GPON-serial patterns; MAC addresses outside the
  documentation, multicast and broadcast ranges; private IPv4 ranges;
  device-serial patterns.
* **Binaries and size.** No file with NUL bytes and no file above a size cap,
  except an allow-list in which every entry carries a provenance line.
* **Commit messages.** A component subject prefix, imperative mood, and a
  length limit; a ``Signed-off-by:`` line present; no issue numbers, no paths
  of the lab and no forbidden words.
* **One component per commit.**
* **Licensing.** ``reuse lint``.

Git history
===========

This repository starts from an empty history. It is never made from a
filtered copy, a branch or a fork of another repository; it shares no stored
object with one except pristine upstream files, links to no private
repository and cites no revision of one. Commits are dated when they are made
-- nothing is back-dated. This is checked before the first publication and
before every release.

Commit messages
===============

* **Kernel** commits (under ``linux/``) use the kernel subject form
  ``subsystem: component: imperative summary``. The body states the problem,
  then the change in hardware terms; cites standards by clause; and gives
  measurements as results on the named board, with counts. It never contains
  comparative narrative, the vocabulary of recovering code from binaries,
  another party's names or addresses, investigation history, issue numbers,
  paths or addresses of the lab, or unit identifiers.
* **Userspace and distribution** commits use their component's prefix
  (``omcid:``, ``package/omcid:``, ``board/<board>:``, ``configs/<board>:``,
  ``tools/bench:``, ``docs:``).
* **Trailers.** A ``Signed-off-by:`` by the human author (Developer
  Certificate of Origin), using the identity meant to be public; ``Link:`` only
  to public URLs; ``Fixes:`` only for commits of this repository. Tool
  assistance is credited with a ``Co-Authored-By:`` trailer, not
  ``Co-developed-by:`` -- which the kernel defines as authorship that must be
  followed by that co-author's own ``Signed-off-by:``.

A note on scope
===============

This is an engineering provenance policy, not legal advice. It keeps hardware
facts and our own expression and removes everything else. Whether that is
sufficient for publication -- in particular regarding register names that may
coincide with names used elsewhere, and regarding trademarks -- is a question
for counsel in the owner's jurisdiction before the first public release.
