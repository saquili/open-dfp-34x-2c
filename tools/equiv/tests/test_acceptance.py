# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The acceptance criteria of the equivalence tools, for every language.

For one sample file per language:

* a comment-only edit compares identical (class ``comment``: the token
  streams are equal, the bytes are not);
* a whitespace edit compares identical;
* an identifier rename is a token difference (class ``code``);
* a string-literal change is reported as literal-only, with the literal
  listed -- in C, device-tree source and Kconfig, the languages with a
  literal class.  In assembly, makefiles and YAML schemas every string is
  code, so there the same edit must come out as ``code``.

Each case is checked through the library and through ``compare-files``,
whose exit status is 0 for identical, 1 for code and 3 for literal-only.
"""

import os
import unittest

from equivtest import TempDirTest, classify, replace, run_tool


class Acceptance:
    """Mixin: the four criteria over self.path and self.sample."""

    path = None
    sample = None
    literal_class = 'literal'

    def comment_edit(self, s):
        raise NotImplementedError

    def whitespace_edit(self, s):
        raise NotImplementedError

    def rename_edit(self, s):
        raise NotImplementedError

    def string_edit(self, s):
        raise NotImplementedError

    def check(self, edit, want):
        b = edit(self.sample)
        self.assertNotEqual(self.sample, b, 'the edit changed nothing')
        res = classify(self.path, self.sample, b)
        self.assertEqual(res.cls, want, res.as_dict())
        return res

    def test_comment_edit_is_identical(self):
        self.check(self.comment_edit, 'comment')

    def test_whitespace_edit_is_identical(self):
        self.check(self.whitespace_edit, 'comment')

    def test_identifier_rename_is_a_code_difference(self):
        res = self.check(self.rename_edit, 'code')
        self.assertTrue(res.detail.hunks, 'a code difference lists hunks')

    def test_string_change(self):
        res = self.check(self.string_edit, self.literal_class)
        if self.literal_class == 'literal':
            self.assertEqual(len(res.detail.literals), 1)
            self.assertIn(self.literal_old, res.detail.literals[0].a_text)
            self.assertIn(self.literal_new, res.detail.literals[0].b_text)

    def test_compare_files_exit_status(self):
        want = {'comment': 0, 'code': 1, 'literal': 3}
        edits = ((self.comment_edit, 'comment'),
                 (self.whitespace_edit, 'comment'),
                 (self.rename_edit, 'code'),
                 (self.string_edit, self.literal_class))
        name = os.path.basename(self.path)
        a = self.write('a/' + self.path, self.sample)
        for k, (edit, cls) in enumerate(edits):
            b = self.write('b%d/%s' % (k, self.path), edit(self.sample))
            status, out, err = run_tool('compare-files', a, b)
            self.assertEqual(status, want[cls], (name, cls, out, err))
            self.assertTrue(out.startswith(cls + ':'), out)


class CTest(Acceptance, TempDirTest):
    path = 'drivers/misc/frob.c'
    sample = r'''// A test driver.
/*
 * Frobnicator driver.
 */
#include <linux/io.h>
#include "frob.h"

#define FROB_CTRL	0x10	/* control register */
#define FROB_CTRL_EN	BIT(0)

static int frob_enable(struct frob *fp, bool on)
{
	u32 val = readl(fp->base + FROB_CTRL);

	/* Set or clear the enable bit. */
	if (on)
		val |= FROB_CTRL_EN;
	else
		val &= ~FROB_CTRL_EN;
	writel(val, fp->base + FROB_CTRL);
	dev_dbg(fp->dev, "frobnicator %s\n", on ? "on" : "off");
	return 0;
}
'''
    literal_old = '"frobnicator %s\\n"'
    literal_new = '"frob is %s\\n"'

    def comment_edit(self, s):
        s = replace(s, ' * Frobnicator driver.\n',
                    ' * Driver for the frobnicator,\n * rewritten.\n')
        s = replace(s, '/* control register */', '/* CTRL */')
        s = replace(s, '/* Set or clear the enable bit. */',
                    '// Update the enable bit.')
        return replace(s, '// A test driver.', '/* Another header. */')

    def whitespace_edit(self, s):
        s = replace(s, '#define FROB_CTRL\t0x10', '#  define   FROB_CTRL 0x10')
        s = replace(s, 'static int frob_enable(',
                    'static int\nfrob_enable(')
        s = replace(s, '\tif (on)\n\t\tval |=', '    if(on) val|=')
        s = replace(s, '\n\n\t/* Set', '\n\n\n\t/* Set')
        return replace(s, 'return 0;', 'return 0 ;   ')

    def rename_edit(self, s):
        return replace(s, 'FROB_CTRL_EN', 'FROB_CTRL_ENABLE', 3)

    def string_edit(self, s):
        return replace(s, self.literal_old, self.literal_new)

    def test_split_string_keeps_its_value(self):
        b = replace(self.sample, '"frobnicator %s\\n"',
                    '"frobni" "cator %s\\n"')
        res = classify(self.path, self.sample, b)
        self.assertEqual(res.cls, 'literal')
        self.assertIs(res.detail.literals[0].same_value, True)


class AsmTest(Acceptance, TempDirTest):
    path = 'arch/mips/kernel/frob.S'
    sample = '''/*
 * Low-level frobnication.
 */
#include <asm/asm.h>
#include <asm/regdef.h>

	.section ".text.frob", "ax"
	.set	noreorder
LEAF(frob_flush)		# flush the frobnicator
	li	t0, 0x10	# control offset
	sw	zero, 0(a0)
	jr	ra
	 nop			/* delay slot */
	END(frob_flush)
'''
    literal_class = 'code'

    def comment_edit(self, s):
        s = replace(s, '# flush the frobnicator', '# drain it')
        s = replace(s, '/* delay slot */', '# branch delay')
        return replace(s, ' * Low-level frobnication.\n',
                       ' * Low-level code,\n * reworded.\n')

    def whitespace_edit(self, s):
        s = replace(s, '\tli\tt0, 0x10', '        li    t0,0x10')
        return replace(s, '\t.set\tnoreorder\n', '\n  .set noreorder\n\n')

    def rename_edit(self, s):
        return replace(s, 'frob_flush', 'frob_drain', 2)

    def string_edit(self, s):
        # A section name is code: the object changes where the text goes.
        return replace(s, '".text.frob"', '".text.frob2"')


class DtsTest(Acceptance, TempDirTest):
    path = 'arch/mips/boot/dts/acme/frob-board.dts'
    sample = '''// A test board.
/dts-v1/;

#include <dt-bindings/interrupt-controller/irq.h>

/ {
	compatible = "acme,frob-board";	/* the board */
	#address-cells = <1>;
	#size-cells = <1>;

	frob: frob@18000000 {
		compatible = "acme,frob";
		reg = <0x18000000 0x100>;
		interrupts = <12 IRQ_TYPE_LEVEL_HIGH>;
		status = "okay";
	};
};
'''
    literal_old = '"okay"'
    literal_new = '"disabled"'

    def comment_edit(self, s):
        s = replace(s, '/* the board */', '// board')
        return replace(s, '// A test board.', '/*\n * Board.\n */')

    def whitespace_edit(self, s):
        s = replace(s, '\t#address-cells = <1>;\n\t#size-cells = <1>;',
                    '    #address-cells=<1>; #size-cells = < 1 >;')
        return replace(s, 'reg = <0x18000000 0x100>;',
                       'reg = <0x18000000\n\t\t       0x100>;')

    def rename_edit(self, s):
        return replace(s, 'frob: frob@18000000', 'frob0: frob@18000000')

    def string_edit(self, s):
        return replace(s, self.literal_old, self.literal_new)


class KconfigTest(Acceptance, TempDirTest):
    path = 'drivers/misc/Kconfig'
    sample = '''# Frobnicator options.
config FROB
	tristate "Frobnicator support"
	depends on OF && HAS_IOMEM
	default n
	help
	  Say Y here to support the frobnicator.

	  To compile this driver as a module, choose M here.

config FROB_DEBUG
	bool "Frobnicator debugging"
	depends on FROB # only with the driver
'''
    literal_old = '"Frobnicator support"'
    literal_new = '"Frobnicator driver"'

    def comment_edit(self, s):
        s = replace(s, '# only with the driver', '# needs FROB')
        s = replace(s, '# Frobnicator options.', '#\n# Options.\n#')
        # Help text is prose, like a comment.
        return replace(s, '\t  Say Y here to support the frobnicator.\n',
                       '\t  The frobnicator frobs.  Enable it\n'
                       '\t  on boards that have one.\n')

    def whitespace_edit(self, s):
        s = replace(s, '\tdepends on OF && HAS_IOMEM',
                    '        depends on   OF&&HAS_IOMEM')
        return replace(s, '\n\nconfig FROB_DEBUG', '\n\n\nconfig FROB_DEBUG')

    def rename_edit(self, s):
        return replace(s, 'depends on OF &&', 'depends on OF_ADDRESS &&')

    def string_edit(self, s):
        return replace(s, self.literal_old, self.literal_new)


class MakefileTest(Acceptance, TempDirTest):
    path = 'drivers/misc/Makefile'
    sample = '''#
# Makefile for the frobnicator.
#

obj-$(CONFIG_FROB) += frob.o	# the driver
frob-y := frob-core.o \\
	  frob-irq.o
ccflags-$(CONFIG_FROB_DEBUG) += -DDEBUG

quiet_cmd_frob = FROB    $@
      cmd_frob = echo "frob table" > $@
'''
    literal_class = 'code'

    def comment_edit(self, s):
        s = replace(s, '# the driver', '# core')
        return replace(s, '# Makefile for the frobnicator.\n',
                       '# Build rules,\n# reworded.\n')

    def whitespace_edit(self, s):
        s = replace(s, 'frob-y := frob-core.o \\\n\t  frob-irq.o',
                    'frob-y   :=  frob-core.o frob-irq.o')
        return replace(s, '+= -DDEBUG', '+=   -DDEBUG  ')

    def rename_edit(self, s):
        return replace(s, 'frob-irq.o', 'frob-intr.o')

    def string_edit(self, s):
        # A makefile hands its strings to the shell: code.
        return replace(s, '"frob table"', '"frob list"')


class YamlTest(Acceptance, TempDirTest):
    path = 'Documentation/devicetree/bindings/misc/acme,frob.yaml'
    sample = '''# A test schema.
%YAML 1.2
---
$id: http://devicetree.org/schemas/misc/acme,frob.yaml#
$schema: http://devicetree.org/meta-schemas/core.yaml#

title: Frobnicator

maintainers:
  - A. Developer <dev@example.org>

description: |
  The frobnicator frobs.

  It sits on the local bus.

properties:
  compatible:
    const: acme,frob

  reg:
    maxItems: 1
    description: The register window.

required:
  - compatible
  - reg

additionalProperties: false

examples:
  - |
    frob@18000000 {
        compatible = "acme,frob";
        reg = <0x18000000 0x100>;
    };
'''
    literal_class = 'code'

    def comment_edit(self, s):
        s = replace(s, '# A test schema.', '# Another header.\n# Two lines.')
        # title and description are prose, like comments.
        s = replace(s, 'title: Frobnicator', 'title: Acme frobnicator')
        s = replace(s, '  The frobnicator frobs.\n',
                    '  A frobnicator,\n  described again.\n')
        return replace(s, 'description: The register window.',
                       'description:\n      One register window.')

    def whitespace_edit(self, s):
        s = replace(s, '\nproperties:\n', '\n\n\nproperties:   \n')
        return replace(s, '  - reg\n', '  - reg   \n\n')

    def rename_edit(self, s):
        return replace(s, 'const: acme,frob', 'const: acme,frob2')

    def string_edit(self, s):
        # The example is device-tree source kept verbatim: code.
        return replace(s, 'compatible = "acme,frob";',
                       'compatible = "acme,frob2";')


if __name__ == '__main__':
    unittest.main()
