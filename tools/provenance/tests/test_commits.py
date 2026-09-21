# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The commit-message lint and the component map."""

from __future__ import annotations

import unittest

from gates.commits import commit_component, component_findings, lint_message, mood_problem
from gates.components import DISTRO, DOCS, LINUX, OMCID, TESTS, TOOLS, component_of

from .support import AUTHOR, SIGNOFF

BODY = "Explain the problem first, then the change.\n"


def message(subject: str, body: str = BODY, trailers: str = SIGNOFF) -> str:
    return "%s\n\n%s\n\n%s\n" % (subject, body.rstrip("\n"), trailers)


def rules(msg: str, paths: list[str], **kwargs) -> set[str]:
    kwargs.setdefault("author", AUTHOR)
    return {f.rule for f in lint_message(msg, component=commit_component(paths), paths=paths,
                                         **kwargs)}


class Components(unittest.TestCase):

    def test_every_directory_maps_to_its_component(self) -> None:
        expect = {
            "linux/drivers/net/pon/pon.c": LINUX,
            "omcid/src/main.c": OMCID,
            "br2-external/package/omcid/omcid.mk": DISTRO,
            "buildroot/VERSION": DISTRO,
            "tools/bench/console.py": TOOLS,
            "tests/device/test_boot.py": TESTS,
            "docs/handbook/index.rst": DOCS,
            "README.rst": DOCS,
            "LICENSES/preferred/GPL-2.0-only.txt": DOCS,
            ".github/workflows/ci.yml": DOCS,
            "newdir/file": "newdir/",
        }
        for path, component in expect.items():
            self.assertEqual(component_of(path), component, path)

    def test_one_component_rule(self) -> None:
        self.assertEqual(component_findings(["linux/a.c", "linux/b.c"], None), [])
        self.assertEqual(component_findings(["README.rst", "LICENSES/x", "docs/y"], None), [])
        found = component_findings(["linux/a.c", "omcid/b.c", "omcid/c.c", "omcid/d.c"], "c0ffee")
        self.assertEqual(len(found), 1)
        self.assertIn("2 components", found[0].message)
        self.assertIn(", ...", found[0].message)
        self.assertEqual(found[0].commit, "c0ffee")


class Mood(unittest.TestCase):

    def test_imperatives_pass(self) -> None:
        for first in ("add", "fix", "process", "embed", "bring", "focus", "use", "drop", ""):
            self.assertIsNone(mood_problem(first), first)

    def test_other_moods_fail(self) -> None:
        for first in ("added", "adding", "adds", "fixes", "applies", "is", "the", "WIP"):
            self.assertIsNotNone(mood_problem(first), first)


class Subjects(unittest.TestCase):

    def test_a_good_message_passes_for_each_component(self) -> None:
        cases = [
            ("net: pon: add the OMCI port", ["linux/drivers/net/pon/pon.c"]),
            ("omcid: parse the MIB upload", ["omcid/src/mib.c"]),
            ("package/omcid: bump the version", ["br2-external/package/omcid/omcid.mk"]),
            ("buildroot: pin the release", ["buildroot/VERSION"]),
            ("configs/board_defconfig: enable the watchdog",
             ["br2-external/configs/board_defconfig"]),
            ("tools/bench: add the console", ["tools/bench/console.py"]),
            ("tools: list the tools", ["tools/README.rst"]),
            ("tests/device: add the boot test", ["tests/device/test_boot.py"]),
            ("docs: add the handbook", ["docs/handbook/index.rst"]),
            ("Makefile: run the gates", ["Makefile"]),
            ("README: state the scope", ["README.rst"]),
            ("docs: add a licence text", ["LICENSES/preferred/X.txt", "REUSE.toml"]),
        ]
        for subject, paths in cases:
            self.assertEqual(rules(message(subject), paths), set(), subject)

    def test_prefix_problems(self) -> None:
        self.assertEqual(rules(message("add the handbook"), ["docs/x.rst"]), {"subject-prefix"})
        self.assertEqual(rules(message("omcid: add the handbook"), ["docs/x.rst"]),
                         {"subject-prefix"})
        self.assertEqual(rules(message("package/other: bump it"),
                               ["br2-external/package/omcid/omcid.mk"]), {"subject-prefix"})
        self.assertEqual(rules(message("net: p{on}: add the port"), ["linux/a.c"]),
                         {"subject-prefix"})
        self.assertEqual(rules(message("docs: "), ["docs/x.rst"]), {"subject-summary"})
        self.assertEqual(rules(message(""), ["docs/x.rst"]), {"subject-prefix"})

    def test_prefix_rules_by_component(self) -> None:
        from gates.components import prefix_names_component
        self.assertTrue(prefix_names_component("net", LINUX, ["linux/a.c"]))
        self.assertTrue(prefix_names_component("buildroot/patches", DISTRO,
                                               ["buildroot/patches/0001-mips.patch"]))
        self.assertFalse(prefix_names_component("buildroot/other", DISTRO,
                                                ["buildroot/patches/0001-mips.patch"]))
        found = lint_message(message("add it"), component=TOOLS, paths=[])
        self.assertIn('"tools: ..."', found[0].message)

    def test_prefix_hint_names_the_component(self) -> None:
        found = lint_message(message("add the port"), component=LINUX, paths=["linux/a.c"])
        self.assertIn("subsystem: component", found[0].message)
        found = lint_message(message("add the tool"), component=TOOLS,
                             paths=["tools/bench/a.py", "tools/bench/b.py"])
        self.assertIn('"tools/bench: ..."', found[0].message)
        found = lint_message(message("add the package"), component=DISTRO,
                             paths=["br2-external/package/x/x.mk"])
        self.assertIn('"package/x: ..."', found[0].message)

    def test_mood_length_and_full_stop(self) -> None:
        self.assertEqual(rules(message("docs: added the handbook"), ["docs/x.rst"]),
                         {"subject-imperative"})
        long_subject = "docs: add " + "a" * 70
        self.assertEqual(rules(message(long_subject), ["docs/x.rst"]), {"subject-length"})
        self.assertEqual(rules(message("docs: add the handbook."), ["docs/x.rst"]),
                         {"subject-period"})
        self.assertEqual(rules(message("docs: add the handbook..."), ["docs/x.rst"]), set())

    def test_revert_subjects_skip_the_prefix(self) -> None:
        self.assertEqual(rules(message('Revert "docs: add the handbook"'), ["docs/x.rst"]), set())


