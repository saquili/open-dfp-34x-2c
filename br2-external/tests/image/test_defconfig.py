# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of configs/odi_dfp_34x_2c3_defconfig: what it must say, and that what it names exists."""

import os
import re
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
EXTERNAL = os.path.dirname(os.path.dirname(HERE))
TOP = os.path.dirname(EXTERNAL)
BOARD_DEFCONFIG = os.path.join(EXTERNAL, "configs", "odi_dfp_34x_2c3_defconfig")
TOOLCHAIN_DEFCONFIG = os.path.join(EXTERNAL, "configs", "toolchain_defconfig")


def settings(path):
    """The lines of a defconfig as a dictionary; a line that is not set maps to None."""
    result = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            match = re.match(r"^(BR2_[A-Za-z0-9_]+)=(.*)$", line)
            if match:
                result[match.group(1)] = match.group(2)
                continue
            match = re.match(r"^# (BR2_[A-Za-z0-9_]+) is not set$", line)
            if match:
                result[match.group(1)] = None
    return result


class Toolchain(unittest.TestCase):
    """The board's toolchain is the toolchain defconfig's, with static libraries only."""

    SECTION = ("BR2_mips", "BR2_mips_1", "BR2_BINUTILS_VERSION_2_44_X", "BR2_GCC_VERSION_14_X",
               "BR2_TOOLCHAIN_BUILDROOT_UCLIBC", "BR2_PTHREADS_NONE",
               "BR2_TOOLCHAIN_BUILDROOT_USE_SSP", "BR2_UCLIBC_CONFIG_FRAGMENT_FILES",
               "BR2_KERNEL_HEADERS_CUSTOM_TARBALL", "BR2_KERNEL_HEADERS_CUSTOM_TARBALL_LOCATION",
               "BR2_PACKAGE_HOST_LINUX_HEADERS_CUSTOM_6_12", "BR2_REPRODUCIBLE",
               "BR2_BINUTILS_EXTRA_CONFIG_OPTIONS")

    def test_the_toolchain_section_is_the_same(self):
        board, toolchain = settings(BOARD_DEFCONFIG), settings(TOOLCHAIN_DEFCONFIG)
        for key in self.SECTION:
            self.assertIn(key, toolchain, key)
            self.assertEqual(board.get(key, "missing"), toolchain[key], key)

    def test_every_executable_is_static(self):
        board = settings(BOARD_DEFCONFIG)
        self.assertEqual(board["BR2_STATIC_LIBS"], "y")
        self.assertNotIn("BR2_SHARED_LIBS", board)
        self.assertNotIn("BR2_SHARED_STATIC_LIBS", board)

    def test_the_build_is_reproducible(self):
        self.assertEqual(settings(BOARD_DEFCONFIG)["BR2_REPRODUCIBLE"], "y")

    def test_the_header_says_why_the_image_is_static(self):
        with open(BOARD_DEFCONFIG, encoding="utf-8") as handle:
            head = handle.read().split("\nBR2_", 1)[0]
        self.assertIn("static", head)
        self.assertIn("docs/adr/0010", head)


class Kernel(unittest.TestCase):
    def test_it_is_the_kernel_of_this_repository_with_the_boards_configuration(self):
        board = settings(BOARD_DEFCONFIG)
        self.assertEqual(board["BR2_LINUX_KERNEL"], "y")
        self.assertEqual(board["BR2_LINUX_KERNEL_CUSTOM_TARBALL"], "y")
        self.assertEqual(board["BR2_LINUX_KERNEL_CUSTOM_TARBALL_LOCATION"],
                         board["BR2_KERNEL_HEADERS_CUSTOM_TARBALL_LOCATION"])
        self.assertEqual(board["BR2_LINUX_KERNEL_DEFCONFIG"], '"rtl9602c"')
        self.assertTrue(os.path.exists(os.path.join(TOP, "linux", "arch", "mips", "configs",
                                                    "rtl9602c_defconfig")))

    def test_the_image_is_the_lzma_compressed_uimage_the_boot_loader_takes(self):
        board = settings(BOARD_DEFCONFIG)
        self.assertEqual(board["BR2_LINUX_KERNEL_UIMAGE"], "y")
        self.assertEqual(board["BR2_LINUX_KERNEL_LZMA"], "y")


class Root(unittest.TestCase):
    def test_the_files_it_names_exist(self):
        with open(BOARD_DEFCONFIG, encoding="utf-8") as handle:
            text = handle.read()
        paths = re.findall(r'\$\(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH\)(/[^" ]+)', text)
        self.assertGreaterEqual(len(paths), 4)
        for path in paths:
            self.assertTrue(os.path.exists(EXTERNAL + path), path)

    def test_it_makes_no_root_filesystem_image_of_its_own(self):
        board = settings(BOARD_DEFCONFIG)
        for key in board:
            if key.startswith("BR2_TARGET_ROOTFS_"):
                self.assertIsNone(board[key], key)

    def test_busybox_and_dropbear_are_the_only_packages(self):
        board = settings(BOARD_DEFCONFIG)
        packages = sorted(k for k, v in board.items()
                          if k.startswith("BR2_PACKAGE_") and v == "y")
        self.assertEqual(packages, ["BR2_PACKAGE_DROPBEAR", "BR2_PACKAGE_DROPBEAR_DISABLE_REVERSEDNS",
                                    "BR2_PACKAGE_HOST_LINUX_HEADERS_CUSTOM_6_12",
                                    "BR2_PACKAGE_HOST_SQUASHFS"])

    def test_the_post_image_script_and_the_overlay_are_set(self):
        board = settings(BOARD_DEFCONFIG)
        self.assertTrue(board["BR2_ROOTFS_POST_IMAGE_SCRIPT"].endswith('/post-image.sh"'))
        self.assertIn("/board/dfp-34x-2c3/overlay", board["BR2_ROOTFS_OVERLAY"])

    def test_a_bench_can_lay_a_second_overlay_over_the_first(self):
        overlay = settings(BOARD_DEFCONFIG)["BR2_ROOTFS_OVERLAY"].strip('"').split()
        self.assertEqual(len(overlay), 2)
        self.assertTrue(overlay[0].endswith("/board/dfp-34x-2c3/overlay"))
        self.assertEqual(overlay[1], "$(OPEN_DFP_34X_2C_LOCAL_OVERLAY)")

    def test_no_init_system_of_the_build_system_is_installed(self):
        self.assertEqual(settings(BOARD_DEFCONFIG)["BR2_INIT_NONE"], "y")


if __name__ == "__main__":
    unittest.main()
