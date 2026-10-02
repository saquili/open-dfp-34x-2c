#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""make-env.py DIR: write the files of the test tree that are not text: the
two copies of the boot loader's environment, DIR/dev/mtd1 and DIR/dev/mtd2,
and the board's model as the device tree gives it, NUL-terminated.

Each copy is 8 KiB: a CRC-32 of the rest, big-endian; a flag byte that
says which copy is newer; then NUL-terminated name=value entries and
padding. The two copies differ in one variable, so that the tests can
check which copy pond reads. The values are made up.
"""

import binascii
import os
import struct
import sys

SIZE = 8192

ENTRIES = [
    "baudrate=115200",
    "bootcmd=if itest.s ${sw_tryactive} == 2; then run boot_by_commit; else run boot_by_tryactive; fi",
    "bootdelay=5",
    "ethaddr=00:00:5e:00:53:01",
    "ipaddr=192.0.2.2",
    "sw_active=0",
    "sw_commit=0",
    "sw_crc0=5bfcffa5",
    "sw_crc1=5bfcffa5",
    "sw_tryactive=2",
    "sw_valid0=1",
    "sw_valid1=1",
    "sw_version0=1.0",
    "sw_version1=1.0",
]


def image(entries, flag):
    data = b"".join(e.encode() + b"\0" for e in entries) + b"\0"
    data = data.ljust(SIZE - 5, b"\0")
    return struct.pack(">IB", binascii.crc32(data) & 0xFFFFFFFF, flag) + data


def main():
    out = os.path.join(sys.argv[1], "dev")
    os.makedirs(out, exist_ok=True)
    stale = [e if not e.startswith("sw_version0=") else "sw_version0=STALE-COPY" for e in ENTRIES]
    with open(os.path.join(out, "mtd1"), "wb") as f:
        f.write(image(ENTRIES, 1))
    with open(os.path.join(out, "mtd2"), "wb") as f:
        f.write(image(stale, 0))
    dt = os.path.join(sys.argv[1], "proc", "device-tree")
    os.makedirs(dt, exist_ok=True)
    with open(os.path.join(dt, "model"), "wb") as f:
        f.write(b"ODI DFP-34X-2C3\0")


if __name__ == "__main__":
    main()
