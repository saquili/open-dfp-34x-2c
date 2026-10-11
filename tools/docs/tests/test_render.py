# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of render: a clean page, a page with a warning, the links between
pages, and the exit statuses. The pages are written into a temporary top of a
repository; without docutils every test is skipped.
"""

import contextlib
import importlib.machinery
import importlib.util
import io
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "render")
_loader = importlib.machinery.SourceFileLoader("docs_render", TOOL)
_spec = importlib.util.spec_from_loader("docs_render", _loader)
render = importlib.util.module_from_spec(_spec)
_loader.exec_module(render)

try:
    import docutils  # noqa: F401
    HAVE_DOCUTILS = True
except ImportError:
    HAVE_DOCUTILS = False

CLEAN = """\
=====
Title
=====

A paragraph with a link to `the other page <sub/other.rst>`__ and to
`its section <sub/other.rst#part>`__, and one to `a site
<https://example.org/page.rst>`__.
"""

SHORT = """\
====
Title
====

A paragraph.
"""


@unittest.skipUnless(HAVE_DOCUTILS, "docutils is not installed")
class RenderTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.top = self.tmp.name
        self.out = os.path.join(self.top, "out")
        self.saved = render.TOP
        render.TOP = self.top
        self.addCleanup(setattr, render, "TOP", self.saved)

    def write(self, page, text):
        path = os.path.join(self.top, page)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)

    def run_tool(self, args):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = render.main(args)
        return status, stdout.getvalue(), stderr.getvalue()

    def test_a_clean_page_renders_with_its_links_pointed_at_html(self):
        self.write("page.rst", CLEAN)
        status, out, _ = self.run_tool(["--out", self.out, "page.rst"])
        self.assertEqual(status, 0)
        self.assertIn("0 with warnings", out)
        with open(os.path.join(self.out, "page.html"), encoding="utf-8") as f:
            html = f.read()
        self.assertIn('href="sub/other.html"', html)
        self.assertIn('href="sub/other.html#part"', html)
        self.assertIn('href="https://example.org/page.rst"', html)

    def test_a_warning_fails_the_run_and_is_printed(self):
        self.write("short.rst", SHORT)
        status, out, _ = self.run_tool(["--check", "short.rst"])
        self.assertEqual(status, 1)
        self.assertIn("short.rst:1", out)
        self.assertIn("Title overline too short", out)
        self.assertFalse(os.path.exists(self.out))

    def test_a_missing_page_is_a_usage_error(self):
        status, _, err = self.run_tool(["--check", "nowhere.rst"])
        self.assertEqual(status, 2)
        self.assertIn("no such page", err)


if __name__ == "__main__":
    unittest.main()
