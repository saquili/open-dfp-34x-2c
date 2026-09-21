# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Kconfig, configuration fragments, makefiles, YAML schemas, and how a
file's language is chosen."""

import unittest

from equivtest import classify, tokens

from equivlib import kconfig, lang, yaml
from equivlib.tok import CSTR, EOL, LINE, STR, WORD

KCONFIG = 'drivers/misc/Kconfig'
FRAGMENT = 'kernel/config/frob.config'
MAKEFILE = 'drivers/misc/Makefile'
SCHEMA = 'Documentation/devicetree/bindings/misc/acme,frob.yaml'


class KconfigTest(unittest.TestCase):

    def test_help_ends_at_a_line_indented_less(self):
        a = ('config A\n\tbool "a"\n\thelp\n\t  Text.\n\n\t  More.\n'
             '\tdepends on B\n')
        b = a.replace('depends on B', 'depends on C')
        self.assertEqual(classify(KCONFIG, a, b).cls, 'code')
        self.assertIn((WORD, 'B'), tokens(a, KCONFIG))
        self.assertNotIn((WORD, 'Text.'), tokens(a, KCONFIG))

    def test_help_ends_at_column_zero_and_at_the_end(self):
        a = 'config A\n\tbool\n\thelp\n\t  x\nconfig B\n\tbool\n\thelp\n  y'
        self.assertEqual(kconfig.strip(a),
                         'config A\nbool\nconfig B\nbool\n')

    def test_help_text_and_comments_are_prose(self):
        a = 'config A # c\n\tbool "a"\n\thelp\n\t  One.\n'
        b = '# new\nconfig A\n\tbool "a"\n\t---help---\n\t  Two.\n\t  Three.\n'
        self.assertEqual(classify(KCONFIG, a, b).cls, 'comment')

    def test_prompts_are_literals_other_strings_code(self):
        toks = tokens('config A\n\tstring "Name"\n\tdefault "x#y"\n'
                      'source "drivers/x/Kconfig"\n', KCONFIG)
        self.assertEqual([t for t in toks if t[0] in (STR, CSTR)],
                         [(STR, '"Name"'), (CSTR, '"x#y"'),
                          (CSTR, '"drivers/x/Kconfig"')])

    def test_macro_calls_keep_their_blanks(self):
        a = 'config A\n\tdef_bool $(cc-option,-mfoo)\n'
        b = 'config A\n\tdef_bool $(cc-option, -mfoo)\n'
        self.assertEqual(classify(KCONFIG, a, b).cls, 'code')

    def test_statements_end_with_the_line(self):
        self.assertEqual(tokens('menu "M"\nendmenu\n', KCONFIG),
                         [(WORD, 'menu'), (STR, '"M"'), (EOL, ''),
                          (WORD, 'endmenu'), (EOL, '')])


class FragmentTest(unittest.TestCase):

    def test_not_set_lines_are_code(self):
        a = 'CONFIG_A=y\n# CONFIG_B is not set\n'
        self.assertEqual(classify(FRAGMENT, a, 'CONFIG_A=y\n').cls, 'code')
        self.assertEqual(
            classify(FRAGMENT, a, '# note\nCONFIG_A=y\n  #  CONFIG_B  is not '
                     'set\n').cls, 'comment')

    def test_values_are_code(self):
        self.assertEqual(classify(FRAGMENT, 'CONFIG_A=y\n',
                                  'CONFIG_A=m\n').cls, 'code')


