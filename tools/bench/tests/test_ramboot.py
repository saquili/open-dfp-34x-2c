# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""RAM boot against the scripted board: the good run, the observed failures, the refusals."""

from __future__ import annotations

import os

from bench import console as con
from bench import image
from bench import ramboot as rb
from bench.console import ConsoleInterrupted, UnexpectedState, WaitTimeout
from bench.guard import Refusal
from bench.image import ImageError
from bench.ramboot import (AttemptFailed, MissingVariable, RamBootConfig, RamBootError,
                           WrongKernel)

from .images import make_uimage
from .simboard import Mode
from .simnet import BenchCase


class RamBootCase(BenchCase):
    """A board running Linux, an image host and the ssh route between them."""

    def setUp(self) -> None:
        super().setUp()
        self.config = self.boot_config
        self.image_path = os.path.join(self.tmp, "uImage")
        self.write_image(self.kernel_image)

    def write_image(self, data: bytes) -> None:
        with open(self.image_path, "wb") as handle:
            handle.write(data)

    def go(self):
        return rb.ramboot(self.ctx, self.image_path, self.config, self.server_source)

    def board_ops(self) -> set[str]:
        return {op.kind for op in self.board.model.ops}


class GoodRun(RamBootCase):

    def test_a_ramboot_from_a_running_kernel(self) -> None:
        started = self.clock.now()
        result = self.go()
        self.assertEqual(result.attempts, 1)
        self.assertEqual(result.transferred, len(self.kernel_image))
        self.assertEqual(result.image.name, "Linux-6.18.48-test")
        self.assertEqual(self.board.kernels_started, ["Linux-6.18.48-test"])
        self.assertEqual(self.board.mode, Mode.LINUX)
        self.assertEqual(self.board.restarts, ["sysrq"])
        self.assertLess(self.clock.now() - started, 40.0, "about thirty seconds, not minutes")
        self.assertTrue(self.transcript.text().rstrip().endswith("login:"))

    def test_the_tool_typed_only_what_the_guard_allows_and_wrote_no_flash(self) -> None:
        self.go()
        executed = set(self.board.executed)
        self.assertIn("run ramboot", executed)
        names = {line.split()[0] for line in executed if line}
        self.assertLessEqual(names, {"printenv", "run", "boot"})
        self.assertLessEqual(self.board_ops(), {"store", "exec"})
        self.assertEqual(bytes(self.board.lost_busy), b"", "nothing was typed while busy")

    def test_only_enters_were_typed_while_the_board_was_not_at_the_prompt(self) -> None:
        self.go()
        self.assertLessEqual(set(self.board.lost), {0x0D})
        self.assertEqual(bytes(self.board.typed_at_login), b"\r", "one empty line only")

    def test_the_restart_goes_over_the_control_address_first(self) -> None:
        self.go()
        self.assertEqual(self.ssh.started, ["root@198.51.100.2"[5:]])
        argv = self.runner.spawned[0].argv
        self.assertEqual(argv[-1], "echo b > /proc/sysrq-trigger")
        self.assertIn("ConnectTimeout=6", argv)
        self.assertTrue(self.runner.spawned[0].terminated, "the client is ended by the tool")

    def test_the_data_address_is_the_second_choice(self) -> None:
        self.ssh.reachable = {"192.0.2.2"}
        self.go()
        self.assertEqual(self.ssh.started, ["198.51.100.2", "192.0.2.2"])
        self.assertEqual(self.board.kernels_started, ["Linux-6.18.48-test"])

    def test_the_image_host_is_prepared_before_anything_is_typed(self) -> None:
        self.go()
        self.assertTrue(os.path.isfile(os.path.join(self.served, "uImage")))
        self.assertIn(69, self.runner.host.listening)
        self.assertEqual(len(self.runner.host.rules), 1)
        self.assertIn("ss", self.runner.programs())

    def test_a_board_already_at_the_prompt_is_not_restarted(self) -> None:
        self.board.at_prompt()
        started = self.clock.now()
        self.go()
        self.assertEqual(self.runner.spawned, [])
        self.assertEqual(self.board.restarts, [])
        self.assertLess(self.clock.now() - started, 15.0)

    def test_a_board_in_its_countdown_is_broken_into(self) -> None:
        self.board.restart("power")
        self.clock.advance(3.0)
        self.go()
        self.assertEqual(self.runner.spawned, [])
        self.assertEqual(self.board.kernels_started, ["Linux-6.18.48-test"])

    def test_a_kernel_that_is_still_starting_is_given_time_to_settle(self) -> None:
        self.board.start_kernel("Linux-previous")
        self.go()
        self.assertTrue(any("still starting" in line for line in self.said))
        self.assertEqual(self.board.kernels_started[-1], "Linux-6.18.48-test")
        self.assertEqual(self.board.restarts, ["sysrq"])

    def test_a_board_that_just_reset_is_waited_for(self) -> None:
        self.board.restart("power")
        self.go()
        self.assertEqual(self.runner.spawned, [])

    def test_the_ready_marker_is_the_callers(self) -> None:
        self.config.ready_pattern = "Welcome to the bench"
        self.go()
        self.board.ready_text = "login: "
        self.board.boot_linux()
        self.config.ready_pattern = "never printed"
        self.config.ready_limit = 20.0
        with self.assertRaises(RamBootError) as caught:
            self.go()
        self.assertIn("ready marker 'never printed'", str(caught.exception))

    def test_a_ready_marker_from_before_the_banner_does_not_count(self) -> None:
        """The board shows 'login:' before the new kernel starts; that is not readiness."""
        self.board.kernel_mode = "silent"
        with self.assertRaises(RamBootError) as caught:
            self.go()
        self.assertIn("Starting kernel", str(caught.exception))
        self.assertIn("kernel stopped before its console", str(caught.exception))

    def test_the_kernel_banner_without_the_marker_is_not_success(self) -> None:
        self.board.ready_text = "no marker here" + "\r\r\n"
        self.config.ready_limit = 30.0
        with self.assertRaises(RamBootError) as caught:
            self.go()
        self.assertIn("did not appear in time", str(caught.exception))

    def test_the_run_is_logged_without_the_environment(self) -> None:
        self.go()
        text = "\n".join(self.logged)
        self.assertIn("ramboot attempt 1", text)
        self.assertNotIn("00:00:5e", text.lower())
        self.assertNotIn("ethaddr", text)


