# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The loader driver against a scripted board: break-in, typing, the environment."""

from __future__ import annotations

from bench import console as con
from bench.console import (ConsoleInterrupted, UnexpectedState, WaitTimeout)
from bench.guard import Approval, Guard, Refusal
from bench.uboot import (BreakInFailed, CommandFailed, Loader, LoaderError, NotAtPrompt,
                         UnexpectedReset, parse_environment)

from .simboard import LoaderCase, Mode, SimBoard


class BreakIn(LoaderCase):

    def test_breakin_lands(self) -> None:
        self.restart_to_countdown()
        started = self.clock.now()
        self.loader.break_in()
        self.assertEqual(self.board.mode, Mode.PROMPT)
        self.assertLess(self.clock.now() - started, 3.0)
        text = self.transcript.text()
        self.assertTrue(text.endswith("9601D# "))
        self.assertIn("\x08\x08\x08 0 ", text, "the countdown was stopped, not run out")

    def test_only_enters_are_ever_typed_into_a_countdown(self) -> None:
        self.restart_to_countdown()
        self.loader.break_in()
        self.assertEqual(set(self.tmux.all_typed()), {0x0D})
        self.assertEqual(self.tmux.calls[0][3:], ["Enter"] * 20, "the burst is one call")
        self.assertEqual(self.board.lost, bytearray())

    def test_breakin_lands_in_a_window_of_five_ticks(self) -> None:
        board = SimBoard(self.clock, self.transcript, bootdelay=5)
        self.tmux.board = board
        mark = self.console.mark()
        board.restart("power")
        self.assertEqual(self.loader.wait_for_loader(mark), "countdown")
        self.loader.break_in()
        self.assertEqual(board.mode, Mode.PROMPT)
        self.assertGreaterEqual(board.countdown, 3, "it was stopped, not run out")

    def test_breakin_retries(self) -> None:
        for deaf, rounds in ((40, 2), (80, 3)):
            with self.subTest(deaf=deaf):
                self.tmux.typed.clear()
                self.board.deaf_bytes = deaf
                self.restart_to_countdown()
                self.board.deaf_bytes = deaf
                self.loader.break_in()
                self.assertEqual(self.board.mode, Mode.PROMPT)
                self.assertGreaterEqual(len(self.tmux.typed), rounds)

    def test_breakin_reports_what_the_console_shows_after_three_rounds(self) -> None:
        self.restart_to_countdown()
        self.board.deaf_bytes = 10_000
        with self.assertRaises(BreakInFailed) as caught:
            self.loader.break_in()
        message = str(caught.exception)
        self.assertIn("3 break-in rounds", message)
        self.assertIn("autoboot countdown", message)
        self.assertEqual(set(self.tmux.all_typed()), {0x0D})

    def test_a_window_that_runs_out_during_break_in_is_a_failure_not_a_retry(self) -> None:
        board = SimBoard(self.clock, self.transcript, bootdelay=3)
        self.tmux.board = board
        mark = self.console.mark()
        board.restart("power")
        self.loader.wait_for_loader(mark)
        board.deaf_bytes = 10_000
        with self.assertRaises(BreakInFailed) as caught:
            self.loader.break_in()
        self.assertIn("booted", str(caught.exception))
        self.assertEqual(board.mode, Mode.KERNEL)

    def test_a_reset_during_break_in_is_a_failure(self) -> None:
        self.restart_to_countdown()
        self.board.deaf_bytes = 10_000
        self.clock.schedule(1.0, lambda: self.board.restart("fuse"))
        with self.assertRaises(BreakInFailed):
            self.loader.break_in()

    def test_a_cut_stream_during_break_in_is_an_interruption(self) -> None:
        self.restart_to_countdown()
        self.board.deaf_bytes = 10_000
        self.clock.schedule(1.0, self.board.flap)
        with self.assertRaises(ConsoleInterrupted):
            self.loader.break_in()


