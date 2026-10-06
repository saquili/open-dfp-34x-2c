# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The command line: exit codes, dry runs, settings, and a real subprocess."""

from __future__ import annotations

import io
import os
import signal
import subprocess
import sys
import unittest
from unittest import mock

from bench import cli
from bench import flash as fl
from bench import settings as settingsmod
from bench.settings import SettingsError

from . import vectors
from .images import make_squashfs, make_uimage
from .simboard import Mode
from .simflash import FlashBoard
from .simnet import BenchCase
from .support import TempDirTestCase

TOOLS = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class CliCase(BenchCase):
    """The command line over the simulated board, image host and console."""

    board_class = FlashBoard
    kernel_size = 4000

    def setUp(self) -> None:
        super().setUp()
        self.out, self.err = io.StringIO(), io.StringIO()
        self.svc = cli.Services(clock=self.clock, tmux=self.tmux, runner=self.runner,
                                environ={}, stdout=self.out, stderr=self.err,
                                install_signals=False)
        self.config_path = os.path.join(self.tmp, "bench.toml")
        self.write_config()
        self.board.flash.preload("k0", make_uimage(1500, name="Linux-old", seed=3))
        self.board.flash.preload("r0", make_squashfs(3000, seed=4))
        self.before = self.board.flash.snapshot()
        self.kernel_path = os.path.join(self.tmp, "uImage")
        with open(self.kernel_path, "wb") as handle:
            handle.write(self.kernel_image)
        self.rootfs = make_squashfs(40_000, seed=5)
        self.rootfs_path = os.path.join(self.tmp, "rootfs")
        with open(self.rootfs_path, "wb") as handle:
            handle.write(self.rootfs)
        self.board.tftp_files["rootfs"] = self.rootfs

    def write_config(self, extra: str = "") -> None:
        with open(self.config_path, "w", encoding="utf-8") as handle:
            handle.write('[console]\nsession = "uart-wifi-1"\ntranscript = "%s"\n'
                         '[host]\nssh = "local"\nserved_dir = "%s"\nwork_dir = "%s"\n%s'
                         % (self.transcript.path, self.served, os.path.join(self.tmp, "work"),
                            extra))

    def run_cli(self, *argv: str) -> int:
        self.out.seek(0)
        self.out.truncate()
        self.err.seek(0)
        self.err.truncate()
        return cli.main(["--config", self.config_path, *argv], self.svc)


