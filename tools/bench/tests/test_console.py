# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The shared console: typing, reading by offset, waiting and checking it is usable."""

from __future__ import annotations

import os
from unittest import mock

from bench import console as con
from bench.console import (BoardState, Console, ConsoleBusy, ConsoleConfig, ConsoleInterrupted,
                           ConsoleUnusable, EchoMismatch, ForbiddenTmuxCall, ToolLog,
                           WaitTimeout)

from .support import ConsoleCase, TempDirTestCase


class Typing(ConsoleCase):

    def test_text_is_typed_literally_and_enter_is_a_separate_call(self) -> None:
        self.console.type_text("-dash x")
        self.console.press("Enter")
        self.assertEqual(self.tmux.calls[0],
                         ["send-keys", "-t", "uart-wifi-1", "-l", "--", "-dash x"])
        self.assertEqual(self.tmux.calls[1], ["send-keys", "-t", "uart-wifi-1", "Enter"])
        self.assertEqual(self.tmux.all_typed(), b"-dash x\r")

    def test_a_trailing_semicolon_is_refused_before_tmux_can_drop_it(self) -> None:
        with self.assertRaises(ConsoleUnusable):
            self.console.type_text("semi;")
        self.console.type_text("a;b")
        self.assertEqual(self.tmux.all_typed(), b"a;b")

    def test_only_printable_ascii_is_typed(self) -> None:
        for text in ("a\nb", "a\x03", "caf\u00e9", "tab\there"):
            with self.subTest(text=text), self.assertRaises(ConsoleUnusable):
                self.console.type_text(text)
        self.assertEqual(self.tmux.calls, [])

    def test_only_enter_and_control_c_are_pressed(self) -> None:
        for key in ("Tab", "C-d", "Escape", "enter", "C-]"):
            with self.subTest(key=key), self.assertRaises(ConsoleUnusable):
                self.console.press(key)
        self.console.press("C-c")
        self.assertEqual(self.tmux.all_typed(), b"\x03")

    def test_a_burst_of_enters_is_one_call(self) -> None:
        self.console.press("Enter", count=20)
        self.assertEqual(self.tmux.calls, [["send-keys", "-t", "uart-wifi-1"] + ["Enter"] * 20])
        self.console.press("Enter", count=0)
        self.assertEqual(len(self.tmux.calls), 1)

    def test_empty_text_types_nothing(self) -> None:
        self.console.type_text("")
        self.assertEqual(self.tmux.calls, [])

    def test_a_dead_pane_is_reported_with_the_cure_for_a_person(self) -> None:
        self.tmux.pane_dead = True
        with self.assertRaises(ConsoleUnusable) as caught:
            self.console.type_text("x")
        self.assertIn("respawn-pane", str(caught.exception))
        self.assertIn("remain-on-exit", str(caught.exception))
        self.assertFalse(self.console.alive())


