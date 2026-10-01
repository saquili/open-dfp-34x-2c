# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The bank guard against its verdict tables, and its smaller rules."""

from __future__ import annotations

import unittest

from bench import guard
from bench.guard import Refusal, Session

from . import vectors


def verdict(call) -> tuple[str, str, str]:
    """Run a check; return ``("allow", "", "")`` or ``("refuse", code, detail)``."""
    try:
        call()
    except Refusal as refusal:
        return "refuse", refusal.code, refusal.detail
    return "allow", "", ""


class GuardVectors(unittest.TestCase):
    """Every row of the verdict tables, each failing with its identifier."""

    def assertVerdict(self, vector_id: str, got: tuple[str, str, str], verdict: str,
                      code: str, detail: str | None, what: str) -> None:
        if got[0] != verdict:
            self.fail("%s: expected %s, the guard said %s [%s] %s for %s"
                      % (vector_id, verdict, got[0], got[1], got[2], what))
        if verdict == "refuse":
            if got[1] != code:
                self.fail("%s: expected code %s, got %s (%s) for %s"
                          % (vector_id, code, got[1], got[2], what))
            if detail is not None and got[2] != detail:
                self.fail("%s: expected detail %r, got %r for %s"
                          % (vector_id, detail, got[2], what))

    def test_the_tables_are_complete(self) -> None:
        self.assertEqual([v.id for v in vectors.LINE_VECTORS],
                         ["GV-%d" % n for n in range(1, 134)])
        self.assertEqual([v.id for v in vectors.MACRO_VECTORS],
                         ["PV-%d" % n for n in range(1, 41)])
        self.assertEqual([v.id for v in vectors.TARGET_VECTORS],
                         ["NV-%d" % n for n in range(1, 15)])
        self.assertEqual([v.id for v in vectors.BANK_VECTORS],
                         ["BV-%d" % n for n in range(1, 6)])
        self.assertEqual([v.id for v in vectors.SEQUENCE_VECTORS],
                         ["SV-%d" % n for n in range(1, 9)])

    def test_line_vectors(self) -> None:
        for vec in vectors.LINE_VECTORS:
            with self.subTest(vec.id):
                got = verdict(lambda: guard.check_line(
                    vec.line, vectors.ENVIRONMENTS[vec.env], Session()))
                self.assertVerdict(vec.id, got, vec.verdict, vec.code, vec.detail,
                                   repr(vec.line))

    def test_line_vectors_through_the_guard_object(self) -> None:
        """The stateful guard agrees with the function on every line row.

        Rows that name ``run`` or ``boot`` are judged on the line alone: the
        board's macros are the subject of the preflight rows.
        """
        for vec in vectors.LINE_VECTORS:
            with self.subTest(vec.id):
                grd = guard.Guard(variables=vectors.ENVIRONMENTS[vec.env])
                got = verdict(lambda: grd.check(vec.line, macros=False))
                self.assertVerdict(vec.id, got, vec.verdict, vec.code, vec.detail,
                                   repr(vec.line))

    def test_macro_vectors(self) -> None:
        for vec in vectors.MACRO_VECTORS:
            with self.subTest(vec.id):
                env = vectors.macro_environment(vec)
                got = verdict(lambda: guard.preflight(vec.line, env))
                self.assertVerdict(vec.id, got, vec.verdict, vec.code, None,
                                   "%r with %s" % (vec.line, vec.changes))

    def test_macro_vectors_through_the_guard_object(self) -> None:
        for vec in vectors.MACRO_VECTORS:
            with self.subTest(vec.id):
                grd = guard.Guard(variables=vectors.macro_environment(vec))
                got = verdict(lambda: grd.check(vec.line))
                self.assertVerdict(vec.id, got, vec.verdict, vec.code, None, repr(vec.line))

    def test_target_vectors(self) -> None:
        for vec in vectors.TARGET_VECTORS:
            with self.subTest(vec.id):
                result: list[str] = []
                got = verdict(lambda: result.append(guard.check_target(vec.text)))
                self.assertVerdict(vec.id, got, vec.verdict, vec.code, vec.detail,
                                   repr(vec.text))
                if vec.verdict == "allow":
                    self.assertEqual(result, [vec.result], vec.id)

    def test_bank_vectors(self) -> None:
        for vec in vectors.BANK_VECTORS:
            with self.subTest(vec.id):
                result: list[int] = []
                got = verdict(lambda: result.append(guard.check_bank(vec.text)))
                self.assertVerdict(vec.id, got, vec.verdict, vec.code, vec.detail,
                                   "BANK=" + vec.text)
                if vec.verdict == "allow":
                    self.assertEqual(result, [int(vec.result)], vec.id)

    def test_sequence_vectors(self) -> None:
        for vec in vectors.SEQUENCE_VECTORS:
            with self.subTest(vec.id):
                session = Session(commit=vec.commit)
                for position, (line, (want, code)) in enumerate(
                        zip(vec.lines, vec.verdicts), start=1):
                    got = verdict(lambda: guard.check_line(line, {}, session))
                    self.assertVerdict("%s line %d" % (vec.id, position), got, want, code,
                                       None, repr(line))


