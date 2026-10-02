# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of check-config: the verdicts on a fake .config, the exit statuses, and the committed lists.

The first half runs the checker on small configurations written here: a missing
required option fails, a dropped option that came back fails, and so on. The
second half reads the lists that are committed: the flash profile and the
required options must agree with each other, with the board's defconfig and with
the kernel's own symbols, so that a typo in either cannot make the check pass by
asking about an option that does not exist.
"""

import importlib.machinery
import importlib.util
import io
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
TOOL = os.path.join(HERE, "check-config")
EXTERNAL = os.path.dirname(os.path.dirname(HERE))
TOP = os.path.dirname(EXTERNAL)
FRAGMENT = os.path.join(EXTERNAL, "board", "dfp-34x-2c3", "kernel-flash.fragment")
REQUIRED = os.path.join(HERE, "required-options.txt")

_loader = importlib.machinery.SourceFileLoader("check_config", TOOL)
_spec = importlib.util.spec_from_loader("check_config", _loader)
cc = importlib.util.module_from_spec(_spec)
sys.modules["check_config"] = cc
_loader.exec_module(cc)

REQUIRED_TEXT = """\
# options the product needs
CONFIG_NET=y            networking
CONFIG_BRIDGE=y         the bridge of the user ports
CONFIG_NET_CLS_FLOWER=y the VLAN translation is a flower rule
CONFIG_NET_DSA=y        the switch is a DSA switch
CONFIG_MTD_BLOCK=m      the root filesystem is on a block device of the flash
CONFIG_MIPS_FP_SUPPORT=n  the core has no FPU
"""

FRAGMENT_TEXT = """\
# the flash profile
# no symbol table
# CONFIG_KALLSYMS is not set
# CONFIG_IPV6 is not set
CONFIG_LOG_BUF_SHIFT=14
"""

GOOD_CONFIG = """\
# Automatically generated file; DO NOT EDIT.
CONFIG_NET=y
CONFIG_BRIDGE=y
CONFIG_NET_CLS_FLOWER=y
CONFIG_NET_DSA=y
CONFIG_MTD_BLOCK=m
# CONFIG_MIPS_FP_SUPPORT is not set
# CONFIG_KALLSYMS is not set
CONFIG_LOG_BUF_SHIFT=14
CONFIG_SYSFS=y
"""


def put(directory, name, text):
    path = os.path.join(directory, name)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    return path


class Fake(unittest.TestCase):
    """A directory with a required list, a profile and a configuration, written to be changed."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = self._tmp.name
        self.required = put(self.dir, "required.txt", REQUIRED_TEXT)
        self.fragment = put(self.dir, "profile.fragment", FRAGMENT_TEXT)
        self.config = put(self.dir, ".config", GOOD_CONFIG)

    def run_tool(self, config=None, required=None, fragment=None):
        out, err = io.StringIO(), io.StringIO()
        status = cc.main(["--required", required or self.required, "--fragment", fragment or self.fragment,
                          config or self.config], out, err)
        return status, out.getvalue(), err.getvalue()

    def change(self, old, new, text=GOOD_CONFIG):
        self.assertIn(old, text)
        return put(self.dir, ".config.changed", text.replace(old, new, 1))