class Reading(ConsoleCase):

    def test_console_offsets(self) -> None:
        """A prompt printed before the mark does not satisfy a wait."""
        self.transcript.emit("old output\r\r\n9601D# ")
        mark = self.console.mark()
        self.assertEqual(mark, self.transcript.size())
        self.assertEqual(self.console.since(mark), "")
        self.transcript.emit("new\r\r\nmore")
        self.assertEqual(self.console.since(mark), "new\nmore")
        with self.assertRaises(WaitTimeout):
            con.wait(self.console, self.clock, mark, [con.pattern("prompt", r"9601D# ")], 1.0)
        self.transcript.emit("\r\r\n9601D# ")
        seen = con.wait(self.console, self.clock, mark, [con.pattern("prompt", r"9601D# $")], 1.0)
        self.assertEqual(seen.label, "prompt")

    def test_the_transcript_is_decoded_as_latin_1_without_carriage_returns(self) -> None:
        self.transcript.emit(b"a\r\r\nb\xe9\x08\x08\x08 9 ")
        self.assertEqual(self.console.since(0), "a\nb\u00e9\x08\x08\x08 9 ")
        self.assertEqual(con.fields("599 \x08\x08\x08598 "), ["599 ", "598 "])

    def test_since_reads_only_what_follows_the_mark(self) -> None:
        self.transcript.emit(b"x" * 3_000_000)
        mark = self.console.mark()
        self.transcript.emit("tail")
        sizes: list[int] = []
        real_open = open

        def counting_open(path, mode="r", *args, **kwargs):
            handle = real_open(path, mode, *args, **kwargs)
            real_read = handle.read

            def read(*a):
                data = real_read(*a)
                sizes.append(len(data))
                return data

            handle.read = read  # type: ignore[method-assign]
            return handle

        with mock.patch("bench.console.open", counting_open, create=True):
            self.assertEqual(self.console.since(mark), "tail")
        self.assertEqual(sizes, [4])

    def test_a_shorter_transcript_is_an_error_not_an_empty_answer(self) -> None:
        self.transcript.emit("abc")
        mark = self.console.mark()
        with open(self.transcript.path, "wb"):
            pass
        with self.assertRaises(ConsoleUnusable):
            self.console.since(mark)

    def test_a_missing_transcript_is_unusable(self) -> None:
        os.unlink(self.transcript.path)
        with self.assertRaises(ConsoleUnusable):
            self.console.mark()
        with self.assertRaises(ConsoleUnusable):
            self.console.since(0)

    def test_the_tool_never_writes_the_transcript(self) -> None:
        self.transcript.emit("abc")
        before = os.stat(self.transcript.path)
        self.console.mark()
        self.console.since(0)
        self.console.tail()
        after = os.stat(self.transcript.path)
        self.assertEqual((before.st_size, before.st_mtime_ns), (after.st_size, after.st_mtime_ns))


