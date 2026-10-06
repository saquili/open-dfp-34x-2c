# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The images the loader boots: the legacy kernel image and the root filesystem.

The tool reads an image before it sends anything: a bad file is found before
the board is touched. A kernel is a legacy U-Boot image: a 64-byte
big-endian header (magic, header CRC, creation time, data size, load address,
entry point, data CRC, then one byte each for operating system, architecture,
type and compression, and a 32-byte name) followed by the data. The root
filesystem is a SquashFS image. What the loader prints when it starts a
kernel identifies the image that is about to run, so the tool also compares
that report with the file's own header.
"""

from __future__ import annotations

import calendar
import re
import struct
import zlib
from dataclasses import dataclass

from .console import BenchError

UIMAGE_MAGIC = 0x27051956
HEADER_SIZE = 64
OS_LINUX = 5
ARCH_MIPS = 5
TYPE_KERNEL = 2
COMPRESSION_LZMA = 3
LOAD_ADDRESS = 0x80100000
_HEADER = struct.Struct(">IIIIIIIBBBB32s")

K0_SIZE = 1900544
R0_SIZE = 2031616
RAM_BOOT_LIMIT = 12 << 20

SQUASHFS_MAGIC = b"hsqs"
SQUASHFS_GZIP = 1
SQUASHFS_XZ = 4
SQUASHFS_LZ4 = 5


class ImageError(BenchError):
    """An image the tool refuses to send."""


@dataclass(frozen=True)
class UImage:
    """A checked kernel image and what the loader will print about it."""

    name: str
    created: int  # seconds since the epoch, UTC
    data_size: int
    load_address: int
    entry_point: int
    length: int  # bytes in the file
    crc32: int  # CRC-32 of the whole file, as the loader computes it over RAM


def check_uimage(data: bytes, limit: int, *, what: str = "the image") -> UImage:
    """Check a kernel image the way the loader will, and describe it.

    The magic, the architecture (MIPS), the compression (LZMA), the load
    address, both CRCs and the size are checked; each rejected field is named.
    ``limit`` is the largest file the destination takes.
    """
    if len(data) < HEADER_SIZE:
        raise ImageError("%s is shorter than a 64-byte image header" % what)
    (magic, header_crc, created, size, load, entry, data_crc,
     os_id, arch, image_type, compression, raw_name) = _HEADER.unpack_from(data)
    if magic != UIMAGE_MAGIC:
        raise ImageError("%s has no image magic (found %08x)" % (what, magic))
    zeroed = data[:4] + b"\0\0\0\0" + data[8:HEADER_SIZE]
    if zlib.crc32(zeroed) != header_crc:
        raise ImageError("%s has a bad header CRC" % what)
    if arch != ARCH_MIPS:
        raise ImageError("%s is for architecture %d, not MIPS (5)" % (what, arch))
    if compression != COMPRESSION_LZMA:
        raise ImageError("%s uses compression %d, not LZMA (3)" % (what, compression))
    if load != LOAD_ADDRESS:
        raise ImageError("%s loads at %08x, not %08x" % (what, load, LOAD_ADDRESS))
    if len(data) < HEADER_SIZE + size:
        raise ImageError("%s is truncated: the header says %d bytes of data, the file has %d"
                         % (what, size, len(data) - HEADER_SIZE))
    if zlib.crc32(data[HEADER_SIZE:HEADER_SIZE + size]) != data_crc:
        raise ImageError("%s has a bad data CRC" % what)
    if len(data) > limit:
        raise ImageError("%s is %d bytes, more than the %d its destination takes"
                         % (what, len(data), limit))
    name = raw_name.split(b"\0", 1)[0].decode("latin-1")
    return UImage(name=name, created=created, data_size=size, load_address=load,
                  entry_point=entry, length=len(data), crc32=zlib.crc32(data))


@dataclass(frozen=True)
class RootFs:
    """A checked root filesystem image."""

    length: int
    compression: int
    crc32: int


def check_squashfs(data: bytes, compressions: tuple[int, ...] = (SQUASHFS_XZ,),
                   limit: int = R0_SIZE, *, what: str = "the image") -> RootFs:
    """Check a SquashFS image: magic, a compression the kernel supports, and size."""
    if len(data) < 24 or data[:4] != SQUASHFS_MAGIC:
        raise ImageError("%s is not a SquashFS image (no hsqs magic)" % what)
    compression = struct.unpack_from("<H", data, 20)[0]
    if compression not in compressions:
        raise ImageError("%s uses SquashFS compression %d; the kernel takes %s"
                         % (what, compression, ", ".join(str(c) for c in compressions)))
    if len(data) > limit:
        raise ImageError("%s is %d bytes, more than the %d its partition takes"
                         % (what, len(data), limit))
    return RootFs(length=len(data), compression=compression, crc32=zlib.crc32(data))


def read_file(path: str) -> bytes:
    """The bytes of an image file, or an ``ImageError`` that says why not."""
    try:
        with open(path, "rb") as handle:
            return handle.read()
    except OSError as exc:
        raise ImageError("cannot read %s: %s" % (path, exc.strerror)) from exc


# --------------------------------------------------------------------------
# What the loader prints when it starts an image
# --------------------------------------------------------------------------

_FIELDS = {
    "address": re.compile(r"Booting kernel from Legacy Image at ([0-9A-Fa-f]+)"),
    "Image Name": re.compile(r"Image Name:[ \t]*(.*)"),
    "Created": re.compile(r"Created:[ \t]*(\d{4})-(\d\d)-(\d\d)[ \t]+(\d{1,2}):(\d\d):(\d\d) UTC"),
    "Data Size": re.compile(r"Data Size:[ \t]*(\d+) Bytes"),
    "Load Address": re.compile(r"Load Address:[ \t]*([0-9A-Fa-f]+)"),
    "Entry Point": re.compile(r"Entry Point:[ \t]*([0-9A-Fa-f]+)"),
}


@dataclass(frozen=True)
class BootReport:
    """The fields of the loader's report, as found; missing ones are ``None``."""

    address: int | None
    name: str | None
    created: int | None
    data_size: int | None
    load_address: int | None
    entry_point: int | None


