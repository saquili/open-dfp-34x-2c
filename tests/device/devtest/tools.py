# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tools the flash's image does not carry, put on the stick for a run.

The cases that install rules of their own run iproute2's tc, bridge, devlink
and dcb, and others ubus and ethtool, which only a RAM image carries (the
post-build script sets them aside in build/image/diag,
docs/handbook/building.rst). A case that names such
a tool finds it on the stick, or, when the run was given a build's diag
directory (--tools), has it put there: a small file system of memory of its
own, /run/devtest-tools, with the programs, the C library whole as the diag
directory has it (the image's own is cut to what the image's programs use) and
a script for each program that runs it with that library. Nothing is written
to the flash and nothing of the image changes, so the image under test is the
flash's. The directory is lost with a reset of the stick, and the next case
that needs a tool puts it there again; the run takes it away at its end.
"""

from __future__ import annotations

import glob
import gzip
import io
import os
import shlex
import tarfile
from typing import TYPE_CHECKING, Iterable

if TYPE_CHECKING:
    from .core import Context

TOOLS_DIR = "/run/devtest-tools"
# The programs, by name, where the diag directory has them.
PROGRAMS = {"tc": "sbin/tc", "bridge": "sbin/bridge", "devlink": "sbin/devlink",
            "dcb": "sbin/dcb", "ethtool": "usr/sbin/ethtool", "ubus": "usr/bin/ubus"}
# What the programs read, if the diag directory has it: tc's tables.
TABLES = ("usr/lib/tc", "usr/share/iproute2")
SIZE_KIB = 4096


class ToolsError(Exception):
    """The directory given cannot supply the tools: a fault of the bench."""


def has(ctx: Context, name: str) -> bool:
    return ctx.stick.run(f"command -v {shlex.quote(name)}").ok


def script(name: str) -> bytes:
    return (f"#!/bin/sh\nLD_LIBRARY_PATH={TOOLS_DIR}/lib exec {TOOLS_DIR}/{PROGRAMS[name]}"
            ' "$@"\n').encode()


def archive(directory: str) -> bytes:
    """The programs, the C library whole as libc.so.0, the tables and the scripts,
    as a gzip-compressed tar for a stick that unpacks it as root."""
    if not os.path.isdir(directory):
        raise ToolsError(f"--tools {directory}: no such directory")
    libs = sorted(glob.glob(os.path.join(directory, "lib", "libuClibc-*.so")))
    missing = [rel for rel in PROGRAMS.values() if not os.path.isfile(os.path.join(directory, rel))]
    if missing or not libs:
        raise ToolsError(f"--tools {directory} is no diag directory of a build: "
                         f"{', '.join(missing + ([] if libs else ['lib/libuClibc-*.so']))} missing")

    def plain(info: tarfile.TarInfo) -> tarfile.TarInfo:
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        return info

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tar:
        for rel in PROGRAMS.values():
            tar.add(os.path.join(directory, rel), arcname=rel, filter=plain)
        tar.add(libs[0], arcname=f"lib/{os.path.basename(libs[0])}", filter=plain)
        link = tarfile.TarInfo("lib/libc.so.0")
        link.type, link.linkname, link.mode = tarfile.SYMTYPE, os.path.basename(libs[0]), 0o777
        tar.addfile(link)
        for rel in TABLES:
            if os.path.isdir(os.path.join(directory, rel)):
                tar.add(os.path.join(directory, rel), arcname=rel, filter=plain)
        for name in PROGRAMS:
            body = script(name)
            info = tarfile.TarInfo(f"bin/{name}")
            info.size, info.mode = len(body), 0o755
            tar.addfile(info, io.BytesIO(body))
    return gzip.compress(buf.getvalue(), 6, mtime=0)


def install(ctx: Context) -> None:
    """Put the tools on the stick, in a file system of memory of their own,
    and have the stick's commands find them first."""
    data = archive(ctx.settings.tools)
    where = shlex.quote(TOOLS_DIR)
    ctx.stick.run(f"mkdir -p {where} && {{ mountpoint -q {where} || "
                  f"mount -t tmpfs -o size={SIZE_KIB}k,mode=755 tmpfs {where}; }} && "
                  f"gunzip -c | tar -x -C {where}", stdin=data, timeout=120, check=True)
    ctx.stick.prefix = f"export PATH={TOOLS_DIR}/bin:$PATH; "
    ctx.memo["tools"] = ctx.settings.tools


def remove(ctx: Context) -> None:
    """Take the tools away again, when the run put them there."""
    if ctx.memo.pop("tools", None):
        ctx.stick.prefix = ""
        ctx.stick.run(f"umount {shlex.quote(TOOLS_DIR)} 2>/dev/null; "
                      f"rmdir {shlex.quote(TOOLS_DIR)} 2>/dev/null; true")


def ensure(ctx: Context, names: Iterable[str]) -> list[str]:
    """The tools of names that the stick still lacks: those it has, or got
    from the diag directory of the run's settings, are not among them."""
    missing = [n for n in names if not has(ctx, n)]
    if missing and ctx.settings.tools:
        install(ctx)
        missing = [n for n in missing if not has(ctx, n)]
    return missing