class GuardCheck(CliCase):

    def test_allowed_and_refused_lines(self) -> None:
        status = self.run_cli("guard-check", "sf erase 80000 +1d0000", "")
        self.assertEqual(status, 0)
        self.assertEqual(self.out.getvalue(), "allow: sf erase 80000 +1d0000\nallow: \n")
        status = self.run_cli("guard-check", "sf erase 440000 +14c000")
        self.assertEqual(status, 3)
        self.assertEqual(self.err.getvalue(), "refused [protected-range] k1: sf erase 440000 +14c000\n")
        self.assertEqual(self.out.getvalue(), "")

    def test_every_line_is_judged_and_the_status_is_the_worst(self) -> None:
        status = self.run_cli("guard-check", "printenv", "sf erase 0 +40000", "version",
                              "mw.l 94440000 0")
        self.assertEqual(status, 3)
        self.assertEqual(self.out.getvalue(), "allow: printenv\nallow: version\n")
        self.assertEqual(self.err.getvalue().splitlines(), [
            "refused [protected-range] boot: sf erase 0 +40000",
            "refused [flash-aperture] 94440000: mw.l 94440000 0"])

    def test_the_session_is_kept_between_lines(self) -> None:
        status = self.run_cli("guard-check", "setenv sw_tryactive 0", "saveenv", "saveenv")
        self.assertEqual(status, 3)
        self.assertEqual(self.out.getvalue(), "allow: setenv sw_tryactive 0\nallow: saveenv\n")
        self.assertIn("[state]", self.err.getvalue())

    def test_commit_mode_is_a_flag(self) -> None:
        self.assertEqual(self.run_cli("guard-check", "setenv sw_commit 0"), 3)
        self.assertEqual(self.run_cli("guard-check", "--commit", "setenv sw_commit 0"), 0)

    def test_variables_and_macros_can_be_given(self) -> None:
        status = self.run_cli("guard-check", "--var", "fl_kernel1=80000", "--var",
                              "fl_kernel1_sz=1d0000", "sf erase ${fl_kernel1} +${fl_kernel1_sz}")
        self.assertEqual(status, 0)
        self.assertEqual(self.out.getvalue(), "allow: sf erase 80000 +1d0000\n")
        status = self.run_cli("guard-check", "--var", "sdsup=mw.l 94440000 0", "run sdsup")
        self.assertEqual(status, 3)
        self.assertIn("[flash-aperture]", self.err.getvalue())

    def test_the_boards_variables_are_judged_as_the_board_prints_them(self) -> None:
        """Partition tables with parentheses, given as ``printenv`` prints them."""
        names = ("sdsup", "ramboot", "b0", "b1", "bootargs_base", "mtdparts0", "mtdparts1",
                 "img0_kernel", "img1_kernel", "sw_tryactive", "sw_commit", "bootcmd",
                 "boot_by_commit", "boot_by_tryactive", "set_act0", "set_act1", "en_wdt")
        given: list[str] = []
        for name in names:
            given += ["--var", "%s=%s" % (name, vectors.BOARD[name])]
        lines = ("run ramboot", "run b0", "run sdsup", "boot")
        self.assertEqual(self.run_cli("guard-check", *given, *lines), 0)
        self.assertEqual(self.out.getvalue().splitlines(), ["allow: " + line for line in lines])
        hostile = [item + "; sf erase 440000 +14c000" if item.startswith("b0=") else item
                   for item in given]
        self.assertEqual(self.run_cli("guard-check", *hostile, *lines), 3)
        self.assertIn("[not-allowed] command sf in a macro (macro b0)", self.err.getvalue())

    def test_without_variables_only_the_line_is_judged(self) -> None:
        self.assertEqual(self.run_cli("guard-check", "run ramboot", "boot"), 0)
        self.assertEqual(self.run_cli("guard-check", "--var", "x=1", "run ramboot"), 3)
        self.assertIn("[missing-macro]", self.err.getvalue())

    def test_a_read_only_console(self) -> None:
        self.assertEqual(self.run_cli("guard-check", "--read-only", "printenv", "crc32 80000000 10"), 0)
        self.assertEqual(self.run_cli("guard-check", "--read-only", "reset"), 3)

    def test_targets_and_banks(self) -> None:
        self.assertEqual(self.run_cli("guard-check", "--target", "kernel", "--target", "r0",
                                      "--bank", "0"), 0)
        self.assertEqual(self.out.getvalue(), "allow: target k0\nallow: target r0\nallow: bank 0\n")
        self.assertEqual(self.run_cli("guard-check", "--target", "k1"), 3)
        self.assertEqual(self.run_cli("guard-check", "--bank", "1"), 3)
        self.assertIn("refused [protected-range] bank 1", self.err.getvalue())

    def test_the_alias_and_nothing_to_check(self) -> None:
        self.assertEqual(self.run_cli("guard", "printenv"), 0)
        self.assertEqual(self.run_cli("guard-check"), 2)
        self.assertEqual(self.run_cli("guard-check", "--var", "oops", "printenv"), 2)

    def test_nothing_is_sent_to_anything(self) -> None:
        self.run_cli("guard-check", "reset", "sf erase 0 +40000")
        self.assertEqual(self.tmux.calls, [])
        self.assertEqual(self.runner.calls, [])