class WaitForLoader(LoaderCase):

    def test_the_countdown_opens_1_81_seconds_after_a_restart(self) -> None:
        started = self.clock.now()
        self.restart_to_countdown()
        self.assertAlmostEqual(self.clock.now() - started, 1.81, delta=0.1)

    def test_a_second_reset_restarts_the_wait(self) -> None:
        mark = self.console.mark()
        self.board.restart("first")
        self.clock.schedule(1.0, lambda: self.board.restart("second"))
        started = self.clock.now()
        self.assertEqual(self.loader.wait_for_loader(mark), "countdown")
        self.assertAlmostEqual(self.clock.now() - started, 1.0 + 1.81, delta=0.1)

    def test_a_board_that_goes_straight_to_the_prompt_does_not_hang_the_wait(self) -> None:
        mark = self.console.mark()
        self.board.at_prompt()
        self.assertEqual(self.loader.wait_for_loader(mark), "prompt")

    def test_a_kernel_that_started_on_its_own_is_reported(self) -> None:
        board = SimBoard(self.clock, self.transcript, bootdelay=2)
        self.tmux.board = board
        mark = self.console.mark()
        board.restart("power")
        self.clock.advance(10.0)
        with self.assertRaises(UnexpectedState) as caught:
            self.loader.wait_for_loader(mark)
        self.assertIn("autoboot", str(caught.exception))

    def test_a_board_that_never_answers_times_out_after_120_seconds(self) -> None:
        mark = self.console.mark()
        self.board.power_off()
        started = self.clock.now()
        with self.assertRaises(WaitTimeout):
            self.loader.wait_for_loader(mark)
        self.assertAlmostEqual(self.clock.now() - started, 120.0, delta=0.2)


class Session(LoaderCase):

    def test_clean_line_first(self) -> None:
        self.restart_to_countdown()
        self.loader.break_in()
        # A person left half a line at the prompt.
        self.tmux(["send-keys", "-t", self.SESSION, "-l", "--", "sf er"])
        self.assertEqual(self.board.line, "sf er")
        self.loader.begin_session()
        self.assertEqual(self.board.line, "")
        self.assertIn(b"\x03", self.tmux.typed)
        self.assertLess(self.tmux.typed.index(b"\x03"), self.tmux.typed.index(b"printenv"))
        self.assertEqual(self.loader.command("printenv sw_commit").output, "sw_commit=0")

    def test_the_first_command_waits_until_the_queued_enters_have_printed(self) -> None:
        self.restart_to_countdown()
        self.loader.break_in()
        last_output = self.clock.now()
        self.loader.settle()
        self.assertGreaterEqual(self.clock.now() - last_output, 0.3)

    def test_settle_needs_a_fresh_prompt(self) -> None:
        self.board.show_login()
        with self.assertRaises(WaitTimeout):
            self.loader.settle()
        self.assertEqual(self.tmux.all_typed(), b"\x03", "only Ctrl-C was sent")

    def test_env_parse(self) -> None:
        env = self.reach_and_read()
        self.assertEqual(env["sw_commit"], "0")
        self.assertEqual(env["bootdelay"], "600")
        self.assertEqual(env["ramboot"], self.board.model.env["ramboot"])
        self.assertNotIn("ethaddr", env, "the MAC address is never kept")
        self.assertEqual(self.guard.variables, env)
        self.assertIn("fl_kernel1", env)

    def reach_and_read(self) -> dict[str, str]:
        self.restart_to_countdown()
        self.loader.break_in()
        return self.loader.begin_session()

    def test_the_parser_splits_at_the_first_equals_and_stops_at_the_size_line(self) -> None:
        text = ("a=b=c\nlong=" + "x" * 500 + "\nno equals sign\nethaddr=00:00:5e:00:53:01\n"
                "\nEnvironment size: 10/8187 bytes\nafter=1")
        env = parse_environment(text)
        self.assertEqual(env["a"], "b=c")
        self.assertEqual(len(env["long"]), 500)
        self.assertNotIn("after", env)
        self.assertNotIn("ethaddr", env)

    def test_ethaddr_not_logged(self) -> None:
        self.reach_and_read()
        with self.assertRaises(Refusal):
            self.loader.command("sf erase 440000 +14c000")

        def noisy_hang(args, suffix, depth) -> None:
            self.board.model.print("ethaddr=00:00:5E:00:53:01")
            self.board.model.print("a line with 00:00:5e:00:53:02 in it")
            self.board.model.terminal = "hang"

        self.board.model.cmd_version = noisy_hang  # type: ignore[method-assign]
        with self.assertRaises(WaitTimeout) as caught:
            self.loader.command("version", timeout=2.0)
        everything = "\n".join(self.logged) + str(caught.exception)
        self.assertNotIn("00:00:5e", everything.lower())
        self.assertNotIn("ethaddr", everything)
        self.assertIn("a line with", str(caught.exception))
        self.assertTrue(self.logged, "the log has entries")

    def test_getenv(self) -> None:
        self.reach_and_read()
        self.assertEqual(self.loader.getenv("sw_tryactive"), "2")
        self.assertIsNone(self.loader.getenv("nosuchvariable"))
        self.assertIsNone(self.loader.getenv("ethaddr"))
        self.assertEqual(self.board.executed.count("printenv ethaddr"), 0)

    def test_set_saved_sets_saves_and_reads_back(self) -> None:
        self.reach_and_read()
        self.loader.set_saved(("sw_tryactive", "0"))
        self.assertEqual(self.board.model.env["sw_tryactive"], "0")
        self.assertEqual(self.board.valid_environment, 2)
        self.assertEqual(self.guard.session.pending, [])
        self.assertEqual(self.guard.variables["sw_tryactive"], "0")
        self.loader.set_saved(("sw_tryactive", "2"))
        self.assertEqual(self.board.valid_environment, 1)
        executed = [line for line in self.board.executed if line != "printenv"]
        self.assertIn("setenv sw_tryactive 0", executed)
        self.assertIn("saveenv", executed)

    def test_set_saved_through_the_guard_refuses_a_bank_one_trial(self) -> None:
        self.reach_and_read()
        executed = list(self.board.executed)
        with self.assertRaises(Refusal):
            self.loader.set_saved(("sw_tryactive", "1"))
        self.assertEqual(self.board.executed, executed)

    def test_saveenv_must_report_a_valid_environment(self) -> None:
        self.reach_and_read()
        self.board.model.cmd_saveenv = lambda a, s, d: None  # type: ignore[method-assign]
        self.loader.command("setenv sw_tryactive 0")
        with self.assertRaises(LoaderError):
            self.loader.save_environment()

    def test_crc32_is_parsed_from_the_loader_line(self) -> None:
        self.reach_and_read()
        self.assertEqual(self.loader.crc32(0x80000000, 0x100), 0)
        self.assertIn("crc32 80000000 100", self.board.executed)


