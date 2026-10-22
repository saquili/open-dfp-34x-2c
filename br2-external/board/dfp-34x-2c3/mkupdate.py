#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""mkupdate.py: pack a kernel image and a root filesystem image into an
update file, which the stick's onu-update writes into one bank of its flash.

Usage: mkupdate.py --board BOARD --version VERSION [--time SECONDS]
                   KERNEL ROOTFS OUTPUT

The format is that of onu-utils/src/update.h: a header of 256 bytes, the
kernel image, the root filesystem image, and the SHA-256 of the three. The
header, its integers big-endian, holds the magic "ONUF", the format version 1,
its own length, the lengths of the two images, flags 0, the time of the build,
the board and the version as NUL-padded texts of 64 bytes, zeros, and at 252
the CRC32 of the bytes before it. The time is the kernel image's own, that
of its uImage header, unless --time names another: onu-update read takes it
from there when it gives a bank back as an update file, which is then the
file that went in, byte for byte. The output is written under a name of its
own and renamed, so that an error never leaves half a file under its name.
A release is signed afterwards (signupdate.py).
"""

import argparse
import hashlib
import os
import struct
import sys
import zlib

MAGIC = b"ONUF"
FORMAT = 1
HEADER = 256
TEXT = 64


def text(value, what):
    raw = value.encode("ascii")
    if not raw or len(raw) >= TEXT or any(b < 0x20 or b > 0x7E for b in raw):
        raise ValueError(f"{what} {value!r}: 1 to {TEXT - 1} printable ASCII characters")
    return raw.ljust(TEXT, b"\0")


def header(kernel_len, rootfs_len, time, board, version):
    head = MAGIC + struct.pack(">IIIIIQ", FORMAT, HEADER, kernel_len, rootfs_len, 0, time)
    head += text(board, "board") + text(version, "version")
    head = head.ljust(252, b"\0")
    return head + struct.pack(">I", zlib.crc32(head) & 0xFFFFFFFF)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--board", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--time", type=int)
    parser.add_argument("kernel")
    parser.add_argument("rootfs")
    parser.add_argument("output")
    args = parser.parse_args(argv)

    with open(args.kernel, "rb") as handle:
        kernel = handle.read()
    with open(args.rootfs, "rb") as handle:
        rootfs = handle.read()
    if kernel[:4] != b"\x27\x05\x19\x56":
        sys.exit(f"mkupdate: {args.kernel} is not a uImage")
    time = args.time if args.time is not None else struct.unpack(">I", kernel[8:12])[0]
    if rootfs[:4] != b"hsqs":
        sys.exit(f"mkupdate: {args.rootfs} is not a SquashFS image")
    try:
        head = header(len(kernel), len(rootfs), time, args.board, args.version)
    except ValueError as err:
        sys.exit(f"mkupdate: {err}")

    digest = hashlib.sha256(head + kernel + rootfs).digest()
    part = args.output + ".part"
    try:
        with open(part, "wb") as handle:
            handle.write(head + kernel + rootfs + digest)
        os.replace(part, args.output)
    finally:
        if os.path.exists(part):
            os.remove(part)


if __name__ == "__main__":
    main()