class ExitCodes(CliCase):

    def test_exit_codes(self) -> None:
        self.assertEqual(self.run_cli("guard-check", "printenv"), 0)  # success
        self.assertEqual(self.run_cli("ramboot", "/no/such/image"), 1)  # the operation failed
        self.assertEqual(cli.main([], self.svc), 2)  # usage
        self.assertEqual(self.run_cli("guard-check", "reset", "go 1"), 3)  # a refusal
        self.tmux.session_exists = False
        self.assertEqual(self.run_cli("console", "check"), 4)  # the console is not usable
        self.tmux.session_exists = True
        self.board.model.env["fl_kernel1"] = "1"
        self.board.at_prompt()
        self.assertEqual(self.run_cli("flash", "--target", "kernel", self.kernel_path), 5)  # state
        self.assertEqual(cli.EXIT_TIMEOUT, 6)

    def test_a_timeout_is_6(self) -> None:
        self.board.at_prompt()
        self.board.model.cmd_version = lambda a, s, d: setattr(  # type: ignore[method-assign]
            self.board.model, "terminal", "hang")
        self.assertEqual(self.run_cli("console", "type", "--timeout", "5", "version"), 6)
        self.assertIn("timed out", self.err.getvalue())

    def test_console_type_takes_its_option_before_or_after_the_line(self) -> None:
        for argv in (("--timeout", "5", "printenv sw_commit"), ("printenv sw_commit", "--timeout", "5")):
            with self.subTest(argv=argv):
                self.board.at_prompt()
                self.assertEqual(self.run_cli("console", "type", *argv), 0, self.err.getvalue())
                self.assertEqual(self.out.getvalue().strip(), "sw_commit=0")

    def test_no_parser_has_an_optional_positional_after_another_positional(self) -> None:
        """Older argparse takes the empty match of the second before an option that follows
        the first, and then calls the line that comes after the option unrecognized."""
        import argparse

        def walk(parser: argparse.ArgumentParser, name: str):
            positionals = [a for a in parser._actions if not a.option_strings
                           and not isinstance(a, argparse._SubParsersAction)]
            for index, action in enumerate(positionals):
                if index > 0:
                    self.assertNotIn(action.nargs, ("?", "*"), "%s: %s" % (name, action.dest))
            for action in parser._actions:
                if isinstance(action, argparse._SubParsersAction):
                    for child_name, child in action.choices.items():
                        walk(child, name + " " + child_name)

        walk(cli.build_parser(), "bench")

    def test_usage_errors_are_2(self) -> None:
        for argv in (["--bogus"], ["flash", "image"], ["serve", "explode"], ["--set", "x"],
                     ["--set", "no.such=1", "console", "check"],
                     ["--set", "host.port=text", "console", "check"],
                     ["--set", "boot.recovery=maybe", "console", "check"], ["console"],
                     ["console", "type"]):
            with self.subTest(argv=argv):
                self.assertEqual(cli.main(argv, self.svc), 2, self.err.getvalue())

    def test_help_is_0(self) -> None:
        self.assertEqual(cli.main(["--help"], self.svc), 0)
        text = self.out.getvalue()
        for word in ("ramboot", "flash", "guard-check", "serve", "console", "selftest",
                     "console.session", "host.served_dir", "boot.ready_pattern"):
            self.assertIn(word, text)

    def test_every_error_class_has_its_status(self) -> None:
        from bench import console as con, guard as guardmod, host, image as img, ramboot, uboot
        expected = [(guardmod.Refusal, 3), (guardmod.LayoutMismatch, 5), (con.ConsoleUnusable, 4),
                    (con.ConsoleBusy, 4), (con.ConsoleInterrupted, 5), (con.UnexpectedState, 5),
                    (con.WaitTimeout, 6), (con.EchoMismatch, 1), (img.ImageError, 1),
                    (host.HostError, 1), (uboot.NotAtPrompt, 5), (uboot.UnexpectedReset, 5),
                    (uboot.BreakInFailed, 5), (uboot.CommandFailed, 1), (ramboot.RamBootError, 1),
                    (ramboot.MissingVariable, 5), (ramboot.WrongKernel, 5), (fl.FlashError, 1),
                    (fl.Stalled, 6), (fl.Bank1Changed, 5), (fl.Interrupted, 1),
                    (SettingsError, 2)]
        for cls, status in expected:
            with self.subTest(cls=cls.__name__):
                self.assertEqual(cls.exit_status, status)


