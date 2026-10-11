#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""mkweb.py: pack a kernel image and a root filesystem image into the package
that the web page of the stick's shipped firmware installs (ADR 0014, step 9).

Usage: mkweb.py --version VERSION --time SECONDS KERNEL ROOTFS OUTPUT

The shipped firmware's page takes a tar file smaller than its own bank, the
1328 KiB of its kernel partition and the 2512 KiB of its root filesystem
partition, 3932160 bytes, that holds md5.txt; it runs the file's fwu.sh, once
md5.txt gives fwu.sh's digest, with the bank to write. The package holds:

  fwu.sh   board/dfp-34x-2c3/fwu.sh, with the digests and the version in it
  md5.txt  the digests of the three other files, as md5sum prints them
  k        the first 1328 KiB of this system's bank: the kernel image,
           padded with 0xff to 1856 KiB, then the root filesystem image
  r        the rest of that bank

so that fwu.sh, which writes k into the shipped firmware's kernel partition
and r into its root filesystem partition, leaves the bank as this system's
split has it. The digests in fwu.sh are those of the two partitions read
back after the write: k, and r followed by the erased flash. Every member is
root's and has the build's time, so that the same images give the same file.
The output is written under a name of its own and renamed, so that an error
never leaves half a file under its name.
"""

import argparse
import hashlib
import io
import os
import re
import sys
import tarfile

KERNEL_PART = 0x1D0000      # this system's kernel partition, 1856 KiB
ROOTFS_PART = 0x1F0000      # this system's root filesystem partition, 1984 KiB
SHIPPED_KERNEL = 0x14C000   # the shipped firmware's kernel partition, 1328 KiB
SHIPPED_ROOTFS = 0x274000   # its root filesystem partition, 2512 KiB
LIMIT = SHIPPED_KERNEL + SHIPPED_ROOTFS  # the page takes files smaller than this

FWU = os.path.join(os.path.dirname(os.path.realpath(__file__)), "fwu.sh")


def member(name, data, mode, time):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    info.mtime = time
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    return info, io.BytesIO(data)


def package(kernel, rootfs, version, time):
    """The package's bytes, or ValueError with what does not fit."""
    if kernel[:4] != b"\x27\x05\x19\x56":
        raise ValueError("the kernel image is not a uImage")
    if rootfs[:4] != b"hsqs":
        raise ValueError("the root filesystem image is not a SquashFS image")
    if len(kernel) > KERNEL_PART:
        raise ValueError(f"the kernel image is {len(kernel)} bytes, its partition {KERNEL_PART}")
    if len(rootfs) > ROOTFS_PART:
        raise ValueError(f"the root filesystem is {len(rootfs)} bytes, its partition {ROOTFS_PART}")
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,63}", version):
        raise ValueError(f"version {version!r}: 1 to 63 of A-Z a-z 0-9 . _ -")

    bank = kernel + b"\xff" * (KERNEL_PART - len(kernel)) + rootfs
    k, r = bank[:SHIPPED_KERNEL], bank[SHIPPED_KERNEL:]
    k_md5 = hashlib.md5(k).hexdigest()
    r_md5 = hashlib.md5(r + b"\xff" * (SHIPPED_ROOTFS - len(r))).hexdigest()
    with open(FWU, encoding="utf-8") as handle:
        script = handle.read()
    for key, value in (("@K_MD5@", k_md5), ("@R_MD5@", r_md5), ("@VERSION@", version)):
        if script.count(key) != 1:
            raise ValueError(f"{FWU} has not one {key}")
        script = script.replace(key, value)
    fwu = script.encode("utf-8")
    md5 = "".join(f"{hashlib.md5(data).hexdigest()}  {name}\n"
                  for name, data in (("fwu.sh", fwu), ("k", k), ("r", r))).encode("ascii")

    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for name, data, mode in (("fwu.sh", fwu, 0o755), ("md5.txt", md5, 0o644),
                                 ("k", k, 0o644), ("r", r, 0o644)):
            tar.addfile(*member(name, data, mode, time))
    data = out.getvalue()
    if len(data) >= LIMIT:
        raise ValueError(f"the package is {len(data)} bytes; the page takes fewer than {LIMIT}")
    return data


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True)
    parser.add_argument("--time", required=True, type=int)
    parser.add_argument("kernel")
    parser.add_argument("rootfs")
    parser.add_argument("output")
    args = parser.parse_args(argv)

    with open(args.kernel, "rb") as handle:
        kernel = handle.read()
    with open(args.rootfs, "rb") as handle:
        rootfs = handle.read()
    try:
        data = package(kernel, rootfs, args.version, args.time)
    except ValueError as err:
        sys.exit(f"mkweb: {err}")

    part = args.output + ".part"
    try:
        with open(part, "wb") as handle:
            handle.write(data)
        os.replace(part, args.output)
    finally:
        if os.path.exists(part):
            os.remove(part)


if __name__ == "__main__":
    main()
