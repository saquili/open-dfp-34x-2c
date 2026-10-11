.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==========
tools/docs
==========

``render`` renders the repository's reStructuredText pages to HTML with
docutils, and reports every warning docutils gives for them. ``make docs``
renders every page that git tracks outside ``linux/`` into ``build/docs``;
``make check`` runs it with ``--check``, which writes nothing and fails on a
warning, and skips it with a note on a host without docutils. The kernel's
pages are rendered by ``make -C linux htmldocs``.

::

    tools/docs/render [--check] [--if-available] [--out DIR] [PAGE ...]

A link from one page to another (a relative link that names a ``.rst`` file)
is pointed at the other page's HTML. The pages are read as they are written:
the raw HTML of the top-level README is passed through, and no file is
included into a page.

It needs Python 3 and docutils (``python3-docutils`` on Debian). Its tests,
``tests/run``, skip without docutils.
