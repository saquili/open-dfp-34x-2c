# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of flash-layout, on device trees and kernel configurations made for each test.

The board's own device tree and defconfig are checked too: they are what the
tool is for.
"""

import importlib.machinery
import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "flash-layout")
TOP = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DEFCONFIG = os.path.join(TOP, "linux", "arch", "mips", "configs", "rtl9602c_defconfig")

_loader = importlib.machinery.SourceFileLoader("flash_layout", TOOL)
_spec = importlib.util.spec_from_loader("flash_layout", _loader)
fl = importlib.util.module_from_spec(_spec)
_loader.exec_module(fl)

# A layout in the style of the board's: partitions on sectors, one read-only, a comment with
# braces in it, and the nodes in another order than the flash's.
GOOD = """\
/dts-v1/;
/ {
	flash@0 {
		compatible = "jedec,spi-nor";
		/* a comment { with braces } that the parser must not count */
		partitions {
			compatible = "fixed-partitions";
			#address-cells = <1>;
			#size-cells = <1>;

			partition@40000 {
				label = "env";
				reg = <0x040000 0x002000>;
			};
			partition@0 {
				label = "boot";
				reg = <0x000000 0x040000>;
				read-only;
			};
			// a line comment { too
			partition@42000 {
				label = "config";
				reg = <0x042000 0x03e000>;
			};
		};
	};
};
"""

GOOD_CONFIG = """\
CONFIG_MTD=y
CONFIG_MTD_SPI_NOR=y
CONFIG_MTD_SPI_NOR_USE_4K_SECTORS=y
"""


def run_tool(*args):
    done = subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)
    return done.returncode, done.stdout, done.stderr


class Files:
    """Temporary files named by their contents."""

    def __init__(self):
        self.dir = tempfile.TemporaryDirectory()

    def make(self, name, text):
        path = os.path.join(self.dir.name, name)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)
        return path

    def close(self):
        self.dir.cleanup()


class Parsing(unittest.TestCase):
    def test_partitions_in_the_order_of_the_file_with_their_fields(self):
        parts = fl.parse_partitions(GOOD)
        self.assertEqual([p["label"] for p in parts], ["env", "boot", "config"])
        self.assertEqual([(p["offset"], p["size"]) for p in parts],
                         [(0x40000, 0x2000), (0, 0x40000), (0x42000, 0x3e000)])
        self.assertEqual([p["read_only"] for p in parts], [False, True, False])

    def test_a_good_layout_has_no_finding(self):
        self.assertEqual(fl.check_partitions(fl.parse_partitions(GOOD), 4096), [])

    def test_no_node_of_partitions_is_a_problem(self):
        with self.assertRaises(fl.Problem):
            fl.parse_partitions("/ { flash { compatible = \"jedec,spi-nor\"; }; };")

    def test_a_node_without_partitions_is_a_problem(self):
        with self.assertRaises(fl.Problem):
            fl.parse_partitions("/ { p { compatible = \"fixed-partitions\"; }; };")

    def test_decimal_and_hexadecimal_numbers(self):
        parts = fl.parse_partitions(
            '/ { p { compatible = "fixed-partitions"; partition@1000 { label = "a"; '
            "reg = <4096 0x1000>; }; }; };")
        self.assertEqual((parts[0]["offset"], parts[0]["size"]), (4096, 4096))


class Findings(unittest.TestCase):
    def found(self, text, sector=4096):
        return fl.check_partitions(fl.parse_partitions(text), sector)

    def partition(self, unit, label, offset, size):
        label_line = 'label = "%s";' % label if label else ""
        return ('/ { p { compatible = "fixed-partitions"; partition@%s { %s '
                "reg = <%s %s>; }; }; };" % (unit, label_line, offset, size))

    def test_an_offset_off_the_sector(self):
        found = self.found(self.partition("1800", "a", "0x1800", "0x1000"))
        self.assertEqual(len(found), 1)
        self.assertIn("starts at 0x1800", found[0])
        self.assertIn("not a multiple of 4096", found[0])

    def test_a_size_off_the_sector(self):
        found = self.found(self.partition("1000", "a", "0x1000", "0x1800"))
        self.assertEqual(len(found), 1)
        self.assertIn("is 0x1800 long", found[0])

    def test_an_empty_partition(self):
        self.assertIn("a is empty", self.found(self.partition("1000", "a", "0x1000", "0")))

    def test_a_unit_address_that_is_not_the_offset(self):
        found = self.found(self.partition("2000", "a", "0x1000", "0x1000"))
        self.assertEqual(len(found), 1)
        self.assertIn("unit address 0x2000 but starts at 0x1000", found[0])

    def test_a_missing_label(self):
        found = self.found(self.partition("1000", None, "0x1000", "0x1000"))
        self.assertEqual(len(found), 1)
        self.assertIn("has no label", found[0])

    def test_a_label_twice(self):
        text = GOOD.replace('label = "config"', 'label = "env"')
        self.assertIn("the label env is used twice", self.found(text))

    def test_an_overlap_found_by_offset_not_by_the_order_of_the_file(self):
        text = GOOD.replace("reg = <0x042000 0x03e000>", "reg = <0x041000 0x03f000>") \
                   .replace("partition@42000", "partition@41000")
        found = self.found(text)
        self.assertEqual(len(found), 1)
        self.assertIn("config overlaps env", found[0])

    def test_larger_blocks_fail_the_layout_that_the_board_has(self):
        # What the kernel does without 4 KiB sectors: the 8 KiB environment is no whole block.
        found = self.found(GOOD, 65536)
        self.assertTrue(any("env starts at 0x40000" in line or "env is 0x2000 long" in line
                            for line in found), found)

    def test_a_larger_sector_the_layout_fits_is_not_a_finding(self):
        self.assertEqual(self.found(GOOD, 8192), [])


class KernelConfig(unittest.TestCase):
    def found(self, text):
        return fl.check_kernel_config(fl.parse_kernel_config(text))

    def test_a_kernel_that_erases_in_sectors(self):
        self.assertEqual(self.found(GOOD_CONFIG), [])

    def test_the_option_unset(self):
        text = GOOD_CONFIG.replace("CONFIG_MTD_SPI_NOR_USE_4K_SECTORS=y",
                                   "# CONFIG_MTD_SPI_NOR_USE_4K_SECTORS is not set")
        found = self.found(text)
        self.assertEqual(len(found), 1)
        self.assertIn("MTD_SPI_NOR_USE_4K_SECTORS", found[0])

    def test_the_option_missing(self):
        found = self.found("CONFIG_MTD=y\nCONFIG_MTD_SPI_NOR=y\n")
        self.assertEqual(len(found), 1)

    def test_a_flash_driver_that_is_a_module_or_missing(self):
        for text in (GOOD_CONFIG.replace("MTD_SPI_NOR=y", "MTD_SPI_NOR=m"), "CONFIG_MTD=y\n"):
            self.assertTrue(any("MTD_SPI_NOR is not set to y" in line
                                for line in self.found(text)), text)


class Command(unittest.TestCase):
    def setUp(self):
        self.files = Files()
        self.addCleanup(self.files.close)

    def test_a_good_layout_and_kernel_pass(self):
        dts = self.files.make("a.dts", GOOD)
        cfg = self.files.make("a.config", GOOD_CONFIG)
        code, out, err = run_tool("--dts", dts, "--kernel-config", cfg)
        self.assertEqual((code, err), (0, ""))
        self.assertIn("3 partitions on 4096-byte sectors, and the kernel uses 4 KiB sectors", out)

    def test_a_finding_is_status_1_and_named_on_stderr(self):
        dts = self.files.make("a.dts", GOOD.replace("0x042000 0x03e000", "0x042000 0x03e800"))
        code, _, err = run_tool("--dts", dts)
        self.assertEqual(code, 1)
        self.assertIn("config is 0x3e800 long", err)

    def test_a_kernel_without_the_option_is_status_1(self):
        dts = self.files.make("a.dts", GOOD)
        cfg = self.files.make("a.config", "CONFIG_MTD_SPI_NOR=y\n")
        code, _, err = run_tool("--dts", dts, "--kernel-config", cfg)
        self.assertEqual(code, 1)
        self.assertIn("MTD_SPI_NOR_USE_4K_SECTORS", err)

    def test_a_missing_file_and_a_bad_sector_are_status_2(self):
        code, _, err = run_tool("--dts", os.path.join(self.files.dir.name, "none.dts"))
        self.assertEqual(code, 2)
        self.assertIn("cannot read the device tree", err)
        dts = self.files.make("a.dts", GOOD)
        self.assertEqual(run_tool("--dts", dts, "--sector", "0")[0], 2)
        self.assertEqual(run_tool("--dts", dts, "--sector", "x")[0], 2)

    def test_run_prints_the_table(self):
        dts = self.files.make("a.dts", GOOD)
        out, err = io.StringIO(), io.StringIO()
        self.assertEqual(fl.run(dts, None, 4096, out, err), 0)
        self.assertIn("boot     0x000000 0x040000  read-only", out.getvalue())


class TheBoard(unittest.TestCase):
    """The board's own files, which are what the tool is for."""

    def test_the_device_tree_is_on_sectors(self):
        code, out, err = run_tool()
        self.assertEqual((code, err), (0, ""), out)
        parts = fl.parse_partitions(fl.read(fl.DEFAULT_DTS, "the device tree"))
        self.assertEqual([p["label"] for p in parts],
                         ["boot", "env", "env2", "config", "k0", "r0", "k1", "r1"])

    def test_the_partitions_follow_each_other_to_the_end_of_the_chip(self):
        parts = sorted(fl.parse_partitions(fl.read(fl.DEFAULT_DTS, "the device tree")),
                       key=lambda p: p["offset"])
        end = 0
        for part in parts:
            self.assertEqual(part["offset"], end, "%s leaves a gap or overlaps" % part["label"])
            end = part["offset"] + part["size"]
        self.assertEqual(end, 8 * 1024 * 1024, "the chip is 64 Mbit")

    def test_the_defconfig_says_that_it_wants_the_sectors(self):
        with open(DEFCONFIG, encoding="utf-8") as handle:
            options = fl.parse_kernel_config(handle.read())
        self.assertEqual(options.get("CONFIG_MTD_SPI_NOR"), "y")
        self.assertEqual(options.get("CONFIG_MTD_SPI_NOR_USE_4K_SECTORS"), "y",
                         "the defconfig leaves the option to its default")

    def test_the_bench_and_the_device_tree_agree_on_the_partitions_they_both_describe(self):
        tools = os.path.join(TOP, "tools")
        sys.path.insert(0, tools)
        try:
            from bench.guard import PARTITIONS  # pylint: disable=import-outside-toplevel
        except ImportError as exc:
            self.skipTest("the bench needs a newer Python: %s" % exc)
        finally:
            sys.path.remove(tools)
        dts = {p["label"]: (p["offset"], p["size"])
               for p in fl.parse_partitions(fl.read(fl.DEFAULT_DTS, "the device tree"))}
        for part in PARTITIONS:
            # Bank 1 of the bench is the shipped firmware's split, which the bench never writes.
            if part.name in ("k1", "r1"):
                continue
            self.assertEqual((part.start, part.size), dts[part.name], part.name)

    def test_the_budget_of_the_images_is_the_size_of_their_partitions(self):
        sys.path.insert(0, os.path.join(TOP, "tools", "image-budget"))
        try:
            loader = importlib.machinery.SourceFileLoader(
                "image_budget", os.path.join(TOP, "tools", "image-budget", "image-budget"))
            spec = importlib.util.spec_from_loader("image_budget", loader)
            budget = importlib.util.module_from_spec(spec)
            loader.exec_module(budget)
        finally:
            sys.path.pop(0)
        dts = {p["label"]: p["size"]
               for p in fl.parse_partitions(fl.read(fl.DEFAULT_DTS, "the device tree"))}
        self.assertEqual({name: size for name, (_, size) in budget.BUDGETS.items()},
                         {"k0": dts["k0"], "r0": dts["r0"]})


if __name__ == "__main__":
    unittest.main()
