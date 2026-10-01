# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Flashing bank 0 against the scripted board and its simulated flash."""

from __future__ import annotations

import os
import unittest
import zlib
from contextlib import contextmanager
from unittest import mock

from bench import flash as fl
from bench import guard as guardmod
from bench.console import (BenchError, Console, ConsoleBusy, ConsoleInterrupted,
                           UnexpectedState, WaitTimeout)
from bench.flash import (Bank1Changed, FlashConfig, FlashError, FlashOptions, Interrupted,
                         SignalGate, Stalled)
from bench.guard import LayoutMismatch, Refusal
from bench.image import ImageError
from bench.rawport import RawConsole
from bench.ramboot import MissingVariable

from .images import make_squashfs, make_uimage
from .simflash import BoardPort, FlashBoard, SimFlash, window_offset
from .simnet import BenchCase
from .simymodem import Faults


def step_lines(loader) -> list[str]:
    """The lines the loader typed as steps, not as the guard's reads of variables."""
    return [text for kind, text in loader.typed if kind == "step"]


class SimulatedFlash(BenchCase):
    """The simulated flash: patterns, erase in units, writes that only clear bits."""

    board_class = FlashBoard

    def test_flash_model(self) -> None:
        flash = SimFlash()
        regions = flash.snapshot()
        self.assertEqual(len(flash.data), 0x800000)
        self.assertEqual(len(set(regions.values())), 8, "each region has its own pattern")
        for name, content in regions.items():
            self.assertGreater(len(set(content)), 50, name)
        flash.erase(0x81000, 0x2800)  # 4 KiB units: 0x81000 to 0x84000 are cleared
        self.assertEqual(set(flash.read(0x81000, 0x3000)), {0xFF})
        self.assertEqual(flash.read(0x80FFF, 1), regions["k0"][0xFFF:0x1000])
        self.assertEqual(flash.read(0x84000, 1), regions["k0"][0x4000:0x4001])
        flash.write(0x81000, b"\xf0\x0f")
        flash.write(0x81000, b"\x3c\xff")
        self.assertEqual(flash.read(0x81000, 2), bytes([0xF0 & 0x3C, 0x0F]), "AND of the writes")
        self.assertEqual(flash.log, [("erase", 0x81000, 0x2800), ("write", 0x81000, 2),
                                     ("write", 0x81000, 2)])
        flash.write(0x90000, b"\x00")
        self.assertEqual(flash.read(0x90000, 1), bytes([regions["k0"][0x10000] & 0]))

    def test_a_write_without_an_erase_shows_as_wrong_data(self) -> None:
        flash = SimFlash()
        flash.write(0x80000, b"\xaa" * 16)
        self.assertNotEqual(flash.read(0x80000, 16), b"\xaa" * 16)

    def test_the_windows_read_the_same_bytes(self) -> None:
        board = self.board
        for address in (0x94080000, 0xB4080000, 0x14080000):
            self.assertEqual(board.read_memory(address, 64), board.flash.read(0x80000, 64))
        self.assertEqual(window_offset(0x94440000), 0x440000)
        self.assertIsNone(window_offset(0x80000000))

    def test_protected_regions_and_outside_operations_are_reported(self) -> None:
        flash = SimFlash()
        before = flash.snapshot()
        flash.erase(0x80000, 0x14C000)
        flash.write(0x80000, b"\x00" * 10)
        self.assertEqual(flash.protected_unchanged(before), [])
        self.assertEqual(flash.operations_outside_bank_0(), [])
        flash.erase(0x44000, 0x1000)
        self.assertEqual(flash.protected_unchanged(before), ["config"])
        self.assertEqual(flash.operations_outside_bank_0(), [("erase", 0x44000, 0x1000)])