class Waiting(ConsoleCase):

    def test_matchers_are_tried_in_order_and_report_their_label(self) -> None:
        mark = self.console.mark()
        self.transcript.emit("Hit any key to stop autoboot: 5 ")
        seen = con.wait(self.console, self.clock, mark,
                        [con.pattern("prompt", r"9601D# $"),
                         con.pattern("countdown", r"Hit any key")], 1.0)
        self.assertEqual(seen.label, "countdown")
        self.assertIn("autoboot", seen.text)

    def test_a_timeout_reports_the_tail_without_a_mac_address(self) -> None:
        mark = self.console.mark()
        self.transcript.emit("ethaddr=00:00:5e:00:53:01\r\r\nnothing useful 00:00:5E:00:53:02\r\r\n")
        with self.assertRaises(WaitTimeout) as caught:
            con.wait(self.console, self.clock, mark, [con.pattern("x", "never")], 3.0, what="x")
        message = str(caught.exception)
        self.assertNotIn("00:00:5e", message.lower())
        self.assertNotIn("ethaddr", message)
        self.assertIn("nothing useful", message)
        self.assertGreaterEqual(caught.exception.waited, 3.0)

    def test_the_wait_runs_on_the_virtual_clock(self) -> None:
        mark = self.console.mark()
        before = self.clock.now()
        with self.assertRaises(WaitTimeout):
            con.wait(self.console, self.clock, mark, [con.pattern("x", "never")], 120.0)
        self.assertAlmostEqual(self.clock.now() - before, 120.0, delta=0.1)

    def test_output_that_arrives_during_the_wait_is_seen(self) -> None:
        mark = self.console.mark()
        self.clock.schedule(7.0, lambda: self.transcript.emit("ready"))
        seen = con.wait(self.console, self.clock, mark, [con.pattern("ready", "ready")], 30.0)
        self.assertAlmostEqual(seen.elapsed, 7.0, delta=0.1)

    def test_a_stall_ends_the_wait_without_waiting_for_the_limit(self) -> None:
        mark = self.console.mark()
        for second in range(1, 20):
            self.clock.schedule(second, lambda: self.transcript.emit("."))
        with self.assertRaises(WaitTimeout) as caught:
            con.wait(self.console, self.clock, mark, [con.pattern("x", "never")], 600.0,
                     idle=30.0, what="progress")
        self.assertAlmostEqual(caught.exception.waited, 19 + 30, delta=1.0)

    def test_console_flap_aborts_step(self) -> None:
        mark = self.console.mark()
        self.clock.schedule(2.0, lambda: self.transcript.emit(
            "\r\r\n--- console: the bridge closed the connection at 12:00:01 ---\r\r\n"))
        with self.assertRaises(ConsoleInterrupted) as caught:
            con.wait(self.console, self.clock, mark, [con.pattern("x", "never")], 30.0,
                     what="the transfer")
        self.assertIn("bridge closed", str(caught.exception))
        self.assertEqual(con.interruption("--- console: link failed at 01:02:03 ---"),
                         "link failed")
        self.assertEqual(con.interruption("--- console: reconnecting in 4 s at 01:02:03 ---"),
                         "reconnecting in 4 s")

    def test_a_connected_marker_alone_is_not_an_interruption(self) -> None:
        mark = self.console.mark()
        self.transcript.emit("--- console: connected at 12:00:01 ---\r\r\nready")
        seen = con.wait(self.console, self.clock, mark, [con.pattern("r", "ready")], 5.0)
        self.assertEqual(seen.label, "r")
        self.assertIsNone(con.interruption("--- console: connected at 12:00:01 ---"))

    def test_a_marker_before_the_mark_is_not_an_interruption(self) -> None:
        self.transcript.emit("--- console: link failed at 12:00:01 ---\r\r\n")
        mark = self.console.mark()
        self.transcript.emit("ok")
        seen = con.wait(self.console, self.clock, mark, [con.pattern("ok", "ok")], 5.0)
        self.assertEqual(seen.label, "ok")

    def test_wait_quiet_returns_when_nothing_new_prints(self) -> None:
        for step in range(5):
            self.clock.schedule(0.1 * step, lambda: self.transcript.emit("9601D# \r\r\n"))
        started = self.clock.now()
        con.wait_quiet(self.console, self.clock, 0.3, 5.0)
        self.assertAlmostEqual(self.clock.now() - started, 0.4 + 0.3, delta=0.1)

    def test_wait_quiet_times_out_on_a_chatty_console(self) -> None:
        for step in range(200):
            self.clock.schedule(0.05 * step, lambda: self.transcript.emit("."))
        with self.assertRaises(WaitTimeout):
            con.wait_quiet(self.console, self.clock, 0.3, 2.0)


class TypeLine(ConsoleCase):

    def test_a_line_is_typed_checked_and_only_then_sent(self) -> None:
        mark = con.type_line(self.console, self.clock, "printenv sw_commit")
        self.assertEqual(self.tmux.typed[0], b"printenv sw_commit")
        self.assertEqual(self.tmux.typed[1], b"\r")
        self.assertEqual(self.console.since(mark), "\n9601D# ")

    def test_an_empty_line_is_just_enter(self) -> None:
        con.type_line(self.console, self.clock, "")
        self.assertEqual(self.tmux.typed, [b"\r"])

    def test_echo_mismatch_never_executes(self) -> None:
        faults = {
            "dropped": lambda ch: "" if ch == "x" else ch,
            "doubled": lambda ch: ch * 2 if ch == "x" else ch,
            "substituted": lambda ch: "y" if ch == "x" else ch,
        }
        for name, fault in faults.items():
            with self.subTest(name):
                self.tmux.typed.clear()
                self.pane.echo = fault
                with self.assertRaises(EchoMismatch):
                    con.type_line(self.console, self.clock, "sf erase 1x0000 +1000")
                self.assertEqual(self.tmux.typed[-1], b"\x03", "the line is cancelled")
                self.assertNotIn(b"\r", self.tmux.all_typed(), "Enter is never sent")

    def test_a_cut_stream_while_typing_is_an_interruption(self) -> None:
        self.pane.echo = lambda ch: "\r\r\n--- console: link failed at 12:00:01 ---\r\r\n"
        with self.assertRaises(ConsoleInterrupted):
            con.type_line(self.console, self.clock, "version")
        self.assertNotIn(b"\r", self.tmux.all_typed())


