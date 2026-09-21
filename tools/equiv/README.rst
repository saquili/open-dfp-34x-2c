.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==========
tools/equiv
==========

The equivalence tools that prove the kernel code did not change while its prose
was re-authored and its identifiers were renamed.

A source-to-token canonicaliser (comments removed, whitespace collapsed, string
and character literals kept byte-exact), helpers that strip help and
description text from Kconfig and YAML, a tree comparator that classifies each
file's difference (identical, comment-only, string-literal-only, or code), an
object-manifest comparator, and the reproducible-build wrapper.

Empty for now. It is populated by **WI-03**, with self-tests: a comment-only
edit compares identical, an identifier rename shows a token difference, a string
change is reported as literal-only, and two builds of one tree match.