class Plan(BenchCase):
    """The lines of a successful run, as the procedure's steps."""

    board_class = FlashBoard

    def test_the_kernel_by_tftp(self) -> None:
        steps = fl.plan("kernel", 0x5F41A, "tftp", arm=False, commit=False)
        self.assertEqual(fl.plan_lines(steps), [
            "printenv", "crc32 94440000 14c000", "crc32 9458c000 274000",
            "run sdsup", "tftpboot 80000000 uImage", "crc32 80000000 5f41a",
            "mw.l b8003268 0",
            "sf erase 80000 +14c000", "sf write 80000000 80000 5f41a", "crc32 94080000 5f41a",
            "crc32 94440000 14c000", "crc32 9458c000 274000"])

    def test_the_root_filesystem_by_ymodem_with_a_trial(self) -> None:
        steps = fl.plan("rootfs", 0x274000, "ymodem", arm=True, commit=True)
        self.assertEqual(fl.plan_lines(steps), [
            "printenv", "crc32 94440000 14c000", "crc32 9458c000 274000",
            "mw.l b8003268 0", "loady 80000000", "crc32 80000000 274000",
            "sf erase 1cc000 +274000", "sf write 80000000 1cc000 274000",
            "crc32 941cc000 274000", "crc32 94440000 14c000", "crc32 9458c000 274000",
            "setenv sw_commit 0", "setenv sw_tryactive 0", "saveenv",
            "printenv sw_commit", "printenv sw_tryactive", "boot"])

    def test_the_watchdog_is_stopped_after_the_tftp_check_and_before_the_ymodem_transfer(self) -> None:
        tftp = fl.plan_lines(fl.plan("k0", 100, "tftp", arm=False, commit=False))
        ymodem = fl.plan_lines(fl.plan("k0", 100, "ymodem", arm=False, commit=False))
        self.assertLess(tftp.index("crc32 80000000 64"), tftp.index("mw.l b8003268 0"))
        self.assertLess(ymodem.index("mw.l b8003268 0"), ymodem.index("loady 80000000"))

    def test_every_planned_line_passes_the_guard_in_order(self) -> None:
        for target in ("kernel", "rootfs", "k0", "r0"):
            for via in ("tftp", "ymodem"):
                for arm, commit in ((False, False), (True, False), (False, True), (True, True)):
                    lines = fl.plan_lines(fl.plan(target, 1234, via, arm=arm, commit=commit))
                    session = guardmod.Session(commit=commit)
                    for line in lines:
                        with self.subTest(target=target, via=via, arm=arm, commit=commit,
                                          line=line):
                            guardmod.check_line(line, {}, session)

    def test_the_targets_are_words_never_offsets(self) -> None:
        for word in ("k1", "r1", "boot", "env", "config", "linux", "all", "0x80000", ""):
            with self.subTest(word=word), self.assertRaises(Refusal):
                fl.plan(word, 100, "tftp", arm=False, commit=False)

    def test_the_line_builders_use_the_tools_table(self) -> None:
        k0, r0 = guardmod.BY_NAME["k0"], guardmod.BY_NAME["r0"]
        self.assertEqual(fl.erase_line(k0), "sf erase 80000 +14c000")
        self.assertEqual(fl.erase_line(r0), "sf erase 1cc000 +274000")
        self.assertEqual(fl.write_line(r0, 0x1234), "sf write 80000000 1cc000 1234")
        self.assertEqual(fl.flash_crc_line(k0, 0x5F41A), "crc32 94080000 5f41a")
        self.assertEqual(fl.bank1_lines(), ["crc32 94440000 14c000", "crc32 9458c000 274000"])

    def test_custom_served_names(self) -> None:
        config = FlashConfig(kernel_name="kernel.img", rootfs_name="root.img")
        self.assertIn("tftpboot 80000000 kernel.img",
                      fl.plan_lines(fl.plan("kernel", 1, "tftp", arm=False, commit=False,
                                            config=config)))
        self.assertIn("tftpboot 80000000 root.img",
                      fl.plan_lines(fl.plan("rootfs", 1, "tftp", arm=False, commit=False,
                                            config=config)))


class FlashCase(BenchCase):
    board_class = FlashBoard
    kernel_size = 4000

    def setUp(self) -> None:
        super().setUp()
        self.board.flash.preload("k0", make_uimage(1500, name="Linux-old", seed=3))
        self.board.flash.preload("r0", make_squashfs(3000, seed=4))
        self.before = self.board.flash.snapshot()
        self.kernel_path = os.path.join(self.tmp, "uImage")
        self.write(self.kernel_path, self.kernel_image)
        self.rootfs = make_squashfs(150_000, seed=5)
        self.rootfs_path = os.path.join(self.tmp, "rootfs")
        self.write(self.rootfs_path, self.rootfs)
        self.board.tftp_files["rootfs"] = self.rootfs
        self.config = FlashConfig(boot=self.boot_config)
        self.gate = SignalGate()
        self.flash_chip = self.board.flash
        self.addCleanup(self.assertBank0Only)  # after any scenario, whatever happened in it

    @staticmethod
    def write(path: str, data: bytes) -> None:
        with open(path, "wb") as handle:
            handle.write(data)

    def run_flash(self, path: str | None = None, *, connect=None, **options):
        target = options.setdefault("target", "kernel")
        path = path or (self.kernel_path if target in ("kernel", "k0") else self.rootfs_path)
        return fl.flash_image(self.ctx, path, FlashOptions(**options), self.config,
                              gate=self.gate, ymodem_connect=connect,
                              server_source=self.server_source)

    @contextmanager
    def serial_connection(self):
        """A raw connection to the board, as the bridge's would be, with the pane released."""
        self.tmux.on_respawn = lambda cmd: self.board.set_pane_attached(
            cmd == self.tmux.original_command)
        with self.console.released_pane():
            raw = RawConsole(BoardPort(self.board, self.clock), self.clock, self.console.log)
            try:
                yield raw
            finally:
                raw.close()

    def assertBank0Only(self) -> None:
        self.assertEqual(self.flash_chip.operations_outside_bank_0(), [])
        self.assertEqual(self.flash_chip.protected_unchanged(self.before), [])

    def assertNothingWritten(self) -> None:
        self.assertEqual(self.flash_chip.log, [])
        self.assertEqual(self.flash_chip.snapshot(), self.before)

    def assertNotArmed(self) -> None:
        self.assertEqual(self.board.model.env["sw_tryactive"], "2")
        self.assertNotIn("saveenv", self.board.executed)
        self.assertFalse([line for line in self.board.executed if line.startswith("setenv")])


