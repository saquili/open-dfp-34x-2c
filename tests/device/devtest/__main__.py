# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""python3 -m devtest: run the device tests and write down what they found."""

from __future__ import annotations

import argparse
import datetime
import os
import subprocess
import sys

from . import cases_system, cases_net, cases_pon, cases_data, cases_update  # noqa: F401
from .access import NETNS, Settings, bench_remote, redact, stick_remote
from .core import CASES, FAIL, Context, run_case, select
from .tools import PROGRAMS, TOOLS_DIR, remove
from .report import as_json, counts, log, page


def git(*args: str) -> str:
    done = subprocess.run(["git", *args], capture_output=True, text=True,
                          cwd=os.path.dirname(os.path.abspath(__file__)))
    return done.stdout.strip()


def copyright_holder() -> str:
    """The holder of the files of this directory, as its README names them."""
    readme = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "README.rst")
    with open(readme, encoding="utf-8") as handle:
        for line in handle:
            if "SPDX-FileCopyrightText:" in line:
                return line.split("SPDX-FileCopyrightText:", 1)[1].strip().split(" ", 1)[1]
    raise SystemExit("devtest: the README names no copyright holder")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="devtest", description=__doc__)
    parser.add_argument("--bench", default=os.environ.get("DEVTEST_BENCH", ""),
                        help="the bench host, as ssh knows it (DEVTEST_BENCH)")
    parser.add_argument("--stick", default=os.environ.get("DEVTEST_STICK", ""),
                        help="the stick's management address (DEVTEST_STICK)")
    parser.add_argument("--bench-netns", default=os.environ.get("DEVTEST_BENCH_NETNS", ""),
                        metavar="NAME",
                        help="the bench host's network namespace that holds the stick: the "
                             "stick is reached, and every bench command run, inside it, as "
                             "root (DEVTEST_BENCH_NETNS)")
    parser.add_argument("--olt-port", default=os.environ.get("DEVTEST_OLT_PORT", ""),
                        help="the bench host's port on the network side, behind the OLT")
    parser.add_argument("--subscriber-port", default=os.environ.get("DEVTEST_SUBSCRIBER_PORT", ""),
                        help="the bench host's port at the far end of the stick's host link")
    parser.add_argument("--token-file", default=os.environ.get("DEVTEST_TOKEN_FILE", ""),
                        help="a file with the status daemon's token")
    parser.add_argument("--tools", default=os.environ.get("DEVTEST_TOOLS", ""), metavar="DIR",
                        help="a build's diag directory (build/image/diag): the cases that need "
                             "tc, bridge, devlink, dcb, ethtool or ubus, which the flash's image "
                             "lacks, have them put on the stick, in memory, for the run "
                             "(DEVTEST_TOOLS)")
    parser.add_argument("--update", default=os.environ.get("DEVTEST_UPDATE", ""), metavar="FILE",
                        help="an update file (update.bin) for the case that writes the other "
                             "bank, starts it on trial, keeps it and goes back (DEVTEST_UPDATE)")
    parser.add_argument("--overwrite-shipped", action="store_true",
                        help="let that case write over a bank that holds the shipped firmware")
    parser.add_argument("--no-sudo", action="store_true",
                        help="the bench host runs the packet generator without sudo")
    parser.add_argument("--destructive", action="store_true",
                        help="also run the cases that reset the stick")
    parser.add_argument("--reload", default=os.environ.get("DEVTEST_RELOAD", ""),
                        help="a command, run here, that brings the stick back right after a "
                             "case resets it, such as a RAM boot that breaks into the "
                             "loader's countdown (DEVTEST_RELOAD); without one, the case waits "
                             "for the loader")
    parser.add_argument("--out", default="", help="write NAME.rst, NAME.log and NAME.json")
    parser.add_argument("--title", default="", help="the title of the page")
    parser.add_argument("--list", action="store_true", help="list the cases and stop")
    parser.add_argument("cases", nargs="*", help="cases or areas to run (all by default)")
    args = parser.parse_args(argv)

    if args.list:
        for c in CASES:
            print(f"{c.id:24} {c.check}{'  [destructive]' if c.destructive else ''}")
        return 0
    if not args.bench or not args.stick:
        parser.error("--bench and --stick are needed")
    if args.bench_netns and not NETNS.fullmatch(args.bench_netns):
        parser.error(f"--bench-netns: {args.bench_netns!r} is not a namespace's name")

    token = ""
    if args.token_file:
        with open(args.token_file, encoding="utf-8") as handle:
            token = handle.read().strip()
    settings = Settings(bench=args.bench, stick=args.stick, bench_netns=args.bench_netns,
                        bench_sudo=not args.no_sudo,
                        olt_port=args.olt_port, subscriber_port=args.subscriber_port,
                        pond_token=token, reload=args.reload, tools=args.tools,
                        update=args.update, overwrite_shipped=args.overwrite_shipped)
    trail: list[str] = []
    ctx = Context(settings, stick_remote(settings, trail), bench_remote(settings, trail), trail,
                  destructive=args.destructive)

    # What ran, read before the first case: a case may reset the stick onto
    # another boot, and the tests are the ones loaded now.
    started = datetime.datetime.now(datetime.timezone.utc)
    header = {
        "Date": started.strftime("%Y-%m-%d %H:%M UTC"),
        "Source": f"{git('rev-parse', '--short=12', 'HEAD')}"
                  + (" with local changes" if git("status", "--porcelain") else ""),
        "Kernel": redact(ctx.stick.run("uname -rv").out.strip(), settings),
        "Image": redact(ctx.stick.run(". /etc/os-release && "
                                      "echo \"$PRETTY_NAME, build ${BUILD_ID:-unknown}\"")
                        .out.strip(), settings),
    }
    results = []
    for c in select(args.cases):
        result = run_case(c, ctx)
        results.append(result)
        print(f"{result.status.upper():4}  {result.id:24} {result.evidence}", flush=True)
    if ctx.memo.get("tools"):
        names = list(PROGRAMS)
        header["Tools"] = (f"{', '.join(names[:-1])} and {names[-1]} put on the stick in "
                           f"{TOOLS_DIR}, in memory")
    remove(ctx)
    print(counts(results))

    if args.out:
        title = args.title or f"Device tests of {started:%Y-%m-%d}"
        holder = f"{started:%Y} {copyright_holder()}"
        with open(args.out + ".rst", "w", encoding="utf-8") as handle:
            handle.write(page(title, header, results, holder))
        with open(args.out + ".log", "w", encoding="utf-8") as handle:
            handle.write(log(results))
        with open(args.out + ".json", "w", encoding="utf-8") as handle:
            handle.write(as_json(header, results))
        print(f"written {args.out}.rst, .log and .json")
    return 1 if any(r.status == FAIL for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
