# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Classify the difference between two token streams.

The classes, from harmless to not:

``comment``
    The streams are equal: the files differ only in comments, whitespace or
    help/description prose.
``literal``
    Replace every maximal run of adjacent literal tokens (see ``tok``) by one
    placeholder; the two skeletons are then equal.  The files differ only in
    the text of string literals -- adjacent literals count as one, since the
    compiler joins them.  Each changed run is listed.  This is a review
    class, not an equivalence: a changed literal can still change behaviour
    (a ``compatible`` string, a buffer sized by ``sizeof("...")``).
``code``
    Anything else.

The definition of ``literal`` does not depend on how a diff aligns the two
files, so the class is exact; the hunks shown for ``code`` are only a guide.
"""

import collections
import difflib

from . import ctok
from .tok import LITERAL_KINDS

LiteralChange = collections.namedtuple(
    'LiteralChange', 'a_line b_line a_text b_text same_value')
Hunk = collections.namedtuple('Hunk', 'a_line b_line a_tokens b_tokens')


class Result:
    """The outcome of comparing two token streams."""

    def __init__(self, cls, literals=(), hunks=(), a_count=0, b_count=0):
        self.cls = cls
        self.literals = list(literals)
        self.hunks = list(hunks)
        self.a_count = a_count
        self.b_count = b_count

    def as_dict(self):
        d = {'class': self.cls}
        if self.literals:
            d['literals'] = [c._asdict() for c in self.literals]
        if self.hunks:
            d['hunks'] = [{
                'a_line': h.a_line, 'b_line': h.b_line,
                'a': [list(t.key) for t in h.a_tokens],
                'b': [list(t.key) for t in h.b_tokens],
            } for h in self.hunks]
        return d


def _skeleton(tokens):
    """(skeleton keys, literal runs) of a token list."""
    skel = []
    runs = []
    run = None
    for t in tokens:
        if t.kind in LITERAL_KINDS:
            if run is None:
                run = []
                runs.append(run)
                skel.append(('<literal>', ''))
            run.append(t)
        else:
            run = None
            skel.append(t.key)
    return skel, runs


def _run_text(run):
    return ' '.join(t.text for t in run)


def _same_value(run_a, run_b, language):
    if language not in ('c', 'asm', 'dts'):
        return None
    va = ctok.string_value([t.text for t in run_a])
    vb = ctok.string_value([t.text for t in run_b])
    if va is None or vb is None:
        return None
    return va == vb


def _hunks(a, b, limit):
    """Up to *limit* differing regions of two token lists, as a guide."""
    ka = [t.key for t in a]
    kb = [t.key for t in b]
    # Trim the common prefix and suffix first: most edits are local and the
    # matcher is slow on long, repetitive token lists.
    lo = 0
    while lo < len(ka) and lo < len(kb) and ka[lo] == kb[lo]:
        lo += 1
    hi = 0
    while (hi < len(ka) - lo and hi < len(kb) - lo
           and ka[len(ka) - 1 - hi] == kb[len(kb) - 1 - hi]):
        hi += 1
    ma = ka[lo:len(ka) - hi]
    mb = kb[lo:len(kb) - hi]
    out = []
    matcher = difflib.SequenceMatcher(None, ma, mb, autojunk=len(ma) > 5000)
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == 'equal':
            continue
        ta = a[lo + i1:lo + i2]
        tb = b[lo + j1:lo + j2]
        a_line = ta[0].line if ta else (a[lo + i1 - 1].line if lo + i1 else 1)
        b_line = tb[0].line if tb else (b[lo + j1 - 1].line if lo + j1 else 1)
        out.append(Hunk(a_line, b_line, ta[:12], tb[:12]))
        if len(out) >= limit:
            break
    return out


def compare(a, b, language=None, hunk_limit=5):
    """Compare two token lists; return a Result."""
    if [t.key for t in a] == [t.key for t in b]:
        return Result('comment', a_count=len(a), b_count=len(b))
    skel_a, runs_a = _skeleton(a)
    skel_b, runs_b = _skeleton(b)
    if skel_a == skel_b:
        changes = []
        for ra, rb in zip(runs_a, runs_b):
            if [t.key for t in ra] != [t.key for t in rb]:
                changes.append(LiteralChange(
                    ra[0].line, rb[0].line, _run_text(ra), _run_text(rb),
                    _same_value(ra, rb, language)))
        return Result('literal', literals=changes, a_count=len(a),
                      b_count=len(b))
    return Result('code', hunks=_hunks(a, b, hunk_limit), a_count=len(a),
                  b_count=len(b))
