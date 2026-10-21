# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Baseline OMCI messages as an OLT sends them (G.988 11.2), for the PON's inject file.

A message is 48 bytes: its transaction identifier, its type with the acknowledgement
request set, the device identifier 0x0a, the managed entity's class and instance, 32
bytes of contents, and the trailer, whose last four bytes are the CRC-32 of ITU-T
I.363.5 of the 44 before them, as omcid checks it (omcid/src/omci/frame.c). The size
of each attribute, and which of them a Create sets, come from the table of the classes
omcid serves, omcid/data/g988-me-min.tsv.

A value is bytes of the attribute's size, an int, which is written big-endian in it,
or a str, written as text and padded with zero bytes.
"""

from __future__ import annotations

import os

TABLE = os.path.join(os.path.dirname(__file__), "..", "..", "..", "omcid", "data",
                     "g988-me-min.tsv")

CREATE, DELETE, SET = 0x44, 0x46, 0x48
DEVICE_ID = 0x0A
TRAILER = bytes.fromhex("00000028")
CONTENTS = 32

Value = bytes | int | str


def classes(path: str = TABLE) -> dict[int, dict[int, tuple[int, bool]]]:
    """Each class's attributes: their size and whether a Create sets them."""
    out: dict[int, dict[int, tuple[int, bool]]] = {}
    with open(path, encoding="utf-8") as table:
        for line in table:
            if line.startswith("#"):
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) >= 6 and f[0].isdigit() and f[2].isdigit() and f[4].isdigit():
                out.setdefault(int(f[0]), {})[int(f[2])] = (int(f[4]), "C" in f[5].split(","))
    return out


def crc32(data: bytes) -> int:
    """The CRC-32 of I.363.5: polynomial 0x04c11db7, most significant bit first."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = (crc << 1) ^ 0x04C11DB7 if crc & 0x80000000 else crc << 1
            crc &= 0xFFFFFFFF
    return crc ^ 0xFFFFFFFF


def encode(value: Value, size: int) -> bytes:
    if isinstance(value, int):
        return value.to_bytes(size, "big")
    raw = value.encode() if isinstance(value, str) else value
    if len(raw) > size or (not isinstance(value, str) and len(raw) != size):
        raise ValueError(f"{value!r} does not fit {size} bytes")
    return raw + bytes(size - len(raw))


class Olt:
    """The messages of one stand-in OLT, numbered from @first on."""

    def __init__(self, first: int = 0x7E00, table: dict | None = None):
        self.tci = first
        self.table = table if table is not None else classes()

    def message(self, kind: int, class_id: int, instance: int, contents: bytes = b"") -> bytes:
        if len(contents) > CONTENTS:
            raise ValueError(f"class {class_id}: {len(contents)} bytes of contents")
        self.tci = (self.tci + 1) & 0xFFFF
        body = (self.tci.to_bytes(2, "big") + bytes([kind, DEVICE_ID])
                + class_id.to_bytes(2, "big") + instance.to_bytes(2, "big")
                + contents + bytes(CONTENTS - len(contents)) + TRAILER)
        return body + crc32(body).to_bytes(4, "big")

    def attrs(self, class_id: int) -> dict[int, tuple[int, bool]]:
        if class_id not in self.table:
            raise ValueError(f"class {class_id} is not one omcid serves")
        return self.table[class_id]

    def create(self, class_id: int, instance: int, **values: Value) -> bytes:
        """A Create: the attributes a Create sets, in order, each given or zero."""
        attrs = self.attrs(class_id)
        given = {int(k[1:]): v for k, v in values.items()}
        for n in given:
            if not attrs.get(n, (0, False))[1]:
                raise ValueError(f"class {class_id}: a Create does not set attribute {n}")
        return self.message(CREATE, class_id, instance,
                            b"".join(encode(given.get(n, 0), size)
                                     for n, (size, by_create) in sorted(attrs.items()) if by_create))

    def set(self, class_id: int, instance: int, **values: Value) -> bytes:
        """A Set of the attributes given, a1=..., with its mask."""
        attrs = self.attrs(class_id)
        given = {int(k[1:]): v for k, v in values.items()}
        mask = 0
        for n in given:
            if n not in attrs:
                raise ValueError(f"class {class_id} has no attribute {n}")
            mask |= 1 << (16 - n)
        return self.message(SET, class_id, instance, mask.to_bytes(2, "big") + b"".join(
            encode(given[n], attrs[n][0]) for n in sorted(given)))

    def delete(self, class_id: int, instance: int) -> bytes:
        return self.message(DELETE, class_id, instance)


def hexes(messages: list[bytes]) -> str:
    """The messages as lines of hex, one per line, as the inject file takes each."""
    return "".join(m.hex() + "\n" for m in messages)
