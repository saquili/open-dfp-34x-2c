.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==============================
Contributing to rtl9601d-linux
==============================

Developer Certificate of Origin
===============================

Every commit must carry a ``Signed-off-by:`` line with the author's real name
and a reachable e-mail address, for example::

    Signed-off-by: Random Developer <random@example.org>

The sign-off certifies that you wrote the change, or otherwise have the right
to submit it under the repository's licence, in the terms of the Developer
Certificate of Origin 1.1 (https://developercertificate.org/). Use the identity
you are willing to publish. ``git commit -s`` adds the line for you.

Licensing
=========

Everything authored here is ``GPL-2.0-only``. Put the SPDX tag at the top of
every file you add, in that file's comment syntax, together with a copyright
line -- for a reStructuredText file, for instance::

    .. SPDX-License-Identifier: GPL-2.0-only
    .. SPDX-FileCopyrightText: 2026 Your Name <you@example.org>

``reuse lint`` must pass. Third-party code is never committed here; it is
fetched by hash at build time and keeps its own licence.

Provenance
==========

This project is built from original work, upstream open-source code, hardware
facts written in our own words with a stated way to verify them, and public
standards. Material of any other origin -- and the categories the policy names
-- must not enter. Read ``docs/policy/provenance.rst`` before you contribute
and follow it: the same rules bind code, comments, commit messages, generated
files and release assets, and the continuous-integration gates enforce them.

Commit messages
===============

Make one logical change per commit, with an imperative subject that states the
problem first and then the change.

* **Kernel commits** (under ``linux/``) use the kernel form
  ``subsystem: component: imperative summary`` -- for example
  ``net: dsa: realtek: rtl9601d: add PTP hardware clock`` or
  ``dt-bindings: net: add realtek,rtl9601d-eth``. The body explains the
  problem, then the change in hardware terms, cites standards by clause, and
  gives measurements as results on the named board, with counts.
* **Userspace and distribution commits** use their component prefix:
  ``omcid:``, ``package/omcid:``, ``board/<board>:``, ``configs/<board>:``,
  ``tools/bench:``, ``docs:``.
* **One component per commit**: a single commit touches only ``linux/``, only
  ``omcid/``, or only the distribution and tooling directories -- never a mix.
  This keeps each component liftable on its own and keeps the kernel history
  clean.

Trailers
========

* ``Signed-off-by:`` -- required, as above.
* ``Link:`` -- only to publicly reachable URLs.
* ``Fixes:`` -- only referencing commits of this repository.
* Tool assistance is credited with a ``Co-Authored-By:`` trailer. Do not use
  ``Co-developed-by:`` for a tool: the kernel defines it as authorship that
  must be followed by that co-author's own ``Signed-off-by:``.

A commit message must not contain comparative narrative about another
implementation, reverse-engineering vocabulary, third-party code names or
addresses, investigation history, private issue numbers, laboratory paths or
addresses, or unit identifiers. See ``docs/policy/provenance.rst``.
