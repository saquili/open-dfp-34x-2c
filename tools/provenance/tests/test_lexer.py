# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The lexer that tells code, comments, strings and prose apart."""

from __future__ import annotations

import unittest

from gates.lexer import CODE, COMMENT, STRING, TEXT, SegmentMap, language, segments


class Lexer(unittest.TestCase):

    def kinds(self, path: str, text: str) -> list[tuple[str, str]]:
        return [(text[a:b], kind) for a, b, kind in segments(path, text)]

    def test_languages(self) -> None:
        for path, lang in (("a.c", "c"), ("a.dtsi", "c"), ("Kconfig.debug", "kconfig"),
                           ("Makefile", "hash"), ("x/Makefile.lib", "hash"), ("a.py", "hash"),
                           ("check", "hash"), ("a.rst", "text"), ("README", "text")):
            self.assertEqual(language(path), lang, path)

    def test_c(self) -> None:
        self.assertEqual(self.kinds("a.c", 'x /* c */ "s" // d\n\'q\''),
                         [("x ", CODE), ("/* c */", COMMENT), (" ", CODE), ('"s"', STRING),
                          (" ", CODE), ("// d", COMMENT), ("\n", CODE), ("'q'", STRING)])

    def test_hash(self) -> None:
        self.assertEqual(self.kinds("a.py", 'x = "s"  # c\n"""doc"""'),
                         [("x = ", CODE), ('"s"', STRING), ("  ", CODE), ("# c", COMMENT),
                          ("\n", CODE), ('"""doc"""', STRING)])

    def test_kconfig_help_is_prose(self) -> None:
        text = 'config X\n\tbool "prompt"\n\thelp\n\t  Help text.\n\n\t  More.\nconfig Y\n'
        spans = self.kinds("Kconfig", text)
        self.assertIn(('"prompt"', STRING), spans)
        self.assertIn(("\t  Help text.\n", COMMENT), spans)
        self.assertIn(("\t  More.\n", COMMENT), spans)
        self.assertIn(("config Y\n", CODE), spans)

    def test_text_and_empty(self) -> None:
        self.assertEqual(self.kinds("a.rst", "prose"), [("prose", TEXT)])
        self.assertEqual(segments("a.rst", ""), [])

    def test_segment_map(self) -> None:
        smap = SegmentMap("a.c", "x /* c */ y")
        self.assertEqual([smap.kind_at(i) for i in (0, 3, 10)], [CODE, COMMENT, CODE])
        self.assertEqual(SegmentMap("a.rst", "").kind_at(0), CODE)


if __name__ == "__main__":
    unittest.main()
