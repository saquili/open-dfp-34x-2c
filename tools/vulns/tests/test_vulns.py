# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of vulns, on files made for each test; no network.

The repository's own accepted list is read too: a malformed line in it would
stop the daily run.
"""

import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "vulns")

_loader = importlib.machinery.SourceFileLoader("vulns", TOOL)
_spec = importlib.util.spec_from_loader("vulns", _loader)
vulns = importlib.util.module_from_spec(_spec)
_loader.exec_module(vulns)


class Files:
    def __init__(self, test):
        self.tmp = tempfile.TemporaryDirectory()
        test.addCleanup(self.tmp.cleanup)

    def write(self, rel, text):
        path = os.path.join(self.tmp.name, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return path


def run(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = vulns.main(argv)
    return rc, out.getvalue(), err.getvalue()


def stats(**packages):
    return json.dumps({"packages": packages})


def pkg(target=True, version="1.0", cves=(), unsure=()):
    infras = [["target", "generic"]] if target else [["host", "generic"]]
    return {"infras": infras, "current_version": version, "cves": list(cves),
            "unsure_cves": list(unsure)}


class Packages(unittest.TestCase):
    def setUp(self):
        self.f = Files(self)
        self.accepted = self.f.write("accepted", "# none\n")

    def check(self, text, accepted=None):
        path = self.f.write("stats.json", text)
        return run(["packages", path, "--accepted", accepted or self.accepted])

    def test_clean(self):
        rc, out, _ = self.check(stats(busybox=pkg()))
        self.assertEqual(rc, 0)
        self.assertIn("No CVE in a package the stick runs.", out)

    def test_cve_of_a_package_on_the_stick_is_a_finding(self):
        rc, out, _ = self.check(stats(busybox=pkg(cves=["CVE-2026-1"])))
        self.assertEqual(rc, 1)
        self.assertIn("| busybox | 1.0 | CVE-2026-1 |", out)

    def test_host_package_is_listed_not_judged(self):
        rc, out, _ = self.check(stats(bison=pkg(target=False, cves=["CVE-2026-2"])))
        self.assertEqual(rc, 0)
        self.assertIn("CVE-2026-2 in bison", out)

    def test_accepted_cve_is_kept_with_its_reason(self):
        acc = self.f.write("acc", "CVE-2026-1 busybox the applet is not built\n")
        rc, out, _ = self.check(stats(busybox=pkg(cves=["CVE-2026-1"])), acc)
        self.assertEqual(rc, 0)
        self.assertIn("CVE-2026-1 in busybox 1.0: the applet is not built", out)

    def test_acceptance_is_per_package(self):
        acc = self.f.write("acc", "CVE-2026-1 dropbear another package\n")
        rc, _, _ = self.check(stats(busybox=pkg(cves=["CVE-2026-1"])), acc)
        self.assertEqual(rc, 1)

    def test_stale_acceptance_is_named(self):
        acc = self.f.write("acc", "CVE-2026-9 busybox fixed long ago\n")
        rc, out, _ = self.check(stats(busybox=pkg()), acc)
        self.assertEqual(rc, 0)
        self.assertIn("CVE-2026-9 in busybox", out)
        self.assertIn("can go", out)

    def test_kernel_and_local_programs_are_left_out(self):
        rc, _, _ = self.check(stats(linux=pkg(version="custom", cves=["CVE-2026-3"]),
                                    pond=pkg(version="local", cves=["CVE-2026-4"])))
        self.assertEqual(rc, 0)

    def test_unsure_is_listed(self):
        rc, out, _ = self.check(stats(uclibc=pkg(unsure=["CVE-2026-5"])))
        self.assertEqual(rc, 0)
        self.assertIn("CVE-2026-5 in uclibc", out)

    def test_bad_accepted_line(self):
        acc = self.f.write("acc", "CVE-2026-1 busybox\n")
        rc, _, err = self.check(stats(busybox=pkg()), acc)
        self.assertEqual(rc, 2)
        self.assertIn("want CVE PACKAGE REASON", err)

    def test_bad_json(self):
        rc, _, _ = self.check("{")
        self.assertEqual(rc, 2)

    def test_repository_list_reads(self):
        vulns.load_accepted(vulns.ACCEPTED)


RELEASES = json.dumps({"releases": [
    {"moniker": "mainline", "version": "7.4-rc3"},
    {"moniker": "stable", "version": "7.3.5"},
    {"moniker": "longterm", "version": "6.18.64"},
    {"moniker": "longterm", "version": "6.12.118"},
]})

MAKEFILE = "VERSION = 6\nPATCHLEVEL = 18\nSUBLEVEL = {}\nEXTRAVERSION =\n"


class Kernel(unittest.TestCase):
    def setUp(self):
        self.f = Files(self)
        self.releases = self.f.write("releases.json", RELEASES)

    def check(self, sub, *extra):
        mk = self.f.write("Makefile", MAKEFILE.format(sub))
        return run(["kernel", "--makefile", mk, "--releases", self.releases, *extra])

    def test_newest(self):
        rc, out, _ = self.check(64)
        self.assertEqual(rc, 0)
        self.assertIn("It is the newest release of 6.18", out)

    def test_behind(self):
        rc, out, _ = self.check(63)
        self.assertEqual(rc, 1)
        self.assertIn("6.18.64 is out", out)

    def test_series_gone(self):
        mk = self.f.write("Makefile", "VERSION = 6\nPATCHLEVEL = 17\nSUBLEVEL = 9\n")
        rc, out, _ = run(["kernel", "--makefile", mk, "--releases", self.releases])
        self.assertEqual(rc, 1)
        self.assertIn("the series has ended", out)

    def dyad(self, name, *pairs):
        lines = ["# dyad version: test"]
        lines += [f"{v}:{'a' * 40}:{f}:{'b' * 40}" for v, f in pairs]
        self.f.write(f"cves/cve/published/2026/{name}.dyad", "\n".join(lines) + "\n")

    def test_cves(self):
        self.dyad("CVE-2026-10", ("6.6", "6.12.114"), ("6.6", "6.18.64"), ("6.6", "7.3"))
        self.dyad("CVE-2026-11", ("2.6.12", "6.18.50"), ("2.6.12", "6.19"))
        self.dyad("CVE-2026-12", ("2.6.35", "6.12.114"), ("2.6.35", "6.19"))
        self.dyad("CVE-2026-13", ("6.19", "6.19.7"), ("6.18.57", "0"))
        self.dyad("CVE-2026-14", ("5.10", "6.14"))
        self.dyad("CVE-2026-15", ("6.19", "7.0"))
        rc, out, _ = self.check(63, "--cves", os.path.join(self.f.tmp.name, "cves"))
        self.assertEqual(rc, 1)
        self.assertIn("- CVE-2026-10 (fixed in 6.18.64)", out)
        self.assertNotIn("CVE-2026-11", out)
        self.assertIn("CVE-2026-12, CVE-2026-13", out)
        self.assertNotIn("CVE-2026-14", out)
        self.assertNotIn("CVE-2026-15", out)

    def test_judge(self):
        ours = (6, 18, 63)
        self.assertEqual(vulns.judge([((6, 6), (6, 18, 64))], ours), ("later", (6, 18, 64)))
        self.assertEqual(vulns.judge([((6, 6), (6, 18, 60))], ours), "fixed")
        self.assertEqual(vulns.judge([((6, 18, 70), (6, 18, 80))], ours), "unaffected")
        self.assertEqual(vulns.judge([((), ())], ours), "unfixed")
        self.assertEqual(vulns.judge([((5, 4), (6, 18))], ours), "fixed")

    def test_no_makefile_version(self):
        mk = self.f.write("Makefile", "nothing\n")
        rc, _, err = run(["kernel", "--makefile", mk, "--releases", self.releases])
        self.assertEqual(rc, 2)
        self.assertIn("no VERSION", err)


class Mongoose(unittest.TestCase):
    def setUp(self):
        self.f = Files(self)
        self.header = self.f.write("mongoose.h", '#define MG_VERSION "7.23"\n')

    def test_newest(self):
        rc, _, _ = run(["mongoose", "--header", self.header, "--latest", "7.23"])
        self.assertEqual(rc, 0)

    def test_behind(self):
        rc, out, _ = run(["mongoose", "--header", self.header, "--latest", "7.24"])
        self.assertEqual(rc, 1)
        self.assertIn("7.24 is out", out)

    def test_ordering_is_numeric(self):
        rc, _, _ = run(["mongoose", "--header", self.header, "--latest", "7.3"])
        self.assertEqual(rc, 0)


INDEX = """<a href="buildroot-2025.02.18.tar.xz">x</a>
<a href="buildroot-2025.02.19.tar.xz">x</a> <a href="buildroot-2025.05.3.tar.xz">x</a>
<a href="buildroot-2025.02.tar.xz">x</a>"""


class Buildroot(unittest.TestCase):
    def setUp(self):
        self.f = Files(self)
        self.index = self.f.write("index.html", INDEX)

    def check(self, ver):
        pin = self.f.write("VERSION", f"# pin\nNAME=buildroot\nVERSION={ver}\n")
        return run(["buildroot", "--pin", pin, "--index", self.index])

    def test_newest(self):
        rc, out, _ = self.check("2025.02.19")
        self.assertEqual(rc, 0)
        self.assertIn("newest release of 2025.02", out)

    def test_behind(self):
        rc, out, _ = self.check("2025.02.10")
        self.assertEqual(rc, 1)
        self.assertIn("2025.02.19 is out", out)

    def test_other_series_ignored(self):
        rc, _, _ = self.check("2025.02.19")
        self.assertEqual(rc, 0)

    def test_series_missing(self):
        rc, _, err = self.check("2024.11.1")
        self.assertEqual(rc, 2)
        self.assertIn("no release of 2024.11", err)


if __name__ == "__main__":
    unittest.main()