class ObservedFailures(RamBootCase):

    def setUp(self) -> None:
        super().setUp()
        self.board.fail_first_after_sysrq = True

    def test_ramboot_recovers_after_fuse_reset(self) -> None:
        """The first attempt after a SysRq restart shows only T marks, then the fuse resets."""
        started = self.clock.now()
        result = self.go()
        self.assertEqual(result.attempts, 2)
        self.assertEqual(self.board.restarts, ["sysrq", "fuse"])
        self.assertGreaterEqual(self.board.tmarks_printed, 2)
        self.assertEqual(self.board.kernels_started, ["Linux-6.18.48-test"])
        elapsed = self.clock.now() - started
        self.assertGreater(elapsed, 46.9, "the watchdog reset is waited for")
        self.assertLess(elapsed, 70.0)
        self.assertTrue(any("watchdog" in line for line in self.said))

    def test_the_attempt_is_declared_failed_after_two_marks(self) -> None:
        self.go()
        times = self.board.tmark_times
        self.assertEqual(len(self.board.busy_input_times), 1, "one Ctrl-C reached the board")
        pressed = self.board.busy_input_times[0]
        self.assertGreaterEqual(pressed, times[1], "after the second T mark")
        self.assertLess(pressed, times[2], "and before the third")
        self.assertAlmostEqual(pressed - times[0] + 5.0, 10.0, delta=1.5, msg="about ten seconds in")

    def test_nothing_is_typed_between_the_stall_and_the_reset(self) -> None:
        self.go()
        pressed = self.board.busy_input_times[0]
        reset = self.board.restart_times[1]  # the watchdog reset
        between = [at for at, chunk in zip(self.tmux.typed_at, self.tmux.typed)
                   if pressed < at < reset]
        self.assertEqual(between, [], "the tool waits for the reset and types nothing")
        self.assertNotIn(b"reset", b"".join(self.tmux.typed))
        self.assertEqual(bytes(self.board.lost_busy), b"\x03", "only the Ctrl-C reached a busy board")

    def test_a_second_failure_ends_the_run_with_the_console_tail(self) -> None:
        self.board.tftp_server_ok = False
        with self.assertRaises(RamBootError) as caught:
            self.go()
        self.assertIn("again after a recovery", str(caught.exception))
        self.assertIn("Loading:", str(caught.exception))
        self.assertEqual(self.board.restarts, ["sysrq", "fuse"], "one recovery only")

    def test_recovery_by_reset_is_faster_when_ctrl_c_stops_the_transfer(self) -> None:
        self.config.recovery = "reset"
        self.board.ctrl_c_aborts = True
        started = self.clock.now()
        result = self.go()
        self.assertEqual(result.attempts, 2)
        self.assertEqual(self.board.restarts, ["sysrq", "command"])
        self.assertLess(self.clock.now() - started, 40.0, "much sooner than the watchdog reset")
        self.assertIn("reset", [line for line in self.board.executed if line == "reset"])

    def test_recovery_by_reset_falls_back_to_the_watchdog_when_the_prompt_stays_away(self) -> None:
        self.config.recovery = "reset"
        self.board.ctrl_c_aborts = False
        result = self.go()
        self.assertEqual(result.attempts, 2)
        self.assertEqual(self.board.restarts, ["sysrq", "fuse"])
        self.assertTrue(any("watchdog reset instead" in line for line in self.said))

    def no_fuse(self) -> None:
        """The restart leaves no watchdog counting, so no reset will come."""
        original = self.board.on_restart

        def restart_without_fuse(cause: str) -> None:
            original(cause)
            self.board.fuse_armed = False

        self.board.on_restart = restart_without_fuse  # type: ignore[method-assign]

    def test_a_watchdog_reset_that_does_not_come_is_replaced_by_reset_at_the_prompt(self) -> None:
        """RB-6: the default waits for the fuse; ``reset`` is typed only if it does not come."""
        self.no_fuse()
        self.board.ctrl_c_aborts = True
        started = self.clock.now()
        result = self.go()
        self.assertEqual(result.attempts, 2)
        self.assertEqual(self.board.restarts, ["sysrq", "command"])
        self.assertGreaterEqual(self.clock.now() - started, self.config.fuse_wait,
                                "the watchdog reset was waited for first")
        self.assertTrue(any("watchdog reset did not come" in line for line in self.said))
        self.assertEqual(self.board.executed.count("reset"), 1)

    def test_reset_is_not_typed_while_the_watchdog_reset_can_still_come(self) -> None:
        self.board.ctrl_c_aborts = True  # the prompt is back at once; the fuse resets all the same
        self.go()
        self.assertEqual(self.board.restarts, ["sysrq", "fuse"])
        self.assertNotIn("reset", self.board.executed)

    def test_no_watchdog_reset_and_no_prompt_is_a_timeout(self) -> None:
        self.no_fuse()
        self.board.ctrl_c_aborts = False  # the transfer goes on hanging: nothing can be typed
        with self.assertRaises(WaitTimeout) as caught:
            self.go()
        self.assertIn("watchdog reset", str(caught.exception))
        self.assertNotIn("reset", self.board.executed)

    def test_a_reset_that_does_not_restart_the_board_is_a_timeout(self) -> None:
        self.no_fuse()
        self.board.ctrl_c_aborts = True
        self.board.model.cmd_reset = lambda args, suffix, depth: None  # type: ignore[method-assign]
        with self.assertRaises(WaitTimeout) as caught:
            self.go()
        self.assertIn("the board to restart", str(caught.exception))

    def test_a_file_that_is_not_served_is_reported_by_the_servers_words(self) -> None:
        self.board.fail_first_after_sysrq = False
        del self.board.tftp_files["uImage"]
        with self.assertRaises(RamBootError) as caught:
            self.go()
        self.assertIn("no such file", str(caught.exception))
        self.assertEqual(self.board.restarts, ["sysrq"], "no recovery for a refusal")


