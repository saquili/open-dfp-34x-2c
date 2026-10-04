# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Command line of ``python3 -m bench``.

The commands are ``ramboot``, ``flash``, ``guard-check``, ``serve`` and
``console``, plus ``selftest``. Every command that acts on a board takes
``--dry-run``, which prints the exact lines it would type, each already
approved by the guard, and touches neither the console nor the image host.

Exit status: 0 success; 1 the operation failed; 2 usage; 3 a guard refusal
(reported on standard error as ``refused [<code>] <detail>: <line>``, nothing
having been typed); 4 the console is not usable; 5 the board is in an
unexpected state; 6 a timeout.
"""

from __future__ import annotations

import argparse
import contextlib
import os
import signal
import sys
import unittest
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, field
from typing import TextIO

from . import client as clientmod
from . import flash as flashmod
from . import image, ramboot as rambootmod, settings as settingsmod, tftp
from .console import (BenchError, BoardState, Clock, Console, SystemClock, TmuxRunner, ToolLog,
                      UnexpectedState, WaitTimeout, redact, system_tmux)
from .guard import BY_NAME, Guard, LayoutMismatch, Refusal, check_bank, check_target
from .host import ImageHost, Runner, SubprocessRunner
from .rawport import RawConsole, SerialPort
from .uboot import Loader

EXIT_OK, EXIT_FAILED, EXIT_USAGE, EXIT_REFUSED, EXIT_CONSOLE, EXIT_STATE, EXIT_TIMEOUT = range(7)
HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
DEFAULT_KERNEL = os.path.join(os.path.dirname(TOOLS), "build", "linux", "arch", "mips", "boot",
                              "uImage")


@dataclass
class Services:
    """What the commands use of the outside world; tests substitute all of it."""

    clock: Clock = field(default_factory=SystemClock)
    tmux: TmuxRunner = system_tmux
    runner: Runner = field(default_factory=SubprocessRunner)
    environ: Mapping[str, str] = field(default_factory=lambda: os.environ)
    stdout: TextIO | None = None
    stderr: TextIO | None = None
    install_signals: bool = True

    def out(self) -> TextIO:
        return self.stdout or sys.stdout

    def err(self) -> TextIO:
        return self.stderr or sys.stderr


# --------------------------------------------------------------------------
# The parser
# --------------------------------------------------------------------------

EPILOG = """\
commands:
  ramboot [IMAGE]            boot a trial kernel from RAM over TFTP (writes nothing)
  flash --target T IMAGE     write bank 0: T is kernel or rootfs
  guard-check LINE...        say whether the guard lets lines through (sends nothing)
  serve start|check|stop|run|remove-rule
                             the image host's TFTP server and its firewall allowance
  console check|type LINE|client
                             check the shared console, type one guarded line, or run the
                             pane's client (console.bridge_host and console.bridge_port)
  selftest                   run the unit tests

