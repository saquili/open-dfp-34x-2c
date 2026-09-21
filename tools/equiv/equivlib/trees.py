# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Walk two trees and classify every file that differs.

A tree is a directory, or a revision of a git repository.  Reading revisions
through git never touches the file system, so a kernel tree can be compared
on a case-insensitive volume as well: nothing is checked out.

Per-file classes: ``identical`` (same bytes), ``comment``, ``literal``,
``code`` (see ``compare``), ``added`` and ``removed`` (present on one side),
``other`` (different bytes in a file with no token model here: prose,
scripts, binaries) and ``error`` (the file could not be tokenised).
"""

import os
import subprocess

from . import compare, lang
from .tok import LexError

CLASSES = ('identical', 'comment', 'literal', 'code', 'added', 'removed',
           'other', 'error')


class FileResult:
    def __init__(self, path, cls, language=None, detail=None, note=None):
        self.path = path
        self.cls = cls
        self.language = language
        self.detail = detail      # a compare.Result for comment/literal/code
        self.note = note          # a short explanation for other classes

    def as_dict(self):
        d = {'path': self.path, 'class': self.cls}
        if self.language:
            d['language'] = self.language
        if self.detail is not None:
            d.update({k: v for k, v in self.detail.as_dict().items()
                      if k != 'class'})
        if self.note:
            d['note'] = self.note
        return d


def classify_bytes(path, a, b, arch=lang.DEFAULT_ARCH, language=None):
    """Classify one file present on both sides with contents *a* and *b*."""
    if a == b:
        return FileResult(path, 'identical')
    language = language or lang.detect(path)
    if language is None:
        binary = b'\0' in a or b'\0' in b
        return FileResult(path, 'other',
                          note='binary' if binary else 'no token model')
    try:
        ta = lang.tokenize(lang.decode(a), language, path, arch)
        tb = lang.tokenize(lang.decode(b), language, path, arch)
    except LexError as e:
        return FileResult(path, 'error', language, note=str(e))
    res = compare.compare(ta, tb, language)
    return FileResult(path, res.cls, language, detail=res)


# -- directories ---------------------------------------------------------------------

def _walk(root, skip_git=True):
    """{relative path: ('file'|'link', abs path)} of everything under root."""
    entries = {}
    stack = ['']
    while stack:
        rel = stack.pop()
        with os.scandir(os.path.join(root, rel) if rel else root) as it:
            for e in it:
                r = rel + '/' + e.name if rel else e.name
                if e.is_symlink():
                    entries[r] = ('link', e.path)
                elif e.is_dir():
                    if skip_git and e.name == '.git':
                        continue
                    stack.append(r)
                elif e.is_file():
                    entries[r] = ('file', e.path)
    return entries


def _read(kind, path):
    if kind == 'link':
        return os.fsencode(os.readlink(path))
    with open(path, 'rb') as f:
        return f.read()


def compare_dirs(a_root, b_root, arch=lang.DEFAULT_ARCH):
    """Yield a FileResult for every path under either directory."""
    a = _walk(a_root)
    b = _walk(b_root)
    for path in sorted(set(a) | set(b)):
        if path not in b:
            yield FileResult(path, 'removed')
            continue
        if path not in a:
            yield FileResult(path, 'added')
            continue
        (ka, pa), (kb, pb) = a[path], b[path]
        if ka != kb:
            yield FileResult(path, 'code', note='%s became %s' % (ka, kb))
            continue
        da, db = _read(ka, pa), _read(kb, pb)
        if ka == 'link':
            yield FileResult(path, 'identical' if da == db else 'code',
                             note=None if da == db else 'symlink target')
            continue
        res = classify_bytes(path, da, db, arch)
        xa = os.stat(pa).st_mode & 0o111 != 0
        xb = os.stat(pb).st_mode & 0o111 != 0
        if xa != xb and res.cls in ('identical', 'comment'):
            res = FileResult(path, 'code', note='executable bit %s' %
                             ('set' if xb else 'cleared'))
        yield res


# -- git revisions -------------------------------------------------------------------

def _git(repo, *args, **kw):
    return subprocess.run(('git', '-C', repo) + args, check=True,
                          stdout=subprocess.PIPE, **kw).stdout


class _Blobs:
    """Read blobs through one 'git cat-file --batch' process."""

    def __init__(self, repo):
        self.p = subprocess.Popen(('git', '-C', repo, 'cat-file', '--batch'),
                                  stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE)

    def get(self, sha):
        self.p.stdin.write(sha.encode() + b'\n')
        self.p.stdin.flush()
        header = self.p.stdout.readline().split()
        if len(header) != 3:
            raise RuntimeError('git cat-file: no object %s' % sha)
        size = int(header[2])
        data = self.p.stdout.read(size)
        self.p.stdout.read(1)
        return data

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def _count_files(repo, rev):
    out = _git(repo, 'ls-tree', '-r', '-z', '--name-only', rev)
    return len([p for p in out.split(b'\0') if p])


def compare_git(repo, rev_a, rev_b, arch=lang.DEFAULT_ARCH):
    """Yield a FileResult for every path that differs between two revisions
    (or tree-ishes such as HEAD:linux), then ('counts', n_a, n_b)."""
    out = _git(repo, 'diff-tree', '-r', '-z', '--no-renames', rev_a, rev_b)
    fields = out.split(b'\0')
    blobs = _Blobs(repo)
    try:
        k = 0
        while k + 1 < len(fields):
            meta = fields[k].decode()
            path = os.fsdecode(fields[k + 1])
            k += 2
            if not meta.startswith(':'):
                continue
            mode_a, mode_b, sha_a, sha_b, status = meta[1:].split()
            if status == 'A':
                yield FileResult(path, 'added')
            elif status == 'D':
                yield FileResult(path, 'removed')
            elif mode_a != mode_b and '120000' in (mode_a, mode_b):
                yield FileResult(path, 'code',
                                 note='mode %s became %s' % (mode_a, mode_b))
            elif mode_a == '160000' or mode_b == '160000':
                yield FileResult(path, 'code', note='submodule commit')
            elif mode_a == '120000':
                yield FileResult(path, 'code', note='symlink target')
            else:
                res = classify_bytes(path, blobs.get(sha_a), blobs.get(sha_b),
                                     arch)
                if mode_a != mode_b and res.cls in ('identical', 'comment'):
                    res = FileResult(path, 'code',
                                     note='mode %s became %s' % (mode_a,
                                                                  mode_b))
                yield res
    finally:
        blobs.close()
    yield ('counts', _count_files(repo, rev_a), _count_files(repo, rev_b))
