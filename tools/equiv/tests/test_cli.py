# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The command-line tools: output, exit status, argument checks.

Exit status of the comparing tools: 0 no difference beyond comments,
whitespace and prose; 3 string literals only; 1 code; 2 error.
"""

import json
import os
import unittest

from equivtest import TempDirTest, run_tool

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


if __name__ == '__main__':
    unittest.main()