class Refusals(RamBootCase):

    def test_missing_macro_stops(self) -> None:
        for name in ("sdsup", "ramboot", "ipaddr", "serverip", "netmask"):
            with self.subTest(name):
                self.setUp()
                del self.board.model.env[name]
                with self.assertRaises(MissingVariable) as caught:
                    self.go()
                self.assertIn(repr(name), str(caught.exception))
                self.assertNotIn("run ramboot", self.board.executed)
                self.assertEqual(self.board_ops(), set())

    def test_a_wrong_server_address_is_reported_before_the_transfer(self) -> None:
        self.board.model.env["serverip"] = "198.51.100.9"
        with self.assertRaises(UnexpectedState) as caught:
            self.go()
        self.assertIn("serverip", str(caught.exception))
        self.assertIn("198.51.100.9", str(caught.exception))
        self.assertNotIn("run ramboot", self.board.executed)

    def test_a_macro_that_writes_flash_is_refused_before_it_is_run(self) -> None:
        self.board.model.env["ramboot"] += "; sf erase 440000 +14c000"
        with self.assertRaises(Refusal) as caught:
            self.go()
        self.assertEqual(caught.exception.code, "not-allowed")
        self.assertEqual(caught.exception.exit_status, 3)
        self.assertNotIn("run ramboot", self.board.executed)
        self.assertEqual(self.board_ops(), set())

    def test_a_macro_that_writes_the_flash_window_is_refused(self) -> None:
        self.board.model.env["sdsup"] = "mw.l 94440000 0"
        with self.assertRaises(Refusal) as caught:
            self.go()
        self.assertEqual(caught.exception.code, "flash-aperture")
        self.assertNotIn("run ramboot", self.board.executed)

    def test_a_read_only_console_cannot_boot(self) -> None:
        self.guard.read_only = True
        with self.assertRaises(Refusal) as caught:
            self.go()
        self.assertIn("read-only", caught.exception.detail)
        self.assertEqual(self.board.executed, ["printenv"], "only a read was allowed")

    def test_wrong_kernel_is_failure(self) -> None:
        """Another image of the same size is on the server: the header gives it away."""
        other = make_uimage(self.kernel_size, name="Linux-3.18.24-old", seed=9)
        self.assertEqual(len(other), len(self.kernel_image))
        self.board.tftp_files["uImage"] = other
        with self.assertRaises(WrongKernel) as caught:
            self.go()
        self.assertIn("name", str(caught.exception))
        self.assertIn("Linux-3.18.24-old", str(caught.exception))
        self.assertEqual(WrongKernel.exit_status, 5)

    def test_a_different_size_is_reported_at_the_transfer(self) -> None:
        self.board.tftp_files["uImage"] = make_uimage(self.kernel_size + 100)
        with self.assertRaises(RamBootError) as caught:
            self.go()
        self.assertIn("received %d bytes" % (self.kernel_size + 100 + 64), str(caught.exception))

    def test_a_ready_marker_that_is_not_a_regular_expression_is_refused_first(self) -> None:
        self.config.ready_pattern = "login:("
        with self.assertRaises(RamBootError) as caught:
            self.go()
        self.assertIn("not a regular expression", str(caught.exception))
        self.assertEqual(self.tmux.calls, [])
        self.assertEqual(self.runner.calls, [])

    def test_a_bad_image_is_refused_before_the_board_is_touched(self) -> None:
        for data, words in ((make_uimage(bad_data_crc=True), "data CRC"),
                            (make_uimage(magic=1), "magic"),
                            (make_uimage(load=image.LOAD_ADDRESS + 0x100000), "loads at"),
                            (make_uimage(13 * 1024 * 1024), "more than")):
            with self.subTest(words=words):
                self.write_image(data)
                with self.assertRaises(ImageError) as caught:
                    self.go()
                self.assertIn(words, str(caught.exception))
        self.assertEqual(self.tmux.calls, [])
        self.assertEqual(self.runner.calls, [])

    def test_a_missing_image_file(self) -> None:
        os.unlink(self.image_path)
        with self.assertRaises(ImageError):
            self.go()
        self.assertEqual(self.tmux.calls, [])

    def test_a_busy_console_is_not_shared(self) -> None:
        from bench.console import Console, ConsoleBusy
        second = Console(self.console.config, self.tmux, self.clock)
        with second.exclusive("flash"):
            with self.assertRaises(ConsoleBusy):
                self.go()
        self.assertEqual(self.tmux.typed, [])

    def test_a_dead_pane_is_not_a_console(self) -> None:
        self.tmux.pane_dead = True
        with self.assertRaises(con.ConsoleUnusable):
            self.go()
        self.assertEqual(self.tmux.typed, [])