class Classify(ConsoleCase):

    def test_console_classify(self) -> None:
        cases = [
            ("\n9601D# ", BoardState.LOADER_PROMPT),
            ("9601D# \n9601D# ", BoardState.LOADER_PROMPT),
            ("U-Boot 2011.12.NA\nHit any key to stop autoboot:  5 ", BoardState.COUNTDOWN),
            ("Hit any key to stop autoboot: 600 \x08\x08\x08599 ", BoardState.COUNTDOWN),
            ("\n9601D\nPRELOADER <name> <version>\nII: x", BoardState.RESET),
            ("PRELOADER x\nHit any key to stop autoboot: 3 ", BoardState.COUNTDOWN),
            ("Hit any key to stop autoboot:  0 \n9601D# \n9601D# ", BoardState.LOADER_PROMPT),
            ("\nStarting kernel ...\n", BoardState.KERNEL_STARTING),
            ("[    0.000000] Linux version 6.18.48 (x)\n", BoardState.KERNEL_STARTING),
            ("Starting kernel ...\n[ 2.0] x\nlogin: ", BoardState.LOGIN),
            ("\nPassword: ", BoardState.LOGIN),
            ("\nlogin: ", BoardState.LOGIN),
            ("Linux version 1\nsysrq: Resetting\nPRELOADER x\n", BoardState.RESET),
            ("some shell output\n# ", BoardState.OTHER),
            ("", BoardState.OTHER),
            ("9601D\n", BoardState.OTHER),  # the bare 9601D is not the prompt
        ]
        for text, state in cases:
            with self.subTest(text=text):
                self.assertEqual(con.classify(text), state)

    def test_the_preloader_marker_is_a_line_start(self) -> None:
        self.assertEqual(con.classify("see PRELOADER here"), BoardState.OTHER)


class Usable(ConsoleCase):

    def test_console_usable(self) -> None:
        check = self.console.check_usable()
        self.assertEqual(check.state, BoardState.LOADER_PROMPT)
        self.assertEqual(self.tmux.all_typed(), b"\r", "only an empty line is typed")
        self.assertEqual(self.tmux.subcommands(),
                         ["has-session", "display-message", "send-keys"])

    def test_no_session(self) -> None:
        self.tmux.session_exists = False
        with self.assertRaises(ConsoleUnusable) as caught:
            self.console.check_usable()
        self.assertIn("no tmux session", str(caught.exception))
        self.assertEqual(self.tmux.typed, [])

    def test_a_dead_pane_is_not_a_console(self) -> None:
        self.tmux.pane_dead = True
        with self.assertRaises(ConsoleUnusable) as caught:
            self.console.check_usable()
        self.assertIn("pane_dead", str(caught.exception))
        self.assertEqual(self.tmux.typed, [])

    def test_an_unpiped_pane_is_not_a_console(self) -> None:
        self.tmux.pane_pipe = False
        with self.assertRaises(ConsoleUnusable) as caught:
            self.console.check_usable()
        self.assertIn("pane_pipe", str(caught.exception))
        self.assertEqual(self.tmux.typed, [])

    def test_a_missing_transcript(self) -> None:
        os.unlink(self.transcript.path)
        with self.assertRaises(ConsoleUnusable):
            self.console.check_usable()
        self.assertEqual(self.tmux.typed, [])

    def test_a_silent_console_is_unusable_not_proof_of_anything_else(self) -> None:
        self.pane.mode = "silent"
        started = self.clock.now()
        with self.assertRaises(ConsoleUnusable) as caught:
            self.console.check_usable()
        self.assertIn("silent", str(caught.exception))
        self.assertAlmostEqual(self.clock.now() - started, 2.0, delta=0.2)

    def test_the_login_prompt_is_reported_and_nothing_but_enter_is_typed(self) -> None:
        self.pane.mode = "login"
        check = self.console.check_usable()
        self.assertEqual(check.state, BoardState.LOGIN)
        self.assertEqual(self.tmux.typed, [b"\r"])

    def test_never_types_at_login(self) -> None:
        """Whatever the board shows, the check sends an empty line and no more."""
        for mode in ("prompt", "login", "silent"):
            with self.subTest(mode):
                self.tmux.typed.clear()
                self.pane.mode = mode
                try:
                    self.console.check_usable()
                except ConsoleUnusable:
                    pass
                self.assertEqual(self.tmux.all_typed(), b"\r")

    def test_a_countdown_is_classified_after_the_empty_line(self) -> None:
        self.pane.mode = "silent"
        self.clock.schedule(0.0, lambda: None)
        original = self.pane.receive

        def countdown(data: bytes) -> None:
            original(data)
            self.transcript.emit("\r\r\nHit any key to stop autoboot: 599 ")

        self.pane.receive = countdown  # type: ignore[method-assign]
        self.assertEqual(self.console.check_usable().state, BoardState.COUNTDOWN)

    def test_the_log_records_what_was_probed(self) -> None:
        self.console.check_usable()
        self.assertTrue(any("probe" in line for line in self.logged))


