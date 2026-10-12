# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The tools of a run: what a case declares it needs, how they reach the stick."""

from __future__ import annotations

import ast
import dataclasses
import gzip
import io
import os
import re
import tarfile
import tempfile
import unittest

from devtest import cases_data, cases_net, cases_pon, cases_system, tools  # noqa: F401
from devtest.access import Remote, Settings
from devtest.core import CASES, PASS, SKIP, Case, Context, passed, run_case
from devtest.tests.test_devtest import SETTINGS, Canned

PROGRAMS = ("sbin/tc", "sbin/bridge", "sbin/devlink")


def diag(root: str, programs=PROGRAMS, lib=True, tables=True) -> str:
    """A directory laid out as a build's diag directory."""
    for rel in programs:
        os.makedirs(os.path.join(root, os.path.dirname(rel)), exist_ok=True)
        with open(os.path.join(root, rel), "wb") as handle:
            handle.write(b"\x7fELF " + rel.encode())
        os.chmod(os.path.join(root, rel), 0o755)
    if lib:
        os.makedirs(os.path.join(root, "lib"), exist_ok=True)
        with open(os.path.join(root, "lib", "libuClibc-1.0.59.so"), "wb") as handle:
            handle.write(b"the whole library")
    if tables:
        os.makedirs(os.path.join(root, "usr", "share", "iproute2"), exist_ok=True)
        with open(os.path.join(root, "usr", "share", "iproute2", "rt_tables"), "w") as handle:
            handle.write("255 local\n")
    return root


class Stick(Canned):
    """A stick that has none of the tools until a command unpacks an archive."""

    def __init__(self, trail, has=()):
        super().__init__("stick", [], trail)
        self.has = set(has)
        self.archives: list[bytes] = []

    def run(self, command, *, timeout=60.0, check=False, stdin=None):
        self.asked.append(command)
        self.trail.append(f"stick$ {command}\n")
        match = re.fullmatch(r"command -v (\S+)", command)
        if match:
            return self.reply(0 if match.group(1) in self.has else 1)
        if "gunzip -c | tar -x" in command:
            self.archives.append(stdin)
            self.has |= {"tc", "bridge", "devlink"}
        return self.reply(0)

    @staticmethod
    def reply(code):
        from devtest.access import Reply
        return Reply(["x"], code, "", "", 0.01)


def context(tools_dir: str = "", has=()) -> Context:
    trail: list[str] = []
    settings = dataclasses.replace(SETTINGS, tools=tools_dir)
    return Context(settings, Stick(trail, has), Canned("bench", [], trail), trail)


