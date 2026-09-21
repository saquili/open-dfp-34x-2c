# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Command-line front ends of the equivalence tools.

Exit status, shared by the comparing tools:

  0  no difference beyond comments, whitespace and help/description prose
  3  string-literal differences only (each one listed)
  1  code differences, or files added, removed or without a token model
  2  error: bad usage, an unreadable input, a file that cannot be tokenised
"""

import argparse
import json
import os
import subprocess
import sys

from . import compare, kconfig, lang, manifest, trees, yaml
from .tok import LexError

EXIT_SAME = 0
EXIT_DIFFERENT = 1
EXIT_ERROR = 2
EXIT_LITERAL = 3


def _out(text):
    sys.stdout.buffer.write(lang.encode(text))


def _err(prog, text):
    sys.stderr.write('%s: %s\n' % (prog, text))


def _read_text(path):
    with open(path, 'rb') as f:
        return lang.decode(f.read())


def _short(text, width=110):
    text = text.replace('\t', ' ')
    return text if len(text) <= width else text[:width - 3] + '...'


def _tokens_text(tokens):
    return _short(' '.join(t.text if t.text else '<%s>' % t.kind
                           for t in tokens) or '(nothing)')


def _detail_lines(res, indent='    '):
    lines = []
    if res.cls == 'literal':
        for c in res.literals:
            note = '  (same value, split differently)' if c.same_value else ''
            lines.append('%sA:%-6d %s' % (indent, c.a_line, _short(c.a_text)))
            lines.append('%sB:%-6d %s%s' % (indent, c.b_line, _short(c.b_text),
                                            note))
    elif res.cls == 'code':
        for h in res.hunks:
            lines.append('%sA:%-6d %s' % (indent, h.a_line,
                                          _tokens_text(h.a_tokens)))
            lines.append('%sB:%-6d %s' % (indent, h.b_line,
                                          _tokens_text(h.b_tokens)))
    return lines


def _status(classes):
    if 'error' in classes:
        return EXIT_ERROR
    if classes & {'code', 'added', 'removed', 'other'}:
        return EXIT_DIFFERENT
    if 'literal' in classes:
        return EXIT_LITERAL
    return EXIT_SAME


def _add_lang_args(p):
    p.add_argument('--lang', choices=lang.LANGUAGES,
                   help='language of the input (default: from the file name)')
    p.add_argument('--arch', default=lang.DEFAULT_ARCH,
                   help='architecture the tree is built for; decides whether '
                        "'#' starts a comment in assembly (default: "
                        '%(default)s)')


# -- c-tokens --------------------------------------------------------------------------

def c_tokens(argv, prog):
    p = argparse.ArgumentParser(
        prog=prog,
        description='Print the canonical token stream of source files: '
                    'comments and the amount of whitespace removed, '
                    'identifiers and literals kept exactly.  One token per '
                    'line as KIND<TAB>TEXT.')
    _add_lang_args(p)
    p.add_argument('--lines', action='store_true',
                   help='prefix each token with the line it starts on')
    p.add_argument('files', nargs='+', metavar='FILE')
    args = p.parse_args(argv)
    status = EXIT_SAME
    for path in args.files:
        language = args.lang or lang.detect(path)
        if language is None:
            _err(prog, '%s: unknown language (use --lang)' % path)
            status = EXIT_ERROR
            continue
        try:
            toks = lang.tokenize(_read_text(path), language, path, args.arch)
        except (OSError, LexError) as e:
            _err(prog, '%s: %s' % (path, e))
            status = EXIT_ERROR
            continue
        if len(args.files) > 1:
            _out('==> %s <==\n' % path)
        for t in toks:
            if args.lines:
                _out('%d\t%s\t%s\n' % (t.line, t.kind, t.text))
            else:
                _out('%s\t%s\n' % (t.kind, t.text))
    return status


# -- compare-files -----------------------------------------------------------------------

def compare_files(argv, prog):
    p = argparse.ArgumentParser(
        prog=prog,
        description='Compare two source files token by token and say whether '
                    'they differ in comments and whitespace only, in string '
                    'literals only, or in code.')
    _add_lang_args(p)
    p.add_argument('--json', action='store_true', help='print JSON')
    p.add_argument('-q', '--quiet', action='store_true',
                   help='print nothing; the exit status tells')
    p.add_argument('a', metavar='A')
    p.add_argument('b', metavar='B')
    args = p.parse_args(argv)
    language = args.lang or lang.detect(args.a)
    if language is None or (args.lang is None
                            and lang.detect(args.b) != language):
        _err(prog, 'cannot tell the language of %s and %s alike; use --lang'
             % (args.a, args.b))
        return EXIT_ERROR
    try:
        with open(args.a, 'rb') as f:
            da = f.read()
        with open(args.b, 'rb') as f:
            db = f.read()
    except OSError as e:
        _err(prog, str(e))
        return EXIT_ERROR
    res = trees.classify_bytes(args.b, da, db, args.arch, language)
    status = _status({res.cls})
    if args.quiet:
        return status
    if args.json:
        d = res.as_dict()
        d.update({'a': args.a, 'b': args.b})
        _out(json.dumps(d, indent=2) + '\n')
        return status
    summary = {
        'identical': 'identical: the files are byte-identical',
        'comment': 'comment: the files differ only in comments and '
                   'whitespace',
        'literal': 'literal: the code is token-identical; %d string '
                   'literal(s) differ' % len(res.detail.literals
                                             if res.detail else []),
        'code': 'code: the token streams differ',
        'error': 'error: %s' % res.note,
    }[res.cls]
    _out(summary + '\n')
    if res.detail is not None:
        for line in _detail_lines(res.detail, '  '):
            _out(line + '\n')
    return status


# -- compare-trees -----------------------------------------------------------------------

def compare_trees(argv, prog):
    p = argparse.ArgumentParser(
        prog=prog,
        description='Classify every file that differs between two trees as '
                    'comment (comments and whitespace only), literal (string '
                    'literals only), code, added, removed, other (no token '
                    'model for the file type) or error.')
    p.add_argument('--arch', default=lang.DEFAULT_ARCH,
                   help='architecture the trees are built for (default: '
                        '%(default)s)')
    p.add_argument('--git', metavar='REPO',
                   help='A and B are revisions or tree-ishes (B4, HEAD:linux) '
                        'of the git repository REPO; nothing is checked out')
    p.add_argument('--json', action='store_true', help='print JSON')
    p.add_argument('--brief', action='store_true',
                   help='one line per file, no details')
    p.add_argument('--show-identical', action='store_true',
                   help='list byte-identical files too')
    p.add_argument('a', metavar='A')
    p.add_argument('b', metavar='B')
    args = p.parse_args(argv)

    counts = dict((c, 0) for c in trees.CLASSES)
    listed = []
    try:
        if args.git:
            results = trees.compare_git(args.git, args.a, args.b, args.arch)
        else:
            for d in (args.a, args.b):
                if not os.path.isdir(d):
                    _err(prog, '%s: not a directory' % d)
                    return EXIT_ERROR
            results = trees.compare_dirs(args.a, args.b, args.arch)
        n_a = None
        for r in results:
            if isinstance(r, tuple):
                n_a = r[1]
                continue
            counts[r.cls] += 1
            if r.cls != 'identical' or args.show_identical:
                listed.append(r)
    except (OSError, RuntimeError) as e:
        _err(prog, str(e))
        return EXIT_ERROR
    except subprocess.CalledProcessError as e:
        _err(prog, 'git failed: %s' % e)
        return EXIT_ERROR
    if n_a is not None:
        differing = sum(counts[c] for c in trees.CLASSES
                        if c not in ('identical', 'added'))
        counts['identical'] = n_a - differing
    status = _status(set(c for c in trees.CLASSES
                         if counts[c] and c != 'identical'))

    if args.json:
        _out(json.dumps({
            'a': args.a, 'b': args.b, 'git': args.git,
            'status': status, 'counts': counts,
            'files': [r.as_dict() for r in listed],
        }, indent=2) + '\n')
        return status
    for r in listed:
        note = '  (%s)' % r.note if r.note else ''
        _out('%-9s %s%s\n' % (r.cls, r.path, note))
        if not args.brief and r.detail is not None:
            for line in _detail_lines(r.detail):
                _out(line + '\n')
    total = sum(counts.values())
    _out('%d files: %s\n' % (total, ', '.join(
        '%d %s' % (counts[c], c) for c in trees.CLASSES)))
    return status


# -- kconfig-strip / yaml-strip ------------------------------------------------------------

def _strip_tool(argv, prog, what, fn):
    p = argparse.ArgumentParser(
        prog=prog,
        description='Print %s without its comments and prose, normalised '
                    'for comparison.' % what)
    p.add_argument('files', nargs='+', metavar='FILE')
    args = p.parse_args(argv)
    status = EXIT_SAME
    for path in args.files:
        try:
            text = fn(_read_text(path))
        except (OSError, LexError) as e:
            _err(prog, '%s: %s' % (path, e))
            status = EXIT_ERROR
            continue
        if len(args.files) > 1:
            _out('==> %s <==\n' % path)
        _out(text)
    return status


def kconfig_strip(argv, prog):
    return _strip_tool(argv, prog, 'a Kconfig file (help text is prose)',
                       kconfig.strip)


def yaml_strip(argv, prog):
    return _strip_tool(argv, prog,
                       'a binding schema (description and title are prose)',
                       yaml.strip)


# -- obj-manifest / compare-objs ------------------------------------------------------------

def obj_manifest(argv, prog):
    p = argparse.ArgumentParser(
        prog=prog,
        description='Print the SHA-256 manifest of a kernel build directory: '
                    '.config, vmlinux, every object (*.o, *.ko), device '
                    'trees and boot images, in sha256sum format.')
    p.add_argument('--target-only', action='store_true',
                   help='leave out the objects of host programs (ELF files '
                        'for another machine than vmlinux)')
    p.add_argument('-o', '--output', metavar='FILE',
                   help='write the manifest to FILE instead of stdout')
    p.add_argument('build_dir', metavar='OUT')
    args = p.parse_args(argv)
    try:
        text = manifest.format_manifest(
            manifest.record(args.build_dir, args.target_only))
    except (OSError, manifest.ManifestError) as e:
        _err(prog, str(e))
        return EXIT_ERROR
    if args.output:
        with open(args.output, 'w', encoding='utf-8',
                  errors='surrogateescape') as f:
            f.write(text)
    else:
        _out(text)
    return EXIT_SAME


def compare_objs(argv, prog):
    p = argparse.ArgumentParser(
        prog=prog,
        description='Compare two object manifests (files written by '
                    'obj-manifest, or build directories).  Exit status 0 '
                    'when they list the same files with the same hashes.')
    p.add_argument('--target-only', action='store_true',
                   help='when reading a build directory, leave out the '
                        'objects of host programs')
    p.add_argument('--json', action='store_true', help='print JSON')
    p.add_argument('a', metavar='A')
    p.add_argument('b', metavar='B')
    args = p.parse_args(argv)
    try:
        ma = manifest.load(args.a, args.target_only)
        mb = manifest.load(args.b, args.target_only)
    except (OSError, manifest.ManifestError) as e:
        _err(prog, str(e))
        return EXIT_ERROR
    only_a, only_b, differ = manifest.diff(ma, mb)
    same = len(set(ma) & set(mb)) - len(differ)
    status = EXIT_SAME if not (only_a or only_b or differ) else EXIT_DIFFERENT
    if args.json:
        _out(json.dumps({'a': args.a, 'b': args.b, 'status': status,
                         'identical': same, 'differ': differ,
                         'only_a': only_a, 'only_b': only_b},
                        indent=2) + '\n')
        return status
    for path in differ:
        _out('differ   %s\n' % path)
    for path in only_a:
        _out('only-A   %s\n' % path)
    for path in only_b:
        _out('only-B   %s\n' % path)
    _out('%d entries in A, %d in B: %d identical, %d differ, %d only in A, '
         '%d only in B\n' % (len(ma), len(mb), same, len(differ), len(only_a),
                             len(only_b)))
    return status


_TOOLS = {
    'c-tokens': c_tokens,
    'compare-files': compare_files,
    'compare-trees': compare_trees,
    'kconfig-strip': kconfig_strip,
    'yaml-strip': yaml_strip,
    'obj-manifest': obj_manifest,
    'compare-objs': compare_objs,
}


def main(tool, argv=None):
    """Run *tool* with *argv* (default: sys.argv[1:]); return the status."""
    if argv is None:
        argv = sys.argv[1:]
    try:
        return _TOOLS[tool](argv, tool)
    except BrokenPipeError:
        # The reader went away (| head).  Point stdout at /dev/null so that
        # the flush at exit does not fail again, and exit 1 as Python would.
        devnull = os.open(os.devnull, os.O_WRONLY)
        os.dup2(devnull, sys.stdout.fileno())
        return 1
    except KeyboardInterrupt:
        return 130