class RamBoot(CliCase):

    def test_ramboot_through_the_command_line(self) -> None:
        status = self.run_cli("ramboot", self.kernel_path)
        self.assertEqual(status, 0, self.err.getvalue())
        self.assertIn("ramboot ok: Linux-6.18.48-test, 4064 bytes transferred, 1 attempt(s)",
                      self.out.getvalue())
        self.assertIn("bench: restarting it", self.out.getvalue().replace(
            "the board is running (login prompt); ", ""))
        self.assertEqual(self.board.kernels_started, ["Linux-6.18.48-test"])
        self.assertTrue(os.path.isfile(os.path.join(self.served, "uImage")))

    def test_options_override_the_ready_marker_and_recovery(self) -> None:
        self.board.ready_text = "Hello from the image\r\r\n"
        self.assertEqual(self.run_cli("ramboot", "--ready", "Hello from", "--ready-limit", "30",
                                      "--recovery", "reset", self.kernel_path), 0)

    def test_a_failure_is_reported_with_status_1(self) -> None:
        del self.board.tftp_files["uImage"]
        self.assertEqual(self.run_cli("ramboot", self.kernel_path), 1)
        self.assertIn("no such file", self.err.getvalue())

    def test_a_missing_variable_is_status_5_and_names_it(self) -> None:
        del self.board.model.env["sdsup"]
        self.board.at_prompt()
        self.assertEqual(self.run_cli("ramboot", self.kernel_path), 5)
        self.assertIn("'sdsup'", self.err.getvalue())

    def test_a_hostile_macro_is_a_refusal_with_status_3(self) -> None:
        self.board.model.env["ramboot"] += "; sf erase 440000 +14c000"
        self.board.at_prompt()
        self.assertEqual(self.run_cli("ramboot", self.kernel_path), 3)
        self.assertTrue(self.err.getvalue().startswith("refused [not-allowed]"))

    def test_the_image_is_checked_before_anything_else(self) -> None:
        bad = os.path.join(self.tmp, "bad")
        with open(bad, "wb") as handle:
            handle.write(make_uimage(bad_data_crc=True))
        self.assertEqual(self.run_cli("ramboot", bad), 1)
        self.assertIn("data CRC", self.err.getvalue())
        self.assertEqual(self.tmux.calls, [])

    def test_the_log_file_is_written_without_the_environment(self) -> None:
        log = os.path.join(self.tmp, "log", "bench.log")
        self.assertEqual(self.run_cli("--set", "log.path=" + log, "ramboot", self.kernel_path), 0)
        with open(log, encoding="utf-8") as handle:
            text = handle.read()
        self.assertIn("ramboot attempt 1", text)
        self.assertIn(" type run ramboot", text)
        self.assertNotIn("00:00:5e", text.lower())
        self.assertNotIn("ethaddr", text)

    def test_ramboot_dry_run(self) -> None:
        self.assertEqual(self.run_cli("ramboot", "--dry-run", self.kernel_path), 0)
        shown = self.out.getvalue()
        self.assertIn("would type: printenv\n", shown)
        self.assertIn("would type: run ramboot\n", shown)
        self.assertIn("echo b > /proc/sysrq-trigger", shown)
        self.assertIn("image host:", shown)
        self.assertEqual(self.tmux.calls, [])
        self.assertEqual(self.runner.calls, [])


