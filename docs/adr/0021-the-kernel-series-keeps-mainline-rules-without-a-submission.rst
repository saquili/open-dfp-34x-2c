.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===========================================================================
ADR 0021: The kernel series keeps mainline's rules, without a submission
===========================================================================

:Status: Accepted
:Date: 2026-10-15
:Decided-by: the owner, 2026-10-15
:Amends: ADR 0018

.. note::

   In plain words: the drivers stay written the way the official Linux
   wants them, but this project does not send them there. Anyone who wants
   them in mainline may take them and do it.

Context
=======

ADR 0018 made mainline the aim of the kernel series: new work keeps to what
a submission needs, the drivers were split towards it, and some work was
left "before a submission". A submission is more than code that meets the
rules: it is rounds of review with the maintainers of networking, DSA and a
new PON class with a uAPI of its own, over months or years. The project is
led by one person who builds what the stick needs and publishes it quickly,
and who has no time for that.

Decision
========

The kernel series keeps mainline's rules and is not offered to mainline by
this project.

* The rules stay as ADR 0018 and the gates have them: the kernel's coding
  style and checkpatch ``--strict`` over every commit of the series, the
  device-tree bindings, kernel-doc, KUnit and kselftest tests, the changes to
  the kernel's own files listed with their reasons
  (``docs/handbook/kernel-series.rst``), and the series exported on its own
  (``tools/kernel-series``, ``git subtree split -P linux``).
* Anyone may take the series to mainline under its licence, whole or in
  part. The project helps where it can, and does not lead it.
* The work ADR 0018 left "before a submission" -- the NAPT and flowtable
  paths, the size of ``rtl9602c-offload.c`` -- is ordinary work, done when it
  is wanted, not a condition of anything.

Consequences
============

* No release waits on review outside the project, and no promise is made
  that the drivers will reach mainline.
* The PON class's interfaces -- the ``pon`` generic netlink family, the
  ``gem`` link kind and its ``IFLA_GEM_*`` attributes, ``/dev/ponNomci0`` --
  are this project's own and may change from one build to the next. A
  submission would change them again; whoever submits decides how.
* Because the rules stay, the series is still one that a person who wants
  to submit it can start from without a rewrite.
