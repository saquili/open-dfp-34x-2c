# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The guard and the flows on an environment shaped like the board's.

The partition tables the kernel arguments are made of hold parentheses, and
the macros are as long as the board's: a ``sdsup`` of several register writes
and a boot chain that records the active bank. The rows of the verdict tables
let those tables hold any text, which is how a guard that refused every
parenthesis passed all of them; here the same checks run on the real shape,
through the guard and through the whole of RAM boot and flashing.
"""

from __future__ import annotations

import unittest

from bench.guard import Guard, Refusal

from . import test_flash, test_ramboot, vectors
from .simboard import default_env


def board_environment(bootdelay: int = 600) -> dict[str, str]:
    return default_env(bootdelay, base=vectors.BOARD)


class BoardShape(unittest.TestCase):

    def env(self, **changes: str | None) -> dict[str, str]:
        env = dict(vectors.BOARD)
        for name, value in changes.items():
            if value is None:
                env.pop(name, None)
            else:
                env[name] = value
        return env

    def refusal(self, line: str, env: dict[str, str]) -> Refusal:
        with self.assertRaises(Refusal) as caught:
            Guard(variables=env).check(line)
        return caught.exception

    def test_the_tables_are_the_boards_and_hold_parentheses(self) -> None:
        for name, root in (("mtdparts0", "root=31:5"), ("mtdparts1", "root=31:7")):
            table = vectors.BOARD[name]
            self.assertIn("256K(boot)", table)
            self.assertIn("1328K(k0)ro", table)
            self.assertTrue(table.endswith(root), name)
        self.assertNotIn("rst2dfl_flg", vectors.BOARD)
        self.assertEqual(vectors.BOARD["sdsup"].count("mw.l"), 5)
        for name in ("bootcmd", "boot_by_commit", "boot_by_tryactive", "set_act0", "set_act1",
                     "en_wdt", "b0", "b1"):
            self.assertIn(name, vectors.BOARD)

    def test_the_guard_allows_the_boards_macros_and_the_boot_chain(self) -> None:
        grd = Guard(variables=vectors.BOARD)
        for line in ("run ramboot", "run b0", "run sdsup", "boot"):
            with self.subTest(line=line):
                self.assertEqual(grd.check(line).text, line)

    def test_the_guard_allows_them_when_it_reads_the_board_itself(self) -> None:
        reads: list[str] = []

        def fetch(name: str):
            reads.append(name)
            return vectors.BOARD.get(name)

        grd = Guard(fetch=fetch)
        for line in ("run ramboot", "run b0", "run sdsup", "boot"):
            with self.subTest(line=line):
                grd.check(line)
        for name in ("ramboot", "sdsup", "b0", "mtdparts0", "bootcmd", "boot_by_commit",
                     "boot_by_tryactive", "set_act0", "set_act1", "en_wdt", "b1", "mtdparts1"):
            self.assertIn(name, reads)

    def test_a_macro_that_gains_an_erase_is_refused_wherever_it_hides(self) -> None:
        erase = "; sf erase ${fl_kernel2} +${fl_kernel2_sz}"
        for macro, line in (("ramboot", "run ramboot"), ("sdsup", "run ramboot"),
                            ("b0", "run b0"), ("boot_by_commit", "boot"),
                            ("boot_by_tryactive", "boot"), ("set_act0", "boot"),
                            ("set_act1", "boot"), ("en_wdt", "boot"), ("bootcmd", "boot")):
            with self.subTest(macro=macro):
                caught = self.refusal(line, self.env(**{macro: vectors.BOARD[macro] + erase}))
                self.assertEqual(caught.code, "not-allowed")
                self.assertIn("(macro %s)" % macro, caught.detail)

    def test_a_register_write_outside_the_window_is_refused(self) -> None:
        caught = self.refusal("run sdsup", self.env(sdsup=vectors.BOARD_SDSUP + "; mw.l 94440000 0"))
        self.assertEqual(caught.code, "flash-aperture")
        caught = self.refusal("run sdsup", self.env(sdsup=vectors.BOARD_SDSUP + "; mw.l bc000000 0"))
        self.assertEqual(caught.code, "not-allowed")

    def test_a_table_with_a_second_command_or_another_separator_is_refused(self) -> None:
        for suffix in ("; sf erase 0 +40000", " && reset", " | x", " {", " >x", " $x"):
            with self.subTest(suffix=suffix):
                for line in ("run ramboot", "run b0", "boot"):
                    env = self.env(mtdparts0=vectors.MTDPARTS_BANK0 + suffix,
                                   mtdparts1=vectors.MTDPARTS_BANK1 + suffix)
                    self.assertEqual(self.refusal(line, env).code, "syntax")


class RamBootOnTheBoardsEnvironment(test_ramboot.RamBootCase):
    """The RAM boot against a board whose macros and tables are the real shape."""

    def board_environment(self) -> dict[str, str]:
        return board_environment(self.bootdelay)

    def test_a_ramboot_from_a_running_kernel_runs_to_the_end(self) -> None:
        result = self.go()
        self.assertEqual(result.attempts, 1)
        self.assertEqual(self.board.kernels_started, ["Linux-6.18.48-test"])
        self.assertTrue(self.transcript.text().rstrip().endswith("login:"))

    def test_the_kernel_started_with_the_boards_tables_for_bank_0(self) -> None:
        self.go()
        expected = vectors.BOARD["bootargs_base"] + " " + vectors.MTDPARTS_BANK0
        self.assertEqual(self.board.model.env["bootargs"], expected)
        self.assertIn("bootargs: " + expected, self.transcript.text())
        self.assertIn("256K(boot)", self.transcript.text())

    def test_a_ramboot_from_the_loader_prompt(self) -> None:
        self.board.at_prompt()
        self.go()
        self.assertEqual(self.board.restarts, [])
        self.assertEqual(self.board.kernels_started, ["Linux-6.18.48-test"])

    def test_only_what_the_guard_allows_was_typed_and_no_flash_was_written(self) -> None:
        self.go()
        names = {line.split()[0] for line in self.board.executed if line}
        self.assertLessEqual(names, {"printenv", "run", "boot"})
        self.assertLessEqual(self.board_ops(), {"store", "exec"})

    def test_the_macros_were_read_from_the_board_before_ramboot_ran(self) -> None:
        self.go()
        reads = [text for kind, text in self.loader.typed if kind == "read"]
        for name in ("ramboot", "sdsup", "mtdparts0", "bootargs_base"):
            self.assertIn("printenv " + name, reads)
        steps = [text for kind, text in self.loader.typed if kind == "step"]
        self.assertLess(steps.index("printenv"), steps.index("run ramboot"))

    def test_a_table_that_gains_a_command_stops_the_run_before_anything_is_typed(self) -> None:
        self.board.model.env["mtdparts0"] += "; sf erase 0 +40000"
        with self.assertRaises(Refusal) as caught:
            self.go()
        self.assertEqual(caught.exception.code, "syntax")
        self.assertNotIn("run ramboot", self.board.executed)
        self.assertEqual(self.board_ops(), set())


class FlashOnTheBoardsEnvironment(test_flash.FlashCase):
    """Flashing against a board whose macros and tables are the real shape."""

    def board_environment(self) -> dict[str, str]:
        return board_environment(self.bootdelay)

    def test_a_kernel_by_tftp_runs_to_the_end(self) -> None:
        result = self.run_flash(target="kernel")
        self.assertEqual(self.flash_chip.region("k0")[:len(self.kernel_image)], self.kernel_image)
        self.assertEqual(result.partition, "k0")
        executed = self.board.executed
        self.assertLess(executed.index("run sdsup"), executed.index("tftpboot 80000000 uImage"))
        registers = [op.ranges[0][0] for op in self.board.model.ops if op.detail == "mw"]
        self.assertEqual(registers[:5], [0xBB000100, 0xBB000104, 0xBB000108, 0xBB00010C,
                                         0xBB000110], "the macro wrote its registers")

    def test_a_root_filesystem_by_ymodem_runs_to_the_end(self) -> None:
        self.run_flash(target="rootfs", via="ymodem", connect=self.serial_connection)
        self.assertEqual(self.flash_chip.region("r0")[:len(self.rootfs)], self.rootfs)

    def test_a_trial_is_armed_and_booted_through_the_boards_chain(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        result = self.run_flash(target="kernel", arm=True)
        self.assertTrue(result.armed and result.booted)
        env = self.board.model.env
        self.assertEqual(env["sw_tryactive"], "2", "the chain spent the trial flag")
        self.assertEqual(env["sw_active"], "0", "the chain recorded bank 0")
        self.assertEqual(env["sw_commit"], "1", "the committed bank is the tool's to leave alone")
        self.assertEqual(env["bootargs"],
                         vectors.BOARD["bootargs_base"] + " " + vectors.MTDPARTS_BANK0)
        self.assertEqual(self.board.kernels_started[-1], "Linux-6.18.48-test")
        self.assertIn("256K(boot)", self.transcript.text())

    def test_the_chain_is_read_from_the_board_before_boot(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        self.run_flash(target="kernel", arm=True)
        reads = [text for kind, text in self.loader.typed if kind == "read"]
        for name in ("bootcmd", "boot_by_commit", "boot_by_tryactive", "set_act0", "en_wdt", "b0"):
            self.assertIn("printenv " + name, reads)

    def test_a_chain_that_gains_an_erase_stops_the_trial_and_the_flash_stays_bank_0_only(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        self.board.model.env["set_act0"] += "; sf erase 440000 +14c000"
        with self.assertRaises(Refusal) as caught:
            self.run_flash(target="kernel", arm=True)
        self.assertEqual(caught.exception.code, "not-allowed")
        self.assertIn("(macro set_act0)", caught.exception.detail)
        self.assertEqual(self.flash_chip.protected_unchanged(self.before), [])


if __name__ == "__main__":
    unittest.main()
