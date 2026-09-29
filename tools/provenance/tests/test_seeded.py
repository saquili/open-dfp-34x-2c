# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Seeded violations: every gate fails when its violation is planted.

Each test starts from a fixture repository that passes every gate, plants one
violation, and runs the command line exactly as continuous integration does.
The deny-list here is made by the test, with a fresh identifier standing in
for another firmware's symbol and another for a register name from
non-public tables; test_denylist_committed.py proves the committed list
itself.
"""

from __future__ import annotations

import os

from .support import Repo, Seeds, TempDirTestCase, make_denylist, pairs, run_check, word


class SeededViolations(TempDirTestCase):

    def setUp(self) -> None:
        super().setUp()
        self.seeds = Seeds()
        self.symbol = self.seeds.identifier()
        self.register = self.seeds.identifier().upper()
        self.denylist = os.path.join(self.tmp, "denylist.sha256")
        make_denylist(self.denylist, [(self.symbol, "token", "symbol"),
                                      (self.register, "part", "register")])
        self.repo = Repo(os.path.join(self.tmp, "repo"))
        self.repo.write("tools/demo/README.rst", "A demonstration tool.\n")
        self.repo.write("tools/demo/demo.c", "/* Count the frames. */\nint frames;\n")
        self.base = self.repo.commit("tools/demo: add a demonstration tool",
                                     "Count frames, so that the gates have something to read.")

    def check(self, *argv: str, allowlist=None) -> tuple[int, dict]:
        return run_check(*argv, repo=self.repo.path, denylist=self.denylist, allowlist=allowlist)

    def plant(self, text: str, path: str = "tools/demo/demo.c") -> tuple[int, dict]:
        """Add ``text`` to a file, commit it, and check the tree and the new commit."""
        with open(os.path.join(self.repo.path, path), "a", encoding="utf-8") as handle:
            handle.write(text)
        self.repo.commit("tools/demo: extend the demonstration", "Add one more line to it.")
        return self.check("all", "--base", self.base)

    def assertFails(self, result: tuple[int, dict], gate: str, rule: str) -> list[dict]:
        code, report = result
        self.assertEqual(code, 1, report)
        self.assertFalse(report["ok"])
        self.assertIn((gate, rule), pairs(report))
        return [f for f in report["findings"] if (f["gate"], f["rule"]) == (gate, rule)]

    # -- the baseline ------------------------------------------------------------

    def test_the_clean_fixture_passes(self) -> None:
        code, report = self.check("all", "--history")
        self.assertEqual(code, 0, report)
        self.assertTrue(report["ok"])
        self.assertEqual(report["scanned_commits"], 1)

    # -- the plan's list, one by one ----------------------------------------------

    def test_a_symbol_of_other_firmware(self) -> None:
        found = self.assertFails(self.plant("/* see %s() */\n" % self.symbol), "denylist", "symbol")
        self.assertEqual(found[0]["kind"], "comment")
        self.assertEqual(found[0]["path"], "tools/demo/demo.c")

    def test_a_register_name_from_nonpublic_tables(self) -> None:
        macro = "RTL9602C_%s_CTRL" % self.register
        found = self.assertFails(self.plant("#define %s 0x0400\n" % macro), "denylist", "register")
        self.assertEqual(found[0]["text"], macro)
        self.assertEqual(found[0]["value"], self.register.lower())

    def test_a_code_address(self) -> None:
        address = self.seeds.code_address()
        found = self.assertFails(self.plant("/* entry at %s */\n" % address),
                                 "forbidden-words", "code-address")
        self.assertEqual(found[0]["value"], address)

    def test_the_phrase_the_vendor(self) -> None:
        phrase = "the " + word("ven", "dor")
        self.assertFails(self.plant("/* This is how %s sets it. */\n" % phrase),
                         "forbidden-words", "narrative")

    def test_a_gpon_serial_number(self) -> None:
        serial = self.seeds.gpon_serial()
        found = self.assertFails(self.plant("Registered as %s.\n" % serial,
                                            "tools/demo/README.rst"), "unit-ids", "gpon-serial")
        self.assertEqual(found[0]["text"], serial)

    def test_a_laboratory_ip_address(self) -> None:
        address = self.seeds.private_ipv4()
        found = self.assertFails(self.plant("Reach the board at %s.\n" % address,
                                            "tools/demo/README.rst"), "unit-ids", "ipv4-address")
        self.assertIn("private", found[0]["message"])

    def test_a_binary_blob(self) -> None:
        self.repo.write("tools/demo/table.bin", self.seeds.blob())
        self.repo.commit("tools/demo: add a table", "Add the table the demonstration reads.")
        self.assertFails(self.check("all", "--base", self.base), "binaries", "binary-content")

    def test_a_commit_touching_two_components(self) -> None:
        self.repo.write("linux/drivers/demo.c", "int demo;\n")
        self.repo.write("omcid/src/demo.c", "int demo;\n")
        self.repo.commit("demo: add two halves", "Add both halves at once.")
        found = self.assertFails(self.check("range", "%s..HEAD" % self.base),
                                 "one-component", "one-component")
        self.assertIn("2 components", found[0]["message"])

    # -- every other gate and rule fails too ----------------------------------------

    def test_a_file_above_the_size_cap(self) -> None:
        self.repo.write("tools/demo/big.txt", "frames\n" * 40)
        self.repo.commit("tools/demo: add a long list", "Add the list of frames.")
        self.assertFails(self.check("--size-cap", "100", "tree"), "binaries", "file-size")

    def test_a_symbol_in_a_commit_message(self) -> None:
        self.repo.write("tools/demo/demo.c", "int frames, bursts;\n")
        self.repo.commit("tools/demo: count bursts", "Count them as %s does." % self.symbol)
        found = self.assertFails(self.check("range", "%s..HEAD" % self.base), "denylist", "symbol")
        self.assertEqual(found[0]["kind"], "message")
        self.assertIsNone(found[0]["path"])

    def test_a_commit_without_signoff_or_prefix(self) -> None:
        self.repo.write("tools/demo/demo.c", "int frames, bursts;\n")
        self.repo.commit("Added the bursts", "Count bursts as well.", signoff=False)
        code, report = self.check("range", "%s..HEAD" % self.base)
        self.assertEqual(code, 1)
        self.assertLessEqual({("commit-message", "signed-off-by"),
                              ("commit-message", "subject-prefix")}, pairs(report))

    def test_a_commit_in_the_past_tense(self) -> None:
        self.repo.write("tools/demo/demo.c", "int frames, bursts;\n")
        self.repo.commit("tools/demo: added the bursts", "Count bursts as well.")
        self.assertFails(self.check("range", "%s..HEAD" % self.base),
                         "commit-message", "subject-imperative")

    def test_an_issue_number_in_a_commit_message(self) -> None:
        self.repo.write("tools/demo/demo.c", "int frames, bursts;\n")
        self.repo.commit("tools/demo: count bursts", "This closes " + word("#", "17") + ".")
        self.assertFails(self.check("range", "%s..HEAD" % self.base),
                         "forbidden-words", "issue-reference")

    def test_a_tool_word(self) -> None:
        self.assertFails(self.plant("/* %s from the image */\n" % word("dis", "assembled")),
                         "forbidden-words", "re-tool")

    def test_a_mac_address(self) -> None:
        self.assertFails(self.plant("Its address is %s.\n" % self.seeds.mac(),
                                    "tools/demo/README.rst"), "unit-ids", "mac-address")

    def test_a_laboratory_path(self) -> None:
        path = "/" + word("Us", "ers") + "/someone/notes"
        self.assertFails(self.plant("See %s.\n" % path, "tools/demo/README.rst"),
                         "forbidden-words", "lab-path")

    # -- the allowlist ---------------------------------------------------------------

    def test_an_allowlisted_binary_passes(self) -> None:
        self.repo.write("tools/demo/table.bin", self.seeds.blob())
        self.repo.commit("tools/demo: add a table", "Add the table the demonstration reads.")
        allowlist = os.path.join(self.tmp, "allowlist.toml")
        with open(allowlist, "w", encoding="utf-8") as handle:
            handle.write('[[allow]]\ngate = "binaries"\npaths = ["tools/demo/table.bin"]\n'
                         'provenance = "generated by the demonstration from its own tests"\n'
                         'reason = "the demonstration needs a table it can read back"\n')
        code, report = self.check("all", "--base", self.base, allowlist=allowlist)
        self.assertEqual(code, 0, report)
        # Allowed twice: in the tree, and in the commit that adds it.
        self.assertEqual([(f["path"], f["commit"] is None) for f in report["allowed"]],
                         [("tools/demo/table.bin", True), ("tools/demo/table.bin", False)])

    def test_a_binary_entry_without_provenance_is_refused(self) -> None:
        allowlist = os.path.join(self.tmp, "allowlist.toml")
        with open(allowlist, "w", encoding="utf-8") as handle:
            handle.write('[[allow]]\ngate = "binaries"\npaths = ["tools/demo/table.bin"]\n'
                         'reason = "the demonstration needs a table it can read back"\n')
        code, report = self.check("tree", allowlist=allowlist)
        self.assertEqual(code, 2)
        self.assertIn("needs a provenance line", report["stderr"])