class PowerCycle(RamBootCase):

    def test_when_ssh_cannot_reach_the_board_a_person_power_cycles_it(self) -> None:
        self.ssh.reachable = set()
        self.clock.schedule(25.0, lambda: self.board.restart("power"))
        result = self.go()
        self.assertEqual(result.attempts, 1)
        self.assertEqual(self.ssh.started, ["198.51.100.2", "192.0.2.2"])
        self.assertTrue(any("Power-cycle" in line for line in self.said))
        self.assertEqual(self.board.restarts, ["power"])

    def test_the_catcher_sends_enters_only_after_the_first_stage_banner(self) -> None:
        self.ssh.reachable = set()
        self.clock.schedule(25.0, lambda: self.board.restart("power"))
        self.go()
        banner = self.board.restart_times[0]
        enters = [at for at, chunk in zip(self.tmux.typed_at, self.tmux.typed) if chunk == b"\r"]
        before = [at for at in enters if at < banner]
        after = [at for at in enters if at >= banner]
        self.assertEqual(len(before), 1, "only the console check's empty line, while waiting")
        self.assertGreaterEqual(len(after), 3)
        gaps = [b - a for a, b in zip(after, after[1:])]
        self.assertTrue(all(abs(gap - 0.1) < 0.02 for gap in gaps[:5]), gaps)

    def test_a_person_who_never_comes_is_a_timeout(self) -> None:
        self.ssh.reachable = set()
        self.config.person_wait = 30.0
        with self.assertRaises(WaitTimeout) as caught:
            self.go()
        self.assertIn("power-cycled", str(caught.exception))
        self.assertEqual(self.tmux.typed, [b"\r"], "only the console check was typed")


