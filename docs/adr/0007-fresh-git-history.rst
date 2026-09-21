.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

============================================================
ADR 0007: Fresh git history, separate from the laboratory
============================================================

:Status: Accepted
:Date: 2026-09-21

Context
=======

The bring-up happened in a separate, private laboratory repository. That
repository's history holds material the provenance policy excludes, and it
shares a remote whose object store holds that material. A history rewrite would
keep the structure of that story while breaking the revision identifiers the
laboratory notes cite -- and it would still leave the excluded objects
reachable through the shared store.

Decision
========

Start rtl9601d-linux from an empty history (``git init``). Never create it from
a branch, a fork or a filtered copy of the laboratory repository.

* Share no stored object with the laboratory repository, except pristine
  upstream files (which are identical everywhere by nature).
* Link to no laboratory repository and cite none of their revision identifiers.
* Keep the laboratory repository private, as the notebook of how the facts were
  established.
* Date commits when they are made; nothing is back-dated.

Consequences
============

* The separation is auditable: an object-disjointness check confirms that no
  stored object here matches one in the laboratory repository unless it is a
  pristine upstream file. It runs before the first publication and before every
  release.
* This clean origin is the foundation the provenance policy rests on; without
  it, the plaintext and hashed gates would be guarding a history that already
  contained what they forbid.