class Numbers(unittest.TestCase):

    def test_the_grammar(self) -> None:
        self.assertEqual(guard.parse_number("0"), 0)
        self.assertEqual(guard.parse_number("0x0"), 0)
        self.assertEqual(guard.parse_number("0X14C000"), 0x14C000)
        self.assertEqual(guard.parse_number("ffffffff"), 0xFFFFFFFF)
        self.assertEqual(guard.parse_number("0000000000080000"), 0x80000)
        for bad in ("", "0x", "+1", "-1", "1 ", " 1", "1_0", "0x0x1", "1h", "g", "0y1",
                    "12345678901234567", "١", "1\n"):
            with self.subTest(bad=bad), self.assertRaises(Refusal) as caught:
                guard.parse_number(bad)
            self.assertEqual(caught.exception.code, "syntax")

    def test_a_value_wider_than_32_bits_overflows(self) -> None:
        for wide in ("100000000", "0x100000000", "ffffffffffffffff"):
            with self.subTest(wide=wide), self.assertRaises(Refusal) as caught:
                guard.parse_number(wide)
            self.assertEqual(caught.exception.code, "overflow")

    def test_decimal_looking_digits_are_hexadecimal(self) -> None:
        self.assertEqual(guard.parse_number("4456448"), 0x4456448)


class Expansion(unittest.TestCase):

    def test_both_forms_and_the_longest_name(self) -> None:
        env = {"a": "1", "a_b": "2", "A1": "3"}
        self.assertEqual(guard.expand("${a}${a_b}$a_b $A1x", dict(env, A1x="4")), "12" "2 4")
        self.assertEqual(guard.expand("x$a", env), "x1")

    def test_refusals(self) -> None:
        cases = [("$", "syntax"), ("$$", "syntax"), ("${", "syntax"), ("${}", "syntax"),
                 ("${1a}", "syntax"), ("${a", "syntax"), ("$(a)", "syntax"),
                 ("$1", "syntax"), ("${nope}", "unknown-variable"),
                 ("$nope", "unknown-variable"), ("${v}", "syntax")]
        for text, code in cases:
            with self.subTest(text=text), self.assertRaises(Refusal) as caught:
                guard.expand(text, {"a": "1", "v": "$a"})
            self.assertEqual(caught.exception.code, code)

    def test_an_undefined_name_can_expand_to_nothing(self) -> None:
        self.assertEqual(guard.expand("a${x}b", {}, undefined_empty=True), "ab")


class FlashMap(unittest.TestCase):

    def test_the_partitions_tile_the_chip(self) -> None:
        position = 0
        for part in guard.PARTITIONS:
            self.assertEqual(part.start, position, part.name)
            position = part.end
        self.assertEqual(position, guard.FLASH_SIZE)
        self.assertEqual([p.name for p in guard.WRITABLE], ["k0", "r0"])
        self.assertEqual(guard.BY_NAME["k0"].size, 1359872)
        self.assertEqual(guard.BY_NAME["r0"].size, 2572288)
        self.assertEqual(guard.window_address(guard.BY_NAME["k1"].start), 0x94440000)

    def test_the_aperture_in_every_spelling(self) -> None:
        inside = [0x14000000, 0x14FFFFFF, 0x94000000, 0x94FFFFFF, 0xB4000000, 0xB4FFFFFF,
                  0x1FC00000, 0x1FFFFFFF, 0x9FC00000, 0x9FFFFFFF, 0xBFC00000, 0xBFFFFFFF]
        outside = [0x13FFFFFF, 0x15000000, 0x93FFFFFF, 0x95000000, 0xB3FFFFFF, 0xB5000000,
                   0x1FBFFFFF, 0x9FBFFFFF, 0xBFBFFFFF, 0xC0000000, 0x80000000, 0xB8003268,
                   0x00000000, 0x81000000]
        for value in inside:
            self.assertTrue(guard.in_aperture(value), hex(value))
        for value in outside:
            self.assertFalse(guard.in_aperture(value), hex(value))

    def test_the_range_rule_in_order(self) -> None:
        cases = [
            ((0x80000, 0), "zero length"),
            ((0x7FFFFF, 2), "beyond"),
            ((0x7F000, 0x2000), "config"),  # touches config and k0: config is protected
            ((0x1CB000, 0x2000), "bank0-span"),
            ((0x43FFFF, 2), "k1"),
            ((0x0, 0x800000), "boot"),
        ]
        for (offset, length), detail in cases:
            with self.subTest(offset=offset, length=length), self.assertRaises(Refusal) as caught:
                guard.check_range(offset, length)
            self.assertEqual(caught.exception.detail, detail)
        self.assertEqual(guard.check_range(0x80000, 0x14C000).name, "k0")
        self.assertEqual(guard.check_range(0x1CC000, 0x274000).name, "r0")
        self.assertEqual(guard.check_range(0x43F000, 0x1000).name, "r0")
        self.assertEqual(guard.check_range(0x80000, 1).name, "k0")


