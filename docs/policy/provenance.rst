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

#. Decompiled or disassembled code, pseudo-code, call graphs, traces or logs
   of another party's software; the names of reverse-engineering tools cited
   as a source.
#. Another party's binaries, firmware images, or dumps of any kind -- flash,
   EEPROM, or register snapshots taken while other firmware was running -- and
   calibration files taken from another implementation.
#. Another party's function, symbol, module, file, type and enumeration names;
   its code addresses; its source paths; its internal code names.
#. Text, tables, enumerations or data copied from leaked SDK material or from
   other non-public documents -- including register and field names taken from
   such tables, management-entity tables and attribute strings, and non-public
   sibling datasheets.
#. Narrative about another party's implementation -- statements of the form
   "the vendor does X", or comparisons with its behaviour -- and investigation
   diaries.
#. Unit identifiers and secrets: device serial numbers, GPON serial numbers,
   registration passwords, MAC addresses of real equipment, laboratory IP
   addresses and host names, and captures of an operator's OLT.
#. Internal references: issue numbers of a private tracker, paths into the
   laboratory repository, and the laboratory repository's own name.

Grey zone, decided
------------------

Interoperability identifiers imposed by the bootloader -- partition offsets and
the environment-variable names our tooling must set -- may be stated as
interface facts, without describing the bootloader's internals. Prefer our own
configuration (device-tree partitions) wherever it removes the need to state
them at all.

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
  generic word, but to guarantee that no name was transcribed from non-public
  material.
* Only the registers the driver uses are defined; unused macros are removed.
* Renaming or removing a macro must not change the object code -- that is how
  the naming is proven correct.
* Debug, log and devlink strings use the same names.

Documents and standards
=======================

Standards are cited by number and clause. A third-party datasheet is cited only
when it is publicly downloadable, by title and revision. A preliminary
datasheet obtained through a non-public channel is not cited unless its
publisher has since released it.

Unit identifiers, credentials, captures
=======================================

The GPON serial number and the registration password come from a per-device
configuration written at install time; they never come from the repository.
Examples use ranges reserved for documentation:

* MAC addresses from the IEEE documentation range (see RFC 7042).
* IPv4 addresses from the ranges RFC 5737 reserves for documentation:
  192.0.2.0/24, 198.51.100.0/24 and 203.0.113.0/24.

Captures used in tests are synthetic -- built from the standard -- or recorded
from our own stack against the laboratory OLT, with serial numbers scrubbed.

Two contexts, one procedure
===========================

#. **Extraction** (on the laboratory side, which may read anything): a
   specification is written -- one fact sheet per hardware block, covering
   every register, field, value and sequence the driver uses, in neutral
   words, each with its verification tag. A fact sheet must pass the gates
   below before it leaves the laboratory.
#. **Authoring** (in this repository): comments, commit messages and
   documentation are written from the fact sheets, the public standards and the
   comment-stripped code only. Authors do not receive the old comments or the
   old commit messages -- the phrasing that must not carry over is kept out of
   their input.
#. **Audit**: before a change is merged, the plaintext audit runs on the
   laboratory side against this repository, and the hashed gates run in
   continuous integration.

This separates expression. It does not, by itself, make the same people a legal
clean room (see `A note on scope`_).

Gates
=====

Each gate ships with a seeded-violation test that makes it fail on purpose.

* **Hashed deny-list.** Every identifier-like token of every tracked text file
  and commit message is hashed and checked against a list generated on the
  laboratory side from non-public name tables. Tokens present in pristine
  upstream, in the standards' vocabularies, or in a dictionary are excluded.
  This repository holds only the hashes, never the plaintext list.
* **Forbidden words.** Plain regular expressions for reverse-engineering
  vocabulary, for comparative narrative about another party, for references to
  a non-public source, and for code addresses outside an allow-list of
  documented memory-map constants.
* **Unit identifiers.** GPON-serial patterns; MAC addresses outside the
  documentation, multicast and broadcast ranges; private IPv4 ranges;
  device-serial patterns.
* **Binaries and size.** No file with NUL bytes and no file above a size cap,
  except an allow-list in which every entry carries a provenance line.
* **Commit messages.** A component subject prefix, imperative mood, and a
  length limit; a ``Signed-off-by:`` line present; no issue numbers, no
  laboratory paths and no forbidden words.
* **One component per commit.**
* **Licensing.** ``reuse lint``.
* **Laboratory-side audit.** Plaintext names; a name join against the
  non-public tables; and object disjointness -- no stored object of this
  repository equals a stored object of the laboratory repository unless it
  equals a pristine upstream file.

Git history
===========

This repository starts from an empty history, and never from a filtered copy, a
branch or a fork of the laboratory repository. It shares no stored object with
the laboratory repository except pristine upstream files, links to no
laboratory repository, and cites none of their revision identifiers. Commits
are dated when they are made -- nothing is back-dated. The object-disjointness
audit runs before the first publication and before every release.

Commit messages
===============

* **Kernel** commits (under ``linux/``) use the kernel subject form
  ``subsystem: component: imperative summary``. The body states the problem,
  then the change in hardware terms; cites standards by clause; and gives
  measurements as results on the named board, with counts. It never contains
  comparative narrative, reverse-engineering vocabulary, another party's names
  or addresses, investigation history, issue numbers, laboratory paths or
  addresses, or unit identifiers.
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
sufficient for publication -- in particular regarding register information
whose names may coincide with non-public material, and regarding trademarks --
is a question for counsel in the owner's jurisdiction before the first public
release.