class Bodies(unittest.TestCase):

    def test_second_line_must_be_blank(self) -> None:
        msg = "docs: add the handbook\nno blank line\n\n" + SIGNOFF + "\n"
        self.assertIn("blank-line", rules(msg, ["docs/x.rst"]))

    def test_body_wrap(self) -> None:
        long_line = " ".join(["word"] * 20)
        self.assertEqual(rules(message("docs: add it", long_line), ["docs/x.rst"]),
                         {"body-line-length"})
        exempt = "\n".join(["    " + long_line, "https://example.org/" + "a" * 80, "b" * 90])
        self.assertEqual(rules(message("docs: add it", exempt), ["docs/x.rst"]), set())

    def test_long_trailers_are_not_wrapped(self) -> None:
        trailers = SIGNOFF + "\nLink: https://example.org/" + "a" * 90
        self.assertEqual(rules(message("docs: add it", trailers=trailers), ["docs/x.rst"]), set())


class Trailers(unittest.TestCase):

    def test_signoff_is_required(self) -> None:
        self.assertEqual(rules(message("docs: add it", trailers="Reviewed-by: " + AUTHOR),
                               ["docs/x.rst"]), {"signed-off-by"})
        self.assertEqual(rules("docs: add it\n", ["docs/x.rst"]), {"signed-off-by"})

    def test_a_signoff_needs_an_address(self) -> None:
        self.assertEqual(rules(message("docs: add it", trailers="Signed-off-by: Ada Tester"),
                               ["docs/x.rst"]), {"signed-off-by"})
        self.assertEqual(rules(message("docs: add it"), ["docs/x.rst"], author="no address"),
                         set())

    def test_signoff_must_be_the_authors(self) -> None:
        other = "Signed-off-by: Bo Other <bo@example.org>"
        self.assertEqual(rules(message("docs: add it", trailers=other), ["docs/x.rst"]),
                         {"signed-off-by"})
        self.assertEqual(rules(message("docs: add it", trailers=other + "\n" + SIGNOFF),
                               ["docs/x.rst"]), set())

    def test_trailer_block_must_be_all_trailers(self) -> None:
        self.assertEqual(rules(message("docs: add it", trailers="Not a trailer\n" + SIGNOFF),
                               ["docs/x.rst"]), {"signed-off-by"})
        folded = SIGNOFF + "\nLink: https://example.org/a\n  continued"
        self.assertEqual(rules(message("docs: add it", trailers=folded), ["docs/x.rst"]), set())

    def test_co_developed_by_needs_its_own_signoff(self) -> None:
        other = "Bo Other <bo@example.org>"
        good = "Co-developed-by: %s\nSigned-off-by: %s\n%s" % (other, other, SIGNOFF)
        self.assertEqual(rules(message("docs: add it", trailers=good), ["docs/x.rst"]), set())
        bad = "Co-developed-by: %s\n%s" % (other, SIGNOFF)
        self.assertEqual(rules(message("docs: add it", trailers=bad), ["docs/x.rst"]),
                         {"co-developed-by"})
        last = "%s\nCo-developed-by: %s" % (SIGNOFF, other)
        self.assertEqual(rules(message("docs: add it", trailers=last), ["docs/x.rst"]),
                         {"co-developed-by"})

    def test_links_must_be_public(self) -> None:
        host = ":".join(["2001", "4860", "", "1"])
        public = SIGNOFF + "\nLink: https://example.org/x\nCloses: https://[%s]/x" % host
        self.assertEqual(rules(message("docs: add it", trailers=public), ["docs/x.rst"]), set())
        for url in ("http://localhost/x", "https://" + ".".join(["wiki", "lan"]) + "/x",
                    "ftp://example.org/x",
                    "https://nas/x", "http://" + ".".join(["10", "1", "2", "3"]) + "/x"):
            found = rules(message("docs: add it", trailers=SIGNOFF + "\nLink: " + url),
                          ["docs/x.rst"])
            self.assertEqual(found, {"link-public"}, url)

    def test_fixes_must_name_a_local_commit(self) -> None:
        exists = {"0123456789ab"}.__contains__
        good = SIGNOFF + '\nFixes: 0123456789ab ("docs: add it")'
        self.assertEqual(rules(message("docs: fix it", trailers=good), ["docs/x.rst"],
                               commit_exists=exists), set())
        unknown = SIGNOFF + '\nFixes: ba9876543210 ("docs: add it")'
        self.assertEqual(rules(message("docs: fix it", trailers=unknown), ["docs/x.rst"],
                               commit_exists=exists), {"fixes-local"})
        short = SIGNOFF + "\nFixes: abc123"
        self.assertEqual(rules(message("docs: fix it", trailers=short), ["docs/x.rst"]),
                         {"fixes-local"})

    def test_co_authored_by_is_accepted(self) -> None:
        trailers = SIGNOFF + "\nCo-Authored-By: A Tool <tool@example.org>"
        self.assertEqual(rules(message("docs: add it", trailers=trailers), ["docs/x.rst"]), set())


if __name__ == "__main__":
    unittest.main()
