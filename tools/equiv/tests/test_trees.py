# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Classifying the difference between two files, two directories and two
git revisions."""

import os
import subprocess
import unittest

from equivtest import TempDirTest, classify, needs_git

from equivlib import compare, lang, trees

BASE = '#include "a.h"\nint f(void)\n{\n\treturn g("text", 1);\n}\n'


class ClassTest(unittest.TestCase):

    def test_identical_bytes(self):
        self.assertEqual(classify('x.c', BASE, BASE).cls, 'identical')

    def test_literal_runs_are_compared_as_a_whole(self):
        b = BASE.replace('"text"', '"te" "xt"')
        res = classify('x.c', BASE, b)
        self.assertEqual(res.cls, 'literal')
        change = res.detail.literals[0]
        self.assertEqual((change.a_text, change.b_text, change.same_value),
                         ('"text"', '"te" "xt"', True))
        self.assertEqual((change.a_line, change.b_line), (4, 4))

    def test_a_literal_that_moves_is_code(self):
        b = BASE.replace('g("text", 1)', 'g(1, "text")')
        self.assertEqual(classify('x.c', BASE, b).cls, 'code')

    def test_code_and_literal_together_is_code(self):
        b = BASE.replace('"text", 1', '"other", 2')
        res = classify('x.c', BASE, b)
        self.assertEqual(res.cls, 'code')
        hunk = res.detail.hunks[0]
        self.assertEqual(hunk.a_line, 4)

    def test_no_token_model(self):
        self.assertEqual(classify('README', 'a', 'b').cls, 'other')
        res = trees.classify_bytes('x.bin', b'\0a', b'\0b')
        self.assertEqual((res.cls, res.note), ('other', 'binary'))

    def test_input_that_cannot_be_tokenised(self):
        res = classify('x.c', 'int x;', 'int x; /* open')
        self.assertEqual(res.cls, 'error')
        self.assertIn('unterminated comment', res.note)

    def test_invalid_utf8_is_compared_byte_exact(self):
        a = b'char *s = "\xff";\n'
        b = b'char *s = "\xfe";\n'
        res = trees.classify_bytes('x.c', a, b)
        self.assertEqual(res.cls, 'literal')

    def test_compare_on_token_lists(self):
        ta = lang.tokenize('a = "x";', 'c')
        tb = lang.tokenize('a = "y";', 'c')
        self.assertEqual(compare.compare(ta, ta, 'c').cls, 'comment')
        self.assertEqual(compare.compare(ta, tb, 'c').cls, 'literal')


class DirTest(TempDirTest):

    def make_trees(self):
        files = {
            'same.c': (BASE, BASE),
            'comment.c': (BASE, '/* new */\n' + BASE),
            'literal.c': (BASE, BASE.replace('"text"', '"words"')),
            'code.c': (BASE, BASE.replace('g(', 'h(')),
            'README': ('a\n', 'b\n'),
            'broken.c': (BASE, BASE + '/*'),
        }
        for name, (a, b) in files.items():
            self.write('a/' + name, a)
            self.write('b/' + name, b)
        self.write('a/gone.c', BASE)
        self.write('b/new.c', BASE)
        self.write('a/tool.sh', 'x\n', 0o644)
        self.write('b/tool.sh', 'x\n', 0o755)
        self.write('a/.git/HEAD', 'ignored')
        os.symlink('same.c', os.path.join(self.tmp, 'a', 'link.c'))
        os.symlink('code.c', os.path.join(self.tmp, 'b', 'link.c'))

    def test_every_class(self):
        self.make_trees()
        got = dict((r.path, r.cls) for r in trees.compare_dirs(
            os.path.join(self.tmp, 'a'), os.path.join(self.tmp, 'b')))
        self.assertEqual(got, {
            'same.c': 'identical', 'comment.c': 'comment',
            'literal.c': 'literal', 'code.c': 'code', 'README': 'other',
            'broken.c': 'error', 'gone.c': 'removed', 'new.c': 'added',
            'tool.sh': 'code', 'link.c': 'code',
        })


def git(repo, *args):
    env = dict(os.environ, GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@t',
               GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@t',
               GIT_CONFIG_NOSYSTEM='1', HOME=repo)
    return subprocess.run(('git', '-C', repo) + args, check=True, env=env,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE).stdout.decode()


@needs_git
class GitTest(TempDirTest):

    def test_revisions_and_subtrees(self):
        repo = self.tmp
        git(repo, 'init', '-q')
        self.write('linux/a.c', BASE)
        self.write('linux/b.c', BASE)
        self.write('linux/c.c', BASE)
        self.write('other.c', BASE)
        git(repo, 'add', '-A')
        git(repo, 'commit', '-q', '-m', 'one')
        self.write('linux/a.c', '// c\n' + BASE)
        self.write('linux/b.c', BASE.replace('"text"', '"t"'))
        self.write('linux/d.c', BASE)
        os.remove(os.path.join(repo, 'linux/c.c'))
        self.write('other.c', BASE.replace('g(', 'h('))
        git(repo, 'add', '-A')
        git(repo, 'commit', '-q', '-m', 'two')
        results = list(trees.compare_git(repo, 'HEAD~1:linux', 'HEAD:linux'))
        counts = results.pop()
        self.assertEqual(counts, ('counts', 3, 3))
        self.assertEqual(dict((r.path, r.cls) for r in results), {
            'a.c': 'comment', 'b.c': 'literal', 'c.c': 'removed',
            'd.c': 'added'})
        whole = dict((r.path, r.cls) for r in
                     trees.compare_git(repo, 'HEAD~1', 'HEAD')
                     if not isinstance(r, tuple))
        self.assertEqual(whole['other.c'], 'code')


if __name__ == '__main__':
    unittest.main()