class Kernel(FlashCase):

    def test_the_kernel_is_written_verified_and_nothing_else_is_touched(self) -> None:
        result = self.run_flash(target="kernel")
        k0 = self.flash_chip.region("k0")
        self.assertEqual(k0[:len(self.kernel_image)], self.kernel_image)
        self.assertEqual(set(k0[len(self.kernel_image):]), {0xFF}, "the partition was erased")
        self.assertEqual(self.flash_chip.log, [("erase", 0x80000, 0x14C000),
                                               ("write", 0x80000, len(self.kernel_image))])
        self.assertBank0Only()
        self.assertEqual((result.partition, result.length, result.crc32),
                         ("k0", len(self.kernel_image), zlib.crc32(self.kernel_image)))
        self.assertFalse(result.armed or result.booted or result.committed)
        self.assertNotArmed()
        self.assertEqual(self.board.restarts, ["sysrq"], "the watchdog was stopped in time")

    def test_the_lines_typed_are_the_plan(self) -> None:
        self.run_flash(target="kernel")
        expected = fl.plan_lines(fl.plan("kernel", len(self.kernel_image), "tftp", arm=False,
                                         commit=False))
        self.assertEqual(step_lines(self.loader), expected)

    def test_flash_dry_run_matches_the_real_run_with_a_trial(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        self.run_flash(target="kernel", arm=True)
        expected = fl.plan_lines(fl.plan("kernel", len(self.kernel_image), "tftp", arm=True,
                                         commit=False))
        self.assertEqual(step_lines(self.loader), expected)

    def test_the_guards_reads_of_variables_are_not_steps(self) -> None:
        self.run_flash(target="kernel")
        reads = [text for kind, text in self.loader.typed if kind == "read"]
        self.assertIn("printenv sdsup", reads)
        self.assertTrue(all(text.startswith("printenv ") for text in reads))

    def test_no_byte_was_typed_while_a_command_ran(self) -> None:
        self.run_flash(target="kernel")
        self.assertEqual(bytes(self.board.lost_busy), b"")

    def test_the_image_is_pushed_to_the_image_host_for_tftp(self) -> None:
        self.run_flash(target="kernel")
        self.assertTrue(os.path.isfile(os.path.join(self.served, "uImage")))
        self.assertEqual(len(self.runner.host.rules), 1)

    def test_a_good_old_flash_state_reads_back_through_the_window(self) -> None:
        self.run_flash(target="kernel")
        window = self.board.read_memory(0x94080000, len(self.kernel_image))
        self.assertEqual(window, self.kernel_image)

    def test_a_kernel_for_the_wrong_partition_is_refused_before_the_board_is_touched(self) -> None:
        for path, target in ((self.rootfs_path, "kernel"), (self.kernel_path, "rootfs")):
            with self.subTest(target=target), self.assertRaises(ImageError):
                self.run_flash(path, target=target)
        self.assertEqual(self.tmux.calls, [])
        self.assertEqual(self.runner.calls, [])

    def test_an_image_larger_than_its_partition_is_refused(self) -> None:
        big = os.path.join(self.tmp, "big")
        self.write(big, make_uimage(1359872 - 64 + 1))
        with self.assertRaises(ImageError) as caught:
            self.run_flash(big, target="kernel")
        self.assertIn("1359873", str(caught.exception))
        self.assertEqual(self.tmux.calls, [])

    def test_a_kernel_of_exactly_the_partition_size_is_written_whole(self) -> None:
        exact = make_uimage(1359872 - 64, name="Linux-full")
        path = os.path.join(self.tmp, "exact")
        self.write(path, exact)
        self.board.tftp_files["uImage"] = exact
        self.run_flash(path, target="kernel")
        self.assertEqual(self.flash_chip.region("k0"), exact)
        self.assertBank0Only()


class RootFs(FlashCase):

    def test_the_root_filesystem_is_written_to_r0(self) -> None:
        result = self.run_flash(target="rootfs")
        r0 = self.flash_chip.region("r0")
        self.assertEqual(r0[:len(self.rootfs)], self.rootfs)
        self.assertEqual(set(r0[len(self.rootfs):]), {0xFF})
        self.assertEqual(self.flash_chip.log, [("erase", 0x1CC000, 0x274000),
                                               ("write", 0x1CC000, len(self.rootfs))])
        self.assertBank0Only()
        self.assertEqual(self.flash_chip.region("k0"), self.before["k0"], "the kernel is untouched")
        self.assertEqual(result.partition, "r0")
        expected = fl.plan_lines(fl.plan("rootfs", len(self.rootfs), "tftp", arm=False,
                                         commit=False))
        self.assertEqual(step_lines(self.loader), expected)
        self.assertEqual(self.board.restarts, ["sysrq"], "the long erase outlived the fuse")

    def test_a_squashfs_with_a_compression_the_kernel_lacks_is_refused(self) -> None:
        path = os.path.join(self.tmp, "xz")
        self.write(path, make_squashfs(4000, compression=4))
        with self.assertRaises(ImageError):
            self.run_flash(path, target="rootfs")
        self.config.squashfs_compressions = (4,)
        self.board.tftp_files["rootfs"] = make_squashfs(4000, compression=4)
        self.run_flash(path, target="rootfs")
        self.assertEqual(self.flash_chip.region("r0")[:4], b"hsqs")

    def test_the_largest_root_filesystem_is_written_whole(self) -> None:
        big = make_squashfs(2572288, seed=8)
        path = os.path.join(self.tmp, "big")
        self.write(path, big)
        self.board.tftp_files["rootfs"] = big
        self.run_flash(path, target="rootfs")
        self.assertEqual(self.flash_chip.region("r0"), big)
        self.assertBank0Only()


class Trial(FlashCase):

    def test_arm_and_boot_the_trial(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        result = self.run_flash(target="kernel", arm=True)
        self.assertTrue(result.armed and result.booted)
        self.assertEqual(result.fallback_bank, "1")
        self.assertEqual(self.board.kernels_started[-1], "Linux-6.18.48-test")
        self.assertEqual(self.board.model.env["sw_tryactive"], "2", "the trial flag was consumed")
        self.assertEqual(self.board.model.env["sw_commit"], "1", "the committed bank is unchanged")
        self.assertTrue(any("falls back to bank 1" in line for line in self.said))
        self.assertTrue(self.transcript.text().rstrip().endswith("login:"))
        self.assertBank0Only()

    def test_trial_boot_verified(self) -> None:
        """The loader's report of the trial boot is compared with the file that was flashed."""
        self.board.model.env["sw_commit"] = "1"
        original = self.board.model.cmd_bootm

        def other_kernel(args, suffix, depth):
            self.board.flash.preload("k0", make_uimage(1500, name="Linux-old", seed=3))
            return original(args, suffix, depth)

        self.board.model.cmd_bootm = other_kernel  # type: ignore[method-assign]
        from bench.ramboot import WrongKernel
        with self.assertRaises(WrongKernel) as caught:
            self.run_flash(target="kernel", arm=True)
        self.assertIn("Linux-old", str(caught.exception))

    def test_the_trial_is_armed_for_a_root_filesystem_too(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        result = self.run_flash(target="rootfs", arm=True)
        self.assertTrue(result.armed and result.booted)
        self.assertEqual(self.board.kernels_started[-1], "Linux-old", "the kernel of bank 0 booted")
        self.assertEqual(self.flash_chip.region("r0")[:len(self.rootfs)], self.rootfs)

    def test_a_cmdline_without_the_bank_0_root_fails_the_trial(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        self.board.cmdline = "console=ttyS0,115200 root=31:7"
        with self.assertRaises(fl.RamBootError) as caught:
            self.run_flash(target="kernel", arm=True)
        self.assertIn("root=31:5", str(caught.exception))

    def test_arm_warns_when_committed_bank_is_tested(self) -> None:
        """With sw_commit 0 a failed trial is followed by bank 0 again: refused up front."""
        with self.assertRaises(UnexpectedState) as caught:
            self.run_flash(target="kernel", arm=True)
        message = str(caught.exception)
        self.assertIn("sw_commit is 0", message)
        self.assertIn("run b1", message)
        self.assertIn("--accept-same-bank-fallback", message)
        self.assertTrue(any("falls back to bank 0" in line for line in self.said))
        self.assertNothingWritten()
        self.assertNotArmed()

    def test_arming_with_the_same_bank_fallback_needs_the_explicit_word(self) -> None:
        result = self.run_flash(target="kernel", arm=True, accept_same_bank_fallback=True)
        self.assertTrue(result.armed)
        self.assertEqual(result.fallback_bank, "0")

    def test_the_fallback_is_not_stated_when_nothing_is_armed(self) -> None:
        self.run_flash(target="kernel")
        self.assertFalse(any("falls back" in line for line in self.said))

    def test_arm_only_after_verify(self) -> None:
        """A write that stores a wrong byte is caught by the readback and nothing is armed."""
        self.board.model.env["sw_commit"] = "1"
        self.board.write_faults.wrong_byte = True
        with self.assertRaises(FlashError) as caught:
            self.run_flash(target="kernel", arm=True)
        self.assertIn("reads back with CRC-32", str(caught.exception))
        self.assertIn("not armed", str(caught.exception))
        self.assertNotArmed()

    def test_a_ram_copy_that_differs_stops_before_any_write(self) -> None:
        wrong = bytearray(self.kernel_image)
        wrong[100] ^= 0xFF
        self.board.tftp_files["uImage"] = bytes(wrong)
        with self.assertRaises(FlashError) as caught:
            self.run_flash(target="kernel", arm=True, accept_same_bank_fallback=True)
        self.assertIn("RAM copy", str(caught.exception))
        self.assertNothingWritten()
        self.assertNotArmed()

    def test_bank_1_changing_stops_the_arming(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        original = self.board.flash.program
        hit: list[int] = []

        def stray(offset: int, content: bytes) -> None:
            original(offset, content)
            if not hit:
                hit.append(1)
                self.board.flash.data[0x440010] ^= 0x01  # something reached bank 1

        self.board.flash.program = stray  # type: ignore[method-assign]
        with self.assertRaises(Bank1Changed) as caught:
            self.run_flash(target="kernel", arm=True)
        self.assertIn("bank 1", str(caught.exception))
        self.assertNotArmed()
        self.before = self.flash_chip.snapshot()  # the stray bit was put there by this test

    def test_arm_with_a_trial_already_pending_is_refused(self) -> None:
        self.board.model.env["sw_tryactive"] = "0"
        with self.assertRaises(UnexpectedState) as caught:
            self.run_flash(target="kernel", arm=True, accept_same_bank_fallback=True)
        self.assertIn("already pending", str(caught.exception))
        self.assertNothingWritten()

    def test_a_pending_trial_of_bank_one_is_never_touched(self) -> None:
        self.board.model.env["sw_tryactive"] = "1"
        with self.assertRaises(UnexpectedState):
            self.run_flash(target="kernel")
        self.assertNothingWritten()


class Bank1(FlashCase):

    def test_bank1_unchanged(self) -> None:
        """Both targets over both transports leave bank 1 byte for byte as it was, and the
        loader's checksums of it, read before and after the write, are the same."""
        for target in ("kernel", "rootfs"):
            for via in ("tftp", "ymodem"):
                with self.subTest(target=target, via=via):
                    self.setUp()
                    k1, r1 = self.before["k1"], self.before["r1"]
                    self.run_flash(target=target, via=via,
                                   connect=self.serial_connection if via == "ymodem" else None)
                    self.assertEqual(self.flash_chip.region("k1"), k1)
                    self.assertEqual(self.flash_chip.region("r1"), r1)
                    reads = [line for line in step_lines(self.loader)
                             if line in fl.bank1_lines()]
                    self.assertEqual(reads, fl.bank1_lines() * 2, "read before and after")

    def test_the_bank_1_checksums_are_read_through_the_flash_window(self) -> None:
        self.run_flash(target="kernel")
        self.assertEqual(fl.bank1_lines(), ["crc32 94440000 14c000", "crc32 9458c000 274000"])
        for line in fl.bank1_lines():
            self.assertIn(line, step_lines(self.loader))


class ProtectedRegions(FlashCase):

    def test_protected_regions_unchanged(self) -> None:
        """However a run fails, boot, env, env2, config, k1 and r1 are what they were, and every
        operation the flash logged lies inside k0 or r0."""
        faults = {
            "a stall in the write": lambda: setattr(self.board.write_faults, "stall_at", 0.5),
            "a stored byte that is wrong": lambda: setattr(self.board.write_faults,
                                                           "wrong_byte", True),
            "a transfer that keeps failing": lambda: setattr(self.board, "tftp_server_ok", False),
            "a layout value that differs": lambda: self.board.model.env.update(
                fl_kernel1="440000"),
            "a macro that erases bank 1": lambda: self.board.model.env.update(
                sdsup="sf erase 440000 +14c000"),
        }
        for label, inject in faults.items():
            with self.subTest(label):
                self.setUp()
                self.board.model.env["sw_commit"] = "1"
                inject()
                with self.assertRaises((BenchError, Refusal, LayoutMismatch)):
                    self.run_flash(target="kernel", arm=True)
                self.assertBank0Only()

    def test_a_restart_in_the_write_touches_nothing_outside_bank_0(self) -> None:
        original = self.board.model.on_flash

        def restart_during_write(kind, offset, length, memory=0):
            if kind == "write":
                self.clock.schedule(0.02, lambda: self.board.restart("test"))
            return original(kind, offset, length, memory)

        self.board.model.on_flash = restart_during_write  # type: ignore[method-assign]
        with self.assertRaises(BenchError):
            self.run_flash(target="rootfs")
        self.assertBank0Only()


class Commit(FlashCase):

    def test_commit_sets_the_committed_bank_only_on_request(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        result = self.run_flash(target="kernel", commit=True)
        self.assertTrue(result.committed)
        self.assertEqual(self.board.model.env["sw_commit"], "0")
        self.assertEqual(self.board.model.env["sw_tryactive"], "2")
        self.assertIn("setenv sw_commit 0", step_lines(self.loader))
        self.assertIn("printenv sw_commit", step_lines(self.loader))

    def test_nothing_is_committed_without_the_request(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        self.run_flash(target="kernel")
        self.assertEqual(self.board.model.env["sw_commit"], "1")
        self.assertNotIn("setenv sw_commit 0", self.board.executed)

    def test_a_commit_that_changes_nothing_writes_nothing(self) -> None:
        result = self.run_flash(target="kernel", commit=True)
        self.assertFalse(result.committed)
        self.assertNotIn("saveenv", self.board.executed)

    def test_commit_and_arm_save_the_environment_once(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        self.run_flash(target="kernel", commit=True, arm=True)
        self.assertEqual(self.board.executed.count("saveenv"), 1,
                         "one save for both variables (the trial path's own save is the loader's)")
        steps = step_lines(self.loader)
        self.assertEqual(steps[-6:], ["setenv sw_commit 0", "setenv sw_tryactive 0", "saveenv",
                                      "printenv sw_commit", "printenv sw_tryactive", "boot"])


class Refused(FlashCase):

    def test_bank_1_is_refused_before_any_work(self) -> None:
        for bank in ("1", "2", "00", ""):
            with self.subTest(bank=bank), self.assertRaises(Refusal):
                self.run_flash(target="kernel", bank=bank)
        self.assertEqual(self.tmux.calls, [])
        self.assertEqual(self.runner.calls, [])

    def test_protected_targets_are_refused_before_any_work(self) -> None:
        for target in ("k1", "r1", "boot", "env", "env2", "config", "linux", "all", "K0", ""):
            with self.subTest(target=target), self.assertRaises(Refusal):
                self.run_flash(target=target)
        self.assertEqual(self.tmux.calls, [])
        self.assertEqual(self.runner.calls, [])

    def test_layout_mismatch_stops(self) -> None:
        """A layout variable that differs from the table stops the run before any write."""
        self.board.model.env["fl_kernel1"] = "440000"
        with self.assertRaises(LayoutMismatch) as caught:
            self.run_flash(target="kernel")
        self.assertEqual(caught.exception.name, "fl_kernel1")
        self.assertEqual(caught.exception.exit_status, 5)
        self.assertNothingWritten()
        self.assertEqual(step_lines(self.loader), ["printenv"], "nothing but the read was typed")

    def test_every_layout_variable_is_checked(self) -> None:
        for name in guardmod.LAYOUT_VARIABLES:
            with self.subTest(name=name):
                self.setUp()
                self.board.model.env[name] = "1"
                with self.assertRaises(LayoutMismatch) as caught:
                    self.run_flash(target="kernel")
                self.assertEqual(caught.exception.name, name)
                self.assertNothingWritten()

    def test_missing_macro_stops(self) -> None:
        for name in ("sdsup", "ipaddr", "serverip", "netmask"):
            with self.subTest(name):
                self.setUp()
                del self.board.model.env[name]
                with self.assertRaises(MissingVariable) as caught:
                    self.run_flash(target="kernel")
                self.assertIn(repr(name), str(caught.exception))
                self.assertNothingWritten()

    def test_a_server_address_that_disagrees_stops_before_the_transfer(self) -> None:
        self.board.model.env["serverip"] = "198.51.100.9"
        with self.assertRaises(UnexpectedState):
            self.run_flash(target="kernel")
        self.assertNothingWritten()

    def test_a_macro_that_would_write_a_protected_region_is_refused(self) -> None:
        self.board.model.env["sdsup"] = "mw.l bb000100 1; sf erase 440000 +14c000"
        with self.assertRaises(Refusal):
            self.run_flash(target="kernel")
        self.assertNothingWritten()

    def test_read_only_console_refuses_writes(self) -> None:
        self.guard.read_only = True
        with self.assertRaises(Refusal) as caught:
            self.run_flash(target="kernel")
        self.assertIn("read-only", caught.exception.detail)
        self.assertNothingWritten()

    def test_a_busy_console_is_not_shared(self) -> None:
        other = Console(self.console.config, self.tmux, self.clock)
        with other.exclusive("ramboot"):
            with self.assertRaises(ConsoleBusy):
                self.run_flash(target="kernel")
        self.assertEqual(self.tmux.typed, [])

    def test_the_lock_is_released_after_a_run_and_after_a_failure(self) -> None:
        self.run_flash(target="kernel")
        with self.console.exclusive("next"):
            pass
        self.board.model.env["fl_kernel1"] = "1"
        with self.assertRaises(LayoutMismatch):
            self.run_flash(target="kernel")
        with self.console.exclusive("next"):
            pass

    def test_an_unknown_transport_is_refused(self) -> None:
        with self.assertRaises(FlashError):
            self.run_flash(target="kernel", via="carrier-pigeon")


class Failures(FlashCase):

    def test_stall_mid_write_does_not_arm(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        self.board.write_faults.stall_at = 0.5
        started = self.clock.now()
        with self.assertRaises(Stalled) as caught:
            self.run_flash(target="kernel", arm=True)
        message = str(caught.exception)
        self.assertIn("k0 was being written", message)
        self.assertIn("invalid", message)
        self.assertIn("not armed", message)
        self.assertEqual(caught.exception.exit_status, 6)
        self.assertNotArmed()
        self.assertBank0Only()
        k0 = self.flash_chip.region("k0")
        half = len(self.kernel_image) // 2
        self.assertEqual(k0[:half - 400], self.kernel_image[:half - 400], "the first half is there")
        self.assertEqual(set(k0[len(self.kernel_image):]), {0xFF})
        self.assertNotEqual(k0[:len(self.kernel_image)], self.kernel_image, "the write is partial")
        self.assertLess(self.clock.now() - started, 600.0)

    def test_the_stall_limit_is_30_seconds_without_a_new_byte(self) -> None:
        self.board.write_faults.stall_at = 0.5
        with self.assertRaises(Stalled):
            self.run_flash(target="kernel")
        hung = max(t for t in [self.clock.now()])
        self.assertGreater(hung, 0)

    def test_signal_during_write_waits(self) -> None:
        """A signal during the write is held until the command ends; nothing is typed meanwhile."""
        original = self.board.model.on_flash

        def schedule_stop(kind, offset, length, memory=0):
            if kind == "write":
                self.clock.schedule(0.5, lambda: self.gate.request_stop("SIGINT"))
            return original(kind, offset, length, memory)

        self.board.model.on_flash = schedule_stop  # type: ignore[method-assign]
        with self.assertRaises(Interrupted) as caught:
            self.run_flash(target="rootfs", arm=True, accept_same_bank_fallback=True)
        self.assertIn("SIGINT", str(caught.exception))
        self.assertIn("the write of r0", str(caught.exception))
        r0 = self.flash_chip.region("r0")
        self.assertEqual(r0[:len(self.rootfs)], self.rootfs, "the write ran to the end")
        self.assertEqual(bytes(self.board.lost_busy), b"", "nothing was typed, no Ctrl-C")
        self.assertTrue(self.loader.at_prompt(), "the prompt is back")
        self.assertNotArmed()
        with self.console.exclusive("next"):
            pass

    def test_a_signal_after_the_write_stops_before_the_arming(self) -> None:
        original = self.board.model.on_flash

        def late_stop(kind, offset, length, memory=0):
            if kind == "write":
                self.clock.schedule(2.0, lambda: self.gate.request_stop("SIGTERM"))
            return original(kind, offset, length, memory)

        self.board.model.on_flash = late_stop  # type: ignore[method-assign]
        with self.assertRaises(Interrupted) as caught:
            self.run_flash(target="rootfs", arm=True, accept_same_bank_fallback=True)
        self.assertIn("verification", str(caught.exception))
        self.assertEqual(self.flash_chip.region("r0")[:len(self.rootfs)], self.rootfs)
        self.assertNotArmed()

    def test_a_signal_during_the_erase_stops_after_the_erase(self) -> None:
        original = self.board.model.on_flash

        def schedule_stop(kind, offset, length, memory=0):
            if kind == "erase":
                self.clock.schedule(2.0, lambda: self.gate.request_stop("SIGTERM"))
            return original(kind, offset, length, memory)

        self.board.model.on_flash = schedule_stop  # type: ignore[method-assign]
        with self.assertRaises(Interrupted) as caught:
            self.run_flash(target="kernel")
        self.assertIn("the erase of k0", str(caught.exception))
        self.assertEqual(self.flash_chip.log, [("erase", 0x80000, 0x14C000)])
        self.assertEqual(bytes(self.board.lost_busy), b"")
        self.assertEqual(set(self.flash_chip.region("k0")), {0xFF}, "erased and not written")

    def test_a_signal_before_the_erase_writes_nothing(self) -> None:
        self.gate.request_stop("SIGINT")
        with self.assertRaises(Interrupted):
            self.run_flash(target="kernel")
        self.assertNothingWritten()

    def test_a_failed_first_attempt_is_recovered_and_the_write_happens_once(self) -> None:
        self.board.fail_first_after_sysrq = True
        result = self.run_flash(target="kernel")
        self.assertEqual(self.board.restarts, ["sysrq", "fuse"])
        self.assertEqual(self.flash_chip.log, [("erase", 0x80000, 0x14C000),
                                               ("write", 0x80000, len(self.kernel_image))])
        self.assertEqual(result.partition, "k0")
        self.assertBank0Only()

    def test_a_transfer_that_keeps_failing_ends_with_nothing_written(self) -> None:
        self.board.tftp_server_ok = False
        with self.assertRaises(FlashError) as caught:
            self.run_flash(target="kernel")
        self.assertIn("nothing was written", str(caught.exception))
        self.assertNothingWritten()

    def test_the_file_missing_on_the_server_writes_nothing(self) -> None:
        del self.board.tftp_files["uImage"]
        with self.assertRaises(fl.RamBootError):
            self.run_flash(target="kernel")
        self.assertNothingWritten()

    def test_a_bridge_flap_during_the_write_names_the_partition(self) -> None:
        original = self.board.model.on_flash

        def flap_during_write(kind, offset, length, memory=0):
            if kind == "write":
                self.clock.schedule(0.5, self.board.flap)
            return original(kind, offset, length, memory)

        self.board.model.on_flash = flap_during_write  # type: ignore[method-assign]
        with self.assertRaises(ConsoleInterrupted) as caught:
            self.run_flash(target="rootfs", arm=True, accept_same_bank_fallback=True)
        self.assertIn("r0 was being written", str(caught.exception))
        self.assertIn("may be invalid", str(caught.exception))
        self.assertNotArmed()

    def test_a_trial_that_did_not_start_is_cancelled(self) -> None:
        """If ``boot`` does not consume the armed flag, the tool puts it back."""
        self.board.model.env["sw_commit"] = "1"

        def refuses(args, suffix, depth):
            self.board.model.print("Wrong Image Format for bootm command")
            self.board.model.print("ERROR: can't get kernel image!")
            return 1

        self.board.model.cmd_boot = refuses  # type: ignore[method-assign]
        with self.assertRaises(fl.RamBootError) as caught:
            self.run_flash(target="kernel", arm=True)
        self.assertIn("could not start the image", str(caught.exception))
        self.assertEqual(self.board.model.env["sw_tryactive"], "2", "the trial is cancelled")
        self.assertTrue(any("was cancelled" in line for line in self.said))
        self.assertIn("setenv sw_tryactive 2", self.board.executed)

    def test_a_bad_ready_marker_is_refused_before_the_board_is_touched(self) -> None:
        self.board.model.env["sw_commit"] = "1"
        self.config.boot.ready_pattern = "("
        with self.assertRaises(fl.RamBootError):
            self.run_flash(target="kernel", arm=True)
        self.assertEqual(self.tmux.calls, [])

    def test_a_reset_in_the_middle_of_the_write_is_reported(self) -> None:
        original = self.board.model.on_flash

        def reset_during_write(kind, offset, length, memory=0):
            if kind == "write":
                self.clock.schedule(0.02, lambda: self.board.restart("test"))
            return original(kind, offset, length, memory)

        self.board.model.on_flash = reset_during_write  # type: ignore[method-assign]
        with self.assertRaises(UnexpectedState) as caught:
            self.run_flash(target="kernel", arm=True, accept_same_bank_fallback=True)
        self.assertIn("restarted", str(caught.exception))
        self.assertIn("k0 was being written", str(caught.exception))
        self.assertNotArmed()


class Ymodem(FlashCase):

    def test_a_kernel_by_ymodem_through_a_released_pane(self) -> None:
        result = self.run_flash(target="kernel", via="ymodem", connect=self.serial_connection)
        self.assertEqual(self.flash_chip.region("k0")[:len(self.kernel_image)], self.kernel_image)
        self.assertEqual(self.board.ymodem_received[0][0], "uImage")
        self.assertEqual(self.board.ymodem_received[0][1], len(self.kernel_image))
        self.assertEqual(result.partition, "k0")
        expected = fl.plan_lines(fl.plan("kernel", len(self.kernel_image), "ymodem", arm=False,
                                         commit=False))
        self.assertEqual(step_lines(self.loader), expected)
        self.assertBank0Only()
        self.assertEqual(self.tmux.respawns[-1], self.tmux.original_command,
                         "the pane's client is back")
        self.assertTrue(self.tmux.pane_pipe)
        self.assertEqual(self.runner.host.rules, [], "no image host involvement")
        self.assertEqual(self.board.restarts, ["sysrq"])

    def test_the_watchdog_is_stopped_before_the_transfer(self) -> None:
        self.run_flash(target="kernel", via="ymodem", connect=self.serial_connection)
        steps = step_lines(self.loader)
        self.assertLess(steps.index("mw.l b8003268 0"), steps.index("loady 80000000"))

    def test_a_root_filesystem_by_ymodem_with_the_summary_checked(self) -> None:
        self.run_flash(target="rootfs", via="ymodem", connect=self.serial_connection)
        self.assertEqual(self.flash_chip.region("r0")[:len(self.rootfs)], self.rootfs)
        name, length, data = self.board.ymodem_received[0]
        self.assertEqual((name, length), ("rootfs", len(self.rootfs)))
        self.assertEqual(data, self.rootfs)

    def test_ymodem_faults_are_survived(self) -> None:
        self.board.ymodem_faults = Faults(corrupt={2, 40}, drop_ack={5}, lose={7})
        self.run_flash(target="rootfs", via="ymodem", connect=self.serial_connection)
        self.assertEqual(self.flash_chip.region("r0")[:len(self.rootfs)], self.rootfs)

    def test_a_transfer_that_dies_writes_nothing_and_restores_the_pane(self) -> None:
        self.board.ymodem_faults = Faults(silence_after=3)
        with self.assertRaises(FlashError) as caught:
            self.run_flash(target="kernel", via="ymodem", connect=self.serial_connection)
        self.assertIn("nothing was written", str(caught.exception))
        self.assertNothingWritten()
        self.assertEqual(self.tmux.respawns[-1], self.tmux.original_command)

    def test_a_cancel_from_the_receiver_writes_nothing(self) -> None:
        self.board.ymodem_faults = Faults(cancel_after=2)
        with self.assertRaises(FlashError):
            self.run_flash(target="rootfs", via="ymodem", connect=self.serial_connection)
        self.assertNothingWritten()

    def test_a_receiver_that_got_different_bytes_is_caught_by_the_ram_check(self) -> None:
        original = self.board.ram_store

        def damaged(address: int, data: bytes) -> None:
            original(address, data[:100] + bytes([data[100] ^ 1]) + data[101:])

        self.board.ram_store = damaged  # type: ignore[method-assign]
        with self.assertRaises(FlashError) as caught:
            self.run_flash(target="kernel", via="ymodem", connect=self.serial_connection)
        self.assertIn("RAM copy", str(caught.exception))
        self.assertNothingWritten()

    def test_ymodem_through_the_bridge_needs_an_explicit_opt_in(self) -> None:
        with self.assertRaises(UnexpectedState) as caught:
            self.run_flash(target="kernel", via="ymodem")
        self.assertIn("direct serial port", str(caught.exception))
        self.assertIn("--ymodem-over-bridge", str(caught.exception))
        self.assertEqual(self.tmux.calls, [], "refused before the console was touched")
        self.assertEqual(self.runner.calls, [])

    def test_the_opt_in_without_the_bridge_host_is_refused_too(self) -> None:
        self.config.ymodem_over_bridge = True
        self.config.bridge_host = ""
        with self.assertRaises(UnexpectedState) as caught:
            self.run_flash(target="kernel", via="ymodem")
        self.assertIn("direct serial port", str(caught.exception))
        self.assertEqual(self.tmux.calls, [], "refused before the console was touched")

    def test_the_bridge_opt_in_stops_the_client_and_opens_one_connection(self) -> None:
        self.config.ymodem_over_bridge = True
        self.config.bridge_host = "bridge.example"
        self.config.bridge_port = 2001
        opened: list[tuple[str, int]] = []
        self.tmux.on_respawn = lambda cmd: self.board.set_pane_attached(
            cmd == self.tmux.original_command)

        def fake_tcp(host: str, port: int) -> BoardPort:
            opened.append((host, port))
            return BoardPort(self.board, self.clock)

        with mock.patch("bench.flash.TcpPort", fake_tcp):
            self.run_flash(target="kernel", via="ymodem")
        self.assertEqual(opened, [("bridge.example", 2001)])
        self.assertEqual(self.flash_chip.region("k0")[:len(self.kernel_image)], self.kernel_image)
        self.assertEqual(self.tmux.respawns[-1], self.tmux.original_command)

    def test_the_guard_judges_the_raw_connections_lines_too(self) -> None:
        @contextmanager
        def hostile():
            with self.serial_connection() as raw:
                real_type = raw.type_text

                def sneaky(text: str) -> None:
                    real_type(text.replace("80000000", "94440000"))

                raw.type_text = sneaky  # type: ignore[method-assign]
                yield raw

        with self.assertRaises(Exception):
            self.run_flash(target="kernel", via="ymodem", connect=hostile)
        self.assertNothingWritten()


class SignalGateUnit(unittest.TestCase):

    def test_the_gate_holds_a_request_until_it_is_checked(self) -> None:
        gate = SignalGate()
        gate.check("anything")
        gate.request_stop("SIGINT")
        self.assertTrue(gate.stop_requested)
        with self.assertRaises(Interrupted) as caught:
            gate.check("the write")
        self.assertIn("SIGINT", str(caught.exception))
        self.assertIn("the write", str(caught.exception))
        self.assertTrue(issubclass(Interrupted, FlashError))
        self.assertTrue(issubclass(Bank1Changed, UnexpectedState))
        self.assertEqual(WaitTimeout.exit_status, Stalled.exit_status)
