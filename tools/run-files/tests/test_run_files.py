# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of run-files, on trees and pages made for each test.

The repository's own page and sources are checked too: they are what the
tool is for.
"""

import importlib.machinery
import importlib.util
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "run-files")
TOP = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))

_loader = importlib.machinery.SourceFileLoader("run_files", TOOL)
_spec = importlib.util.spec_from_loader("run_files", _loader)
rf = importlib.util.module_from_spec(_spec)
_loader.exec_module(rf)


class Tree:
    """A tree with the directories the tool scans, and a page."""

    def __init__(self, test):
        self.tmp = tempfile.TemporaryDirectory()
        test.addCleanup(self.tmp.cleanup)
        self.top = self.tmp.name
        self.page = os.path.join(self.top, "page.rst")

    def write(self, rel, text):
        path = os.path.join(self.top, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)

    def check(self, page_text):
        with open(self.page, "w") as f:
            f.write(page_text)
        return rf.check(self.top, self.page)


class StemTest(unittest.TestCase):
    def test_what_a_program_fills_in_is_cut(self):
        self.assertEqual(rf.stem("onu-pppoeN"), "onu-pppoe")
        self.assertEqual(rf.stem("onu-pdN.kept"), "onu-pd")
        self.assertEqual(rf.stem("udhcpd-lanN.conf"), "udhcpd-lan")
        self.assertEqual(rf.stem("session/N/role"), "session")
        self.assertEqual(rf.stem("network.d/PART"), "network.d")

    def test_a_plain_name_stays(self):
        self.assertEqual(rf.stem("omcid/gem-vlans"), "omcid/gem-vlans")
        self.assertEqual(rf.stem("onu.conf"), "onu.conf")

    def test_a_name_that_stops_at_a_fill_in_loses_its_dash(self):
        self.assertEqual(rf.stem("udhcpd-"), "udhcpd")


class PathsTest(unittest.TestCase):
    def names(self, text):
        return sorted(rf.names_in(text))

    def opened(self, text):
        return sorted(n for n, fills in rf.names_in(text).items() if fills)

    def test_the_forms_a_program_writes(self):
        self.assertEqual(self.names('f=$RUN/onu-mode; g="${RUN}/onu-dns"; h=/run/onu-lan'),
                         ["onu-dns", "onu-lan", "onu-mode"])

    def test_a_file_below_a_directory(self):
        self.assertEqual(self.names('cat "$RUN/omcid/mib"'), ["omcid/mib"])

    def test_a_filled_in_name_ends_the_path(self):
        self.assertEqual(self.names('snprintf(p, n, "/run/onu-pppoe%d", i);'), ["onu-pppoe"])
        self.assertEqual(self.names('echo x >"$RUN/onu-pppoe$n"'), ["onu-pppoe"])
        self.assertEqual(self.opened('echo x >"$RUN/onu-pppoe$n"; y=$RUN/onu-mode'), ["onu-pppoe"])

    def test_a_trailing_slash_is_not_part_of_the_name(self):
        self.assertEqual(self.names("mkdir /run/pond/"), ["pond"])

    def test_a_file_named_by_omcids_macro(self):
        self.assertEqual(self.names('#define XLATE_PATH OMCI_RUN_DIR "/xlate"'), ["omcid/xlate"])

    def test_text_with_no_path(self):
        self.assertEqual(self.names("the run of the stick; running"), [])


class CheckTest(unittest.TestCase):
    def test_agreement(self):
        t = Tree(self)
        t.write("onu-utils/scripts/a", 'echo >"$RUN/onu-mode"\nmkdir /run/session/1\n')
        t.write("pppoed/src/main.c", 'snprintf(a, n, "/run/onu-pppoe%d", s);')
        self.assertEqual(t.check("``/run/onu-mode``, ``/run/session/N/``, ``/run/onu-pppoeN``\n"), [])

    def test_a_path_the_page_lacks(self):
        t = Tree(self)
        t.write("onu-utils/scripts/a", 'echo >"$RUN/onu-mode"\necho >"$RUN/onu-new"\n')
        findings = t.check("``/run/onu-mode``\n")
        self.assertEqual(len(findings), 1)
        self.assertIn("/run/onu-new is named by onu-utils/scripts/a and not on the page", findings[0])

    def test_a_path_no_program_names(self):
        t = Tree(self)
        t.write("onu-utils/scripts/a", 'echo >"$RUN/onu-mode"\n')
        findings = t.check("``/run/onu-mode``, ``/run/onu-gone``\n")
        self.assertEqual(len(findings), 1)
        self.assertIn("/run/onu-gone is on the page and no program names it", findings[0])

    def test_a_directory_with_a_slash_names_its_files(self):
        t = Tree(self)
        t.write("onu-utils/scripts/a", 'x=$RUN/onu-trial.d/mgmt; y=$RUN/onu-trial.d/lane\n')
        self.assertEqual(t.check("``/run/onu-trial.d/``\n"), [])

    def test_a_directory_without_a_slash_does_not(self):
        t = Tree(self)
        t.write("onu-utils/scripts/a", 'x=$RUN/omcid/mib\n')
        findings = t.check("``/run/omcid``\n")
        self.assertTrue(any("/run/omcid/mib is named by" in f for f in findings), findings)

    def test_a_name_a_program_fills_in_is_named_by_any_form(self):
        t = Tree(self)
        t.write("onu-utils/scripts/a", 'x=$RUN/udhcpd-$lan.conf; y=$RUN/onu-pppoe1\n')
        self.assertEqual(t.check("``/run/udhcpd-lanN.conf``, ``/run/onu-pppoeN``\n"), [])

    def test_a_name_a_program_spells_out_is_not_named_by_a_prefix(self):
        t = Tree(self)
        t.write("onu-utils/scripts/a", 'x=$RUN/omcid.log\n')
        findings = t.check("``/run/omcid``\n")
        self.assertEqual(len(findings), 2, findings)

    def test_the_tests_of_a_component_are_not_scanned(self):
        t = Tree(self)
        t.write("onu-utils/scripts/a", 'echo >"$RUN/onu-mode"\n')
        t.write("onu-utils/tests/b.c", 'x = "/run/only-a-stand-in";\n')
        t.write("omcid/src/tests/c.c", 'x = "/run/only-a-stand-in";\n')
        self.assertEqual(t.check("``/run/onu-mode``\n"), [])

    def test_a_file_that_is_not_text_of_a_program_is_not_scanned(self):
        t = Tree(self)
        t.write("onu-utils/src/README.rst", "see /run/not-a-program-path\n")
        self.assertEqual(t.check("nothing\n"), [])

    def test_a_missing_page_is_a_problem(self):
        t = Tree(self)
        with self.assertRaises(rf.Problem):
            rf.check(t.top, os.path.join(t.top, "no-such-page.rst"))


class RepositoryTest(unittest.TestCase):
    def test_the_page_and_the_sources_agree(self):
        self.assertEqual(rf.check(TOP, rf.DEFAULT_PAGE), [])

    def test_the_sources_name_the_files_the_board_has(self):
        names = rf.source_names(TOP)
        for expected in ("onu.conf", "omcid/mib", "pond/token", "session", "onu-pppoe"):
            self.assertIn(expected, names)


if __name__ == "__main__":
    unittest.main()