class MakefileTest(unittest.TestCase):

    def words(self, text):
        return [t for k, t in tokens(text, MAKEFILE) if k == WORD]

    def test_escaped_hash_and_hash_in_a_reference_are_no_comments(self):
        self.assertEqual(self.words('X := a\\#b # c\n'),
                         ['X', ':=', 'a\\#b'])
        self.assertEqual(self.words("X := $(shell echo '#') # c\n"),
                         ['X', ':=', '$(shell', 'echo', "'#')"])

    def test_recipes_keep_quoted_text_and_drop_shell_comments(self):
        toks = tokens('t:\n\t@echo "a # b"  # c\n', MAKEFILE)
        self.assertEqual(toks[-4:], [('recipe', ''), (WORD, '@echo'),
                                     (WORD, '"a # b"'), (EOL, '')])

    def test_a_recipe_comment_with_a_reference_stays(self):
        # make expands $(x) even there, so it is not prose.
        self.assertIn((WORD, '$(shell'),
                      tokens('t:\n\t# $(shell touch f)\n', MAKEFILE))

    def test_recipe_tab_is_syntax(self):
        a = 't:\n\techo x\n'
        self.assertEqual(classify(MAKEFILE, a, 't:\n    echo x\n').cls,
                         'code')

    def test_define_bodies_are_kept_verbatim(self):
        a = 'define frob\n  # kept\n  echo x\nendef\n'
        self.assertIn((LINE, '  # kept'), tokens(a, MAKEFILE))
        self.assertEqual(classify(MAKEFILE, a, a.replace('kept', 'k')).cls,
                         'code')

    def test_continuations_are_one_blank(self):
        self.assertEqual(tokens('X := a \\\n\tb \\\n  c\n', MAKEFILE),
                         tokens('X := a b c\n', MAKEFILE))


class YamlTest(unittest.TestCase):

    def test_description_forms_are_dropped(self):
        a = ('properties:\n  a:\n    description: |\n      Block\n\n'
             '      text.\n    type: object\n  b:\n    description: >-\n'
             '      Folded.\n    const: 1\n  c:\n    "description": x\n'
             '      continued\n    const: 2\n')
        self.assertEqual(yaml.strip(a),
                         'properties:\n  a:\n    type: object\n  b:\n'
                         '    const: 1\n  c:\n    const: 2\n')

    def test_sequence_items_of_prose_keep_their_markers(self):
        a = 'reg:\n  items:\n    - description: first\n    - description: 2\n'
        b = 'reg:\n  items:\n    - description: the only one\n'
        self.assertEqual(classify(SCHEMA, a, b).cls, 'code')
        self.assertEqual(
            classify(SCHEMA, a, a.replace('first', 'the first')).cls,
            'comment')

    def test_comments_but_not_quoted_hashes(self):
        a = 'pattern: "^a#b$"  # c\n'
        self.assertEqual(yaml.strip(a), 'pattern: "^a#b$"\n')

    def test_example_blocks_are_kept_verbatim(self):
        a = 'examples:\n  - |\n    // a comment in the example\n    n {};\n'
        self.assertEqual(yaml.strip(a), a)
        self.assertEqual(classify(SCHEMA, a, a.replace('a comment',
                                                        'text')).cls, 'code')

    def test_indentation_is_syntax(self):
        a = 'a:\n  b: 1\n'
        self.assertEqual(classify(SCHEMA, a, 'a:\nb: 1\n').cls, 'code')


class DetectTest(unittest.TestCase):

    def test_names(self):
        cases = {
            'drivers/x.c': 'c', 'include/x.h': 'c',
            'arch/mips/kernel/vmlinux.lds.S': 'c', 'arch/mips/x.S': 'asm',
            'arch/mips/boot/dts/x.dts': 'dts', 'x.dtsi': 'dts',
            'x.dtso': 'dts', 'Kconfig': 'kconfig', 'Kconfig.debug': 'kconfig',
            'Makefile': 'make', 'Kbuild': 'make', 'arch/mips/x/Platform':
            'make', 'scripts/Makefile.lib': 'make', 'x.mk': 'make',
            'Documentation/devicetree/bindings/x.yaml': 'yaml',
            'arch/mips/configs/x_defconfig': 'defconfig',
            'kernel/configs/tiny.config': 'defconfig',
            'README': None, 'scripts/x.py': None, 'MAINTAINERS': None,
        }
        for path, want in cases.items():
            self.assertEqual(lang.detect(path), want, path)

    def test_hash_comments_follow_the_architecture(self):
        self.assertTrue(lang.asm_hash_comments('arch/mips/kernel/x.S'))
        self.assertFalse(lang.asm_hash_comments('arch/arm/kernel/x.S'))
        self.assertTrue(lang.asm_hash_comments('lib/x.S'))
        self.assertFalse(lang.asm_hash_comments('lib/x.S', arch='arm'))


if __name__ == '__main__':
    unittest.main()