class Typing(LoaderCase):

    def setUp(self) -> None:
        super().setUp()
        self.reach_prompt()

    def test_a_command_and_its_output(self) -> None:
        result = self.loader.command("printenv sw_commit")
        self.assertEqual(result.output, "sw_commit=0")
        self.assertEqual(result.line, "printenv sw_commit")
        self.assertTrue(self.loader.at_prompt())

    def test_the_typed_line_is_the_approved_expanded_text(self) -> None:
        self.board.model.env["x"] = "sw_commit"
        self.loader.read_environment()  # the copy that typed lines expand from
        self.assertEqual(self.loader.command("printenv   ${x}").output, "sw_commit=0")
        self.assertIn("printenv sw_commit", self.board.executed)

    def test_a_typed_line_expands_from_the_copy_read_at_the_start_of_the_session(self) -> None:
        self.board.model.env["x"] = "sw_commit"  # set after the copy was read
        with self.assertRaises(Refusal) as caught:
            self.loader.command("printenv ${x}")
        self.assertEqual(caught.exception.code, "unknown-variable")
        self.loader.read_environment()
        self.assertEqual(self.loader.command("printenv ${x}").output, "sw_commit=0")

    def test_an_empty_line_is_allowed_and_prints_a_fresh_prompt(self) -> None:
        result = self.loader.command("")
        self.assertEqual(result.output, "")
        self.assertEqual(self.board.executed.count(""), 0)

    def test_refusal_sends_nothing(self) -> None:
        typed = len(self.tmux.typed)
        executed = list(self.board.executed)
        size = self.transcript.size()
        for line in ("sf erase 440000 +14c000", "mw.l 94440000 0", "run yu", "go 94440000",
                     "printenv `reset`", "setenv sw_tryactive 1"):
            with self.subTest(line=line), self.assertRaises(Refusal):
                self.loader.command(line)
        self.assertEqual(len(self.tmux.typed), typed)
        self.assertEqual(self.board.executed, executed)
        self.assertEqual(self.transcript.size(), size, "the console shows no trace of the line")

    def test_refusal_format(self) -> None:
        with self.assertRaises(Refusal) as caught:
            self.loader.command("sf erase 440000 +14c000")
        self.assertEqual(str(caught.exception),
                         "refused [protected-range] k1: sf erase 440000 +14c000")
        self.assertEqual(caught.exception.exit_status, 3)

    def test_echo_mismatch_never_executes(self) -> None:
        """A character lost, doubled or changed on its way in cancels the line."""
        line = "sf erase 80000 +1d0000"
        faults = {"dropped": lambda ch: "" if ch == "d" else ch,
                  "doubled": lambda ch: ch * 2 if ch == "1" else ch,
                  "substituted": lambda ch: "c" if ch == "d" else ch}
        for name, fault in faults.items():
            with self.subTest(name):
                executed = list(self.board.executed)
                self.board.input_fault = fault
                with self.assertRaises(con.EchoMismatch):
                    self.loader.command(line)
                self.board.input_fault = None
                self.assertEqual(self.board.executed, executed)
                self.assertEqual(self.board.model.ops, [])
                self.assertEqual(self.board.line, "", "Ctrl-C cleared the line")
                self.assertTrue(self.loader.at_prompt())
                self.assertEqual(self.loader.command("printenv sw_commit").output,
                                 "sw_commit=0")

    def test_a_command_is_never_typed_where_the_prompt_is_not(self) -> None:
        self.board.show_login()
        typed = len(self.tmux.typed)
        with self.assertRaises(NotAtPrompt):
            self.loader.command("printenv")
        self.assertEqual(len(self.tmux.typed), typed)
        self.assertEqual(bytes(self.board.typed_at_login), b"")

    def test_never_types_at_login(self) -> None:
        self.board.show_login()
        for line in ("", "version", "reset", "run ramboot"):
            with self.subTest(line=line), self.assertRaises(NotAtPrompt):
                self.loader.command(line)
        self.assertEqual(bytes(self.board.typed_at_login), b"")

    def test_a_command_is_never_typed_into_a_countdown(self) -> None:
        self.restart_to_countdown()
        with self.assertRaises(NotAtPrompt):
            self.loader.command("version")
        self.assertEqual(set(self.tmux.all_typed()[-1:]), {0x0D} | set(self.tmux.all_typed()[-1:]))
        self.assertNotIn(b"version", self.tmux.all_typed())

    def test_the_preflight_reads_the_macro_as_the_board_has_it_now(self) -> None:
        self.loader.command("run sdsup")
        self.board.model.env["sdsup"] = "mw.l 94440000 0"
        executed = list(self.board.executed)
        with self.assertRaises(Refusal) as caught:
            self.loader.command("run sdsup")
        self.assertEqual(caught.exception.code, "flash-aperture")
        self.assertEqual([x for x in self.board.executed if x != "printenv sdsup"],
                         [x for x in executed if x != "printenv sdsup"])
        self.assertIn("printenv sdsup", self.board.executed)

    def test_the_boot_chain_is_read_live_before_boot(self) -> None:
        self.board.model.env["en_wdt"] = "mw 94440000 0"
        executed = list(self.board.executed)
        with self.assertRaises(Refusal) as caught:
            self.loader.command("boot")
        self.assertEqual(caught.exception.code, "flash-aperture")
        self.assertFalse([x for x in self.board.executed if x == "boot"])
        self.assertTrue(len(self.board.executed) > len(executed), "it read the macros")

    def test_a_trial_for_bank_one_that_appears_behind_the_tools_back_refuses_boot(self) -> None:
        self.board.model.env["sw_tryactive"] = "1"
        with self.assertRaises(Refusal) as caught:
            self.loader.command("boot")
        self.assertEqual(caught.exception.code, "bank1-trial")

    def test_an_unknown_command_is_a_failure_with_the_loaders_words(self) -> None:
        class Permissive(Guard):
            def check(self, line: str, *, macros: bool = True) -> Approval:
                return Approval(line, line, line.split()[0] if line.split() else "")

        loader = Loader(self.console, Permissive(), self.clock, self.console.log)
        with self.assertRaises(CommandFailed) as caught:
            loader.command("un ramboot")
        self.assertIn("Unknown command 'un'", str(caught.exception))

    def test_a_reset_in_the_middle_of_a_command(self) -> None:
        self.loader.command("setenv sw_tryactive 0")
        self.clock.schedule(0.2, lambda: self.board.restart("fuse"))
        with self.assertRaises(UnexpectedReset) as caught:
            self.loader.command("saveenv", timeout=15.0)
        self.assertIn("saveenv", str(caught.exception))

    def test_a_cut_stream_in_the_middle_of_a_command(self) -> None:
        self.loader.command("setenv sw_tryactive 0")
        self.clock.schedule(0.2, self.board.flap)
        with self.assertRaises(ConsoleInterrupted):
            self.loader.command("saveenv", timeout=15.0)

    def test_a_stalled_command_ends_after_the_idle_limit(self) -> None:
        self.board.model.cmd_version = lambda a, s, d: setattr(  # type: ignore[method-assign]
            self.board.model, "terminal", "hang")
        started = self.clock.now()
        with self.assertRaises(WaitTimeout) as caught:
            self.loader.command("version", timeout=600.0, idle=30.0)
        self.assertAlmostEqual(self.clock.now() - started, 30.0, delta=1.0)
        self.assertIn("version", str(caught.exception))

    def test_start_returns_a_mark_for_a_command_that_never_returns_to_the_prompt(self) -> None:
        approval, mark = self.loader.start("reset")
        self.assertEqual(approval.text, "reset")
        seen = con.wait(self.console, self.clock, mark,
                        [con.pattern("banner", r"(?m)^PRELOADER")], 5.0)
        self.assertEqual(seen.label, "banner")

    def test_the_log_records_typed_lines_and_outcomes(self) -> None:
        self.loader.command("version")
        self.assertTrue(any(" type version" in line for line in self.logged))
        self.assertTrue(any(" done version in " in line for line in self.logged))
