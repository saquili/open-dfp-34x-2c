# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests that the build holds both images to their partitions.

The check is only worth anything if the build calls it the way these tests
expect. The top-level Makefile is read for the default of IMAGE_ENFORCE and for
the line that runs the tool, and that line is then run as a program on images of
the sizes that matter: either image over its partition fails the build, and
both within it pass.
"""

import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "image-budget")
TOP = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
MAKEFILE = os.path.join(TOP, "Makefile")

K0 = 1359872
R0 = 2572288


def makefile():
    with open(MAKEFILE, encoding="utf-8") as handle:
        return handle.read()


def recipe(text, target):
    """The lines of a target's recipe, without their leading tab."""
    match = re.search(r"^%s:.*\n((?:\t.*\n)+)" % re.escape(target), text, re.M)
    if match is None:
        raise AssertionError("no target %s in the Makefile" % target)
    return [line[1:] for line in match.group(1).splitlines()]


def image(directory, name, size):
    with open(os.path.join(directory, name), "wb") as handle:
        handle.truncate(size)


class TheBuild(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = self._tmp.name

    def enforced(self):
        match = re.search(r"^IMAGE_ENFORCE \?= (.*)$", makefile(), re.M)
        self.assertIsNotNone(match, "IMAGE_ENFORCE has no default in the Makefile")
        return match.group(1).split()

    def run_as_the_build_does(self, k0, r0):
        """The line of the Makefile that enforces, with its variables filled in, run as a program."""
        image(self.dir, "k0.uImage", k0)
        image(self.dir, "r0.squashfs", r0)
        lines = [line for line in recipe(makefile(), "image-check") if "--enforce" in line]
        self.assertEqual(len(lines), 1)
        words = lines[0].replace('"$(IMAGE_OUT)/images"', self.dir).replace("$(IMAGE_ENFORCE)", " ".join(self.enforced())).split()
        self.assertEqual(words[0], "tools/image-budget/image-budget")
        done = subprocess.run([sys.executable, TOOL, *words[1:]], capture_output=True, text=True)
        return done.returncode, done.stdout, done.stderr

    def test_both_images_are_enforced_by_default(self):
        self.assertEqual(sorted(self.enforced()), ["k0", "r0"])

    def test_both_within_their_partitions_pass(self):
        self.assertEqual(self.run_as_the_build_does(K0, R0)[0], 0)

    def test_a_kernel_image_over_its_partition_fails_the_build(self):
        status, _out, err = self.run_as_the_build_does(K0 + 1, 1000)
        self.assertEqual(status, 1)
        self.assertIn("FAIL -- over the budget: k0", err)

    def test_a_root_filesystem_image_over_its_partition_fails_the_build(self):
        status, _out, err = self.run_as_the_build_does(1000, R0 + 1)
        self.assertEqual(status, 1)
        self.assertIn("FAIL -- over the budget: r0", err)

    def test_both_over_are_named_in_one_failure(self):
        status, _out, err = self.run_as_the_build_does(K0 + 1, R0 + 1)
        self.assertEqual(status, 1)
        self.assertIn("FAIL -- over the budget: k0, r0", err)

    def test_the_images_that_are_not_enforced_are_still_reported(self):
        lines = recipe(makefile(), "image-check")
        self.assertTrue([line for line in lines if "$(filter-out $(IMAGE_ENFORCE),k0 r0)" in line])

    def test_the_check_comes_before_the_digests_and_runs_in_the_build(self):
        text = makefile()
        image_target = recipe(text, "image")
        self.assertIn("$(MAKE) image-check", image_target)
        lines = recipe(text, "image-check")
        enforce = next(i for i, line in enumerate(lines) if "--enforce" in line)
        digests = next(i for i, line in enumerate(lines) if "sha256sum -c" in line)
        self.assertLess(enforce, digests)


if __name__ == "__main__":
    unittest.main()