class NeverCaptures(ConsoleCase):

    def test_never_captures_pane(self) -> None:
        """Every console operation runs against a tmux double that fails on capture-pane."""
        self.console.check_usable()
        self.console.type_text("version")
        self.console.press("Enter")
        self.console.mark()
        self.console.since(0)
        self.console.alive()
        self.console.tail()
        with self.console.released_pane():
            pass
        used = set(self.tmux.subcommands())
        self.assertTrue(used <= {"has-session", "send-keys", "display-message", "pipe-pane",
                                 "respawn-pane", "set-option"}, used)

    def test_the_double_does_fail_on_a_capture(self) -> None:
        for call in (["capture-pane", "-p", "-t", "uart-wifi-1"], ["kill-session", "-t", "x"],
                     ["kill-server"], ["pipe-pane", "-o", "-t", "x", "cat"]):
            with self.subTest(call=call), self.assertRaises(AssertionError):
                self.tmux(call)

    def test_the_console_refuses_the_calls_itself(self) -> None:
        for call in (("capture-pane", "-p"), ("kill-session",), ("kill-server",),
                     ("pipe-pane", "-o", "-t", "x", "cat"), ("new-session", "-d")):
            with self.subTest(call=call), self.assertRaises(ForbiddenTmuxCall):
                self.console._tmux(*call)
        self.assertEqual(self.tmux.calls, [])

    def test_the_default_server_is_used_unless_a_socket_is_configured(self) -> None:
        self.console.has_session()
        self.assertNotIn("-L", self.tmux.calls[0])
        seen: list[list[str]] = []
        other = Console(ConsoleConfig(session="s", transcript=self.transcript.path,
                                      tmux_socket="private"),
                        lambda args: seen.append(list(args)) or con.TmuxResult(0), self.clock)
        other.has_session()
        self.assertEqual(seen[0][:2], ["-L", "private"])


