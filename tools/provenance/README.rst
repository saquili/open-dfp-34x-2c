.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===============
tools/provenance
===============

The gates that enforce ``docs/policy/provenance.rst``.

These run in continuous integration over the whole tree and over commit
messages: a tokeniser and a hashed deny-list, a forbidden-words check, a
unit-identifier check, a binaries-and-size check, a commit-message check, and
the one-component-per-commit rule. This repository holds only hashes and
generic patterns; the plaintext lists and their generator stay on the
laboratory side.

Empty for now. It is populated by **WI-02**, with a seeded-violation test for
every gate and a justified allow-list.