settings (--set KEY=VALUE, or a TOML file with --config / BENCH_CONFIG):
"""


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python3 -m bench", description="The serial-console bench: bring-up and flashing.",
        epilog=EPILOG + "\n".join("  " + line for line in settingsmod.describe()),
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", help="a TOML file of settings (default: $BENCH_CONFIG)")
    parser.add_argument("--set", action="append", default=[], metavar="KEY=VALUE",
                        help="override one setting; may be repeated")
    sub = parser.add_subparsers(dest="command", metavar="COMMAND")

    guard = sub.add_parser("guard-check", aliases=["guard"],
                           help="say whether lines may reach the loader")
    guard.add_argument("lines", nargs="*", metavar="LINE")
    guard.add_argument("--var", action="append", default=[], metavar="NAME=VALUE",
                       help="a variable as the board has it (a macro too)")
    guard.add_argument("--commit", action="store_true", help="commit mode")
    guard.add_argument("--read-only", action="store_true", help="a read-only console")
    guard.add_argument("--target", action="append", default=[], metavar="WORD")
    guard.add_argument("--bank", action="append", default=[], metavar="N")

    boot = sub.add_parser("ramboot", help="boot a trial kernel from RAM")
    boot.add_argument("image", nargs="?", default=DEFAULT_KERNEL)
    boot.add_argument("--dry-run", action="store_true")
    boot.add_argument("--ready", metavar="PATTERN", help="the image's ready marker (a regex)")
    boot.add_argument("--ready-limit", type=float, metavar="SECONDS")
    boot.add_argument("--recovery", choices=("fuse", "reset"))

    flash = sub.add_parser("flash", help="write bank 0")
    flash.add_argument("image")
    flash.add_argument("--target", required=True, metavar="WORD",
                       help="kernel or rootfs (also k0 or r0); never an offset")
    flash.add_argument("--bank", default="0", metavar="N", help="only 0 is accepted")
    flash.add_argument("--via", choices=("tftp", "ymodem"), default="tftp")
    flash.add_argument("--arm", action="store_true", help="arm a one-shot trial and boot it")
    flash.add_argument("--accept-same-bank-fallback", action="store_true",
                       help="arm although sw_commit names bank 0, so a failed trial returns to it")
    flash.add_argument("--commit", action="store_true",
                       help="make bank 0 the committed bank (sw_commit 0)")
    flash.add_argument("--ymodem-over-bridge", action="store_true",
                       help="try YMODEM through the bridge (not validated)")
    flash.add_argument("--dry-run", action="store_true")

    serve = sub.add_parser("serve", help="the image host's TFTP server")
    serve.add_argument("action", choices=("start", "check", "stop", "run", "remove-rule"))
    serve.add_argument("--image", help="an image to push (start)")
    serve.add_argument("--name", help="the served name (start, check)")
    serve.add_argument("--remove-rule", action="store_true", help="also remove the allowance (stop)")
    serve.add_argument("--dry-run", action="store_true")

    console = sub.add_parser("console", help="check or use the shared console")
    actions = console.add_subparsers(dest="action", metavar="ACTION", required=True)
    actions.add_parser("check", help="is the console usable, and what does the board show")
    typed = actions.add_parser("type", help="type lines through the guard, in one session")
    typed.add_argument("lines", nargs="+", metavar="LINE")
    typed.add_argument("--timeout", type=float, default=30.0, metavar="SECONDS")
    typed.add_argument("--restart", action="store_true",
                       help="restart the board and break in first, unless it is at the prompt")
    actions.add_parser("client", help="run the process of the tmux pane")

    selftest = sub.add_parser("selftest", help="run the unit tests")
    selftest.add_argument("-v", "--verbose", action="store_true")
    selftest.add_argument("--pattern", default="test*.py")
    return parser


# --------------------------------------------------------------------------
# Wiring
# --------------------------------------------------------------------------

class Bench:
    """The objects a command that acts on a board uses."""

    def __init__(self, settings: settingsmod.Settings, svc: Services, stack: contextlib.ExitStack,
                 *, commit: bool = False, use_serial: bool = False) -> None:
        self.settings = settings
        self.svc = svc
        sink = None
        if settings.log_path:
            os.makedirs(os.path.dirname(settings.log_path) or ".", exist_ok=True)
            sink = stack.enter_context(open(settings.log_path, "a", encoding="utf-8"))
        self.log = ToolLog(sink)
        self.console: Console | RawConsole
        if use_serial:
            if not settings.serial_device:
                raise settingsmod.SettingsError("console.serial_device is not set")
            self.console = RawConsole(SerialPort(settings.serial_device), svc.clock, self.log)
            stack.callback(self.console.close)
        else:
            self.console = Console(settings.console, svc.tmux, svc.clock, self.log)
        self.guard = Guard(commit=commit, read_only=settings.console.read_only)
        self.loader = Loader(self.console, self.guard, svc.clock, self.log)
        self.host = ImageHost(settings.host, svc.runner, self.log, svc.clock)
        self.ctx = rambootmod.Context(
            console=self.console, loader=self.loader, guard=self.guard, clock=svc.clock,
            runner=svc.runner, access=settings.access, host=self.host, log=self.log,
            say=lambda text: _say(svc, text))


def _say(svc: Services, text: str) -> None:
    print("bench: " + text, file=svc.out(), flush=True)


def _settings(args: argparse.Namespace, svc: Services) -> settingsmod.Settings:
    return settingsmod.load(args.config, args.set, svc.environ)


def _approved(guard: Guard, line: str) -> str:
    return guard.check(line, macros=False).text


def _print_lines(svc: Services, heading: str, lines: Sequence[str]) -> None:
    for line in lines:
        print("%s %s" % (heading, line), file=svc.out())


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------

def cmd_guard_check(args: argparse.Namespace, svc: Services) -> int:
    variables = {}
    for item in args.var:
        name, sep, value = item.partition("=")
        if not sep:
            print("bench: --var takes NAME=VALUE, not %r" % item, file=svc.err())
            return EXIT_USAGE
        variables[name] = value
    guard = Guard(commit=args.commit, read_only=args.read_only, variables=variables)
    macros = bool(variables)  # the macros can be judged only when they are given
    checks: list[Callable[[], str]] = []
    for line in args.lines:
        checks.append(lambda line=line: "allow: " + guard.check(line, macros=macros).text)
    for word in args.target:
        checks.append(lambda word=word: "allow: target %s" % check_target(word))
    for value in args.bank:
        checks.append(lambda value=value: "allow: bank %d" % check_bank(value))
    if not checks:
        print("bench: guard-check needs a line, --target or --bank", file=svc.err())
        return EXIT_USAGE
    status = EXIT_OK
    for check in checks:
        try:
            print(check(), file=svc.out())
        except Refusal as refusal:
            print(str(refusal), file=svc.err())
            status = EXIT_REFUSED
    return status


def cmd_ramboot(args: argparse.Namespace, svc: Services) -> int:
    settings = _settings(args, svc)
    config = settings.ramboot
    if args.ready is not None:
        config.ready_pattern = args.ready
    if args.ready_limit is not None:
        config.ready_limit = args.ready_limit
    if args.recovery:
        config.recovery = args.recovery
    data = image.read_file(args.image)
    img = image.check_uimage(data, image.RAM_BOOT_LIMIT, what=args.image)
    if args.dry_run:
        guard = Guard(read_only=settings.console.read_only)
        host = ImageHost(settings.host, svc.runner, clock=svc.clock)
        print("image %s: %d bytes, name %r" % (args.image, img.length, img.name), file=svc.out())
        _print_lines(svc, "image host:", host.plan(rambootmod.SERVER_SOURCE, args.image,
                                                   config.image_name))
        print("restart (only when the board is not at the loader prompt): "
              "ssh ... echo b > /proc/sysrq-trigger", file=svc.out())
        _print_lines(svc, "would type:", [_approved(guard, "printenv"),
                                          _approved(guard, "run ramboot")])
        print("(the macros that run ramboot would execute are read from the board and checked "
              "when it is run)", file=svc.out())
        return EXIT_OK
    with contextlib.ExitStack() as stack:
        bench = Bench(settings, svc, stack)
        result = rambootmod.ramboot(bench.ctx, args.image, config)
    print("ramboot ok: %s, %d bytes transferred, %d attempt(s), %.1f s"
          % (result.image.name, result.transferred, result.attempts, result.elapsed),
          file=svc.out())
    for note in result.notes:
        print("note: " + note, file=svc.out())
    return EXIT_OK


def cmd_flash(args: argparse.Namespace, svc: Services) -> int:
    check_bank(args.bank)
    check_target(args.target)
    settings = _settings(args, svc)
    config = settings.flash
    config.ymodem_over_bridge = args.ymodem_over_bridge
    options = flashmod.FlashOptions(
        target=args.target, bank=args.bank, via=args.via, arm=args.arm, commit=args.commit,
        accept_same_bank_fallback=args.accept_same_bank_fallback)
    if args.dry_run:
        return _flash_dry_run(args, svc, settings, options)
    use_serial = args.via == "ymodem" and bool(settings.serial_device)
    gate = flashmod.SignalGate()
    previous: dict[int, object] = {}
    if svc.install_signals:
        def stop(number: int, _frame: object) -> None:
            gate.request_stop(signal.Signals(number).name)
            print("bench: %s received; waiting for the running command to finish"
                  % signal.Signals(number).name, file=svc.err(), flush=True)
        for number in (signal.SIGINT, signal.SIGTERM):
            previous[number] = signal.signal(number, stop)
    try:
        with contextlib.ExitStack() as stack:
            bench = Bench(settings, svc, stack, commit=args.commit, use_serial=use_serial)
            connect = (lambda: contextlib.nullcontext(bench.console)) if use_serial else None
            result = flashmod.flash_image(bench.ctx, args.image, options, config, gate=gate,
                                          ymodem_connect=connect)  # type: ignore[arg-type]
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)  # type: ignore[arg-type]
    print("flash ok: %s, %d bytes, CRC-32 %08x%s%s%s" % (
        result.partition, result.length, result.crc32,
        ", trial armed and booted" if result.booted else "",
        ", bank 0 committed" if result.committed else "",
        ", fallback bank %s" % result.fallback_bank if result.armed else ""), file=svc.out())
    for note in result.notes:
        print("note: " + note, file=svc.out())
    return EXIT_OK


def _flash_dry_run(args: argparse.Namespace, svc: Services, settings: settingsmod.Settings,
                   options: flashmod.FlashOptions) -> int:
    part = BY_NAME[check_target(options.target)]
    data = image.read_file(args.image)
    if part.name == "k0":
        image.check_uimage(data, part.size, what=args.image)
    else:
        image.check_squashfs(data, settings.flash.squashfs_compressions, part.size,
                             what=args.image)
    steps = flashmod.plan(options.target, len(data), options.via, arm=options.arm,
                          commit=options.commit, config=settings.flash)
    guard = Guard(commit=options.commit, read_only=settings.console.read_only)
    print("%s: %d bytes for %s by %s" % (args.image, len(data), part.name, options.via),
          file=svc.out())
    if options.via == "tftp":
        host = ImageHost(settings.host, svc.runner, clock=svc.clock)
        served = settings.flash.kernel_name if part.name == "k0" else settings.flash.rootfs_name
        _print_lines(svc, "image host:", host.plan(rambootmod.SERVER_SOURCE, args.image, served))
    for step in steps:
        print("# %s" % step.name, file=svc.out())
        for line in step.lines:
            print("would type: %s" % _approved(guard, line), file=svc.out())
    if options.commit:
        print("(sw_commit is set only when it does not already read 0)", file=svc.out())
    return EXIT_OK


def cmd_serve(args: argparse.Namespace, svc: Services) -> int:
    settings = _settings(args, svc)
    cfg = settings.host
    if args.action == "run":
        return tftp.main(["--dir", cfg.served_dir, "--bind", cfg.bind, "--port", str(cfg.port)])
    host = ImageHost(cfg, svc.runner, clock=svc.clock)
    name = args.name or settings.flash.kernel_name
    if args.dry_run:
        _print_lines(svc, "image host:", host.plan(
            rambootmod.SERVER_SOURCE, args.image, name if args.image else None))
        return EXIT_OK
    if args.action == "start":
        if args.image:
            host.push_image(args.image, name)
            _say(svc, "pushed %s as %s" % (os.path.basename(args.image), name))
        host.start_server(rambootmod.SERVER_SOURCE)
        _say(svc, "the TFTP server is listening on %s:%d" % (cfg.bind, cfg.port))
        outcome = host.ensure_rule()
        _say(svc, "the firewall allowance: %s" % outcome)
        return EXIT_OK
    if args.action == "check":
        status = host.check(name)
        print("file %s: %s" % (name, "%d bytes" % status.file_size if status.file_size is not None
                               else "missing"), file=svc.out())
        print("server: %s" % ("listening" if status.listening else "not listening"),
              file=svc.out())
        print("firewall allowance: %s" % status.rule, file=svc.out())
        return EXIT_OK if not status.problems(name) else EXIT_FAILED
    if args.action == "stop":
        host.stop_server()
        _say(svc, "the TFTP server was stopped")
        if args.remove_rule:
            _say(svc, "removed %d allowance rule(s)" % host.remove_rule())
        return EXIT_OK
    _say(svc, "removed %d allowance rule(s)" % host.remove_rule())
    return EXIT_OK


def cmd_console(args: argparse.Namespace, svc: Services) -> int:
    settings = _settings(args, svc)
    if args.action == "client":
        if not settings.bridge_host:
            print("bench: console.bridge_host is not set", file=svc.err())
            return EXIT_USAGE
        clientmod.Client(settings.bridge_host, settings.bridge_port, clock=svc.clock).run()
        return EXIT_OK
    with contextlib.ExitStack() as stack:
        bench = Bench(settings, svc, stack)
        console = bench.console
        assert isinstance(console, Console)
        if args.action == "check":
            check = console.check_usable()
            print("console %s is usable; the board shows: %s" % (
                settings.console.session, check.state.value), file=svc.out())
            return EXIT_OK
        with console.exclusive("console type"):
            check = console.check_usable()
            if check.state is not BoardState.LOADER_PROMPT:
                if not args.restart:
                    raise UnexpectedState("the console shows: %s, not the loader prompt; nothing "
                                          "was typed" % check.state.value)
                _say(svc, rambootmod.reach_loader_prompt(bench.ctx, settings.ramboot))
            # One guard session: a saveenv is judged by the setenv before it.
            for line in args.lines:
                try:
                    result = bench.loader.command(line, timeout=args.timeout)
                except WaitTimeout as exc:
                    print("bench: %s" % exc, file=svc.err())
                    return EXIT_TIMEOUT
                print(redact(result.output), file=svc.out())  # the environment holds a MAC
        return EXIT_OK


def cmd_selftest(args: argparse.Namespace, svc: Services) -> int:
    suite = unittest.defaultTestLoader.discover(os.path.join(HERE, "tests"),
                                                pattern=args.pattern, top_level_dir=TOOLS)
    result = unittest.TextTestRunner(stream=svc.err(), verbosity=2 if args.verbose else 1).run(suite)
    return EXIT_OK if result.wasSuccessful() else EXIT_FAILED


COMMANDS: dict[str, Callable[[argparse.Namespace, Services], int]] = {
    "guard-check": cmd_guard_check, "guard": cmd_guard_check, "ramboot": cmd_ramboot,
    "flash": cmd_flash, "serve": cmd_serve, "console": cmd_console, "selftest": cmd_selftest,
}


def main(argv: Sequence[str] | None = None, services: Services | None = None) -> int:
    """Run a command; return its exit status."""
    svc = services or Services()
    parser = build_parser()
    try:
        with contextlib.redirect_stdout(svc.out()), contextlib.redirect_stderr(svc.err()):
            args = parser.parse_args(argv)
    except SystemExit as exit_:
        return exit_.code if isinstance(exit_.code, int) else EXIT_USAGE
    if not args.command:
        parser.print_usage(svc.err())
        return EXIT_USAGE
    try:
        return COMMANDS[args.command](args, svc)
    except Refusal as refusal:
        print(str(refusal), file=svc.err())
        return refusal.exit_status
    except LayoutMismatch as mismatch:
        print("bench: the board's layout does not match: %s" % mismatch, file=svc.err())
        return mismatch.exit_status
    except BenchError as error:
        print("bench: %s" % error, file=svc.err())
        return error.exit_status
    except KeyboardInterrupt:
        print("bench: interrupted", file=svc.err())
        return 130