class Details(unittest.TestCase):
    """Behaviours the tables do not isolate."""

    def check(self, line: str, env: dict | None = None, **kwargs):
        return guard.check_line(line, env or {}, kwargs.pop("session", None), **kwargs)

    def test_an_approved_line_is_typed_expanded_and_normalised(self) -> None:
        approval = self.check("sf  erase \t${fl_kernel1} +${fl_kernel1_sz}", vectors.E1)
        self.assertEqual(approval.text, "sf erase 80000 +14c000")
        self.assertEqual(approval.name, "sf")
        self.assertEqual(self.check("   ").text, "")
        self.assertEqual(self.check("").name, "")

    def test_the_command_name_is_exact(self) -> None:
        for line in ("prin", "printen", "Printenv", "reboot", "boo", "ru ramboot", "sf  write"
                     " 80000000 80000 1 2"):
            with self.subTest(line=line), self.assertRaises(Refusal) as caught:
                self.check(line)
            self.assertEqual(caught.exception.code, "not-allowed")

    def test_numbers_decide_before_the_form_does(self) -> None:
        with self.assertRaises(Refusal) as caught:
            self.check("sf write 81000000 80000 zz")
        self.assertEqual(caught.exception.code, "syntax")

    def test_the_ram_address_is_judged_before_the_range(self) -> None:
        with self.assertRaises(Refusal) as caught:
            self.check("sf write 81000000 440000 5f41a")
        self.assertEqual(caught.exception.code, "not-allowed")

    def test_sf_write_zero_length_and_numeric_spellings_of_the_ram_address(self) -> None:
        with self.assertRaises(Refusal) as caught:
            self.check("sf write 80000000 80000 0")
        self.assertEqual((caught.exception.code, caught.exception.detail),
                         ("protected-range", "zero length"))
        self.check("sf write 0x80000000 80000 1")
        self.check("loady 0x80000000")
        self.check("tftpboot 0x81000000 uImage")

    def test_a_32_bit_alias_of_a_window_address_is_refused(self) -> None:
        for line in ("mw.l 114440000 0", "loady 194440000", "sf write 114440000 80000 1",
                     "tftpboot 114440000 uImage", "sf erase 100080000 +1000"):
            with self.subTest(line=line), self.assertRaises(Refusal) as caught:
                self.check(line)
            self.assertEqual(caught.exception.code, "overflow")

    def test_crc32_rules(self) -> None:
        self.check("crc32 80000000 5f41a")
        self.check("crc32 a0000000 1")
        self.check("crc32 1000 10")
        self.check("crc32 80000000 7fffffff")
        for line, code in (("crc32 80000000 0", "not-allowed"),
                           ("crc32 82000000 10", "not-allowed"),
                           ("crc32 b8003268 4", "not-allowed"),
                           ("crc32 94440000 ffffffff", "overflow"),
                           ("crc32 80000000 80000001", "overflow"),
                           ("crc32 80000000", "not-allowed"),
                           ("crc32 g 10", "syntax")):
            with self.subTest(line=line), self.assertRaises(Refusal) as caught:
                self.check(line)
            self.assertEqual(caught.exception.code, code)

    def test_tftpboot_file_names(self) -> None:
        self.check("tftpboot 80000000 rootfs")
        self.check("tftpboot 81000000 " + "a" * 64)
        for name in ("", "a" * 65, "a/b", "a b", "..x/", "a;b", "aѕ"):
            with self.subTest(name=name), self.assertRaises(Refusal):
                self.check("tftpboot 80000000 " + name)

    def test_printenv_names(self) -> None:
        self.check("printenv _x9")
        self.check("printenv " + "a" * 64)
        for name in ("9a", "a-b", "a.b", "a" * 65):
            with self.subTest(name=name), self.assertRaises(Refusal) as caught:
                self.check("printenv " + name)
            self.assertEqual(caught.exception.code, "not-allowed")

    def test_boot_refuses_a_bank_one_trial_however_it_is_spelled(self) -> None:
        for value in ("1", "01", "0x1", " 1"):
            with self.subTest(value=value), self.assertRaises(Refusal) as caught:
                self.check("boot", {"sw_tryactive": value})
            self.assertEqual(caught.exception.code, "bank1-trial")
        for value in ("0", "2", "11", "x"):
            with self.subTest(value=value):
                self.check("boot", {"sw_tryactive": value})
        self.check("boot")

    def test_setenv_protected_names(self) -> None:
        for name in ("sw_x", "fl_x", "img9", "bootargs", "boot", "mtdparts7", "b1", "en_wdt",
                     "tftp_base", "ethaddr", "loadaddr", "filesize", "fileaddr", "netmask"):
            with self.subTest(name=name), self.assertRaises(Refusal) as caught:
                self.check("setenv %s x" % name)
            self.assertEqual((caught.exception.code, caught.exception.detail),
                             ("protected-variable", name))
        for line in ("setenv", "setenv foo bar", "setenv SW_x 1", "setenv x"):
            with self.subTest(line=line), self.assertRaises(Refusal) as caught:
                self.check(line)
            self.assertEqual(caught.exception.code, "not-allowed")

    def test_a_refused_setenv_is_not_remembered_and_saveenv_clears(self) -> None:
        session = Session()
        with self.assertRaises(Refusal):
            self.check("setenv sw_tryactive 1", session=session)
        self.assertEqual(session.pending, [])
        self.check("setenv sw_tryactive 0", session=session)
        self.check("setenv sw_tryactive 2", session=session)
        self.assertEqual(len(session.pending), 2)
        self.check("saveenv", session=session)
        self.assertEqual(session.pending, [])
        with self.assertRaises(Refusal) as caught:
            self.check("saveenv", session=session)
        self.assertEqual(caught.exception.code, "state")

    def test_commit_mode_allows_exactly_one_more_form(self) -> None:
        session = Session(commit=True)
        self.check("setenv sw_commit 0", session=session)
        with self.assertRaises(Refusal) as caught:
            self.check("setenv sw_commit 2", session=session)
        self.assertEqual(caught.exception.code, "protected-variable")

    def test_read_only_console_refuses_writes(self) -> None:
        for line in ("", "printenv", "printenv x", "version", "crc32 80000000 10",
                     "crc32 94440000 14c000"):
            with self.subTest(line=line):
                self.check(line, read_only=True)
        for line in ("reset", "boot", "run ramboot", "setenv sw_tryactive 0", "saveenv",
                     "mw.l b8003268 0", "loady 80000000", "sf erase 80000 +14c000",
                     "tftpboot 80000000 uImage"):
            with self.subTest(line=line), self.assertRaises(Refusal) as caught:
                self.check(line, read_only=True)
            self.assertEqual(caught.exception.code, "not-allowed")
            self.assertIn("read-only", caught.exception.detail)

    def test_a_refusal_names_its_line_in_a_fixed_format(self) -> None:
        with self.assertRaises(Refusal) as caught:
            self.check("sf erase 440000 +14c000")
        self.assertEqual(str(caught.exception),
                         "refused [protected-range] k1: sf erase 440000 +14c000")
        with self.assertRaises(Refusal) as caught:
            self.check("printenv\x00\nѕ")
        self.assertEqual(str(caught.exception),
                         "refused [syntax] a character outside printable ASCII: "
                         "printenv\\x00\\x0a\\u0455")
        self.assertEqual(caught.exception.exit_status, 3)
        self.assertIn(caught.exception.code, guard.CODES)

    def test_every_code_in_the_tables_is_a_known_code(self) -> None:
        used = {v.code for v in vectors.LINE_VECTORS if v.code}
        used |= {v.code for v in vectors.MACRO_VECTORS if v.code}
        self.assertLessEqual(used, set(guard.CODES))
        self.assertEqual(used, set(guard.CODES))