def parse_boot_report(text: str) -> BootReport:
    """Read the lines the loader prints before it starts a kernel."""
    found: dict[str, re.Match[str]] = {}
    for key, regex in _FIELDS.items():
        match = regex.search(text)
        if match:
            found[key] = match

    def hexnum(key: str) -> int | None:
        return int(found[key].group(1), 16) if key in found else None

    created = None
    if "Created" in found:
        year, month, day, hour, minute, second = (int(g) for g in found["Created"].groups())
        created = calendar.timegm((year, month, day, hour, minute, second))
    return BootReport(
        address=hexnum("address"),
        name=found["Image Name"].group(1).strip() if "Image Name" in found else None,
        created=created,
        data_size=int(found["Data Size"].group(1)) if "Data Size" in found else None,
        load_address=hexnum("Load Address"), entry_point=hexnum("Entry Point"))


def compare_boot_report(report: BootReport, image: UImage | None, address: int) -> list[str]:
    """What differs between the loader's report and the image that was sent.

    A ready marker alone cannot tell which kernel started, so the boot address
    and, when there is an image file, its name, creation time, size, load
    address and entry point must all agree. Every difference is listed.
    """
    problems: list[str] = []
    if report.address is None:
        problems.append("the loader printed no boot address")
    elif report.address != address:
        problems.append("the loader booted the image at %08x, not at %08x"
                        % (report.address, address))
    if image is None:
        return problems
    expected = (("name", report.name, image.name), ("creation time", report.created, image.created),
                ("data size", report.data_size, image.data_size),
                ("load address", report.load_address, image.load_address),
                ("entry point", report.entry_point, image.entry_point))
    for label, got, want in expected:
        if got is None:
            problems.append("the loader's report has no %s" % label)
        elif got != want:
            problems.append("the %s is %r, the file's is %r" % (label, got, want))
    return problems