class Flash(CliCase):

    def test_flash_the_kernel_through_the_command_line(self) -> None:
        status = self.run_cli("flash", "--target", "kernel", self.kernel_path)
        self.assertEqual(status, 0, self.err.getvalue())
        self.assertIn("flash ok: k0, 4064 bytes", self.out.getvalue())
        self.assertEqual(self.board.flash.region("k0")[:4064], self.kernel_image)
        self.assertEqual(self.board.flash.operations_outside_bank_0(), [])
        self.assertEqual(self.board.flash.protected_unchanged(self.before), [])

    def test_flash_the_root_filesystem_with_a_trial(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        status = self.run_cli("flash", "--target", "rootfs", "--arm", self.rootfs_path)
        self.assertEqual(status, 0, self.err.getvalue())
        self.assertIn("trial armed and booted", self.out.getvalue())
        self.assertIn("fallback bank 1", self.out.getvalue())

    def test_a_trial_with_the_same_bank_fallback_needs_the_word(self) -> None:
        self.assertEqual(self.run_cli("flash", "--target", "kernel", "--arm", self.kernel_path), 5)
        self.assertIn("--accept-same-bank-fallback", self.err.getvalue())
        self.assertEqual(self.board.flash.log, [])
        self.board.at_prompt()
        self.assertEqual(self.run_cli("flash", "--target", "kernel", "--arm",
                                      "--accept-same-bank-fallback", self.kernel_path), 0)

    def test_commit_through_the_command_line(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        self.assertEqual(self.run_cli("flash", "--target", "kernel", "--commit",
                                      self.kernel_path), 0)
        self.assertEqual(self.board.model.env["sw_commit"], "0")

    def test_bank_1_and_protected_targets_are_refused_with_status_3(self) -> None:
        for argv in (["--bank", "1"], ["--target", "k1"], ["--target", "boot"],
                     ["--target", "config"], ["--bank", "2"]):
            with self.subTest(argv=argv):
                args = ["flash", "--target", "kernel", *argv, self.kernel_path]
                if "--target" in argv:
                    args = ["flash", *argv, self.kernel_path]
                self.assertEqual(self.run_cli(*args), 3, self.err.getvalue())
                self.assertTrue(self.err.getvalue().startswith("refused [protected-range]")
                                or self.err.getvalue().startswith("refused [not-allowed]"))
        self.assertEqual(self.tmux.calls, [])
        self.assertEqual(self.runner.calls, [])

    def test_ymodem_through_the_bridge_is_refused_without_the_opt_in(self) -> None:
        self.assertEqual(self.run_cli("flash", "--target", "kernel", "--via", "ymodem",
                                      self.kernel_path), 5)
        self.assertIn("direct serial port", self.err.getvalue())
        self.assertEqual(self.tmux.calls, [])

    def test_flash_dry_run(self) -> None:
        """The printed lines are the plan of the procedure, each approved by the guard."""
        cases = [("kernel", self.kernel_path, "tftp", ()), ("rootfs", self.rootfs_path, "tftp", ()),
                 ("kernel", self.kernel_path, "ymodem", ("--arm", "--commit")),
                 ("rootfs", self.rootfs_path, "tftp", ("--arm",))]
        for target, path, via, extra in cases:
            with self.subTest(target=target, via=via, extra=extra):
                status = self.run_cli("flash", "--target", target, "--via", via, *extra,
                                      "--dry-run", path)
                self.assertEqual(status, 0, self.err.getvalue())
                typed = [line[len("would type: "):] for line in self.out.getvalue().splitlines()
                         if line.startswith("would type: ")]
                size = os.path.getsize(path)
                expected = fl.plan_lines(fl.plan(target, size, via, arm="--arm" in extra,
                                                 commit="--commit" in extra))
                self.assertEqual(typed, expected)
                self.assertEqual(self.tmux.calls, [])
                self.assertEqual(self.runner.calls, [])
        self.assertEqual(self.board.flash.log, [])

    def test_flash_dry_run_checks_the_image_and_the_words_first(self) -> None:
        self.assertEqual(self.run_cli("flash", "--target", "kernel", "--dry-run",
                                      self.rootfs_path), 1)
        self.assertEqual(self.run_cli("flash", "--target", "k1", "--dry-run", self.kernel_path), 3)
        self.assertEqual(self.run_cli("flash", "--target", "kernel", "--bank", "1", "--dry-run",
                                      self.kernel_path), 3)

    def test_the_signal_handlers_are_installed_for_a_flash_and_put_back(self) -> None:
        self.svc.install_signals = True
        before = (signal.getsignal(signal.SIGINT), signal.getsignal(signal.SIGTERM))
        original = self.board.model.on_flash

        def interrupt(kind, offset, length, memory=0):
            if kind == "write":
                self.clock.schedule(0.5, lambda: signal.raise_signal(signal.SIGINT))
            return original(kind, offset, length, memory)

        self.board.model.on_flash = interrupt  # type: ignore[method-assign]
        status = self.run_cli("flash", "--target", "rootfs", self.rootfs_path)
        self.assertEqual(status, 1)
        self.assertIn("SIGINT received; waiting for the running command to finish",
                      self.err.getvalue())
        self.assertIn("stopped on request (SIGINT)", self.err.getvalue())
        self.assertEqual((signal.getsignal(signal.SIGINT), signal.getsignal(signal.SIGTERM)),
                         before)
        self.assertEqual(self.board.flash.region("r0")[:len(self.rootfs)], self.rootfs,
                         "the write ran to the end")
        self.assertEqual(bytes(self.board.lost_busy), b"", "no Ctrl-C was sent into the write")

    def test_serial_ymodem_without_a_device_is_a_settings_error_not_a_crash(self) -> None:
        with contextlib_exit_stack() as stack:
            with self.assertRaises(SettingsError):
                cli.Bench(settingsmod.load(self.config_path), self.svc, stack, use_serial=True)


def contextlib_exit_stack():
    import contextlib
    return contextlib.ExitStack()


class Serve(CliCase):

    def test_start_check_and_stop(self) -> None:
        self.assertEqual(self.run_cli("serve", "check"), 1, "nothing is ready yet")
        self.assertIn("file uImage: missing", self.out.getvalue())
        self.assertIn("server: not listening", self.out.getvalue())
        self.assertEqual(self.run_cli("serve", "start", "--image", self.kernel_path), 0,
                         self.err.getvalue())
        self.assertIn("pushed uImage as uImage", self.out.getvalue())
        self.assertIn("the firewall allowance: added", self.out.getvalue())
        self.assertEqual(self.run_cli("serve", "check"), 0)
        self.assertIn("file uImage: 4064 bytes", self.out.getvalue())
        self.assertIn("server: listening", self.out.getvalue())
        self.assertIn("firewall allowance: present", self.out.getvalue())
        self.assertEqual(self.run_cli("serve", "stop", "--remove-rule"), 0)
        self.assertIn("removed 1 allowance rule(s)", self.out.getvalue())
        self.assertEqual(self.runner.host.rules, [])
        self.assertEqual(self.run_cli("serve", "check"), 1)

    def test_starting_twice_does_not_add_a_second_rule(self) -> None:
        self.run_cli("serve", "start")
        self.run_cli("serve", "start")
        self.assertEqual(len(self.runner.host.rules), 1)
        self.assertIn("the firewall allowance: present", self.out.getvalue())

    def test_remove_rule(self) -> None:
        self.run_cli("serve", "start")
        self.assertEqual(self.run_cli("serve", "remove-rule"), 0)
        self.assertEqual(self.runner.host.rules, [])

    def test_a_name_can_be_chosen(self) -> None:
        self.run_cli("serve", "start", "--image", self.rootfs_path, "--name", "rootfs")
        self.assertTrue(os.path.isfile(os.path.join(self.served, "rootfs")))
        self.assertEqual(self.run_cli("serve", "check", "--name", "rootfs"), 0)

    def test_serve_dry_run_prints_the_commands_and_runs_none(self) -> None:
        self.assertEqual(self.run_cli("serve", "start", "--dry-run", "--image", self.kernel_path), 0)
        shown = self.out.getvalue()
        self.assertIn("nft add rule inet filter input iifname", shown)
        self.assertIn("--bind 192.0.2.1", shown)
        self.assertEqual(self.runner.calls, [])

    def test_serve_run_runs_the_server_in_the_foreground(self) -> None:
        with mock.patch("bench.tftp.main", return_value=0) as server:
            self.assertEqual(self.run_cli("serve", "run"), 0)
        self.assertEqual(server.call_args[0][0],
                         ["--dir", self.served, "--bind", "192.0.2.1", "--port", "69"])


class ConsoleCommands(CliCase):

    def test_check_names_the_state(self) -> None:
        self.board.at_prompt()
        self.assertEqual(self.run_cli("console", "check"), 0)
        self.assertIn("is usable; the board shows: loader prompt", self.out.getvalue())
        self.board.mode = Mode.LINUX
        self.assertEqual(self.run_cli("console", "check"), 0)
        self.assertIn("login prompt", self.out.getvalue())

    def test_a_dead_pane_is_status_4(self) -> None:
        self.tmux.pane_dead = True
        self.assertEqual(self.run_cli("console", "check"), 4)
        self.assertIn("pane_dead", self.err.getvalue())

    def test_type_runs_one_guarded_line(self) -> None:
        self.board.at_prompt()
        self.assertEqual(self.run_cli("console", "type", "printenv sw_commit"), 0)
        self.assertEqual(self.out.getvalue().strip(), "sw_commit=0")

    def test_type_runs_its_lines_in_one_guard_session(self) -> None:
        self.board.at_prompt()
        status = self.run_cli("console", "type", "setenv bootdelay 5", "saveenv", "printenv bootdelay")
        self.assertEqual(status, 0, self.err.getvalue())
        self.assertIn("bootdelay=5", self.out.getvalue())
        self.assertEqual(self.board.model.env["bootdelay"], "5")
        self.assertEqual(self.run_cli("console", "type", "saveenv"), 3)
        self.assertIn("refused [state]", self.err.getvalue())

    def test_type_stops_at_the_first_refused_line(self) -> None:
        self.board.at_prompt()
        executed = list(self.board.executed)
        self.assertEqual(self.run_cli("console", "type", "setenv bootdelay 0", "saveenv"), 3)
        self.assertEqual(self.board.executed, executed)
        self.assertEqual(self.board.model.env["bootdelay"], "600")

    def test_type_with_restart_breaks_in_first(self) -> None:
        self.board.show_login()
        self.assertEqual(self.run_cli("console", "type", "--restart", "printenv sw_commit"), 0,
                         self.err.getvalue())
        self.assertIn("sw_commit=0", self.out.getvalue())
        self.assertIn("restarted and broke in", self.out.getvalue())

    def test_a_countdown_of_five_saved_by_type_still_lets_ramboot_break_in(self) -> None:
        self.board.at_prompt()
        self.assertEqual(self.run_cli("console", "type", "setenv bootdelay 5", "saveenv"), 0,
                         self.err.getvalue())
        self.board.show_login()
        mark = self.transcript.size()
        self.assertEqual(self.run_cli("ramboot", self.kernel_path), 0, self.err.getvalue())
        self.assertIn("Hit any key to stop autoboot:  5", self.transcript.text()[mark:])
        self.assertEqual(self.board.kernels_started, ["Linux-6.18.48-test"])

    def test_the_environment_dump_is_printed_without_the_mac_address(self) -> None:
        self.board.at_prompt()
        self.assertEqual(self.run_cli("console", "type", "printenv"), 0)
        shown = self.out.getvalue()
        self.assertIn("sw_commit=0", shown)
        self.assertNotIn("ethaddr", shown)
        self.assertNotIn("00:00:5e", shown.lower())

    def test_type_refuses_what_the_guard_refuses(self) -> None:
        self.board.at_prompt()
        executed = list(self.board.executed)
        self.assertEqual(self.run_cli("console", "type", "sf erase 440000 +14c000"), 3)
        self.assertEqual(self.board.executed, executed)
        self.assertEqual(self.err.getvalue(), "refused [protected-range] k1: sf erase 440000 +14c000\n")

    def test_type_never_types_at_a_login_prompt(self) -> None:
        self.assertEqual(self.run_cli("console", "type", "printenv"), 5)
        self.assertEqual(bytes(self.board.typed_at_login), b"\r")

    def test_the_read_only_console_of_the_reference_board(self) -> None:
        self.board.at_prompt()
        self.assertEqual(self.run_cli("--set", "console.read_only=true", "console", "type", "reset"), 3)
        self.assertIn("read-only", self.err.getvalue())

    def test_the_second_instance_fails_at_once_naming_the_first(self) -> None:
        self.board.at_prompt()
        from bench.console import Console
        other = Console(self.console.config, self.tmux, self.clock)
        with other.exclusive("flash"):
            self.assertEqual(self.run_cli("console", "type", "printenv"), 4)
        self.assertIn("flash", self.err.getvalue())


class Settings(TempDirTestCase):

    def test_defaults(self) -> None:
        settings = settingsmod.load(None, [], {})
        self.assertEqual(settings.console.session, "uart-wifi-1")
        self.assertFalse(settings.console.read_only)
        self.assertEqual(settings.console.tmux_socket, "")
        self.assertTrue(settings.console.transcript.endswith(os.path.join("bench", "uart-wifi-1.log")))
        self.assertEqual((settings.host.ssh, settings.host.bind, settings.host.port),
                         ("local", "192.0.2.1", 69))
        self.assertEqual((settings.access.control_address, settings.access.data_address),
                         ("198.51.100.2", "192.0.2.2"))
        self.assertEqual((settings.ramboot.expect_ipaddr, settings.ramboot.expect_serverip,
                          settings.ramboot.expect_netmask),
                         ("192.0.2.2", "192.0.2.1", "255.255.255.252"))
        self.assertEqual(settings.flash.squashfs_compressions, (4,))
        self.assertEqual((settings.ramboot.ready_pattern, settings.ramboot.ready_limit), ("login:", 60.0))
        self.assertEqual(settings.log_path, "")

    def test_the_reference_boards_console_is_read_only_by_default(self) -> None:
        self.assertTrue(settingsmod.load(None, ["console.session=uart-wifi-2"], {}).console.read_only)
        self.assertFalse(settingsmod.load(None, ["console.session=uart-wifi-1"], {}).console.read_only)
        self.assertFalse(settingsmod.load(None, ["console.session=uart-wifi-2",
                                                 "console.read_only=false"], {}).console.read_only)

    def test_file_then_overrides_then_types(self) -> None:
        path = os.path.join(self.tmp, "c.toml")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write('[host]\nssh = "bench@image-host.example"\nssh_options = ["-J", "jump.example"]\n'
                         'port = 6969\n[boot]\nready_limit = 90\n')
        settings = settingsmod.load(path, ["host.port=7070", "board.jump=bench@gate.example",
                                           "image.squashfs_compressions=[1, 4, 5]"], {})
        self.assertEqual(settings.host.ssh, "bench@image-host.example")
        self.assertEqual(settings.host.ssh_options, ("-J", "jump.example"))
        self.assertEqual(settings.host.port, 7070)
        self.assertEqual(settings.access.jump, "bench@gate.example")
        self.assertEqual(settings.flash.squashfs_compressions, (1, 4, 5))
        self.assertEqual(settings.ramboot.ready_limit, 90.0)

    def test_the_environment_names_a_file(self) -> None:
        path = os.path.join(self.tmp, "c.toml")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write('[console]\nsession = "other"\n')
        self.assertEqual(settingsmod.load(None, [], {"BENCH_CONFIG": path}).console.session, "other")
        self.assertEqual(settingsmod.load(path, ["console.session=third"],
                                          {"BENCH_CONFIG": "/nonexistent"}).console.session, "third")

    def test_bad_settings_are_errors(self) -> None:
        path = os.path.join(self.tmp, "bad.toml")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("not = = toml")
        for config, overrides in ((None, ["bogus.key=1"]), (None, ["host.port=abc"]),
                                  (None, ["host.sudo=1"]), (None, ["host.ssh_options=text"]),
                                  (None, ["console.read_only=maybe"]), (None, ["boot.recovery=x"]),
                                  (path, []), ("/nonexistent/file.toml", []), (None, ["novalue"])):
            with self.subTest(config=config, overrides=overrides), self.assertRaises(SettingsError):
                settingsmod.load(config, overrides, {})
        other = os.path.join(self.tmp, "section.toml")
        with open(other, "w", encoding="utf-8") as handle:
            handle.write("loose = 1\n")
        with self.assertRaises(SettingsError):
            settingsmod.load(other, [], {})

    def test_every_setting_has_a_documented_default(self) -> None:
        lines = settingsmod.describe()
        self.assertEqual(len(lines), len(settingsmod.SCHEMA))
        for line in lines:
            self.assertRegex(line, r"^\S+\s+\S+\s+\S")


class Subprocess(unittest.TestCase):
    """``python3 -m bench`` from the tools directory, as a person runs it."""

    def run_bench(self, *argv: str) -> subprocess.CompletedProcess:
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        env.pop("BENCH_CONFIG", None)
        return subprocess.run([sys.executable, "-m", "bench", *argv], cwd=TOOLS, env=env,
                              capture_output=True, text=True, timeout=60)

    def test_the_guard_through_the_module_entry_point(self) -> None:
        good = self.run_bench("guard-check", "sf erase 80000 +1d0000")
        self.assertEqual((good.returncode, good.stdout), (0, "allow: sf erase 80000 +1d0000\n"))
        bad = self.run_bench("guard-check", "sf erase 440000 +14c000")
        self.assertEqual(bad.returncode, 3)
        self.assertEqual(bad.stderr, "refused [protected-range] k1: sf erase 440000 +14c000\n")

    def test_no_command_and_help(self) -> None:
        self.assertEqual(self.run_bench().returncode, 2)
        shown = self.run_bench("--help")
        self.assertEqual(shown.returncode, 0)
        self.assertIn("flash", shown.stdout)

    def test_selftest_runs_a_selection_of_the_tests(self) -> None:
        result = self.run_bench("selftest", "--pattern", "test_image.py")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK", result.stderr)

    def test_dry_runs_need_no_hardware(self) -> None:
        with TempDir() as tmp:
            path = os.path.join(tmp, "uImage")
            with open(path, "wb") as handle:
                handle.write(make_uimage(500))
            result = self.run_bench("ramboot", "--dry-run", path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("would type: run ramboot", result.stdout)
            result = self.run_bench("flash", "--target", "kernel", "--dry-run", path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("would type: sf erase 80000 +1d0000", result.stdout)


class TempDir:
    def __enter__(self) -> str:
        import tempfile
        self._handle = tempfile.TemporaryDirectory()
        return self._handle.__enter__()

    def __exit__(self, *exc) -> None:
        self._handle.__exit__(*exc)


if __name__ == "__main__":
    unittest.main()
