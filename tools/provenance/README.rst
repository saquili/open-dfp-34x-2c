.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

================
tools/provenance
================

The gates that enforce ``docs/policy/provenance.rst``: a tokeniser with a
hashed deny-list, forbidden words, unit identifiers, binaries and size, a
commit-message lint and the one-component-per-commit rule. They need Python
3.11 or later and nothing outside its standard library.

This repository holds only hashes and generic patterns. The plaintext name
lists, the generator of ``denylist.sha256`` and the plaintext audit stay on
the laboratory side.

Running
=======

``check`` is the one entry point::

    tools/provenance/check                    # = all: the tree, then main..HEAD
    tools/provenance/check all --base origin/main
    tools/provenance/check all --history      # the tree and every commit
    tools/provenance/check tree [--untracked] [PATH...]
    tools/provenance/check range main..HEAD
    tools/provenance/check changes BASE [REV]
    tools/provenance/check message .git/COMMIT_EDITMSG
    tools/provenance/check selftest

Options come before the command: ``--repo DIR`` (default: the current
repository), ``--gates g1,g2`` (default: all), ``--size-cap BYTES``,
``--denylist FILE``, ``--allowlist FILE``, ``--format json`` and ``-v``,
which also lists what the allowlist let through and which entries matched
nothing. The exit status is 0 without findings, 1 with findings and 2 for a
usage or configuration error.

To check each message as it is written, install the command as a
``commit-msg`` hook::

    printf '#!/bin/sh\nexec tools/provenance/check message "$1"\n' \
        > .git/hooks/commit-msg && chmod +x .git/hooks/commit-msg

What each command reads
-----------------------

``tree``
    Every tracked file (``--untracked`` adds files git does not ignore). Under
    ``linux/``, which is imported from upstream with ``git subtree --squash``,
    it reads only what differs from the latest pristine import: new files
    whole, modified upstream files by their changed lines, pristine files not
    at all. A symbolic link is read as its target.

``range``
    Every commit of the range, oldest first: its message, the one-component
    rule, the message lint, and the lines the commit adds. Merges and subtree
    imports add nothing of their own; only their messages are read.

``changes``
    What one revision adds on top of a base, as a single change: added files
    whole, modified files by their added lines, no messages. This checks a
    branch against the upstream it is built on.

``message``
    One commit message before it is committed, with the paths that are
    staged (or ``--paths``).

The gates
=========

``denylist`` -- the hashed deny-list
------------------------------------

Every file and message is split into tokens:

* identifiers (``[A-Za-z_][A-Za-z0-9_]*``), compared in lower case;
* hexadecimal literals, without a C suffix, as ``0x`` and lower-case digits;
* a bare run of exactly eight hex digits beginning with ``8``, the way a
  kernel-segment address is written without its ``0x``.

Each token is hashed -- SHA-256 over the list's salt followed by the token --
and looked up in ``denylist.sha256``. An entry of kind ``token`` matches a
whole token; an entry of kind ``part`` also matches any run of whole
``_``-separated components inside an identifier, so that a name embedded in
a longer macro name is found too. The report says which category of name was
hit -- ``symbol``, ``source-file``, ``module``, ``address``, ``me-table``,
``register``, ``field`` or ``internal`` -- and never the name.

The list is generated on the laboratory side from the non-public name tables
the policy excludes. Tokens that also occur in the pristine upstream kernel,
in the vocabularies of the public standards the project uses, or in a
dictionary are left out, so that ordinary words and kernel identifiers cannot
trip the gate; the laboratory's plaintext audit still sees them. Before a new
list is written, every token already in this repository is looked up in it,
and the generator refuses to write a list this repository would fail. Never
edit the list by hand.

The one entry of category ``test`` is a canary derived from the salt
(``gates.denylist.canary``). It names nothing; the self-test plants it to
prove that the committed list, its salt and the committed tokeniser agree.

``forbidden-words``
-------------------

Plain regular expressions, generic enough to live here. Where a word also has
an ordinary technical sense, the rule leaves that sense alone by vocabulary;
exceptions that depend on where a word appears belong in the allowlist.

.. list-table::
   :header-rows: 1
   :widths: 20 45 35

   * - rule
     - refuses
     - leaves alone
   * - ``re-tool``
     - decompil..., disassembl..., Ghidra, radare, r2pipe, Binary Ninja,
       Hex-Rays, reverse engineering, IDA, IDA Pro, IDAPython
     - "an IDA", "IDA-allocated": the kernel's ID allocator
   * - ``re-autoname``
     - names a disassembler makes: ``FUN_...``, ``DAT_...``, ``LAB_...``,
       ``sub_...``, ``fcn....``
     -
   * - ``narrative``
     - "vendor", "vendor's", "vendors"
     - standards terms: vendor ID, name, OUI, PN, rev, SN, specific, prefix,
       product, class ...
   * - ``dev-kit``
     - SDK, also with a prefix letter or inside identifiers
     - ``make sdk``, and a Makefile target of that name
   * - ``board-package``
     - BSP, also inside identifiers
     -
   * - ``nonpublic-material``
     - leak, leaked, leaking next to a word for source material (SDK,
       datasheet, source, repository, GitHub ...)
     - a memory leak, leak the skb, leaky bucket
   * - ``source-collection``
     - corpus, corpora
     - a fuzzing or test corpus
   * - ``code-address``
     - ``0x80`` and six more hex digits: a kernel-segment address
     - the memory-map constants in the allowlist
   * - ``issue-reference``
     - a hash sign, or ``GH-``, followed by a number
     - colours, HTML entities, URL fragments
   * - ``lab-path``
     - home-directory, volume and temporary paths, and the laboratory's own
       directory and file names
     -
   * - ``internal-reference``
     - the laboratory's working context
     -

