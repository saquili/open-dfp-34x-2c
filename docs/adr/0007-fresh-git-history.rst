.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

========================================
ADR 0007: Start from a fresh git history
========================================

:Status: Accepted
:Date: 2026-09-21
:Deciders: the maintainer

Summary
=======

The repository began from an empty history and holds only the product. The
project's local lab, where the hardware is measured and tested, keeps its
notes, experiments and tools in a working repository of its own.

Context
=======

The hardware was brought up in the project's local lab, a bench with the
stick, its serial console and an OLT, where every fact about the hardware is
measured or tested before it is written down. The lab's working repository
holds what that takes: notes of experiments, one-off scripts, captures and
dumps of the bench. None of it is part of the product, and some of it, such
as a unit's registration identity, must not be published.

Decision
========

Start the repository from an empty history (``git init``); never create it
from a branch, a fork or a filtered copy of the lab's repository.

* Share no stored object with the lab's repository, except pristine
  upstream files, which are identical everywhere by nature.
* Link to no lab repository, and cite none of its revision identifiers.
* Keep the lab's repository private, as the notebook of how the facts were
  established.
* Date commits when they are made; nothing is back-dated.
* Before the first publication, squash the work into one series of commits,
  one or more per component (ADR 0004), each of which passes the provenance
  gates and the kernel's checks.

Consequences
============

* The separation can be audited: an object-disjointness check confirms that
  no stored object here matches one in the lab's repository unless it is a
  pristine upstream file. It runs before the first publication and before
  every release.
* The provenance gates (``docs/policy/provenance.rst``) guard a history that
  holds only the product from its first commit.
* Records of device runs and pages written before the first publication, on
  2026-10-20, may name commits that are not in the published history; the
  dates they give still place them.
