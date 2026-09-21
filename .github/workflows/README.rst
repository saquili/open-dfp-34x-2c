.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=================
.github/workflows
=================

Continuous-integration workflows.

The gate that a change must pass includes: kernel checkpatch and a
warnings-clean build of the board configuration; sparse; device-tree binding
and schema checks; a documentation build; the kernel selftests build; the OMCI
daemon build, unit tests and coverage; the distribution build; the image-size
and instruction-set gates; reproducibility; ``reuse lint``; and all the
provenance gates. The merge rule is a green suite, full line coverage where it
is measured, and a device run.

Empty for now. The workflows are added with the components they check: the
kernel jobs arrive with **WI-11**, and each later work item adds its own.
