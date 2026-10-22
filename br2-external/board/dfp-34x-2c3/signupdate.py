#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""signupdate.py: sign an update file, or say what it is signed with.

Usage: signupdate.py sign KEY IN OUT
       signupdate.py check FILE [KEYS]
       signupdate.py rootfs FILE OUT
       signupdate.py key KEY NAME

sign writes IN, signed with KEY, an Ed25519 private key in PEM, to OUT. check
says what onu-update check says of FILE's signature (onu-utils/src/
onu-update.c): its VERSION, its SHA256, which a release's SHA256SUMS lists,
and SIGNATURE=valid with SIGNER=, none, unknown or bad, checked with the keys
of KEYS, a file in the form of the stick's /etc/onu-update.keys; it exits 1
for a bad one. rootfs writes FILE's root filesystem part, the SquashFS image
and its signature, to OUT, for mkweb.py, so that the package of the shipped
firmware's page writes the signature into the bank too. key prints the line
of that file for KEY's public half, named NAME.

The signature is the block of onu-utils/src/update.h, 128 bytes right after
the SquashFS image, inside the root filesystem's part: "ONUS", the format 1,
the SHA-256 of the raw public key, and the Ed25519 signature of the text
"open-dfp-34x-2c update" with its NUL, followed by the SHA-256 of the header,
the kernel image and the SquashFS image. The header's length of the root
filesystem then counts the block, and its CRC and the trailer are made anew.
A signed file is signed again over its old signature. The signature itself is
OpenSSL's (openssl pkeyutl), so that this script holds no cryptography of its
own. The output is written under a name of its own and renamed, so that an
error never leaves half a file under its name.
"""

import base64
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HEADER = 256
TRAILER = 32
BLOCK = 128
MAGIC = b"ONUS"
FORMAT = 1
CONTEXT = b"open-dfp-34x-2c update\0"
# The DER of an Ed25519 public key (RFC 8410) before its 32 bytes.
SPKI = bytes.fromhex("302a300506032b6570032100")


def die(text):
    sys.exit("signupdate: " + text)


def split(data):
    """The header, kernel, SquashFS image and signature block of @data."""
    if len(data) < HEADER + TRAILER or data[:4] != b"ONUF":
        die("not an update file")
    if struct.unpack(">I", data[252:256])[0] != zlib.crc32(data[:252]) & 0xFFFFFFFF:
        die("the header's CRC does not match")
    klen, rlen = struct.unpack(">II", data[12:20])
    body = HEADER + klen + rlen
    if len(data) != body + TRAILER:
        die("its length does not match its header")
    if hashlib.sha256(data[:body]).digest() != data[body:]:
        die("the checksum does not match: the file is damaged")
    rootfs = data[HEADER + klen:body]
    if len(rootfs) < 48 or rootfs[:4] != b"hsqs":
        die("the root filesystem is not a SquashFS image")
    used = struct.unpack("<Q", rootfs[40:48])[0]
    if used > rlen:
        die("the root filesystem claims more than the file has")
    block = rootfs[used:]
    return data[:HEADER], data[HEADER:HEADER + klen], rootfs[:used], block


def header_for(head, rootfs_len):
    head = bytearray(head)
    head[16:20] = struct.pack(">I", rootfs_len)
    head[252:256] = struct.pack(">I", zlib.crc32(bytes(head[:252])) & 0xFFFFFFFF)
    return bytes(head)


def openssl(*args):
    done = subprocess.run(["openssl", *args], capture_output=True)
    if done.returncode:
        die("openssl %s: %s" % (args[0], done.stderr.decode(errors="replace").strip()))
    return done.stdout


def ed25519(tmp, message, *args):
    """openssl pkeyutl -rawin over @message, which Ed25519 takes from a file only."""
    path = os.path.join(tmp, "message")
    with open(path, "wb") as handle:
        handle.write(message)
    return subprocess.run(["openssl", "pkeyutl", "-rawin", "-in", path, *args],
                          capture_output=True)


def public_raw(keyfile):
    der = openssl("pkey", "-in", keyfile, "-pubout", "-outform", "DER")
    if len(der) != len(SPKI) + 32 or not der.startswith(SPKI):
        die("%s is not an Ed25519 key" % keyfile)
    return der[len(SPKI):]


def write(path, data):
    part = path + ".part"
    try:
        with open(part, "wb") as handle:
            handle.write(data)
        os.replace(part, path)
    finally:
        if os.path.exists(part):
            os.remove(part)


def sign(keyfile, src, dst):
    with open(src, "rb") as handle:
        head, kernel, squash, _ = split(handle.read())
    head = header_for(head, len(squash) + BLOCK)
    digest = hashlib.sha256(head + kernel + squash).digest()
    with tempfile.TemporaryDirectory() as tmp:
        done = ed25519(tmp, CONTEXT + digest, "-sign", "-inkey", keyfile)
    sig = done.stdout
    if done.returncode or len(sig) != 64:
        die("openssl gave no Ed25519 signature: %s" % done.stderr.decode(errors="replace").strip())
    pub = public_raw(keyfile)
    block = (MAGIC + struct.pack(">I", FORMAT) + hashlib.sha256(pub).digest() + sig).ljust(BLOCK, b"\0")
    body = head + kernel + squash + block
    write(dst, body + hashlib.sha256(body).digest())


def keys_of(path):
    """The keys of a file in the form of /etc/onu-update.keys: (raw, name) by id."""
    keys = {}
    with open(path, encoding="ascii") as handle:
        for line in handle:
            words = line.split(None, 2)
            if not words or words[0].startswith("#"):
                continue
            if len(words) != 3 or words[0] != "ed25519":
                die("%s: not \"ed25519 KEY NAME\": %s" % (path, line.strip()))
            raw = base64.b64decode(words[1], validate=True)
            if len(raw) != 32:
                die("%s: a key of %d bytes" % (path, len(raw)))
            keys[hashlib.sha256(raw).digest()] = (raw, words[2].strip())
    return keys


def verify(raw, sig, message):
    with tempfile.TemporaryDirectory() as tmp:
        pub, sigfile = os.path.join(tmp, "pub.der"), os.path.join(tmp, "sig")
        with open(pub, "wb") as handle:
            handle.write(SPKI + raw)
        with open(sigfile, "wb") as handle:
            handle.write(sig)
        done = ed25519(tmp, message, "-verify", "-pubin", "-inkey", pub, "-keyform", "DER",
                       "-sigfile", sigfile)
        return done.returncode == 0


def check(path, keysfile):
    with open(path, "rb") as handle:
        data = handle.read()
    head, kernel, squash, block = split(data)
    print("VERSION=" + head[96:160].rstrip(b"\0").decode("ascii", "replace"))
    print("SHA256=" + hashlib.sha256(data).hexdigest())
    if len(block) != BLOCK or block[:4] != MAGIC:
        print("SIGNATURE=none")
        return 0
    if struct.unpack(">I", block[4:8])[0] != FORMAT:
        print("SIGNATURE=unknown")
        return 0
    if any(block[104:]):
        print("SIGNATURE=bad")
        return 1
    keys = keys_of(keysfile) if keysfile else {}
    found = keys.get(block[8:40])
    if not found:
        print("SIGNATURE=unknown")
        return 0
    digest = hashlib.sha256(head + kernel + squash).digest()
    if not verify(found[0], block[40:104], CONTEXT + digest):
        print("SIGNATURE=bad")
        return 1
    print("SIGNATURE=valid")
    print("SIGNER=" + found[1])
    return 0


def main(argv):
    if len(argv) == 4 and argv[0] == "sign":
        sign(*argv[1:])
        return 0
    if len(argv) in (2, 3) and argv[0] == "check":
        return check(argv[1], argv[2] if len(argv) == 3 else None)
    if len(argv) == 3 and argv[0] == "rootfs":
        with open(argv[1], "rb") as handle:
            _, _, squash, block = split(handle.read())
        write(argv[2], squash + block)
        return 0
    if len(argv) == 3 and argv[0] == "key":
        print("ed25519 %s %s" % (base64.b64encode(public_raw(argv[1])).decode(), argv[2]))
        return 0
    sys.stderr.write(__doc__.split("\n\n")[1] + "\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