class Archive(unittest.TestCase):
    def members(self, root):
        data = gzip.decompress(tools.archive(root))
        with tarfile.open(fileobj=io.BytesIO(data)) as tar:
            return {m.name: m for m in tar.getmembers()}, tar

    def test_it_holds_the_programs_the_whole_library_the_tables_and_a_script_each(self):
        with tempfile.TemporaryDirectory() as root:
            got, _ = self.members(diag(root))
        self.assertTrue({"sbin/tc", "sbin/bridge", "sbin/devlink", "lib/libuClibc-1.0.59.so",
                         "lib/libc.so.0", "usr/share/iproute2/rt_tables", "bin/tc", "bin/bridge",
                         "bin/devlink"} <= set(got))
        self.assertTrue(got["lib/libc.so.0"].issym())
        self.assertEqual(got["lib/libc.so.0"].linkname, "libuClibc-1.0.59.so")
        self.assertEqual(got["bin/tc"].mode & 0o777, 0o755)
        self.assertEqual(got["sbin/tc"].mode & 0o111, 0o111)
        self.assertEqual({(m.uid, m.gid) for m in got.values()}, {(0, 0)})

    def test_a_script_runs_its_program_with_the_library_of_the_tools_alone(self):
        with tempfile.TemporaryDirectory() as root:
            data = gzip.decompress(tools.archive(diag(root)))
        with tarfile.open(fileobj=io.BytesIO(data)) as tar:
            body = tar.extractfile("bin/devlink").read().decode()
        self.assertEqual(body, "#!/bin/sh\nLD_LIBRARY_PATH=/run/devtest-tools/lib "
                               'exec /run/devtest-tools/sbin/devlink "$@"\n')

    def test_it_is_the_same_bytes_every_time(self):
        with tempfile.TemporaryDirectory() as root:
            diag(root)
            self.assertEqual(tools.archive(root), tools.archive(root))

    def test_a_directory_that_is_no_diag_directory_is_named_for_what_it_lacks(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(tools.ToolsError, r"sbin/bridge.*missing"):
                tools.archive(diag(root, programs=("sbin/tc", "sbin/devlink")))
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(tools.ToolsError, r"lib/libuClibc-\*\.so.*missing"):
                tools.archive(diag(root, lib=False))
        with self.assertRaisesRegex(tools.ToolsError, "no such directory"):
            tools.archive("/nonexistent/diag")

    def test_tables_the_build_lacks_are_left_out(self):
        with tempfile.TemporaryDirectory() as root:
            got, _ = self.members(diag(root, tables=False))
        self.assertFalse([n for n in got if n.startswith("usr/")])


class Ensure(unittest.TestCase):
    def test_a_stick_that_has_the_tool_is_left_alone(self):
        ctx = context(has={"tc"})
        self.assertEqual(tools.ensure(ctx, ("tc",)), [])
        self.assertEqual(ctx.stick.asked, ["command -v tc"])
        self.assertNotIn("tools", ctx.memo)

    def test_without_a_diag_directory_the_missing_tool_is_reported(self):
        ctx = context()
        self.assertEqual(tools.ensure(ctx, ("tc", "bridge")), ["tc", "bridge"])
        self.assertEqual(ctx.stick.archives, [])

    def test_with_one_the_tools_are_put_on_the_stick_once(self):
        with tempfile.TemporaryDirectory() as root:
            ctx = context(diag(root))
            self.assertEqual(tools.ensure(ctx, ("tc",)), [])
            self.assertEqual(tools.ensure(ctx, ("bridge",)), [])
        self.assertEqual(len(ctx.stick.archives), 1)
        unpack = [c for c in ctx.stick.asked if "gunzip" in c][0]
        self.assertIn("mount -t tmpfs -o size=4096k,mode=755 tmpfs /run/devtest-tools", unpack)
        self.assertIn("mountpoint -q /run/devtest-tools ||", unpack)
        self.assertEqual(ctx.memo["tools"], ctx.settings.tools)

    def test_a_tool_the_archive_does_not_bring_stays_missing(self):
        with tempfile.TemporaryDirectory() as root:
            ctx = context(diag(root))
            self.assertEqual(tools.ensure(ctx, ("tc", "nft")), ["nft"])

    def test_after_a_reset_the_next_case_puts_them_there_again(self):
        with tempfile.TemporaryDirectory() as root:
            ctx = context(diag(root))
            tools.ensure(ctx, ("tc",))
            ctx.stick.has.clear()
            self.assertEqual(tools.ensure(ctx, ("tc",)), [])
        self.assertEqual(len(ctx.stick.archives), 2)

    def test_remove_takes_them_away_only_when_the_run_put_them_there(self):
        ctx = context(has={"tc"})
        tools.ensure(ctx, ("tc",))
        tools.remove(ctx)
        self.assertFalse([c for c in ctx.stick.asked if "umount" in c])
        with tempfile.TemporaryDirectory() as root:
            ctx = context(diag(root))
            tools.ensure(ctx, ("tc",))
            tools.remove(ctx)
        self.assertTrue([c for c in ctx.stick.asked if c.startswith("umount /run/devtest-tools")])
        self.assertNotIn("tools", ctx.memo)


class Running(unittest.TestCase):
    def case(self, *needs):
        return Case("t.one", "T", "needs", lambda ctx: passed("ran"), tools=needs)

    def test_a_case_without_the_tool_is_skipped_and_says_what_to_do(self):
        result = run_case(self.case("tc"), context())
        self.assertEqual(result.status, SKIP)
        self.assertIn("tc not on the stick", result.evidence)
        self.assertIn("--tools", result.evidence)

    def test_a_case_with_the_tool_runs(self):
        self.assertEqual(run_case(self.case("tc"), context(has={"tc"})).status, PASS)

    def test_a_case_runs_after_the_tools_were_put_on_the_stick(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertEqual(run_case(self.case("tc"), context(diag(root))).status, PASS)

    def test_a_diag_directory_that_cannot_supply_them_skips_the_case(self):
        with tempfile.TemporaryDirectory() as root:
            result = run_case(self.case("tc"), context(diag(root, lib=False)))
        self.assertEqual(result.status, SKIP)
        self.assertIn("no diag directory", result.evidence)

    def test_a_case_that_needs_nothing_asks_the_stick_nothing(self):
        ctx = context()
        run_case(self.case(), ctx)
        self.assertEqual(ctx.stick.asked, [])


class Declared(unittest.TestCase):
    """A case that runs tc, bridge or devlink says so, so that a run on the flash's
    image skips it with a reason rather than failing on a missing program."""

    COMMAND = re.compile(r"(^|[;|&(\s\"])(tc|bridge|devlink) "
                         r"(filter|qdisc|-s|dev|fdb|link|mdb|-[a-zA-Z]|show|class|chain|monitor)")

    def used(self, path):
        with open(path, encoding="utf-8") as handle:
            tree = ast.parse(handle.read())
        funcs = {n.name: n for n in tree.body if isinstance(n, ast.FunctionDef)}
        direct, calls = {}, {}
        for name, fn in funcs.items():
            direct[name], calls[name] = set(), set()
            for node in ast.walk(fn):
                if isinstance(node, ast.Constant) and isinstance(node.value, str):
                    direct[name] |= {m.group(2) for m in self.COMMAND.finditer(node.value)}
                if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) \
                        and node.func.id in funcs:
                    calls[name].add(node.func.id)

        def whole(name, seen=()):
            found = set(direct[name])
            for callee in calls[name]:
                if callee not in seen:
                    found |= whole(callee, seen + (name,))
            return found

        return {name: whole(name) for name in funcs}

    def test_every_case_declares_the_tools_it_runs(self):
        by_function = {}
        for module in (cases_data, cases_net, cases_pon, cases_system):
            used = self.used(module.__file__)
            for c in CASES:
                if c.run.__module__ == module.__name__:
                    by_function[c.id] = used[c.run.__name__]
        self.assertTrue(by_function)
        for c in CASES:
            self.assertEqual(set(c.tools), by_function[c.id], c.id)
        self.assertEqual({t for c in CASES for t in c.tools}, {"tc", "bridge", "devlink"})

    def test_a_declared_tool_is_one_the_tools_can_supply(self):
        for c in CASES:
            for name in c.tools:
                self.assertIn(name, tools.PROGRAMS, c.id)


class Remotes(unittest.TestCase):
    def test_a_command_may_be_given_bytes_that_are_no_text(self):
        log: list[str] = []
        remote = Remote("x", ["sh", "-c"], log)
        reply = remote.run("wc -c", stdin=bytes(range(256)))
        self.assertEqual(reply.out.strip(), "256")

    def test_the_prefix_runs_before_the_command_but_is_not_recorded(self):
        log: list[str] = []
        remote = Remote("x", ["sh", "-c"], log)
        remote.prefix = "WHERE=here; "
        reply = remote.run('echo "$WHERE"')
        self.assertEqual(reply.out.strip(), "here")
        self.assertTrue(log[0].startswith('x$ echo "$WHERE"\n'))
        self.assertNotIn("WHERE=here", log[0])


if __name__ == "__main__":
    unittest.main()
