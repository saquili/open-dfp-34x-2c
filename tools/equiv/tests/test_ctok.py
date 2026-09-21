# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The C, assembly and device-tree lexer.

Where a case depends on how the GNU preprocessor or assembler read the
text, the expectation is what the cross toolchain does with it (checked
with 'gcc -E' when these tests were written), not what looks plausible.
"""

import unittest

from equivtest import tokens

from equivlib import ctok
from equivlib.tok import (CHR, CSTR, EOL, FMACRO, HDR, ID, MACRO, MSG, NUM,
                          OP, OTHER, PP, STR, LexError)

C = 'x.c'
ASM = 'arch/mips/kernel/x.S'
DTS = 'arch/mips/boot/dts/x.dts'


class CommentTest(unittest.TestCase):

    def test_block_and_line_comments_are_dropped(self):
        self.assertEqual(tokens('int /* a */ x; // b\n/* c\n d */', C),
                         tokens('int x;', C))

    def test_comment_markers_inside_literals_are_text(self):
        self.assertEqual(tokens('s = "/* no */ // no";', C),
                         [(ID, 's'), (OP, '='), (STR, '"/* no */ // no"'),
                          (OP, ';')])
        self.assertEqual(tokens("c = '/';", C)[2], (CHR, "'/'"))

    def test_line_comment_continues_after_a_splice(self):
        # 'int y;' belongs to the comment: the backslash joins the lines
        # before comments are removed.
        self.assertEqual(tokens('int x; // c \\\nint y;\nint z;', C),
                         tokens('int x; int z;', C))

    def test_a_comment_is_one_space(self):
        self.assertNotEqual(tokens('a/**/b', C), tokens('ab', C))
        self.assertEqual(tokens('a/**/b', C), tokens('a b', C))

    def test_unterminated_comment_is_an_error(self):
        with self.assertRaises(LexError):
            ctok.tokenize('int x; /* open', 'c')

    def test_unterminated_quote_takes_the_rest_of_the_line(self):
        self.assertEqual(tokens("#if 0\ndon't /* x */\n#endif\n", C)[3:5],
                         [(ID, 'don'), (OTHER, "'t /* x */")])


class WhitespaceTest(unittest.TestCase):

    def test_amount_of_whitespace_is_not_kept(self):
        self.assertEqual(tokens('int\tx =\n\n  1 ;', C),
                         tokens('int x=1;', C))

    def test_punctuators_are_matched_longest_first(self):
        self.assertEqual(tokens('a+++b', C),
                         [(ID, 'a'), (OP, '++'), (OP, '+'), (ID, 'b')])
        # Removing a blank can change the tokens: that is a difference.
        self.assertNotEqual(tokens('a - -b', C), tokens('a --b', C))
        self.assertNotEqual(tokens('a + +b', C), tokens('a ++b', C))

    def test_directive_line_ends_are_kept(self):
        self.assertNotEqual(tokens('#define A 1\nint x;', C),
                            tokens('#define A 1 int x;', C))
        self.assertEqual(tokens('#define A 1\n', C)[-1], (EOL, ''))

    def test_spliced_directive_equals_one_line(self):
        self.assertEqual(tokens('#define A 1 + \\\n  2\n', C),
                         tokens('#define A 1 + 2\n', C))

    def test_function_like_macro_depends_on_the_blank(self):
        self.assertEqual(tokens('#define F(x) x\n', C)[1], (FMACRO, 'F'))
        self.assertEqual(tokens('#define F (x) x\n', C)[1], (MACRO, 'F'))
        self.assertNotEqual(tokens('#define F(x) x\n', C),
                            tokens('#define F (x) x\n', C))

    def test_directive_spacing_is_free(self):
        self.assertEqual(tokens('#  define A 1\n', C),
                         tokens('#define A 1\n', C))


class DirectiveTest(unittest.TestCase):

    def test_include_names_are_code(self):
        self.assertEqual(tokens('#include <linux/io.h>\n', C)[1],
                         (HDR, '<linux/io.h>'))
        self.assertEqual(tokens('#include "frob.h"\n', C)[1],
                         (HDR, '"frob.h"'))

    def test_error_message_is_one_literal_spelt_as_gcc_does(self):
        # gcc -E reports '#error a/**/b   c' as 'a b c'.
        self.assertEqual(tokens('#error a/**/b   c\n', C),
                         [(PP, 'error'), (MSG, 'a b c'), (EOL, '')])
        self.assertEqual(tokens('#warning x"y"z\n', C)[1], (MSG, 'x"y"z'))

    def test_other_directive_strings_are_code(self):
        self.assertEqual(tokens('#line 7 "f.c"\n', C)[2], (CSTR, '"f.c"'))

    def test_macro_body_strings_are_literals(self):
        self.assertEqual(tokens('#define NAME "frob"\n', C)[2],
                         (STR, '"frob"'))


class CodeStringTest(unittest.TestCase):

    def kinds(self, text):
        return [k for k, _ in tokens(text, C) if k in (STR, CSTR)]

    def test_asm_templates_are_code(self):
        self.assertEqual(self.kinds('asm volatile("sync" ::: "memory");'),
                         [CSTR, CSTR])
        self.assertEqual(self.kinds('__asm__ __volatile__ goto("b %l0");'),
                         [CSTR])

    def test_attribute_arguments_are_code(self):
        self.assertEqual(
            self.kinds('int v __attribute__((section(".x"))) = 1;'), [CSTR])
        self.assertEqual(self.kinds('_Pragma("GCC diagnostic push")'), [CSTR])

    def test_ordinary_strings_are_literals(self):
        self.assertEqual(self.kinds('pr_info("x"); f(asm_x, "y");'),
                         [STR, STR])

    def test_asm_group_in_a_macro_ends_with_the_line(self):
        text = '#define S() asm("sync")\nint f(void) { g("data"); }\n'
        self.assertEqual(self.kinds(text), [CSTR, STR])

    def test_prefixes(self):
        self.assertEqual(tokens('u8"a" L"b" L\'c\'', C),
                         [(STR, 'u8"a"'), (STR, 'L"b"'), (CHR, "L'c'")])


class NumberTest(unittest.TestCase):

    def test_numbers_are_kept_as_spelt(self):
        self.assertEqual(tokens('x = 0x1e+1;', C)[2], (NUM, '0x1e+1'))
        self.assertNotEqual(tokens('x = 0x10;', C), tokens('x = 16;', C))


class LineNumberTest(unittest.TestCase):

    def test_lines_count_in_the_original_file(self):
        toks = ctok.tokenize('#define A \\\n 1\n/* x\n y */ int z;\n', 'c')
        self.assertEqual([(t.text, t.line) for t in toks if t.kind == ID],
                         [('int', 4), ('z', 4)])


class AsmTest(unittest.TestCase):

    def test_hash_starts_a_comment_on_mips(self):
        self.assertEqual(tokens('\tli t0, 1 # one\n', ASM),
                         tokens('\tli t0, 1\n', ASM))

    def test_c_comment_inside_a_hash_comment_joins_the_lines(self):
        # gcc -E turns the comment into a space, so 'li t1, 2' lands on the
        # line of the '#' and the assembler reads it as a comment.
        self.assertEqual(tokens('li t0, 1 # x /* y\n z */ li t1, 2\n', ASM),
                         tokens('li t0, 1\n', ASM))

    def test_line_initial_hash_that_is_no_directive_is_a_comment(self):
        self.assertEqual(tokens('# a note\n\tnop\n', ASM),
                         tokens('\tnop\n', ASM))

    def test_directives_still_work(self):
        self.assertEqual(tokens('#define Q 5\n', ASM),
                         [(PP, 'define'), (MACRO, 'Q'), (NUM, '5'),
                          (EOL, '')])

    def test_hash_is_no_comment_for_another_architecture(self):
        arm = 'arch/arm/kernel/x.S'
        self.assertIn((OP, '#'), tokens('\tmov r0, #1\n', arm))

    def test_every_statement_line_ends_in_eol(self):
        self.assertNotEqual(tokens('\tnop\n\tnop\n', ASM),
                            tokens('\tnop nop\n', ASM))
        self.assertNotEqual(tokens('\tli t0,\n\t1\n', ASM),
                            tokens('\tli t0, 1\n', ASM))

    def test_comment_and_blank_lines_hold_no_statement(self):
        self.assertEqual(tokens('\tnop\n\n/* x */\n# y\n\tnop', ASM),
                         tokens('\tnop\n\tnop\n', ASM))

    def test_strings_are_code(self):
        self.assertEqual(tokens('.asciz "x"\n', ASM)[1], (CSTR, '"x"'))

    def test_dot_names(self):
        self.assertEqual(tokens('.Lfoo: .set noreorder\n', ASM)[:2],
                         [(ID, '.Lfoo'), (OP, ':')])


class DtsTest(unittest.TestCase):

    def test_hash_names_are_not_directives(self):
        self.assertEqual(tokens('#address-cells = <1>;\n', DTS)[0],
                         (ID, '#address-cells'))

    def test_include_is_a_directive(self):
        self.assertEqual(tokens('#include <dt-bindings/x.h>\n', DTS)[:2],
                         [(PP, 'include'), (HDR, '<dt-bindings/x.h>')])
        self.assertEqual(tokens('/include/ "board.dtsi"\n', DTS),
                         [(OP, '/include/'), (HDR, '"board.dtsi"')])

    def test_strings_are_literals(self):
        self.assertEqual(tokens('status = "okay";', DTS)[2],
                         (STR, '"okay"'))

    def test_references_and_labels(self):
        self.assertEqual(tokens('&uart0 { l: n@1 {}; };', DTS)[:5],
                         [(OP, '&'), (ID, 'uart0'), (OP, '{'), (ID, 'l'),
                          (OP, ':')])


class StringValueTest(unittest.TestCase):

    def test_concatenation_and_escapes(self):
        self.assertEqual(ctok.string_value(['"a\\x41"', '"\\102"']),
                         ((), (ord('a'), 0x41, 0o102)))
        self.assertEqual(ctok.string_value(['"ab"']),
                         ctok.string_value(['"a"', '"b"']))
        self.assertIsNone(ctok.string_value(['"\\x"']))


if __name__ == '__main__':
    unittest.main()
