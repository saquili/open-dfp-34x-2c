# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""What each mode of the command line reads, and what it leaves alone.

The kernel is imported as a pristine upstream subtree; the gates must read
only what differs from the import, or every upstream file would have to be
allowlisted. Commits are read by the lines they add, a change by what it adds
on top of its base.
"""

from __future__ import annotations

import os

from .support import (SIGNOFF, Repo, Seeds, TempDirTestCase, empty_denylist, pairs, run_check,
                      word)


class Scanning(TempDirTestCase):

    def setUp(self) -> None:
        super().setUp()
        self.seeds = Seeds()
        # Upstream text that the gates would refuse if they read it.
        self.upstream_ip = self.seeds.private_ipv4()
        self.narrative = "the " + word("ven", "dor")
        # An upstream link whose target the gates would refuse if they read it.
        self.lab_path = "/" + word("Us", "ers") + "/someone/"
        self.repo = Repo(os.path.join(self.tmp, "repo"))
        self.repo.write("README.rst", "A tree.\n")
        self.root = self.repo.commit("README: describe the tree", "Say what the tree holds.")
        self.repo.subtree_import("linux", {
            "net/core.c": "int core;\n/* reach %s */\nint tail;\n" % self.upstream_ip,
            "MAINTAINERS": "x\n" * 10,
        }, links={"net/link": self.lab_path + "upstream"})
        self.merge = self.repo.head()

    def findings(self, report: dict) -> list[tuple[str, str, str, int]]:
        return sorted((f["rule"], f["path"] or "", f["text"], f["line"] or 0)
                      for f in report["findings"])

    def test_a_pristine_import_passes(self) -> None:
        code, report = run_check("all", "--history", repo=self.repo.path)
        self.assertEqual(code, 0, report)
        self.assertEqual(report["scanned_commits"], 3)

    def test_the_tree_reads_only_what_differs_from_upstream(self) -> None:
        with open(os.path.join(self.repo.path, "linux/net/core.c"), "a", encoding="utf-8") as fh:
            fh.write("/* as %s does */\n" % self.narrative)
        self.repo.write("linux/drivers/new.c", "/* at %s */\n" % self.seeds.mac())
        # Uncommitted first: the working tree is compared with the import.
        code, report = run_check("tree", repo=self.repo.path, allowlist=None)
        self.assertEqual(code, 1)
        self.assertEqual({(r, p, n) for r, p, _t, n in self.findings(report)},
                         {("narrative", "linux/net/core.c", 4)})  # new.c is untracked
        code, report = run_check("tree", "--untracked", repo=self.repo.path)
        self.assertEqual({r for r, _p, _t, _n in self.findings(report)},
                         {"narrative", "mac-address"})
        # Then committed: the index is compared with the import.
        self.repo.commit("net: add a driver", "Add it.")
        code, report = run_check("tree", repo=self.repo.path)
        self.assertEqual({(r, p) for r, p, _t, _n in self.findings(report)},
                         {("narrative", "linux/net/core.c"), ("mac-address", "linux/drivers/new.c")})
        self.assertNotIn(self.upstream_ip, str(report))

    def test_an_upstream_link_is_read_once_it_changes(self) -> None:
        # As imported, the link is upstream's and its target is not read.
        code, report = run_check("tree", repo=self.repo.path)
        self.assertEqual(code, 0, report)
        link = os.path.join(self.repo.path, "linux/net/link")
        os.remove(link)
        os.symlink(self.lab_path + "changed", link)
        # Uncommitted, the working tree is compared with the import.
        code, report = run_check("tree", repo=self.repo.path)
        self.assertEqual([(f["rule"], f["path"]) for f in report["findings"]],
                         [("lab-path", "linux/net/link")])
        # Committed, both the tree and the commit read the new target.
        self.repo.commit("net: move the link", "Move it.")
        code, report = run_check("all", "--base", self.merge, repo=self.repo.path)
        found = sorted((f["rule"], f["path"], f["commit"] is None) for f in report["findings"])
        self.assertEqual(found, [("lab-path", "linux/net/link", False),
                                 ("lab-path", "linux/net/link", True)])

    def test_history_skips_the_import_and_reads_added_lines(self) -> None:
        with open(os.path.join(self.repo.path, "linux/net/core.c"), "a", encoding="utf-8") as fh:
            fh.write("/* as %s does */\n" % self.narrative)
        self.repo.commit("net: core: note the tail", "Note it.")
        code, report = run_check("range", "%s..HEAD" % self.root, repo=self.repo.path)
        self.assertEqual(code, 1)
        self.assertEqual([(r, p, n) for r, p, _t, n in self.findings(report)],
                         [("narrative", "linux/net/core.c", 4)])

    def test_a_large_upstream_file_is_not_size_checked(self) -> None:
        with open(os.path.join(self.repo.path, "linux/MAINTAINERS"), "a", encoding="utf-8") as fh:
            fh.write("y\n")
        self.repo.commit("MAINTAINERS: add an entry", "Add it.")
        code, report = run_check("--size-cap", "4", "all", "--base", self.merge,
                                 repo=self.repo.path)
        self.assertEqual(code, 1)
        self.assertEqual({(f["rule"], f["path"]) for f in report["findings"]},
                         {("file-size", "README.rst")})

    def test_changes_reads_what_a_revision_adds_on_its_base(self) -> None:
        base = self.repo.head()
        with open(os.path.join(self.repo.path, "linux/net/core.c"), "a", encoding="utf-8") as fh:
            fh.write("/* as %s does */\n" % self.narrative)
        self.repo.write("linux/drivers/new.c", "/* at %s */\n" % self.seeds.mac())
        self.repo.commit("net: add a driver", "Add it.")
        self.repo.write("linux/drivers/new.c", "int clean;\n")
        self.repo.commit("net: clean the driver", "Clean it.")
        code, report = run_check("changes", base, repo=self.repo.path)
        self.assertEqual(code, 1)
        self.assertEqual([(r, p, n) for r, p, _t, n in self.findings(report)],
                         [("narrative", "linux/net/core.c", 4)])
        self.assertEqual(run_check("changes", base, base, repo=self.repo.path)[0], 0)

    def test_changes_leaves_the_size_of_a_base_file_to_upstream(self) -> None:
        with open(os.path.join(self.repo.path, "README.rst"), "a", encoding="utf-8") as fh:
            fh.write("More.\n")
        self.repo.write("docs/new.rst", "A new page.\n")
        self.repo.commit("docs: extend the tree", "Extend it.")
        code, report = run_check("--size-cap", "4", "changes", self.merge, repo=self.repo.path)
        self.assertEqual({(f["rule"], f["path"]) for f in report["findings"]},
                         {("file-size", "docs/new.rst")})

    def test_the_message_mode_checks_a_message_before_it_is_committed(self) -> None:
        msg = os.path.join(self.tmp, "COMMIT_EDITMSG")
        with open(msg, "w", encoding="utf-8") as handle:
            handle.write("Added the driver\n\nAs %s does.\n# a comment git strips\n"
                         % self.narrative)
        code, report = run_check("message", msg, "--paths", "linux/a.c", "omcid/b.c",
                                 repo=self.repo.path)
        self.assertEqual(code, 1)
        self.assertLessEqual({("forbidden-words", "narrative"), ("one-component", "one-component"),
                              ("commit-message", "signed-off-by")}, pairs(report))
        # Without --paths it takes the staged paths.
        self.repo.write("linux/a.c", "int a;\n")
        self.repo.git("add", "linux/a.c")
        code, report = run_check("message", msg, repo=self.repo.path)
        self.assertNotIn(("one-component", "one-component"), pairs(report))

    def test_a_plain_directory(self) -> None:
        tree = os.path.join(self.tmp, "plain")
        os.makedirs(os.path.join(tree, ".git"))
        with open(os.path.join(tree, "notes.txt"), "w", encoding="utf-8") as handle:
            handle.write("reach %s\n" % self.seeds.private_ipv4())
        with open(os.path.join(tree, ".git", "ignored.txt"), "w", encoding="utf-8") as handle:
            handle.write("reach %s\n" % self.seeds.private_ipv4())
        code, report = run_check("tree", "--dir", tree)
        self.assertEqual(code, 1)
        self.assertEqual([(f["rule"], f["path"]) for f in report["findings"]],
                         [("ipv4-address", "notes.txt")])

    def test_a_symlink_target_is_content(self) -> None:
        os.symlink("/" + word("Us", "ers") + "/someone/tree", os.path.join(self.repo.path, "ln"))
        self.repo.commit("docs: link the tree", "Link it.")
        code, report = run_check("all", "--base", self.merge, repo=self.repo.path)
        self.assertEqual(code, 1)
        found = [(f["rule"], f["path"], f["commit"] is None) for f in report["findings"]]
        self.assertEqual(sorted(found), [("lab-path", "ln", False), ("lab-path", "ln", True)])

    def test_a_submodule_is_skipped(self) -> None:
        # A gitlink names a commit of another repository; there is no blob to read.
        sub = Repo(os.path.join(self.tmp, "sub"))
        sub.write("x.txt", "reach %s\n" % self.seeds.private_ipv4())
        other = sub.commit("docs: add x", "Add it.")
        self.repo.git("update-index", "--add", "--cacheinfo", "160000,%s,sub" % other)
        self.repo.git("commit", "-q", "-m", "docs: add a submodule\n\nAdd it.\n\n" + SIGNOFF)
        code, report = run_check("all", "--base", self.merge, repo=self.repo.path)
        self.assertEqual(code, 0, report)

    def test_a_deletion(self) -> None:
        self.repo.git("rm", "-q", "README.rst")
        self.repo.commit("README: drop it", "Drop it.")
        code, report = run_check("range", "HEAD~1..HEAD", repo=self.repo.path)
        self.assertEqual(code, 0, report)


class GitEdges(TempDirTestCase):

    def setUp(self) -> None:
        super().setUp()
        self.repo = Repo(os.path.join(self.tmp, "repo"))

    def test_a_quoted_non_ascii_path(self) -> None:
        self.repo.write("docs/caf\u00e9.rst", "fine\nreach %s\n" % Seeds().private_ipv4())
        self.repo.commit("docs: add the caf\u00e9 notes", "Add them.")
        code, report = run_check("range", "HEAD", repo=self.repo.path)
        self.assertEqual(code, 1)
        self.assertEqual([(f["path"], f["line"]) for f in report["findings"]],
                         [("docs/caf\u00e9.rst", 2)])

    def test_linux_without_an_import_is_scanned_whole(self) -> None:
        self.repo.write("linux/a.c", "int a;\n")
        code, report = run_check("tree", "--untracked", repo=self.repo.path)
        self.assertEqual(code, 0)
        self.assertIn("linux/ has no subtree import yet; scanned whole", report["notes"])
        # A message that only mentions the trailer is not an import.
        self.repo.commit("linux: add a", "git-subtree-dir: linux")
        code, report = run_check("tree", repo=self.repo.path)
        self.assertIn("linux/ has no subtree import yet; scanned whole", report["notes"])

    def test_plumbing_errors(self) -> None:
        from gates import gitio
        self.repo.write("a.txt", "a\n")
        self.repo.commit("docs: add a", "Add it.")
        self.assertIsNone(gitio.toplevel(self.tmp))
        self.assertEqual(os.path.realpath(gitio.toplevel(self.repo.path)),
                         os.path.realpath(self.repo.path))
        with gitio.BlobReader(self.repo.path) as blobs:
            with self.assertRaises(gitio.GitError):
                blobs.read("0" * 40)
        with self.assertRaises(gitio.GitError):
            gitio.diff_no_index(self.repo.path, os.path.join(self.tmp, "none-a"),
                                os.path.join(self.tmp, "none-b"))

    def test_a_plain_directory_skips_links(self) -> None:
        from gates import gitio
        tree = os.path.join(self.tmp, "plain")
        os.makedirs(tree)
        with open(os.path.join(tree, "a.txt"), "w", encoding="utf-8") as handle:
            handle.write("a\n")
        os.symlink("a.txt", os.path.join(tree, "b.txt"))
        self.assertEqual(list(gitio.walk_files(tree)), ["a.txt"])


class CommandLine(TempDirTestCase):

    def setUp(self) -> None:
        super().setUp()
        self.repo = Repo(os.path.join(self.tmp, "repo"))
        self.repo.write("README.rst", "A tree.\n")
        self.repo.commit("README: describe the tree", "Say what the tree holds.")

    def test_configuration_errors_exit_2(self) -> None:
        code, report = run_check("--gates", "nope", "tree", repo=self.repo.path)
        self.assertEqual((code, report["stderr"]), (2, "unknown gates: nope\n"))
        bad = os.path.join(self.tmp, "bad.sha256")
        with open(bad, "w", encoding="utf-8") as handle:
            handle.write("nonsense\n")
        code, report = run_check("tree", repo=self.repo.path, denylist=bad)
        self.assertEqual(code, 2)
        self.assertIn("the format line must come first", report["stderr"])
        code, report = run_check("range", "no-such-rev", repo=self.repo.path)
        self.assertEqual(code, 2)
        self.assertIn("unknown revision", report["stderr"])

    def test_a_missing_base_is_noted(self) -> None:
        code, report = run_check("all", "--base", "no-such-branch", repo=self.repo.path)
        self.assertEqual(code, 0)
        self.assertIn("base no-such-branch not found; commits not checked", report["notes"])

    def test_gates_can_be_selected(self) -> None:
        self.repo.write("notes.txt", "reach %s\n" % Seeds().private_ipv4())
        self.repo.commit("docs: add notes", "Add them.")
        self.assertEqual(run_check("--gates", "forbidden-words", "tree", repo=self.repo.path)[0], 0)
        self.assertEqual(run_check("--gates", "unit-ids", "tree", repo=self.repo.path)[0], 1)

    def test_the_default_repository_is_the_current_one(self) -> None:
        from gates import cli
        self.assertTrue(os.path.isdir(cli._default_repo()))

    def test_a_message_without_an_author_identity(self) -> None:
        from unittest import mock
        msg = os.path.join(self.tmp, "MSG")
        with open(msg, "w", encoding="utf-8") as handle:
            handle.write("docs: add it\n\nAdd it.\n\nSigned-off-by: Someone <s@example.org>\n")
        env = {"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
               "GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "user.useConfigOnly",
               "GIT_CONFIG_VALUE_0": "true", "EMAIL": ""}
        with mock.patch.dict(os.environ, env):
            for key in ("GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL"):
                os.environ.pop(key, None)
            code, report = run_check("message", msg, "--paths", "docs/x.rst",
                                     repo=self.repo.path)
        self.assertEqual(code, 0, report)

    def test_text_report(self) -> None:
        import contextlib
        import io

        from gates import cli
        self.repo.write("notes.txt", "reach %s\n" % Seeds().private_ipv4())
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cli.main(["--repo", self.repo.path, "--denylist", empty_denylist(),
                             "--allowlist", os.devnull, "tree", "--untracked"])
        self.assertEqual(code, 1)
        text = out.getvalue()
        self.assertIn("== unit-ids: 1 finding\n  notes.txt:1:7 [text]: ipv4-address:", text)
        self.assertIn("tools/provenance: FAIL -- 2 files, 0 commits scanned", text)