class Exclusive(ConsoleCase):

    def test_second_instance_refuses(self) -> None:
        second = Console(self.config, self.tmux, self.clock)
        with self.console.exclusive("ramboot"):
            with self.assertRaises(ConsoleBusy) as caught:
                with second.exclusive("flash"):
                    self.fail("the second instance must not get the console")
            self.assertIn("ramboot", str(caught.exception))
            self.assertIn("pid %d" % os.getpid(), str(caught.exception))
        with second.exclusive("flash"):
            pass

    def test_the_lock_is_a_file_next_to_the_transcript(self) -> None:
        with self.console.exclusive("x"):
            self.assertTrue(os.path.exists(self.transcript.path + ".lock"))

    def test_a_busy_console_is_an_unusable_console(self) -> None:
        self.assertTrue(issubclass(ConsoleBusy, ConsoleUnusable))
        self.assertEqual(ConsoleBusy.exit_status, 4)


class Releasing(ConsoleCase):

    def test_the_pane_is_released_and_restored(self) -> None:
        with self.console.released_pane() as start:
            self.assertEqual(start, "bridge-client 192.0.2.1 2001")
            self.assertNotEqual(self.tmux.start_command, "bridge-client 192.0.2.1 2001")
            self.assertEqual(len(self.tmux.respawns), 1)
        self.assertEqual(self.tmux.respawns[-1], "bridge-client 192.0.2.1 2001")
        self.assertEqual(self.tmux.subcommands(),
                         ["display-message", "respawn-pane", "respawn-pane", "set-option",
                          "display-message", "pipe-pane"])
        self.assertTrue(self.tmux.pane_pipe)
        for call in self.tmux.calls:
            if call[0] == "respawn-pane":
                self.assertEqual(call[:4], ["respawn-pane", "-k", "-t", "uart-wifi-1"])

    def test_the_pane_is_restored_even_when_the_body_fails(self) -> None:
        with self.assertRaises(RuntimeError):
            with self.console.released_pane():
                raise RuntimeError("the transfer failed")
        self.assertEqual(self.tmux.respawns[-1], "bridge-client 192.0.2.1 2001")

    def test_without_a_start_command_nothing_is_stopped(self) -> None:
        self.tmux.start_command = ""
        with self.assertRaises(ConsoleUnusable):
            with self.console.released_pane():
                self.fail("must not release a pane that cannot be restored")
        self.assertEqual(self.tmux.respawns, [])


class Redaction(TempDirTestCase):

    def test_redact_drops_the_ethaddr_line_and_any_mac(self) -> None:
        text = "bootdelay=5\nethaddr=00:00:5E:00:53:01\nx 00-00-5e-00-53-02 y\nok"
        out = con.redact(text)
        self.assertEqual(out, "bootdelay=5\nx xx:xx:xx:xx:xx:xx y\nok")

    def test_the_log_never_holds_a_mac_address(self) -> None:
        lines: list[str] = []
        log = ToolLog(lines.append, wall=lambda: __import__("datetime").datetime(
            2026, 10, 1, 12, 0, 0).astimezone())
        log.event("typed", "setenv x 00:00:5e:00:53:03")
        self.assertEqual(len(lines), 1)
        self.assertNotIn("00:00:5e", lines[0])
        self.assertRegex(lines[0], r"^2026-10-01T12:00:00\.000[+-]\d\d:\d\d typed setenv x ")

    def test_a_log_without_a_sink_is_silent(self) -> None:
        ToolLog().event("typed", "x")

    def test_the_log_can_write_to_a_stream(self) -> None:
        import io
        stream = io.StringIO()
        ToolLog(stream).event("wait", "prompt after 1.2 s")
        self.assertIn("wait prompt after 1.2 s", stream.getvalue())


class Config(TempDirTestCase):

    def test_the_default_transcript_is_a_per_user_file_named_after_the_session(self) -> None:
        with mock.patch.dict(os.environ, {"XDG_STATE_HOME": self.tmp}):
            config = ConsoleConfig(session="uart-wifi-2")
        self.assertEqual(config.transcript, os.path.join(self.tmp, "bench", "uart-wifi-2.log"))

    def test_an_explicit_transcript_wins(self) -> None:
        self.assertEqual(ConsoleConfig(transcript="/x/y.log").transcript, "/x/y.log")
