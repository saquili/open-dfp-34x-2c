# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The tokeniser and the deny-list format and lookup."""

from __future__ import annotations

import hashlib
import unittest

from gates.denylist import DenyList, DenyListError, canary, format_denylist
from gates.tokens import Hasher, component_runs, tokenize

SALT = b"salt"


class Tokeniser(unittest.TestCase):

    def norms(self, text: str) -> list[str]:
        return [t.norm for t in tokenize(text)]

    def test_identifiers_are_lower_cased(self) -> None:
        self.assertEqual(self.norms("Foo_Bar(baz2) + _q"), ["foo_bar", "baz2", "_q"])

    def test_hex_literals_lose_their_suffix(self) -> None:
        self.assertEqual(self.norms("0x1234ABCDul 0X0f"), ["0x1234abcd", "0x0f"])

    def test_a_bare_kernel_segment_address_is_a_token(self) -> None:
        self.assertEqual(self.norms("at 8012ABCD, not 80123 or 812345678"),
                         ["at", "0x" + "8012abcd", "not", "or"])

    def test_tokens_keep_their_spans(self) -> None:
        tok = list(tokenize("  x = QqQ;"))[1]
        self.assertEqual((tok.start, tok.end, tok.text, tok.norm), (6, 9, "QqQ", "qqq"))

    def test_glued_tokens_do_not_split(self) -> None:
        self.assertEqual(self.norms("0x12zz 9abc"), [])

    def test_component_runs(self) -> None:
        self.assertEqual(sorted(component_runs("a_b_c")),
                         sorted(["a", "a_b", "a_b_c", "b", "b_c", "c"]))
        self.assertEqual(sorted(component_runs("a__b", 1)), ["a", "b"])
        self.assertEqual(list(component_runs("0x1234")), [])

    def test_hasher_is_salted_sha256(self) -> None:
        self.assertEqual(Hasher(SALT)("abc"), hashlib.sha256(SALT + b"abc").hexdigest())


def denylist(entries, parts_max: int = 8) -> DenyList:
    hasher = Hasher(SALT)
    return DenyList.parse(format_denylist(
        SALT, [(hasher(name), match, category) for name, match, category in entries],
        ["header line", ""], parts_max=parts_max))


class DenyListFormat(unittest.TestCase):

    def test_token_and_part_entries(self) -> None:
        dl = denylist([("qq_zz", "token", "symbol"), ("rr_ss", "part", "register")])
        self.assertEqual(len(dl), 2)
        self.assertEqual([h.category for h in dl.lookup("qq_zz")], ["symbol"])
        self.assertEqual(dl.lookup("x_qq_zz"), ())  # a token entry is not a part
        hits = dl.lookup("chip_rr_ss_ctrl")
        self.assertEqual([(h.run, h.category) for h in hits], [("rr_ss", "register")])
        self.assertEqual(dl.lookup("chip_rr_ss_ctrl"), hits)  # from the cache
        self.assertEqual(dl.lookup("rrss"), ())

    def test_a_list_without_parts_checks_whole_tokens_only(self) -> None:
        dl = denylist([("qq_zz", "token", "symbol")])
        self.assertEqual(len(dl.lookup("qq_zz")), 1)
        self.assertEqual(dl.lookup("x_qq_zz"), ())

    def test_parts_max_limits_the_runs(self) -> None:
        dl = denylist([("a_b_c", "part", "register")], parts_max=2)
        self.assertEqual(dl.lookup("x_a_b_c_y"), ())
        self.assertEqual(len(dl.lookup("a_b_c")), 1)  # the whole token still matches

    def test_the_file_holds_no_names(self) -> None:
        text = format_denylist(SALT, [(Hasher(SALT)("qq_zz"), "token", "symbol")], ["h"])
        self.assertNotIn("qq_zz", text)
        self.assertTrue(text.startswith("# h\nformat rtl9601d-provenance-denylist 1\n"))

    def test_malformed_files_are_refused(self) -> None:
        digest = "a" * 64
        head = "format rtl9601d-provenance-denylist 1\nsalt 00\n"
        for text, why in (
            ("salt 00\n", "must come first"),
            ("format other 2\n", "unknown format"),
            ("", "missing format"),
            ("format rtl9601d-provenance-denylist 1\n", "missing salt"),
            ("format rtl9601d-provenance-denylist 1\nsalt zz\n", "bad salt"),
            (head + "nonsense\n", "expected"),
            (head + "%s token\n" % digest, "expected"),
            (head + "%s maybe symbol\n" % digest, "unknown match kind"),
            (head + "%s token Bad!\n" % digest, "bad category"),
            (head + "%s token symbol\n%s part symbol\n" % (digest, digest), "duplicate"),
        ):
            with self.assertRaises(DenyListError, msg=text) as ctx:
                DenyList.parse(text)
            self.assertIn(why, str(ctx.exception))
        with self.assertRaises(DenyListError):
            format_denylist(SALT, [("short", "token", "symbol")], [])

    def test_comments_blank_lines_and_parts_max_directive(self) -> None:
        dl = DenyList.parse("# c\n\nformat rtl9601d-provenance-denylist 1\nsalt 00\n"
                            "parts-max 3\n")
        self.assertEqual((dl.salt, dl.parts_max, len(dl)), (b"\0", 3, 0))

    def test_the_canary_is_derived_from_the_salt(self) -> None:
        self.assertEqual(canary(SALT), canary(SALT))
        self.assertNotEqual(canary(SALT), canary(b"other"))
        self.assertEqual([t.norm for t in tokenize(canary(SALT))], [canary(SALT)])


if __name__ == "__main__":
    unittest.main()
