# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The README says what the tool does: every setting, command and status is in it."""

from __future__ import annotations

import os
import re
import unittest

from bench import cli, guard
from bench import settings as settingsmod

README = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "README.rst")


class Readme(unittest.TestCase):

    def setUp(self) -> None:
        with open(README, encoding="utf-8") as handle:
            self.text = handle.read()

    def test_every_setting_is_documented(self) -> None:
        for key in settingsmod.SCHEMA:
            self.assertTrue("``%s``" % key in self.text, "README lacks setting %s" % key)

    def test_every_command_and_option_is_documented(self) -> None:
        parser = cli.build_parser()
        subparsers = next(a for a in parser._actions if a.dest == "command")
        for name, sub in subparsers.choices.items():
            if name == "guard":
                continue  # the alias is mentioned with its command
            self.assertTrue("``%s" % name in self.text, "README lacks command %s" % name)
            for action in sub._actions:
                for option in action.option_strings:
                    if option not in ("-h", "--help"):
                        self.assertTrue(option in self.text,
                                        "README lacks %s of %s" % (option, name))

    def test_the_exit_statuses_are_documented(self) -> None:
        for phrase in ("0 success", "1 the operation failed", "2 usage", "3 a guard refusal",
                       "4 the console is not usable", "5 the board is in an unexpected",
                       "6 a timeout"):
            self.assertTrue(phrase in " ".join(self.text.split()), "README lacks %r" % phrase)

    def test_the_documented_test_command_works(self) -> None:
        import subprocess
        import sys
        self.assertTrue("python3 -m unittest discover -s bench/tests -t ." in self.text)
        result = subprocess.run(
            [sys.executable, "-m", "unittest", "discover", "-s", "bench/tests", "-t", ".",
             "-p", "test_image.py"], cwd=os.path.dirname(os.path.dirname(os.path.dirname(
                 os.path.abspath(__file__)))), capture_output=True, text=True, timeout=60,
            env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK", result.stderr)

    def test_headings_are_underlined_in_full(self) -> None:
        lines = self.text.splitlines()
        for index in range(1, len(lines)):
            if re.fullmatch(r"([=\-~^])\1{2,}", lines[index]) and lines[index - 1].strip() \
                    and not re.fullmatch(r"([=\-~^])\1{2,}", lines[index - 1]):
                self.assertGreaterEqual(len(lines[index]), len(lines[index - 1]), lines[index - 1])

    def test_the_header_and_the_character_set(self) -> None:
        self.assertTrue(self.text.startswith(".. SPDX-License-" "Identifier: GPL-2.0-only\n"
                                             ".. SPDX-FileCopyrightText: 2026 "))
        self.assertNotIn("\t", self.text)
        self.assertEqual(self.text.encode("utf-8").decode("utf-8"), self.text)

    def test_the_partitions_and_addresses_it_names_are_the_tools(self) -> None:
        for name in ("k0", "r0", "k1", "r1", "boot", "env", "env2", "config"):
            self.assertTrue("``%s``" % name in self.text, "README lacks %s" % name)
            self.assertIn(name, guard.BY_NAME)


if __name__ == "__main__":
    unittest.main()
