# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Helpers shared by the self-tests."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
TESTS = os.path.dirname(os.path.realpath(__file__))
TOOLS = os.path.dirname(TESTS)
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

from equivlib import lang, trees  # noqa: E402


needs_git = unittest.skipUnless(shutil.which('git'), 'git is not installed')


def tool(name):
    """Absolute path of the tool *name* in tools/equiv."""
    return os.path.join(TOOLS, name)


def run(argv, stdin=None, env=None, cwd=None):
    """Run *argv*; return (status, stdout, stderr) as text."""
    p = subprocess.run(argv, input=stdin, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, env=env, cwd=cwd,
                       universal_newlines=True)
    return p.returncode, p.stdout, p.stderr


def run_tool(name, *args, **kw):
    return run([tool(name)] + list(args), **kw)


def classify(path, a, b, arch=lang.DEFAULT_ARCH):
    """Classify the edit from text *a* to text *b* of the file *path*."""
    return trees.classify_bytes(path, lang.encode(a), lang.encode(b), arch)


def tokens(text, path):
    """(kind, text) pairs of *text*, read as the file *path*."""
    return [t.key for t in lang.tokenize(text, lang.detect(path), path)]


def replace(text, old, new, count=1):
    """text.replace, failing if *old* occurs fewer than *count* times: an
    edit that silently does nothing would make a test pass for nothing."""
    if text.count(old) < count:
        raise AssertionError('%r not found %d time(s) in the sample'
                             % (old, count))
    return text.replace(old, new)


def case_sensitive(directory):
    """Whether the file system holding *directory* tells 'a' from 'A'."""
    with tempfile.NamedTemporaryFile(prefix='case-', dir=directory) as f:
        other = os.path.join(os.path.dirname(f.name),
                             os.path.basename(f.name).upper())
        return not os.path.exists(other)


class TempDirTest(unittest.TestCase):
    """A test with a fresh temporary directory in self.tmp."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix='equiv-test-')
        self.tmp = os.path.realpath(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, rel, data, mode=None):
        """Write *data* (text or bytes) to tmp/rel; return the path."""
        path = os.path.join(self.tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if isinstance(data, str):
            data = lang.encode(data)
        with open(path, 'wb') as f:
            f.write(data)
        if mode is not None:
            os.chmod(path, mode)
        return path