class Interruptions(RamBootCase):

    kernel_size = 3_000_000  # a transfer of a few seconds

    def flap_during_transfers(self, count: int) -> None:
        """Cut the bridge one second into each of the first ``count`` transfers."""
        original = self.board.model.cmd_tftpboot
        left = [count]

        def cut(args, suffix, depth):
            if left[0] > 0:
                left[0] -= 1
                self.clock.schedule(1.0, self.board.flap)
            return original(args, suffix, depth)

        self.board.model.cmd_tftpboot = cut  # type: ignore[method-assign]

    def test_a_bridge_flap_during_the_transfer_restarts_from_the_console_check(self) -> None:
        self.board.at_prompt()
        self.flap_during_transfers(1)
        result = self.go()
        self.assertEqual(result.attempts, 2, "the first attempt was cut off")
        self.assertTrue(any("connection broke" in line for line in self.said))
        self.assertEqual(self.board.kernels_started[-1], "Linux-6.18.48-test")

    def test_a_second_flap_ends_the_run(self) -> None:
        self.board.at_prompt()
        self.flap_during_transfers(2)
        with self.assertRaises(ConsoleInterrupted):
            self.go()


class Pieces(RamBootCase):

    def test_check_environment(self) -> None:
        env = {"ramboot": "x", "sdsup": "y", "ipaddr": "192.0.2.2", "serverip": "192.0.2.1",
               "netmask": "255.255.255.252"}
        rb.check_environment(env, rb.REQUIRED_VARIABLES, rb.expected_addresses(self.config))
        for name in ("ipaddr", "serverip", "netmask"):
            changed = dict(env, **{name: "203.0.113.7"})
            with self.subTest(name), self.assertRaises(UnexpectedState) as caught:
                rb.check_environment(changed, rb.REQUIRED_VARIABLES,
                                     rb.expected_addresses(self.config))
            self.assertIn(name, str(caught.exception))
        self.assertEqual(rb.REQUIRED_VARIABLES,
                         ("ramboot", "sdsup", "ipaddr", "serverip", "netmask"))

    def watch(self, text: str, size: int = 100):
        self.transcript.emit(text)
        return rb.watch_transfer(self.ctx, 0, size, self.config)

    def test_one_t_mark_is_not_a_stall_and_two_are(self) -> None:
        self.transcript.emit("Loading: *T ")
        with self.assertRaises(WaitTimeout):
            rb.watch_transfer(self.ctx, 0, 100, RamBootConfig(transfer_limit=2.0))
        self.transcript.emit("T ")
        with self.assertRaises(AttemptFailed) as caught:
            rb.watch_transfer(self.ctx, 0, 100, self.config)
        self.assertFalse(caught.exception.reset_seen)

    def test_a_hash_mark_means_the_transfer_is_alive(self) -> None:
        self.transcript.emit("Loading: *T T #####\r\r\n  ###\r\nT T T")
        with self.assertRaises(WaitTimeout):
            rb.watch_transfer(self.ctx, 0, 100, RamBootConfig(transfer_limit=2.0))

    def test_the_t_of_preloader_is_not_a_t_mark(self) -> None:
        self.transcript.emit("Loading: *T \r\r\n9601D\r\r\nPRELOADER x\r\r\n")
        with self.assertRaises(AttemptFailed) as caught:
            rb.watch_transfer(self.ctx, 0, 100, self.config)
        self.assertTrue(caught.exception.reset_seen)

    def test_the_byte_count_must_equal_the_file(self) -> None:
        self.assertEqual(self.watch("done\r\r\nBytes transferred = 100 (64 hex)\r\r\n"), 100)
        with self.assertRaises(RamBootError):
            rb.watch_transfer(self.ctx, 0, 99, self.config)

    def test_an_unknown_command_is_reported(self) -> None:
        self.transcript.emit("Unknown command 'un' - try 'help'\r\r\n")
        with self.assertRaises(RamBootError) as caught:
            rb.watch_transfer(self.ctx, 0, 100, self.config)
        self.assertIn("did not understand", str(caught.exception))

    def test_read_cmdline_asks_the_control_address_first(self) -> None:
        self.assertIn("root=31:5", rb.read_cmdline(self.ctx))
        self.assertEqual(self.runner.calls[0][-2], "root@198.51.100.2")
        self.assertEqual(self.runner.calls[0][-1], "cat /proc/cmdline")
        self.assertEqual(len(self.runner.calls), 1)
        self.ssh.reachable = set()
        self.runner.calls.clear()
        self.assertIsNone(rb.read_cmdline(self.ctx))
        self.assertEqual([call[-2] for call in self.runner.calls],
                         ["root@198.51.100.2", "root@192.0.2.2"])
