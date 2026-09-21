# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""SHA-256 manifests of a kernel build directory, and their comparison.

A manifest lists, one per line and sorted by path, the SHA-256 and the path
(relative to the build directory) of:

* ``.config`` and ``vmlinux``;
* every ``*.o`` and ``*.ko``;
* every ``*.dtb`` and ``*.dtbo``;
* the boot images: the regular files directly in ``arch/<arch>/boot/``.

The objects include those of the kernel's host programs, which a build made
with ``kbuild-repro`` reproduces as well.  With *target_only* they are left
out: an object is kept only if it is not an ELF file or has the class, byte
order and machine of ``vmlinux`` (or ``vmlinux.o``).  The format is that of
``sha256sum``, so ``sha256sum -c`` run in the build directory checks a
manifest.
"""

import hashlib
import os
import posixpath
import struct

_ELF_MAGIC = b'\x7fELF'


def elf_ident(path):
    """(class, byte order, machine) of an ELF file, or None if it is not one."""
    try:
        with open(path, 'rb') as f:
            head = f.read(20)
    except OSError:
        return None
    if len(head) < 20 or head[:4] != _ELF_MAGIC:
        return None
    order = '>' if head[5] == 2 else '<'
    return (head[4], head[5], struct.unpack(order + 'H', head[18:20])[0])


def _sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def _is_boot_image(rel):
    parts = rel.split('/')
    return (len(parts) == 4 and parts[0] == 'arch' and parts[2] == 'boot'
            and not parts[3].startswith('.') and not parts[3].endswith('.o'))


class ManifestError(Exception):
    pass


def record(out_dir, target_only=False):
    """Return [(sha256, path)] for the build directory *out_dir*."""
    if not os.path.isdir(out_dir):
        raise ManifestError('%s: not a directory' % out_dir)
    target = None
    if target_only:
        for name in ('vmlinux', 'vmlinux.o'):
            target = elf_ident(os.path.join(out_dir, name))
            if target:
                break
        if target is None:
            raise ManifestError(
                '%s: neither vmlinux nor vmlinux.o is an ELF file, so target '
                'objects cannot be told from host objects' % out_dir)
    entries = []
    for dirpath, dirnames, filenames in os.walk(out_dir):
        dirnames.sort()
        rel_dir = os.path.relpath(dirpath, out_dir)
        rel_dir = '' if rel_dir == '.' else rel_dir.replace(os.sep, '/')
        for name in filenames:
            full = os.path.join(dirpath, name)
            if os.path.islink(full) or not os.path.isfile(full):
                continue
            rel = posixpath.join(rel_dir, name) if rel_dir else name
            if rel in ('.config', 'vmlinux'):
                pass
            elif name.endswith(('.o', '.ko')):
                if target_only:
                    ident = elf_ident(full)
                    if ident is not None and ident != target:
                        continue
            elif name.endswith(('.dtb', '.dtbo')):
                pass
            elif _is_boot_image(rel):
                pass
            else:
                continue
            entries.append((_sha256(full), rel))
    entries.sort(key=lambda e: e[1].encode('utf-8', 'surrogateescape'))
    return entries


def format_manifest(entries):
    return ''.join('%s  %s\n' % (sha, path) for sha, path in entries)


def parse(text, origin='manifest'):
    """Parse sha256sum-format text into {path: sha256}."""
    result = {}
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        sha, sep, path = line.partition('  ')
        if (not sep or len(sha) != 64
                or any(c not in '0123456789abcdef' for c in sha)):
            raise ManifestError('%s:%d: not a sha256sum line' % (origin, lineno))
        if path in result:
            raise ManifestError('%s:%d: %s listed twice'
                                % (origin, lineno, path))
        result[path] = sha
    return result


def load(arg, target_only=False):
    """A manifest from a file, or recorded from a build directory."""
    if os.path.isdir(arg):
        return dict((p, s) for s, p in record(arg, target_only))
    with open(arg, encoding='utf-8', errors='surrogateescape') as f:
        return parse(f.read(), arg)


def diff(a, b):
    """(only in a, only in b, differing) paths of two manifests."""
    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    differ = sorted(p for p in set(a) & set(b) if a[p] != b[p])
    return only_a, only_b, differ
