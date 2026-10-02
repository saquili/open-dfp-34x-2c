# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of image-budget: the verdicts, the exit statuses and the budgets.

The images are sparse files of the sizes under test; the tool reads only their
sizes. The tool runs in this process for the verdicts and as a program for its
exit statuses.
"""

import contextlib
import importlib.machinery
import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "image-budget")
TOOLS = os.path.dirname(os.path.dirname(HERE))
_loader = importlib.machinery.SourceFileLoader("image_budget", TOOL)
_spec = importlib.util.spec_from_loader("image_budget", _loader)
budget = importlib.util.module_from_spec(_spec)
sys.modules["image_budget"] = budget
_loader.exec_module(budget)

K0 = 1359872
R0 = 2572288


def make_image(directory, name, size):
    path = os.path.join(directory, name)
    with open(path, "wb") as handle:
        handle.truncate(size)
    return path


def run(args):
    """Run the tool as a program; return (status, standard output, standard error)."""
    done = subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)
    return done.returncode, done.stdout, done.stderr


class Budgets(unittest.TestCase):
    def test_the_partitions_are_those_of_the_flash_layout(self):
        self.assertEqual(budget.BUDGETS["k0"][1], K0)
        self.assertEqual(budget.BUDGETS["r0"][1], R0)
        self.assertEqual(K0, 1328 * 1024)
        self.assertEqual(R0, 2512 * 1024)

    def test_the_names_of_the_images_are_those_the_build_leaves(self):
        self.assertEqual(budget.BUDGETS["k0"][0], "k0.uImage")
        self.assertEqual(budget.BUDGETS["r0"][0], "r0.squashfs")

    def test_the_bench_takes_the_same_sizes(self):
        sys.path.insert(0, TOOLS)
        try:
            from bench import image
        finally:
            sys.path.remove(TOOLS)
        self.assertEqual(image.K0_SIZE, budget.BUDGETS["k0"][1])
        self.assertEqual(image.R0_SIZE, budget.BUDGETS["r0"][1])

    def test_the_flash_layout_page_gives_the_same_sizes(self):
        page = os.path.join(os.path.dirname(TOOLS), "docs", "handbook", "boot-contract.rst")
        if not os.path.exists(page):
            self.skipTest("the handbook is not part of this checkout")
        with open(page, encoding="utf-8") as handle:
            text = handle.read()
        for needle in ("1328 KiB (``0x14c000``, 1359872 bytes)",
                       "2512 KiB (``0x274000``, 2572288 bytes)"):
            self.assertIn(needle, text)


class Verdicts(unittest.TestCase):
    def test_an_image_that_fits_reports_its_margin(self):
        self.assertEqual(budget.describe("r0", 812345, R0),
                         "r0: 812345 bytes against a budget of 2572288 (2512 KiB): "
                         "margin 1759943 bytes (68.4% of the partition)")

    def test_an_image_that_is_over_reports_the_excess(self):
        self.assertEqual(budget.describe("k0", 2543210, K0),
                         "k0: 2543210 bytes against a budget of 1359872 (1328 KiB): "
                         "over the budget by 1183338 bytes")

    def test_an_image_of_exactly_the_budget_fits_with_no_margin(self):
        self.assertIn("margin 0 bytes (0.0% of the partition)", budget.describe("k0", K0, K0))

    def test_one_byte_over_is_over(self):
        self.assertIn("over the budget by 1 bytes", budget.describe("k0", K0 + 1, K0))

    def test_one_byte_under_has_a_margin_of_one(self):
        self.assertIn("margin 1 bytes", budget.describe("r0", R0 - 1, R0))


class Checking(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = self._tmp.name

    def check(self, words, enforce=False):
        out, err = io.StringIO(), io.StringIO()
        status = budget.check(words, self.dir, enforce, out, err)
        return status, out.getvalue(), err.getvalue()

    def test_both_images_are_checked_by_default(self):
        make_image(self.dir, "k0.uImage", 1000000)
        make_image(self.dir, "r0.squashfs", 2000000)
        status, out, err = self.check([])
        self.assertEqual((status, err), (0, ""))
        lines = out.splitlines()
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[0].startswith("k0: 1000000 bytes"))
        self.assertTrue(lines[1].startswith("r0: 2000000 bytes"))
        self.assertTrue(lines[0].endswith("(%s)" % os.path.join(self.dir, "k0.uImage")))

    def test_an_image_over_its_budget_is_reported_and_is_not_an_error(self):
        make_image(self.dir, "k0.uImage", K0 + 1183338)
        make_image(self.dir, "r0.squashfs", 812345)
        status, out, err = self.check([])
        self.assertEqual((status, err), (0, ""))
        self.assertIn("over the budget by 1183338 bytes", out)
        self.assertIn("margin 1759943 bytes", out)

    def test_enforce_makes_an_image_over_its_budget_a_failure(self):
        make_image(self.dir, "k0.uImage", K0 + 1)
        make_image(self.dir, "r0.squashfs", 812345)
        status, out, err = self.check([], enforce=True)
        self.assertEqual(status, 1)
        self.assertIn("over the budget by 1 bytes", out)
        self.assertIn("FAIL -- over the budget: k0", err)

    def test_enforce_passes_when_everything_fits(self):
        make_image(self.dir, "k0.uImage", K0)
        make_image(self.dir, "r0.squashfs", R0)
        self.assertEqual(self.check([], enforce=True)[0], 0)

    def test_enforce_can_be_limited_to_the_root_filesystem(self):
        make_image(self.dir, "k0.uImage", K0 * 2)
        make_image(self.dir, "r0.squashfs", 812345)
        self.assertEqual(self.check(["r0"], enforce=True)[0], 0)
        self.assertEqual(self.check(["k0"], enforce=True)[0], 1)

    def test_a_limited_check_does_not_read_the_other_image(self):
        make_image(self.dir, "r0.squashfs", 812345)
        status, out, err = self.check(["r0"])
        self.assertEqual((status, err), (0, ""))
        self.assertEqual(len(out.splitlines()), 1)

    def test_an_image_can_be_named_by_its_file(self):
        path = make_image(self.dir, "other.bin", 5000)
        status, out, err = self.check(["r0=" + path])
        self.assertEqual((status, err), (0, ""))
        self.assertTrue(out.startswith("r0: 5000 bytes"))
        self.assertTrue(out.rstrip().endswith("(%s)" % path))

    def test_a_missing_image_is_an_error_even_without_enforce(self):
        make_image(self.dir, "k0.uImage", 1000)
        status, out, err = self.check([])
        self.assertEqual(status, 2)
        self.assertIn("cannot read the image of r0", err)
        self.assertEqual(out, "")

    def test_an_empty_image_is_an_error(self):
        make_image(self.dir, "r0.squashfs", 0)
        status, out, err = self.check(["r0"], enforce=True)
        self.assertEqual(status, 2)
        self.assertIn("is empty", err)

    def test_a_directory_is_not_an_image(self):
        os.mkdir(os.path.join(self.dir, "r0.squashfs"))
        status, out, err = self.check(["r0"])
        self.assertEqual(status, 2)
        self.assertIn("is not a file", err)

    def test_an_unknown_name_is_an_error(self):
        status, out, err = self.check(["k1"])
        self.assertEqual(status, 2)
        self.assertIn("k1 is not an image to check", err)

    def test_a_name_given_twice_is_an_error(self):
        make_image(self.dir, "r0.squashfs", 10)
        status, out, err = self.check(["r0", "r0"])
        self.assertEqual(status, 2)
        self.assertIn("r0 is named twice", err)


class AsAProgram(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = self._tmp.name

    def test_exit_status_zero_and_the_lines_on_standard_output(self):
        make_image(self.dir, "k0.uImage", K0 + 10)
        make_image(self.dir, "r0.squashfs", 100)
        status, out, err = run(["--images", self.dir])
        self.assertEqual(status, 0, err)
        self.assertIn("over the budget by 10 bytes", out)

    def test_exit_status_one_with_enforce(self):
        make_image(self.dir, "k0.uImage", K0 + 10)
        make_image(self.dir, "r0.squashfs", 100)
        status, out, err = run(["--images", self.dir, "--enforce"])
        self.assertEqual(status, 1)
        self.assertIn("FAIL", err)

    def test_exit_status_zero_with_enforce_on_the_image_that_fits(self):
        make_image(self.dir, "k0.uImage", K0 + 10)
        make_image(self.dir, "r0.squashfs", 100)
        self.assertEqual(run(["--images", self.dir, "--enforce", "r0"])[0], 0)

    def test_exit_status_two_for_a_missing_directory(self):
        status, out, err = run(["--images", os.path.join(self.dir, "none")])
        self.assertEqual(status, 2)
        self.assertIn("cannot read", err)

    def test_exit_status_two_for_a_bad_option(self):
        self.assertEqual(run(["--no-such-option"])[0], 2)

    def test_help_exits_with_zero(self):
        status, out, err = run(["--help"])
        self.assertEqual(status, 0)
        self.assertIn("--enforce", out)

    def test_the_default_directory_is_the_one_the_build_leaves_the_images_in(self):
        self.assertTrue(budget.DEFAULT_DIR.endswith(os.path.join("build", "image", "images")))


if __name__ == "__main__":
    with contextlib.suppress(SystemExit):
        unittest.main()