``unit-ids``
------------

.. list-table::
   :header-rows: 1
   :widths: 20 45 35

   * - rule
     - refuses
     - leaves alone
   * - ``gpon-serial``
     - four capitals and eight hex digits (a G.984.3 serial number), also in
       hexadecimal
     - an all-hex "vendor ID"
   * - ``device-serial``
     - a literal after "S/N", "serial number" ...
     - placeholders, expressions
   * - ``mac-address``
     - any unicast MAC address
     - the RFC 7042 documentation range, multicast, broadcast, all zeros
   * - ``ipv4-address``
     - private (laboratory) and public addresses
     - the RFC 5737 documentation ranges, loopback, link-local, multicast,
       netmasks, whole reserved blocks, clause numbers after a standard's name
   * - ``ipv6-address``
     - any address outside the documentation prefix
     - the RFC 3849 prefix, loopback, multicast, short link-local addresses
   * - ``password``
     - a literal after "password", "PLOAM PW", "LOID" ...
     - placeholders, expressions

Examples use the documentation ranges: 192.0.2.0/24, 198.51.100.0/24 and
203.0.113.0/24; 2001:db8::/32; MAC addresses 00:00:5e:00:53:00 to
00:00:5e:00:53:ff. A standards clause that looks like an address is cited
with its standard's name ("G.984.3 9.2.3.1") or the word "clause".

``binaries``
------------

``binary-content`` refuses a file with a NUL byte, or whose first 64 KiB are
more than one tenth control characters. ``file-size`` refuses a file above
the size cap (512 KiB by default). Files that exist upstream keep upstream's
size. Every exception is an allowlist entry that states the file's
provenance.

``commit-message``
------------------

Following ``CONTRIBUTING.rst`` and the kernel's conventions:

* ``subject-prefix`` -- ``component: summary``. Kernel commits use kernel
  subsystem prefixes; elsewhere the first prefix names the component's
  directory or a path in it (Buildroot style inside ``br2-external/``,
  top-level files by name). ``Revert "..."`` subjects are exempt.
* ``subject-summary`` -- something follows the prefix.
* ``subject-imperative`` -- the summary starts with an imperative: not "added",
  "adding" or "adds".
* ``subject-length`` -- at most 75 characters; ``subject-period`` -- no full
  stop at the end.
* ``blank-line`` -- the second line is empty.
* ``body-line-length`` -- body lines wrap at 75 columns; indented lines,
  URLs, single long tokens and trailers are exempt.
* ``signed-off-by`` -- a ``Signed-off-by:`` of the commit's author.
* ``co-developed-by`` -- followed by that person's own sign-off (tools are
  credited with ``Co-Authored-By:``).
* ``link-public`` -- ``Link:`` and ``Closes:`` point to public URLs.
* ``fixes-local`` -- ``Fixes:`` names a commit of this repository.

The message also goes through the ``denylist``, ``forbidden-words`` and
``unit-ids`` gates.

``one-component``
-----------------

A commit touches one component: ``linux/``; ``omcid/``; ``br2-external/``
with ``buildroot/``; ``tools/``; ``tests/``; or ``docs/`` with the top level
(the files at the root, ``LICENSES/`` and ``.github/``). Any other top-level
directory is a component of its own.

The allowlist
=============

``allowlist.toml`` holds the justified exceptions, as ``[[allow]]`` tables:

``gate``
    Required: the gate the entry relaxes.
``reason``
    Required: why the exception is right, in at least five words.
``rules``
    Rule ids within the gate (default: every rule of the gate).
``paths``
    Glob patterns of repository paths; ``*`` also crosses ``/``. An entry
    with paths never applies to a commit message.
``match``
    A regular expression searched in the finding's line; the entry applies
    only where one of its matches covers the finding.
``values``
    Normalised values: an address, a MAC address, a serial number.
``hashes``
    Deny-list digests, as the report prints them in JSON.
``commits``
    Full commit ids; the entry applies only to those commits' messages and
    changes.
``provenance``
    Required for the ``binaries`` gate: where the file comes from.

Every key given must match. An entry must narrow its scope with at least one
of ``paths``, ``commits``, ``values``, ``hashes`` or ``match``: a whole gate
cannot be switched off.

Tests
=====

``check selftest`` runs the tests in ``tests/``. Every gate and every rule
has a seeded-violation test that makes it fail, next to a clean fixture that
passes, and every exception of a rule has a test that keeps it quiet. The
seeds -- forbidden words, unit identifiers, deny-listed names -- are
assembled at run time from fragments or drawn at random, so that no file of
this repository contains one and the repository passes its own gates.
Deny-list tests use a list made by the test; the committed list is proven
with its canary.
