# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Builders for the images the tests send: a kernel image and a root filesystem."""

from __future__ import annotations

import random
import struct
import zlib

from bench import image

CREATED = 1790730282  # a fixed creation time, 2026-09-30 01:04:42 UTC


def make_uimage(size: int = 4096, *, name: str = "Linux-test", created: int = CREATED,
                load: int = image.LOAD_ADDRESS, entry: int = image.LOAD_ADDRESS,
                arch: int = image.ARCH_MIPS, compression: int = image.COMPRESSION_LZMA,
                os_id: int = image.OS_LINUX, image_type: int = image.TYPE_KERNEL,
                seed: int = 1, data: bytes | None = None, magic: int = image.UIMAGE_MAGIC,
                bad_header_crc: bool = False, bad_data_crc: bool = False) -> bytes:
    """A legacy image of ``size`` bytes of data, with the fields a test wants."""
    if data is None:
        rng = random.Random(seed)
        data = rng.randbytes(size)
    data_crc = zlib.crc32(data) ^ (1 if bad_data_crc else 0)
    raw_name = name.encode("latin-1")[:32].ljust(32, b"\0")
    fields = (magic, 0, created, len(data), load, entry, data_crc, os_id, arch, image_type,
              compression, raw_name)
    header = bytearray(struct.pack(">IIIIIIIBBBB32s", *fields))
    crc = zlib.crc32(bytes(header)) ^ (1 if bad_header_crc else 0)
    header[4:8] = struct.pack(">I", crc)
    return bytes(header) + data


def make_squashfs(length: int = 8192, compression: int = image.SQUASHFS_XZ,
                  seed: int = 2) -> bytes:
    """A root filesystem image: the magic, a compression id at offset 20, and filler."""
    rng = random.Random(seed)
    head = bytearray(b"hsqs" + bytes(20))
    struct.pack_into("<H", head, 20, compression)
    return bytes(head) + rng.randbytes(max(length - len(head), 0))