class PreflightDetails(unittest.TestCase):

    def env(self, **changes):
        env = dict(vectors.BOOT_BASE)
        for name, value in changes.items():
            if value is None:
                env.pop(name, None)
            else:
                env[name] = value
        return env

    def refusal(self, text: str, env) -> Refusal:
        with self.assertRaises(Refusal) as caught:
            guard.preflight(text, env)
        return caught.exception

    def test_other_lines_pass_untouched(self) -> None:
        guard.preflight("printenv", {})
        guard.preflight("sf erase 440000 +14c000", {})
        guard.preflight("", {})

    def test_only_three_macros_may_run(self) -> None:
        self.assertEqual(self.refusal("run b1", self.env()).code, "not-allowed")
        guard.preflight_macro("sdsup", self.env())

    def test_an_empty_macro_is_harmless_and_a_bare_separator_is_ignored(self) -> None:
        guard.preflight("run sdsup", self.env(sdsup=""))
        guard.preflight("run sdsup", self.env(sdsup="mw.l bb000100 1;; ;"))

    def test_register_writes_take_no_count(self) -> None:
        env = self.env(sdsup="mw.l bb000100 1 1000")
        self.assertEqual(self.refusal("run sdsup", env).code, "not-allowed")

    def test_the_register_window_edges(self) -> None:
        guard.preflight("run sdsup", self.env(sdsup="mw.l b8000000 0; mw.b bbffffff 1"))
        for edge in ("b7ffffff", "bc000000"):
            with self.subTest(edge=edge):
                env = self.env(sdsup="mw.l %s 0" % edge)
                self.assertEqual(self.refusal("run sdsup", env).code, "not-allowed")

    def test_a_partition_table_with_parentheses_is_data_in_the_kernel_arguments(self) -> None:
        tables = ("256K(boot)", "a(b)", "(", ")", ")(", "x(sf erase 0 +40000)",
                  vectors.MTDPARTS_BANK0, "1328K(k0)ro,2512K(r0)ro root=31:5")
        for text in tables:
            with self.subTest(text=text):
                env = self.env(mtdparts0=text, mtdparts1=text)
                guard.preflight("run b0", env)
                guard.preflight("run ramboot", env)
                guard.preflight("boot", env)

    def test_a_value_holding_another_separator_is_refused_even_beside_parentheses(self) -> None:
        for char in ";&|{}<>$":
            for text in (char, "a(b)" + char, char + "(b)", "256K(boot) " + char + " x"):
                with self.subTest(char=char, text=text):
                    env = self.env(mtdparts0=text)
                    self.assertEqual(self.refusal("run b0", env).code, "syntax")
                    self.assertEqual(self.refusal("run ramboot", env).code, "syntax")

    def test_a_value_holding_a_quote_is_refused_in_a_macro_and_in_a_typed_line(self) -> None:
        for char in "`'\"\\":
            with self.subTest(char=char):
                env = self.env(mtdparts0="256K(boot) " + char + " x")
                self.assertEqual(self.refusal("run b0", env).code, "syntax")
                self.assertEqual(self.refusal("run ramboot", env).code, "syntax")
                with self.assertRaises(Refusal) as caught:
                    guard.check_line("printenv ${q}", {"q": "a" + char + "b"})
                self.assertEqual(caught.exception.code, "syntax")

    def test_the_kernel_arguments_own_text_may_not_hold_a_parenthesis(self) -> None:
        for part in ("setenv bootargs a(b)", "setenv bootargs (", "setenv bootargs x)",
                     "setenv bootargs ${mtdparts0} )", "setenv bootargs (${mtdparts0})",
                     "setenv bootargs ${mtdparts0}(x)"):
            for how in ("gains", "is"):
                with self.subTest(part=part, how=how):
                    text = vectors.RAMBOOT + "; " + part if how == "gains" else part
                    env = self.env(ramboot=text, mtdparts0="256K(boot)")
                    caught = self.refusal("run ramboot", env)
                    self.assertEqual(caught.code, "syntax")

    def test_every_other_form_refuses_a_parenthesis_from_any_source(self) -> None:
        table = "256K(boot)"
        parts = ("tftpboot 0x81000000 ${mtdparts0}", "tftpboot 0x81000000 u(Image)",
                 "bootm ${mtdparts0}", "bootm (0x81000000)", "mw.l bb000100 ${mtdparts0}",
                 "mw.l (bb000100) 1", "run ${mtdparts0}", "run (sdsup)", "setenv x ${mtdparts0}",
                 "setenv bootarg ${mtdparts0}", "setenv bootargs2 ${mtdparts0}",
                 "(sf erase 0 +40000)", "sf (erase) 0 +40000", "${mtdparts0}", "()")
        for part in parts:
            for how in ("gains", "is"):
                with self.subTest(part=part, how=how):
                    text = vectors.RAMBOOT + "; " + part if how == "gains" else part
                    env = self.env(ramboot=text, mtdparts0=table)
                    self.assertEqual(self.refusal("run ramboot", env).code, "syntax")

    def test_the_boot_chains_own_forms_refuse_a_parenthesis_too(self) -> None:
        table = "256K(boot)"
        parts = ("if itest.s ${mtdparts0} == 2", "then itest.s a(b) == 2",
                 "itest.s 1 == (1)", "saveenv ()", "setenv sw_active ${mtdparts0}",
                 "setenv sw_tryactive (2)", "run (b0)", "run ${mtdparts0}")
        for part in parts:
            with self.subTest(part=part):
                env = self.env(boot_by_commit=vectors.BOOT_BY_COMMIT + "; " + part,
                               mtdparts0=table)
                self.assertEqual(self.refusal("boot", env).code, "syntax")

    def test_the_kernel_arguments_may_be_set_on_the_chains_branches(self) -> None:
        chain = ("if itest.s ${sw_commit} == 0; then setenv bootargs ${bootargs_base} "
                 "${mtdparts0}; run b0; else setenv bootargs ${bootargs_base} ${mtdparts1}; "
                 "run b1; fi")
        guard.preflight("boot", self.env(boot_by_commit=chain, mtdparts0="a(b)",
                                         mtdparts1="c(d)"))

    def test_the_exception_is_for_the_first_two_words_only(self) -> None:
        """A part is the kernel arguments only if it begins with the two words."""
        for part in ("setenv  bootargs ${mtdparts0}", "setenv\tbootargs ${mtdparts0}"):
            with self.subTest(part=part):
                guard.preflight("run ramboot", self.env(
                    ramboot=vectors.RAMBOOT + "; " + part, mtdparts0="a(b)"))
        for part in ("Setenv bootargs ${mtdparts0}", "setenv ${bootargs_base} ${mtdparts0}",
                     "${bootargs_base} bootargs ${mtdparts0}", "bootargs ${mtdparts0}"):
            with self.subTest(part=part):
                env = self.env(ramboot=vectors.RAMBOOT + "; " + part, mtdparts0="a(b)",
                               bootargs_base="setenv")
                self.assertIn(self.refusal("run ramboot", env).code, ("syntax", "not-allowed"))

    def test_typed_lines_keep_the_full_refusal(self) -> None:
        board = dict(vectors.E6)
        for line in ("printenv ${mtdparts0}", "setenv bootargs ${mtdparts0}", "run (ramboot)",
                     "sf erase 80000 +14c000 (", "boot ()", "crc32 80000000 (10)"):
            with self.subTest(line=line), self.assertRaises(Refusal) as caught:
                guard.check_line(line, board, Session())
            self.assertEqual(caught.exception.code, "syntax")
            with self.assertRaises(Refusal) as caught:
                guard.Guard(variables=board).check(line)
            self.assertEqual(caught.exception.code, "syntax")

    def test_the_board_shape_is_allowed_through_the_guard_object(self) -> None:
        grd = guard.Guard(variables=vectors.E6)
        for line in ("run ramboot", "run sdsup", "run b0", "boot"):
            with self.subTest(line=line):
                grd.check(line)

    def test_a_refusal_names_the_innermost_macro_that_held_the_text(self) -> None:
        caught = self.refusal("run ramboot", self.env(mtdparts0="a;b"))
        self.assertEqual(caught.code, "syntax")
        self.assertIn("(macro ramboot)", caught.detail)
        caught = self.refusal("run ramboot", self.env(sdsup="mw.l bb000100 1; sf erase 0 +40000"))
        self.assertEqual(caught.code, "not-allowed")
        self.assertIn("(macro sdsup)", caught.detail)
        self.assertEqual(caught.detail.count("(macro "), 1)
        caught = self.refusal("run ramboot", self.env(sdsup=None))
        self.assertEqual(caught.code, "missing-macro")
        self.assertTrue(caught.detail.startswith("sdsup"), caught.detail)

    def test_undefined_names_expand_to_nothing_only_inside_bootargs(self) -> None:
        guard.preflight("run b0", self.env(bootargs_base=None))
        env = self.env(b0="setenv bootargs; bootm ${img0_kernel}")
        self.assertEqual(self.refusal("run b0", env).code, "not-allowed")
        env = self.env(img0_kernel=None)
        self.assertEqual(self.refusal("run b0", env).code, "unknown-variable")

    def test_the_chain_may_start_either_bank(self) -> None:
        guard.preflight("boot", self.env())
        guard.preflight("boot", self.env(sw_commit="1"))
        self.assertEqual(self.refusal("run b1", self.env()).code, "not-allowed")

    def test_the_chain_may_not_use_the_sw_commit_or_other_variables(self) -> None:
        env = self.env(boot_by_tryactive="setenv sw_commit 0; saveenv")
        self.assertEqual(self.refusal("boot", env).code, "not-allowed")
        env = self.env(boot_by_tryactive="setenv sw_tryactive 3")
        self.assertEqual(self.refusal("boot", env).code, "not-allowed")

    def test_conditionals_are_not_allowed_outside_the_chain(self) -> None:
        for word in ("if", "then", "else", "fi"):
            env = self.env(sdsup="%s mw.l bb000100 1" % word)
            self.assertEqual(self.refusal("run sdsup", env).code, "not-allowed")

    def test_loops_are_never_allowed(self) -> None:
        for word in ("while", "for", "do", "done", "case", "esac"):
            env = self.env(boot_by_commit="%s x" % word)
            self.assertEqual(self.refusal("boot", env).code, "not-allowed")

    def test_quotes_and_control_characters_are_syntax(self) -> None:
        for char in ("'", '"', "`", "\\", "\n", "\x00", "ѕ"):
            env = self.env(sdsup="mw.l bb000100 1 %s" % char)
            self.assertEqual(self.refusal("run sdsup", env).code, "syntax", repr(char))

    def test_a_nested_run_in_the_chain_must_name_a_defined_macro(self) -> None:
        env = self.env(boot_by_commit="run nowhere")
        self.assertEqual(self.refusal("boot", env).code, "missing-macro")
        env = self.env(boot_by_commit="run 9bad")
        self.assertEqual(self.refusal("boot", env).code, "not-allowed")

    def test_depth_four_is_allowed_and_five_is_not(self) -> None:
        chain = self.env(bootcmd="run c1", c1="run c2", c2="run c3", c3="setenv sw_active 0")
        guard.preflight("boot", chain)  # four deep: bootcmd, c1, c2, c3
        chain["c3"] = "run c4"
        chain["c4"] = "setenv sw_active 0"
        self.assertEqual(self.refusal("boot", chain).code, "depth")

    def test_a_macro_text_is_never_trusted_from_the_session_snapshot(self) -> None:
        reads: list[str] = []
        board = dict(vectors.RUN_BASE)

        def fetch(name: str):
            reads.append(name)
            return board.get(name)

        grd = guard.Guard(variables={"ramboot": "something else"}, fetch=fetch)
        grd.check("run ramboot")
        self.assertIn("ramboot", reads)
        self.assertEqual(sorted(set(reads)), sorted(reads), "each name is read once")
        board["sdsup"] = "mw.l 94440000 0"
        with self.assertRaises(Refusal) as caught:
            grd.check("run ramboot")
        self.assertEqual(caught.exception.code, "flash-aperture")
        self.assertEqual(caught.exception.line, "run ramboot")

    def test_boot_reads_the_pending_trial_from_the_board(self) -> None:
        """The session's copy says 2; the board says 1: the board decides."""
        board = dict(vectors.BOOT_BASE)
        grd = guard.Guard(variables=board, fetch=board.get)
        grd.check("boot")
        board["sw_tryactive"] = "1"
        with self.assertRaises(Refusal) as caught:
            grd.check("boot")
        self.assertEqual(caught.exception.code, "bank1-trial")
        self.assertEqual(caught.exception.line, "boot")

    def test_a_typed_line_expands_from_the_copy_read_at_the_start_of_the_session(self) -> None:
        board = dict(vectors.E1)
        reads: list[str] = []

        def fetch(name: str):
            reads.append(name)
            return board.get(name)

        grd = guard.Guard(variables=dict(vectors.E1), fetch=fetch)
        line = "sf erase ${fl_kernel1} +${fl_kernel1_sz}"
        self.assertEqual(grd.check(line).text, "sf erase 80000 +14c000")
        board["fl_kernel1"] = "440000"
        self.assertEqual(grd.check(line).text, "sf erase 80000 +14c000", "the copy answers")
        self.assertEqual(reads, [], "a typed line reads nothing from the board")

    def test_a_line_typed_on_its_own_reads_the_variables_it_names(self) -> None:
        board = dict(vectors.E1)
        reads: list[str] = []

        def fetch(name: str):
            reads.append(name)
            return board.get(name)

        grd = guard.Guard(fetch=fetch)
        line = "sf erase ${fl_kernel1} +${fl_kernel1_sz}"
        self.assertEqual(grd.check(line).text, "sf erase 80000 +14c000")
        self.assertEqual(sorted(reads), ["fl_kernel1", "fl_kernel1_sz"])
        board["fl_kernel1"] = "440000"
        with self.assertRaises(Refusal) as caught:
            grd.check(line)
        self.assertEqual((caught.exception.code, caught.exception.detail),
                         ("protected-range", "k1"))

    def test_the_line_alone_can_be_judged_without_a_board(self) -> None:
        grd = guard.Guard()
        grd.check("run ramboot", macros=False)
        grd.check("boot", macros=False)
        with self.assertRaises(Refusal) as caught:
            grd.check("run ramboot")
        self.assertEqual(caught.exception.code, "missing-macro")

    def test_the_read_only_flag_reaches_the_guard_object(self) -> None:
        grd = guard.Guard(read_only=True)
        grd.check("printenv")
        with self.assertRaises(Refusal):
            grd.check("reset")

    def test_commit_mode_reaches_the_guard_object(self) -> None:
        grd = guard.Guard(commit=True)
        grd.check("setenv sw_commit 0")
        grd.check("saveenv")
        with self.assertRaises(Refusal):
            guard.Guard().check("setenv sw_commit 0")


