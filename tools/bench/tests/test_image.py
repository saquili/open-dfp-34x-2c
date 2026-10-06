# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The image checks: what is refused before the board is touched."""

from __future__ import annotations

import os
import unittest
import zlib

from bench import image
from bench.image import ImageError

from .images import CREATED, make_squashfs, make_uimage
from .support import TempDirTestCase


class KernelImage(unittest.TestCase):

    def test_a_good_image_is_described(self) -> None:
        data = make_uimage(1000, name="Linux-6.18.48")
        info = image.check_uimage(data, image.K0_SIZE)
        self.assertEqual(info.name, "Linux-6.18.48")
        self.assertEqual(info.created, CREATED)
        self.assertEqual(info.data_size, 1000)
        self.assertEqual(info.load_address, 0x80100000)
        self.assertEqual(info.length, 1064)
        self.assertEqual(info.crc32, zlib.crc32(data))

    def test_uimage_checks(self) -> None:
        """One crafted header for each rejected field."""
        cases = {
            "magic": (make_uimage(magic=0x12345678), "magic"),
            "header crc": (make_uimage(bad_header_crc=True), "header CRC"),
            "architecture": (make_uimage(arch=3), "architecture 3"),
            "compression": (make_uimage(compression=2), "compression 2"),
            "load address": (make_uimage(load=image.LOAD_ADDRESS + 0x100000), "loads at 80200000"),
            "data crc": (make_uimage(bad_data_crc=True), "data CRC"),
            "truncated": (make_uimage(1000)[:-1], "truncated"),
            "short file": (b"\x27\x05\x19\x56" + bytes(10), "shorter"),
            "empty": (b"", "shorter"),
        }
        for label, (data, words) in cases.items():
            with self.subTest(label), self.assertRaises(ImageError) as caught:
                image.check_uimage(data, image.K0_SIZE)
            self.assertIn(words, str(caught.exception), label)

    def test_a_corrupted_header_byte_is_a_header_crc_error(self) -> None:
        data = bytearray(make_uimage(1000))
        data[20] ^= 0x01  # the load address, under the header CRC
        with self.assertRaises(ImageError) as caught:
            image.check_uimage(bytes(data), image.K0_SIZE)
        self.assertIn("header CRC", str(caught.exception))

    def test_a_corrupted_data_byte_is_a_data_crc_error(self) -> None:
        data = bytearray(make_uimage(1000))
        data[500] ^= 0x80
        with self.assertRaises(ImageError) as caught:
            image.check_uimage(bytes(data), image.K0_SIZE)
        self.assertIn("data CRC", str(caught.exception))

    def test_the_size_limit_is_the_destinations(self) -> None:
        exact = make_uimage(image.K0_SIZE - image.HEADER_SIZE)
        self.assertEqual(len(exact), image.K0_SIZE)
        image.check_uimage(exact, image.K0_SIZE)
        with self.assertRaises(ImageError) as caught:
            image.check_uimage(make_uimage(image.K0_SIZE - image.HEADER_SIZE + 1), image.K0_SIZE)
        self.assertIn("1900545", str(caught.exception))
        image.check_uimage(make_uimage(100), image.RAM_BOOT_LIMIT)
        self.assertEqual(image.RAM_BOOT_LIMIT, 12 * 1024 * 1024)

    def test_trailing_bytes_after_the_data_are_not_refused(self) -> None:
        data = make_uimage(1000) + b"\xff" * 16
        self.assertEqual(image.check_uimage(data, image.K0_SIZE).length, len(data))

    def test_the_name_ends_at_its_first_nul(self) -> None:
        self.assertEqual(image.check_uimage(make_uimage(name=""), 10**6).name, "")
        self.assertEqual(image.check_uimage(make_uimage(name="x" * 40), 10**6).name, "x" * 32)


class RootFs(unittest.TestCase):

    def test_squashfs_checks(self) -> None:
        info = image.check_squashfs(make_squashfs(4096))
        self.assertEqual((info.length, info.compression), (4096, 4))
        cases = {
            "no magic": (b"xxxx" + bytes(100), "hsqs"),
            "empty": (b"", "hsqs"),
            "gzip": (make_squashfs(4096, compression=1), "compression 1"),
            "lz4": (make_squashfs(4096, compression=5), "compression 5"),
            "lzo": (make_squashfs(4096, compression=3), "compression 3"),
        }
        for label, (data, words) in cases.items():
            with self.subTest(label), self.assertRaises(ImageError) as caught:
                image.check_squashfs(data)
            self.assertIn(words, str(caught.exception))

    def test_the_accepted_compressions_come_from_the_caller(self) -> None:
        data = make_squashfs(4096, compression=1)
        self.assertEqual(image.check_squashfs(data, compressions=(1,)).compression, 1)
        with self.assertRaises(ImageError):
            image.check_squashfs(make_squashfs(4096), compressions=(1,))

    def test_the_size_limit_is_the_partitions(self) -> None:
        image.check_squashfs(make_squashfs(image.R0_SIZE))
        with self.assertRaises(ImageError) as caught:
            image.check_squashfs(make_squashfs(image.R0_SIZE + 1))
        self.assertIn("2031617", str(caught.exception))