class Verdicts(Fake):
    def test_a_configuration_that_holds_passes(self):
        status, out, err = self.run_tool()
        self.assertEqual((status, err), (0, ""))
        self.assertIn("6 required options are set and the 3 settings of the flash profile hold", out)

    def test_a_missing_required_option_fails_and_names_its_reason(self):
        config = self.change("CONFIG_NET_CLS_FLOWER=y\n", "")
        status, out, err = self.run_tool(config)
        self.assertEqual(status, 1)
        self.assertIn("1 problems", err)
        self.assertIn("required CONFIG_NET_CLS_FLOWER=y, found CONFIG_NET_CLS_FLOWER not set -- "
                      "the VLAN translation is a flower rule", err)
        self.assertEqual(out, "")

    def test_a_required_option_switched_off_by_a_dependency_fails(self):
        config = self.change("CONFIG_BRIDGE=y\n", "# CONFIG_BRIDGE is not set\n")
        self.assertEqual(self.run_tool(config)[0], 1)

    def test_a_required_option_with_another_value_fails(self):
        config = self.change("CONFIG_MTD_BLOCK=m", "CONFIG_MTD_BLOCK=y")
        status, _out, err = self.run_tool(config)
        self.assertEqual(status, 1)
        self.assertIn("required CONFIG_MTD_BLOCK=m, found CONFIG_MTD_BLOCK=y", err)

    def test_an_option_that_must_not_be_set_fails_when_it_is(self):
        config = self.change("# CONFIG_MIPS_FP_SUPPORT is not set", "CONFIG_MIPS_FP_SUPPORT=y")
        status, _out, err = self.run_tool(config)
        self.assertEqual(status, 1)
        self.assertIn("required CONFIG_MIPS_FP_SUPPORT not set, found CONFIG_MIPS_FP_SUPPORT=y -- "
                      "the core has no FPU", err)

    def test_an_option_that_must_not_be_set_passes_when_it_is_absent(self):
        config = self.change("# CONFIG_MIPS_FP_SUPPORT is not set\n", "")
        self.assertEqual(self.run_tool(config)[0], 0)

    def test_a_dropped_option_that_came_back_fails(self):
        config = self.change("# CONFIG_KALLSYMS is not set", "CONFIG_KALLSYMS=y")
        status, _out, err = self.run_tool(config)
        self.assertEqual(status, 1)
        self.assertIn("1 problems", err)
        self.assertIn("dropped by the flash profile: CONFIG_KALLSYMS not set, found CONFIG_KALLSYMS=y", err)

    def test_a_dropped_option_that_came_back_as_a_module_fails(self):
        config = self.change("CONFIG_SYSFS=y\n", "CONFIG_SYSFS=y\nCONFIG_IPV6=m\n")
        status, _out, err = self.run_tool(config)
        self.assertEqual(status, 1)
        self.assertIn("dropped by the flash profile: CONFIG_IPV6 not set, found CONFIG_IPV6=m", err)

    def test_a_dropped_option_that_is_absent_holds(self):
        config = self.change("# CONFIG_KALLSYMS is not set\n", "")
        self.assertEqual(self.run_tool(config)[0], 0)

    def test_an_option_the_profile_sets_must_have_that_value(self):
        config = self.change("CONFIG_LOG_BUF_SHIFT=14", "CONFIG_LOG_BUF_SHIFT=17")
        status, _out, err = self.run_tool(config)
        self.assertEqual(status, 1)
        self.assertIn("set by the flash profile: CONFIG_LOG_BUF_SHIFT=14, found CONFIG_LOG_BUF_SHIFT=17", err)

    def test_every_problem_is_reported_and_counted(self):
        config = put(self.dir, ".config.bad", "CONFIG_SYSFS=y\nCONFIG_KALLSYMS=y\nCONFIG_IPV6=y\n")
        status, _out, err = self.run_tool(config)
        self.assertEqual(status, 1)
        # five required options are missing, MIPS_FP_SUPPORT holds, both dropped options came back,
        # and the option the profile sets is missing
        self.assertIn("8 problems", err)
        self.assertEqual(len([l for l in err.splitlines() if l.startswith("  ")]), 8)


class CannotCheck(Fake):
    def test_an_unreadable_configuration(self):
        status, _out, err = self.run_tool(os.path.join(self.dir, "nothing"))
        self.assertEqual(status, 2)
        self.assertIn("cannot read the configuration", err)

    def test_a_configuration_with_no_option_proves_nothing(self):
        for text in ("", "# Automatically generated file\n# CONFIG_NET is not set\n"):
            with self.subTest(text=text):
                status, _out, err = self.run_tool(put(self.dir, ".config.empty", text))
                self.assertEqual(status, 2)
                self.assertIn("sets no option", err)

    def test_an_empty_list_of_required_options(self):
        required = put(self.dir, "none.txt", "# nothing\n\n")
        status, _out, err = self.run_tool(required=required)
        self.assertEqual(status, 2)
        self.assertIn("is empty", err)

    def test_an_empty_profile(self):
        fragment = put(self.dir, "none.fragment", "# nothing\n")
        status, _out, err = self.run_tool(fragment=fragment)
        self.assertEqual(status, 2)
        self.assertIn("has no setting", err)

    def test_a_required_option_without_its_reason(self):
        required = put(self.dir, "bad.txt", REQUIRED_TEXT + "CONFIG_PROC_FS=y\n")
        status, _out, err = self.run_tool(required=required)
        self.assertEqual(status, 2)
        self.assertIn("expected 'CONFIG_NAME=VALUE reason'", err)

    def test_a_required_option_listed_twice(self):
        required = put(self.dir, "twice.txt", REQUIRED_TEXT + "CONFIG_NET=y  again\n")
        status, _out, err = self.run_tool(required=required)
        self.assertEqual(status, 2)
        self.assertIn("CONFIG_NET is listed twice", err)

    def test_a_line_of_the_profile_that_is_not_a_setting(self):
        for line in ("CONFIG_KALLSYMS", "KALLSYMS=y", "CONFIG_X = y", "drop KALLSYMS"):
            with self.subTest(line=line):
                fragment = put(self.dir, "bad.fragment", FRAGMENT_TEXT + line + "\n")
                status, _out, err = self.run_tool(fragment=fragment)
                self.assertEqual(status, 2)
                self.assertIn("not a setting of a kernel configuration", err)

    def test_a_list_and_a_profile_that_contradict_each_other(self):
        fragment = put(self.dir, "contradiction.fragment", FRAGMENT_TEXT + "# CONFIG_BRIDGE is not set\n")
        status, _out, err = self.run_tool(fragment=fragment)
        self.assertEqual(status, 2)
        self.assertIn("contradict each other", err)

    def test_a_missing_argument_is_a_usage_error(self):
        done = subprocess.run([sys.executable, TOOL], capture_output=True, text=True)
        self.assertEqual(done.returncode, 2)