class Layout(unittest.TestCase):

    def test_a_matching_layout_passes_in_any_spelling(self) -> None:
        guard.check_layout(vectors.E1)
        spelled = dict(vectors.E1, fl_kernel1="0x80000", fl_env_sz="0002000",
                       img0_kernel="0X94080000")
        guard.check_layout(spelled)

    def test_layout_mismatch_stops_naming_the_variable(self) -> None:
        for name in guard.LAYOUT_VARIABLES:
            with self.subTest(name=name):
                changed = dict(vectors.E1, **{name: "1"})
                with self.assertRaises(guard.LayoutMismatch) as caught:
                    guard.check_layout(changed)
                self.assertEqual(caught.exception.name, name)
                self.assertIn(name, str(caught.exception))
        with self.assertRaises(guard.LayoutMismatch) as caught:
            guard.check_layout(vectors.ENVIRONMENTS["E2"])
        self.assertEqual(caught.exception.name, "fl_kernel1")

    def test_a_missing_or_unreadable_value_is_a_mismatch(self) -> None:
        missing = dict(vectors.E1)
        del missing["fl_rootfs2_sz"]
        with self.assertRaises(guard.LayoutMismatch) as caught:
            guard.check_layout(missing)
        self.assertIsNone(caught.exception.actual)
        with self.assertRaises(guard.LayoutMismatch):
            guard.check_layout(dict(vectors.E1, fl_env="zz"))
        with self.assertRaises(guard.LayoutMismatch):
            guard.check_layout({})

    def test_the_table_agrees_with_the_partition_map(self) -> None:
        table = guard.LAYOUT_VARIABLES
        pairs = (("fl_boot_sz", 0x40000), ("fl_env", guard.BY_NAME["env"].start),
                 ("fl_env2", guard.BY_NAME["env2"].start),
                 ("fl_env_sz", guard.BY_NAME["env"].size),
                 ("fl_cfgfs", guard.BY_NAME["config"].start),
                 ("fl_cfgfs_sz", guard.BY_NAME["config"].size),
                 ("fl_kernel1", guard.BY_NAME["k0"].start),
                 ("fl_kernel1_sz", guard.BY_NAME["k0"].size),
                 ("fl_rootfs1", guard.BY_NAME["r0"].start),
                 ("fl_rootfs1_sz", guard.BY_NAME["r0"].size),
                 ("fl_kernel2", guard.BY_NAME["k1"].start),
                 ("fl_kernel2_sz", guard.BY_NAME["k1"].size),
                 ("fl_rootfs2", guard.BY_NAME["r1"].start),
                 ("fl_rootfs2_sz", guard.BY_NAME["r1"].size),
                 ("img0_kernel", guard.window_address(guard.BY_NAME["k0"].start)),
                 ("img1_kernel", guard.window_address(guard.BY_NAME["k1"].start)))
        for name, value in pairs:
            self.assertEqual(table[name], value, name)
        self.assertEqual(len(table), len(pairs))
        for name, text in vectors.E1.items():
            if name in table:
                self.assertEqual(int(text, 16), table[name], name)


if __name__ == "__main__":
    unittest.main()