class Files(TempDirTestCase):

    def test_a_missing_file_is_an_image_error(self) -> None:
        with self.assertRaises(ImageError) as caught:
            image.read_file(os.path.join(self.tmp, "nothing"))
        self.assertIn("nothing", str(caught.exception))

    def test_a_file_is_read_whole(self) -> None:
        path = os.path.join(self.tmp, "x")
        with open(path, "wb") as handle:
            handle.write(b"abc")
        self.assertEqual(image.read_file(path), b"abc")


REPORT = """\
## Booting kernel from Legacy Image at 81000000 ...
   Image Name:   Linux-6.18.48
   Created:      2026-09-30   1:04:42 UTC
   Image Type:   MIPS Linux Kernel Image (lzma compressed)
   Data Size:    1000 Bytes =  1.0 KiB
   Load Address: 80100000
   Entry Point:  80100000
   Verifying Checksum ... OK
   Uncompressing Kernel Image ... OK

Starting kernel ...
"""


class BootReport(unittest.TestCase):

    def setUp(self) -> None:
        self.image = image.check_uimage(make_uimage(1000, name="Linux-6.18.48"), 10**7)

    def test_the_report_is_read_field_by_field(self) -> None:
        report = image.parse_boot_report(REPORT)
        self.assertEqual(report.address, 0x81000000)
        self.assertEqual(report.name, "Linux-6.18.48")
        self.assertEqual(report.created, CREATED)
        self.assertEqual(report.data_size, 1000)
        self.assertEqual(report.load_address, 0x80100000)
        self.assertEqual(report.entry_point, 0x80100000)

    def test_a_matching_report_has_no_problems(self) -> None:
        report = image.parse_boot_report(REPORT)
        self.assertEqual(image.compare_boot_report(report, self.image, 0x81000000), [])

    def test_wrong_kernel_is_failure(self) -> None:
        """A report for another image, or another address, is listed field by field."""
        cases = {
            "name": (REPORT.replace("Linux-6.18.48", "Linux-3.18.24"), "name"),
            "time": (REPORT.replace("2026-09-30", "2022-01-02"), "creation time"),
            "size": (REPORT.replace("1000 Bytes", "999 Bytes"), "data size"),
            "load": (REPORT.replace("Load Address: 80100000", "Load Address: 80200000"),
                     "load address"),
            "entry": (REPORT.replace("Entry Point:  80100000", "Entry Point:  80100400"),
                      "entry point"),
            "address": (REPORT.replace("at 81000000", "at 94080000"), "94080000"),
        }
        for label, (text, words) in cases.items():
            with self.subTest(label):
                problems = image.compare_boot_report(image.parse_boot_report(text), self.image,
                                                     0x81000000)
                self.assertEqual(len(problems), 1, problems)
                self.assertIn(words, problems[0])

    def test_missing_fields_are_problems(self) -> None:
        problems = image.compare_boot_report(image.parse_boot_report("nothing here"),
                                             self.image, 0x81000000)
        self.assertEqual(len(problems), 6)

    def test_without_an_image_file_only_the_address_is_compared(self) -> None:
        report = image.parse_boot_report(REPORT.replace("at 81000000", "at 94080000"))
        self.assertEqual(image.compare_boot_report(report, None, 0x94080000), [])
        self.assertEqual(len(image.compare_boot_report(report, None, 0x81000000)), 1)

    def test_the_time_may_have_a_one_digit_hour_and_runs_of_spaces(self) -> None:
        for stamp in ("2026-09-30   1:04:42 UTC", "2026-09-30 01:04:42 UTC",
                      "2026-09-30  01:04:42 UTC"):
            text = REPORT.replace("2026-09-30   1:04:42 UTC", stamp)
            self.assertEqual(image.parse_boot_report(text).created, CREATED, stamp)


if __name__ == "__main__":
    unittest.main()