class AsAProgram(Fake):
    def run_program(self, *args):
        done = subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)
        return done.returncode, done.stdout, done.stderr

    def test_exit_status_zero_and_the_line_on_standard_output(self):
        status, out, err = self.run_program("--required", self.required, "--fragment", self.fragment, self.config)
        self.assertEqual((status, err), (0, ""))
        self.assertTrue(out.startswith("check-config: ok -- "))

    def test_exit_status_one_and_the_problems_on_standard_error(self):
        config = self.change("# CONFIG_KALLSYMS is not set", "CONFIG_KALLSYMS=y")
        status, out, err = self.run_program("--required", self.required, "--fragment", self.fragment, config)
        self.assertEqual((status, out), (1, ""))
        self.assertTrue(err.startswith("check-config: FAIL -- 1 problems in "))

    def test_help_exits_with_zero(self):
        status, out, _err = self.run_program("--help")
        self.assertEqual(status, 0)
        self.assertIn("--fragment", out)


class Committed(unittest.TestCase):
    """The lists in the repository."""

    @classmethod
    def setUpClass(cls):
        cls.required = cc.read_required(REQUIRED)
        cls.fragment = cc.read_fragment(FRAGMENT)

    def test_no_required_option_is_dropped_and_no_dropped_option_is_required(self):
        wanted = {name: value for name, value, _reason in self.required}
        for name, value in self.fragment:
            if name in wanted:
                self.assertEqual(wanted[name], value, name)

    def test_the_lists_are_consistent_for_the_checker(self):
        self.assertEqual(cc.check({name: (value or "y") for name, value, _ in self.required
                                   if value is not None}
                                  | {name: value for name, value in self.fragment if value is not None},
                                  self.required, self.fragment), [])

    def test_every_required_option_has_a_reason_of_a_few_words(self):
        for name, _value, reason in self.required:
            self.assertGreaterEqual(len(reason.split()), 3, "%s: %r" % (name, reason))

    def test_every_dropped_option_has_a_reason_in_the_comment_above_it(self):
        with open(FRAGMENT, encoding="utf-8") as handle:
            lines = handle.read().splitlines()
        unset = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$")
        for number, line in enumerate(lines):
            if unset.match(line):
                first = number  # options dropped for one reason are written together under one comment
                while first and unset.match(lines[first - 1]):
                    first -= 1
                reason = []
                for above in reversed(lines[:first]):
                    if not above.startswith("# "):
                        break
                    reason.append(above)
                self.assertGreaterEqual(len(" ".join(reason).split()), 10, "no reason above %s" % line)

    def test_every_option_named_is_a_symbol_of_the_kernel(self):
        symbols = kernel_symbols()
        self.assertGreater(len(symbols), 10000)
        named = [name for name, _v, _r in self.required] + [name for name, _v in self.fragment]
        missing = [name for name in named if name[len("CONFIG_"):] not in symbols]
        self.assertEqual(missing, [], "not symbols of the kernel's Kconfig files")

    def test_every_option_named_is_one_a_configuration_can_set(self):
        """A symbol with a prompt can be switched by a fragment; the others are computed."""
        prompted = kernel_symbols(prompted_only=True)
        for name, _value in self.fragment:
            self.assertIn(name[len("CONFIG_"):], prompted, "%s has no prompt: the profile cannot drop it" % name)


_SYMBOL = re.compile(r"^\s*(?:menu)?config\s+([A-Z0-9_]+)\s*$")
_PROMPT = re.compile(r'^\s*(?:bool|tristate|string|int|hex|def_bool|def_tristate|prompt)\b(.*)$')


def kernel_symbols(prompted_only=False):
    """The symbols the kernel's Kconfig files define (only those with a prompt if asked)."""
    cache = kernel_symbols.__dict__.setdefault("cache", {})
    if cache.get(prompted_only) is not None:
        return cache[prompted_only]
    found = set()
    for top, dirs, files in os.walk(os.path.join(TOP, "linux")):
        dirs[:] = [d for d in dirs if d not in (".git", "Documentation", "LICENSES", "tools")]
        for name in files:
            if name == "Kconfig" or name.startswith("Kconfig."):
                with open(os.path.join(top, name), encoding="utf-8", errors="replace") as handle:
                    found |= symbols_in(handle.read().splitlines(), prompted_only)
    cache[prompted_only] = found
    return found


def symbols_in(lines, prompted_only):
    """The symbols of one Kconfig file; with a prompt means 'bool "text"' (or tristate, int ...) on its own."""
    result = set()
    current, prompted = None, False
    for line in lines + ["config END_OF_FILE"]:
        match = _SYMBOL.match(line)
        if match:
            if current and (prompted or not prompted_only):
                result.add(current)
            current, prompted = match.group(1), False
            continue
        if current and re.match(r'^\s*(bool|tristate|string|int|hex)\s+"', line):
            prompted = True
        elif current and re.match(r'^\s*prompt\s+"', line):
            prompted = True
    return result


if __name__ == "__main__":
    unittest.main()
