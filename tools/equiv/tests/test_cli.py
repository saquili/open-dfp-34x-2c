# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The command-line tools: output, exit status, argument checks.

Exit status of the comparing tools: 0 no difference beyond comments,
whitespace and prose; 3 string literals only; 1 code; 2 error.
"""

import json
import os
import subprocess
import unittest

from equivtest import TempDirTest, case_sensitive, needs_git, run_tool

C_A = 'int f(void)\n{\n\treturn g("text");\n}\n'
C_B_COMMENT = '/* f */\nint f(void) { return g("text"); }\n'
C_B_LITERAL = C_A.replace('"text"', '"other"')
C_B_CODE = C_A.replace('g(', 'h(')


class TokensTest(TempDirTest):

    def test_c_tokens(self):
        path = self.write('x.c', 'int x; /* c */\n#define A "a"\n')
        status, out, _ = run_tool('c-tokens', path)
        self.assertEqual(status, 0)
        self.assertEqual(out, 'id\tint\nid\tx\nop\t;\npp\tdefine\n'
                              'macro\tA\nstr\t"a"\neol\t\n')
        status, out, _ = run_tool('c-tokens', '--lines', path)
        self.assertTrue(out.startswith('1\tid\tint\n'), out)

    def test_c_tokens_language(self):
        path = self.write('notes.txt', 'int x;')
        status, _, err = run_tool('c-tokens', path)
        self.assertEqual(status, 2)
        self.assertIn('unknown language', err)
        status, out, _ = run_tool('c-tokens', '--lang', 'c', path)
        self.assertEqual((status, out.splitlines()[0]), (0, 'id\tint'))

    def test_strip_tools(self):
        k = self.write('Kconfig', 'config A\n\tbool "a" # c\n\thelp\n\t  x\n')
        self.assertEqual(run_tool('kconfig-strip', k)[:2],
                         (0, 'config A\nbool "a"\n'))
        y = self.write('s.yaml', 'title: T\ntype: object  # c\n')
        self.assertEqual(run_tool('yaml-strip', y)[:2], (0, 'type: object\n'))


class CompareFilesTest(TempDirTest):

    def compare(self, a, b, *opts, name='x.c'):
        pa = self.write('a/' + name, a)
        pb = self.write('b/' + name, b)
        return run_tool('compare-files', *(list(opts) + [pa, pb]))

    def test_status(self):
        self.assertEqual(self.compare(C_A, C_A)[0], 0)
        self.assertEqual(self.compare(C_A, C_B_COMMENT)[0], 0)
        self.assertEqual(self.compare(C_A, C_B_LITERAL)[0], 3)
        self.assertEqual(self.compare(C_A, C_B_CODE)[0], 1)
        self.assertEqual(self.compare(C_A, C_A + '/*')[0], 2)

    def test_literal_report_lists_the_literal(self):
        status, out, _ = self.compare(C_A, C_B_LITERAL)
        self.assertIn('"text"', out)
        self.assertIn('"other"', out)

    def test_json_and_quiet(self):
        status, out, _ = self.compare(C_A, C_B_LITERAL, '--json')
        d = json.loads(out)
        self.assertEqual((status, d['class']), (3, 'literal'))
        self.assertEqual(d['literals'][0]['b_text'], '"other"')
        self.assertEqual(self.compare(C_A, C_B_CODE, '-q')[1], '')

    def test_languages_must_agree(self):
        pa = self.write('a/x.c', C_A)
        pb = self.write('b/x.S', C_A)
        self.assertEqual(run_tool('compare-files', pa, pb)[0], 2)
        self.assertEqual(run_tool('compare-files', '--lang', 'c', pa, pb)[0],
                         0)


class CompareTreesTest(TempDirTest):

    def trees(self, files):
        for name, (a, b) in files.items():
            if a is not None:
                self.write('a/' + name, a)
            if b is not None:
                self.write('b/' + name, b)
        return os.path.join(self.tmp, 'a'), os.path.join(self.tmp, 'b')

    def test_status_follows_the_worst_class(self):
        cases = (
            ({'x.c': (C_A, C_A)}, 0),
            ({'x.c': (C_A, C_B_COMMENT)}, 0),
            ({'x.c': (C_A, C_B_COMMENT), 'y.c': (C_A, C_B_LITERAL)}, 3),
            ({'x.c': (C_A, C_B_LITERAL), 'y.c': (C_A, C_B_CODE)}, 1),
            ({'x.c': (C_A, C_A), 'y.c': (None, C_A)}, 1),
            ({'x.c': (C_A, C_A), 'README': ('a', 'b')}, 1),
            ({'x.c': (C_A, C_A + '/*')}, 2),
        )
        for k, (files, want) in enumerate(cases):
            a, b = self.trees(dict(('%d/%s' % (k, n), v)
                                   for n, v in files.items()))
            status, out, err = run_tool('compare-trees',
                                        os.path.join(a, str(k)),
                                        os.path.join(b, str(k)))
            self.assertEqual(status, want, (files.keys(), out, err))

    def test_report(self):
        a, b = self.trees({'x.c': (C_A, C_B_LITERAL), 'y.c': (C_A, C_A),
                           'z.c': (C_A, None)})
        status, out, _ = run_tool('compare-trees', a, b)
        self.assertIn('literal   x.c\n', out)
        self.assertIn('removed   z.c\n', out)
        self.assertNotIn('y.c', out)
        self.assertTrue(out.endswith(
            '3 files: 1 identical, 0 comment, 1 literal, 0 code, 0 added, '
            '1 removed, 0 other, 0 error\n'), out)
        status, out, _ = run_tool('compare-trees', '--json', a, b)
        d = json.loads(out)
        self.assertEqual((d['status'], d['counts']['literal']), (1, 1))

    def test_not_a_directory(self):
        self.assertEqual(run_tool('compare-trees', self.tmp,
                                  os.path.join(self.tmp, 'missing'))[0], 2)


class ManifestToolsTest(TempDirTest):

    def build(self, name, body):
        self.write(name + '/.config', 'CONFIG_A=y\n')
        self.write(name + '/vmlinux', b'\x7fELF\x01\x02\x01' + bytes(9)
                   + b'\x00\x01\x00\x08' + body)
        return os.path.join(self.tmp, name)

    def test_record_and_compare(self):
        a = self.build('a', b'one')
        b = self.build('b', b'one')
        c = self.build('c', b'two')
        ma = os.path.join(self.tmp, 'a.sha256')
        self.assertEqual(run_tool('obj-manifest', '-o', ma, a)[0], 0)
        status, out, _ = run_tool('obj-manifest', b)
        self.assertEqual(status, 0)
        with open(ma) as f:
            self.assertEqual(f.read(), out)
        status, out, _ = run_tool('compare-objs', ma, b)
        self.assertEqual(status, 0)
        self.assertIn('2 entries in A, 2 in B: 2 identical, 0 differ', out)
        status, out, _ = run_tool('compare-objs', ma, c)
        self.assertEqual(status, 1)
        self.assertIn('differ   vmlinux\n', out)
        status, out, _ = run_tool('compare-objs', '--json', ma, c)
        self.assertEqual(json.loads(out)['differ'], ['vmlinux'])

    def test_errors(self):
        missing = os.path.join(self.tmp, 'missing')
        self.assertEqual(run_tool('obj-manifest', missing)[0], 2)
        bad = self.write('bad.sha256', 'not a manifest\n')
        self.assertEqual(run_tool('compare-objs', bad, bad)[0], 2)


class ShellToolsTest(TempDirTest):
    """The shell tools refuse bad input before they start any work."""

    def fake_tree(self):
        self.write('linux/Makefile', 'all:\n')
        self.write('linux/Kconfig', 'config A\n\tbool\n')
        return os.path.join(self.tmp, 'linux')

    def test_help(self):
        for name in ('kbuild-repro', 'in-toolchain', 'repro-check'):
            status, _, err = run_tool(name, '--help')
            self.assertEqual(status, 2, name)
            self.assertTrue(err.startswith('Usage: ' + name), err)

    def test_kbuild_repro_refusals(self):
        tree = self.fake_tree()
        out = os.path.join(self.tmp, 'out')
        cfg = self.write('x.config', 'CONFIG_A=y\n')
        cases = (
            (['--config', cfg, '--defconfig', 'tinyconfig', tree, out],
             'exclude each other'),
            (['--fragment', cfg, tree, out], '--fragment needs --defconfig'),
            ([self.tmp, out], 'does not look like a kernel tree'),
            (['--build-version', 'x', tree, out],
             '--build-version must be a number'),
            (['-j', '0', tree, out], '--jobs must be a positive number'),
            (['--source-date-epoch', '-1', tree, out],
             '--source-date-epoch must be a number'),
            (['--bogus', tree, out], 'unknown option'),
        )
        for argv, message in cases:
            status, _, err = run_tool('kbuild-repro', '--dry-run', *argv)
            self.assertEqual(status, 1, (argv, err))
            self.assertIn(message, err)

    def test_kbuild_repro_dry_run(self):
        tree = self.fake_tree()
        out = os.path.join(self.tmp, 'out')
        status, stdout, err = run_tool('kbuild-repro', '--dry-run', tree, out)
        if not case_sensitive(self.tmp):
            # Refused, and rightly: the kernel holds paths that differ only
            # in case.
            self.assertEqual(status, 1)
            self.assertIn('case-insensitive', err)
            return
        self.assertEqual(status, 0, err)
        for line in ('KBUILD_BUILD_TIMESTAMP=1970-01-01 00:00:00 UTC',
                     'KBUILD_BUILD_USER=build', 'KBUILD_BUILD_HOST=build',
                     'KBUILD_BUILD_VERSION=1', 'SOURCE_DATE_EPOCH=0',
                     'LC_ALL=C', 'TZ=UTC'):
            self.assertIn('  %s\n' % line, stdout)
        maps = '-ffile-prefix-map=%s=.' % tree
        for var in ('KCFLAGS', 'KCPPFLAGS', 'HOSTCFLAGS', 'HOSTCXXFLAGS',
                    'USERCFLAGS'):
            self.assertIn(' %s=' % var, stdout)
        self.assertIn(maps, stdout)
        self.assertIn(' LOCALVERSION= ', stdout)
        self.assertTrue(stdout.rstrip().endswith(' all'), stdout)

    def test_in_toolchain_refusals(self):
        env = dict(os.environ)
        env.pop('EQUIV_IMAGE', None)
        status, _, err = run_tool('in-toolchain', 'true', env=env)
        self.assertEqual(status, 1)
        self.assertIn('no toolchain image', err)
        status, _, err = run_tool('in-toolchain', '--image', 'x', '--mount',
                                  os.path.join(self.tmp, 'missing'), 'true')
        self.assertEqual(status, 1)
        self.assertIn('not a directory', err)

    @needs_git
    def test_repro_check_refusals(self):
        env = dict(os.environ)
        env.pop('EQUIV_IMAGE', None)
        repo = os.path.join(self.tmp, 'repo')
        os.makedirs(repo)
        subprocess.run(['git', '-C', repo, 'init', '-q'], check=True)
        cases = (
            ([], 'no toolchain image'),
            (['--image', 'x'], 'no repository'),
            (['--image', 'x', '--git', repo, '--name', '../x'], '--name'),
            (['--image', 'x', '--git', repo, '--jobs-b', '0'], '--jobs-b'),
            (['--image', 'x', '--git', repo], 'not a revision'),
            (['--image', 'x', '--git', repo, 'extra'], 'unknown argument'),
        )
        for argv, message in cases:
            status, _, err = run_tool('repro-check', *argv, env=env)
            self.assertEqual(status, 2, (argv, err))
            self.assertIn(message, err)


if __name__ == '__main__':
    unittest.main()
