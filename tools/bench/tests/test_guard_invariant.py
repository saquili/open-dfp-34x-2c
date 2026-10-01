# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The guard's invariant, tried on mutated lines against a permissive loader.

For lines made by mutating the verdict tables' lines, whatever the guard
allows must, when the permissive model of the loader executes it, change no
flash byte outside ``k0`` and ``r0``, store nothing into the flash windows,
start no arbitrary code, and change no environment variable except the ones
the loader and the allowed macros set themselves. The environments include
hostile ones: a changed layout, values that hold a second command, nested
variables, macros that erase protected regions, and a boot chain with an
extra erase on the branch that runs. Each hostile variant is made twice, on
the simple tables and on the board's shape, whose partition tables hold
parentheses; the model takes a parenthesis outside ``setenv`` as the start of
a group that runs commands, so a variant whose values hold ``(go)`` is
dangerous to a guard that lets a parenthesis through.

The generator is seeded and the seed is printed on failure. Guards broken on
purpose -- no range rule, any ``run``, an unchecked boot chain, parentheses
accepted in every macro part -- must each produce violations, which shows
that the test can fail.
"""

from __future__ import annotations

import os
import random
import time
import unittest
from unittest import mock

from bench import guard
from bench.guard import Guard, Refusal

from . import vectors
from .loadermodel import LoaderModel

SEED = 20261001
ITERATIONS = int(os.environ.get("BENCH_MUTATIONS", "400000"))

# The invariant is stated here, independently of the guard's own tables.
K0 = (0x80000, 0x1CC000)
R0 = (0x1CC000, 0x440000)
APERTURE = [(low + base, high + base) for base in (0, 0x80000000, 0xA0000000)
            for low, high in ((0x14000000, 0x15000000), (0x1FC00000, 0x20000000))]
SELF_SET = {"sw_tryactive", "sw_active", "bootargs", "filesize", "fileaddr"}


SHAPES = (vectors.BOOT_BASE, vectors.E6)  # simple tables, then the board's
MACROS = dict(
    yu="sf erase 0 +40000", upb="sf erase 0 +40000; sf write 80000000 0 40000",
    upe="erase 94000000 +40000", erase_env="sf erase 40000 +4000",
    erase_cfgfs="sf erase 44000 +3c000", upk1="sf erase 440000 +14c000",
    upr1="sf erase 58c000 +274000", yk="sf erase 80000 +14c000; loady 80000000",
    yr="sf erase 1cc000 +274000", upt="sf update 80000000 0 800000")


def hostile(base: dict[str, str]) -> list[dict[str, str]]:
    """The variants of one base environment: plain, then each way to turn it hostile."""
    tables = base["mtdparts0"]
    return [
        dict(base),
        dict(base, **MACROS),
        dict(base, ramboot=vectors.RAMBOOT + "; sf erase 440000 +14c000"),
        dict(base, ramboot=vectors.RAMBOOT + "; run yu", **MACROS),
        dict(base, sdsup=vectors.SDSUP + "; mw.l 94440000 0"),
        dict(base, sdsup=vectors.SDSUP + "; cp.b 80000000 9fc00000 40000"),
        dict(base, b0=vectors.B0 + "; sf erase 0 +40000"),
        dict(base, b0=vectors.B0.replace("${img0_kernel}", "${img1_kernel}")),
        dict(base, mtdparts0=tables + "; sf erase 0 +40000"),
        dict(base, hide="sf erase 0 +40000", ramboot="run sdsup; ${hide}"),
        dict(base, hide="run yu", ramboot="run sdsup; ${hide}", **MACROS),
        dict(base, b0="setenv bootargs ${bootargs_base}; ${img0_kernel}", img0_kernel="reset"),
        dict(base, img="uImage; sf erase 0 +40000", ramboot=vectors.RAMBOOT.replace(
            "uImage", "${img}")),
        dict(base, boot_by_tryactive=vectors.BOOT_BY_TRYACTIVE + "; sf erase 440000 +14c000",
             sw_tryactive="0"),
        dict(base, boot_by_tryactive=vectors.BOOT_BY_TRYACTIVE + "; run upk1",
             sw_tryactive="0", **MACROS),
        dict(base, boot_by_commit=vectors.BOOT_BY_COMMIT + "; sf erase 58c000 +274000",
             sw_tryactive="2"),
        dict(base, boot_by_commit=vectors.BOOT_BY_COMMIT + "; run erase_env", **MACROS),
        dict(base, bootcmd=vectors.BOOTCMD + "; run upb", **MACROS),
        dict(base, sw_commit="1"),
        dict(base, sw_tryactive="1"),
        dict(base, sw_tryactive="0"),
        dict(base, en_wdt="mw 94440000 0", sw_tryactive="0"),
        dict(base, en_wdt="mw.l bfc00000 0", sw_tryactive="0"),
        dict(base, fl_kernel1="440000", fl_kernel1_sz="14c000"),
        dict(base, a="$b", b="440000", c="${a}"),
        dict(base, x="80000", y="14c000", z="+14c000"),
        # parentheses: data in the kernel arguments, a group that runs a command anywhere else
        dict(base, mtdparts0=tables + " (sf erase 0 +40000)",
             mtdparts1=base["mtdparts1"] + " (sf erase 0 +40000)"),
        dict(base, bootargs_base="(go)"),
        dict(base, mtdparts0=tables + " ) ("),
        dict(base, sw_tryactive="(go)"),
        dict(base, sw_commit="(go)"),
        dict(base, sw_commit="1", img1_kernel="(go)"),
        dict(base, ramboot=vectors.RAMBOOT.replace("uImage", "${mtdparts0}")),
        dict(base, hide="(sf erase 0 +40000)", ramboot="run sdsup; ${hide}"),
    ]


def environments() -> list[dict[str, str]]:
    """The environments of the verdict tables and the hostile ones, on each base."""
    envs = [dict(env) for env in vectors.ENVIRONMENTS.values()]
    for base in SHAPES:
        envs += hostile(base)
    return envs


def plain_environments() -> list[dict[str, str]]:
    """The environments the flash-line generator uses: no variables, E1, and each base's first three."""
    envs = [dict(env) for env in list(vectors.ENVIRONMENTS.values())[:2]]
    for base in SHAPES:
        envs += hostile(base)[:3]
    return envs


def seed_cases() -> list[tuple[str, dict[str, str]]]:
    """The lines that mutations start from, each with its environment."""
    cases = [(v.line, vectors.ENVIRONMENTS[v.env]) for v in vectors.LINE_VECTORS]
    macro_lines = ("run ramboot", "run sdsup", "run b0", "run yu", "run upb", "run b1",
                   "run erase_env", "run upk1", "run yk", "boot", "boot", "bootm 94080000",
                   "saveenv", "setenv sw_tryactive 0", "setenv sw_tryactive 2",
                   "setenv sw_commit 0", "setenv sw_active 1", "sf update 80000000 80000 100",
                   "erase 94080000 +14c000", "erase 94440000 +1000", "cp.b 80000000 94440000 100",
                   "mw.l 94440000 0", "mw.b bfc00000 0", "go 94080000", "loop", "mm.l 94440000",
                   "nm.w b4440000", "crc32 94440000 4 b4440000", "sf erase ${x} ${z}",
                   "sf erase ${x} +${y}", "sf write 80000000 ${x} ${y}")
    for env in environments():
        for line in macro_lines:
            cases.append((line, env))
    return cases


CHARS = "0123456789abcdefABCDEFxX +-_.;&|(){}<>$`'\"\\=/\t\n"
NUMBER_CHARS = "0123456789abcdef"
SEPARATORS = ("; ", " && ", " || ", "\n", ";", "&&", " | ", " `", "$(")


def mutate(rng: random.Random, line: str, other: str, names: list[str]) -> str:
    """Apply one to three edits: insert, delete, swap, replace, wrap, join."""
    for _ in range(rng.choice((1, 1, 1, 2, 2, 3))):
        choice = rng.randrange(12)
        if choice == 0 and line:
            at = rng.randrange(len(line))
            line = line[:at] + rng.choice(CHARS) + line[at + 1:]
        elif choice == 1:
            at = rng.randrange(len(line) + 1)
            line = line[:at] + rng.choice(CHARS) + line[at:]
        elif choice == 2 and line:
            at = rng.randrange(len(line))
            line = line[:at] + line[at + 1:]
        elif choice == 3 and len(line) > 1:
            at = rng.randrange(len(line) - 1)
            line = line[:at] + line[at + 1] + line[at] + line[at + 2:]
        elif choice in (4, 5):
            digits = [i for i, ch in enumerate(line) if ch in NUMBER_CHARS]
            if digits:
                at = rng.choice(digits)
                line = line[:at] + rng.choice(NUMBER_CHARS) + line[at + 1:]
        elif choice == 6:
            words = line.split(" ")
            if words and names:
                at = rng.randrange(len(words))
                words[at] = rng.choice(("${%s}", "$%s", "+${%s}", "x${%s}")) % rng.choice(names)
                line = " ".join(words)
        elif choice == 7:
            line = line + rng.choice(SEPARATORS) + other
        elif choice == 8:
            words, extra = line.split(" "), other.split(" ")
            if words and extra:
                words[rng.randrange(len(words))] = rng.choice(extra)
                line = " ".join(words)
        elif choice == 9:
            line = rng.choice(("", " ", "\t", "  ")) + line + rng.choice(("", " ", "\t"))
        elif choice == 10:
            words = line.split(" ")
            if len(words) > 1:
                at = rng.randrange(len(words))
                if rng.random() < 0.5:
                    del words[at]
                else:
                    words.insert(at, words[at])
                line = " ".join(words)
        elif choice == 11:
            words = line.split(" ")
            at = rng.randrange(len(words))
            if words[at] and all(ch in NUMBER_CHARS for ch in words[at].lstrip("+")):
                words[at] = rng.choice(("0x", "0X", "+", "00", "")) + words[at]
                line = " ".join(words)
    return line


# Values around every partition edge, the chip's end and the 32-bit limits.
EDGES = [0, 0x1000, 0x2000, 0x3C000, 0x3FFFF, 0x40000, 0x41FFF, 0x42000, 0x43FFF, 0x44000,
         0x7FFFF, 0x80000, 0x14C000, 0x1CBFFF, 0x1CC000, 0x274000, 0x43FFFF, 0x440000,
         0x58BFFF, 0x58C000, 0x7FFFFF, 0x800000, 0xFFFFFFFF, 0x100000000, 0x100080000]
WINDOW_EDGES = [0x13FFFFFF, 0x14000000, 0x14440000, 0x14FFFFFF, 0x15000000, 0x1FBFFFFF,
                0x1FC00000, 0x1FFFFFFF, 0x20000000, 0x80000000, 0x81000000, 0xB8003268]


def spell(rng: random.Random, value: int) -> str:
    """A number in one of the spellings the guard has to tell apart."""
    value = max(value, 0)
    return rng.choice(("%x", "%x", "0x%x", "%X", "0X%x", "%08x", "%016x")) % value


def edge(rng: random.Random) -> int:
    return rng.choice(EDGES) + rng.choice((-2, -1, 0, 0, 0, 1, 2))


def generate_flash_line(rng: random.Random) -> str:
    """An ``sf`` line with an offset and a length near the partition edges."""
    offset, length = spell(rng, edge(rng)), spell(rng, edge(rng))
    if rng.random() < 0.5:
        return "sf erase %s +%s" % (offset, length)
    return "sf write %s %s %s" % (rng.choice(("80000000", "0x80000000")), offset, length)


def generate_memory_line(rng: random.Random) -> str:
    """A line that stores or reads at an address near a flash window's edge."""
    base = rng.choice((0, 0x80000000, 0xA0000000))
    address = spell(rng, rng.choice(WINDOW_EDGES) + base + rng.choice((-1, 0, 0, 1))
                    if rng.random() < 0.7 else rng.choice(WINDOW_EDGES))
    template = rng.choice(("mw.l %s 0", "mw.b %s ff", "mw %s 1 10", "loady %s", "tftpboot %s uImage",
                           "cp.b 80000000 %s 100", "crc32 %s 10", "sf write %s 80000 100",
                           "mm.l %s", "erase %s +1000", "crc32 80000000 10 %s"))
    return template % address


def violations(model: LoaderModel, original: dict[str, str], commit: bool) -> list[str]:
    """What an executed line did that the invariant forbids."""
    found: list[str] = []
    for op in model.ops:
        if op.kind in ("erase", "write"):
            for start, end in op.ranges:
                if not any(low <= start and end <= high for low, high in (K0, R0)):
                    found.append("%s %s of %x-%x" % (op.kind, op.detail, start, end))
        elif op.kind == "store":
            for start, end in op.ranges:
                if any(start < high and end > low for low, high in APERTURE):
                    found.append("store %s into %x-%x" % (op.detail, start, end))
        elif op.kind == "exec" and op.detail == "go":
            found.append("go")
    allowed = SELF_SET | ({"sw_commit"} if commit else set())
    for name in set(original) | set(model.env):
        if original.get(name) != model.env.get(name) and name not in allowed:
            found.append("variable %s changed" % name)
    return found


class Tally:
    """What a run of the invariant test saw."""

    def __init__(self) -> None:
        self.lines = 0
        self.allowed = 0
        self.refused = 0
        self.flash_ops = 0
        self.env_changes = 0
        self.violations: list[tuple[str, dict, list[str], bool]] = []

    def add(self, line: str, env: dict[str, str], commit: bool, found: list[str]) -> None:
        if found:
            self.violations.append((line, env, found, commit))


def judge(tally: Tally, lines: list[str], env: dict[str, str], commit: bool) -> None:
    """Judge one case: lines in one session; what is allowed is executed on the model."""
    grd = Guard(variables=env, commit=commit)
    runs: list[tuple[str, str]] = []
    for line in lines:
        tally.lines += 1
        try:
            approval = grd.check(line)
        except Refusal:
            tally.refused += 1
            continue
        tally.allowed += 1
        runs.append((line, approval.text))
    if not runs:
        return
    # What the tool types, and what the loader would read from the raw line.
    for index in (1, 0):
        model = LoaderModel(env=dict(env))
        for pair in runs:
            model.run(pair[index])
        found = violations(model, env, commit)
        tally.flash_ops += sum(1 for op in model.ops if op.kind in ("erase", "write"))
        tally.env_changes += sum(1 for name in model.env if env.get(name) != model.env[name])
        tally.add(" ; ".join(pair[index] for pair in runs), env, commit, found)


def run_invariant(iterations: int, seed: int, stop_at_first: bool = False) -> Tally:
    rng = random.Random(seed)
    envs = environments()
    plain = plain_environments()
    seeds = seed_cases()
    names = sorted({name for env in envs for name in env})
    tally = Tally()
    for line, env in seeds:  # the unmutated rows first
        judge(tally, [line], env, False)
        judge(tally, [line], env, True)
    for _ in range(iterations):
        line, env = rng.choice(seeds)
        if rng.random() < 0.3:
            env = rng.choice(envs)
        other = rng.choice(seeds)[0]
        commit = rng.random() < 0.25
        kind = rng.random()
        if kind < 0.30:
            line, env = generate_flash_line(rng), rng.choice(plain)
        elif kind < 0.40:
            line, env = generate_memory_line(rng), rng.choice(plain[:2])
        first = mutate(rng, line, other, names) if rng.random() < 0.7 or kind >= 0.40 else line
        if rng.random() < 0.15:
            second = "saveenv" if rng.random() < 0.5 else mutate(rng, other, line, names)
            judge(tally, [first, second], env, commit)
        else:
            judge(tally, [first], env, commit)
        if stop_at_first and tally.violations:
            break
    return tally


class PermissiveModel(unittest.TestCase):
    """What the model does with parentheses, which is what the sixth control stands on."""

    @staticmethod
    def run_line(text: str, **env: str) -> LoaderModel:
        model = LoaderModel(env=dict(env))
        model.run(text)
        return model

    def test_parentheses_are_data_inside_the_words_of_a_setenv(self) -> None:
        for text in ("setenv x a(b) c(d)", "setenv x (sf erase 0 +40000)", "setenv x ( ) (",
                     "then setenv x (go)", "if setenv x (go)"):
            with self.subTest(text=text):
                model = self.run_line(text)
                self.assertEqual(model.ops, [], text)
        self.assertEqual(self.run_line("setenv bootargs a(b) c(d)").env["bootargs"], "a(b) c(d)")

    def test_a_parenthesis_anywhere_else_starts_a_group_that_runs_commands(self) -> None:
        erase = self.run_line("(sf erase 0 +40000)")
        self.assertEqual([(op.kind, op.ranges) for op in erase.ops], [("erase", [(0, 0x40000)])])
        for text in ("itest.s a(go) == b", "if itest.s (go) == 2; then true; fi",
                     "tftpboot 81000000 x(go)y", "mw.l b8003268(go) 0", "bootm (go)"):
            with self.subTest(text=text):
                self.assertIn(("exec", "go"), [(op.kind, op.detail) for op in
                                               self.run_line(text).ops])

    def test_a_group_runs_with_its_command_not_before_and_not_in_a_branch_not_taken(self) -> None:
        model = self.run_line("if itest.s 1 == 2; then itest.s (go) == 1; fi")
        self.assertEqual(model.ops, [])
        model = self.run_line("true; itest.s (go) == 1")
        self.assertEqual([op.detail for op in model.ops], ["go"])

    def test_quoted_and_escaped_parentheses_are_ordinary(self) -> None:
        for text in ("echo '(go)'", 'echo "(go)"', "echo \\(go\\)"):
            with self.subTest(text=text):
                model = self.run_line(text)
                self.assertEqual(model.ops, [], text)
                self.assertEqual(model.output, ["(go)"])

    def test_an_unbalanced_group_runs_to_the_end_of_the_line(self) -> None:
        self.assertEqual([op.detail for op in self.run_line("echo x (go").ops], ["go"])

    def test_a_group_inside_a_group_and_a_group_that_recurses_end(self) -> None:
        self.assertEqual([op.detail for op in self.run_line("((go))").ops], ["go"])
        model = self.run_line("(boot)", bootcmd="(boot)")
        self.assertIn("macro nesting too deep", model.output)


class AnyName:
    """A stand-in for the set of runnable macros that contains everything."""

    def __contains__(self, name: object) -> bool:
        return True


class GuardInvariant(unittest.TestCase):

    def test_what_the_guard_allows_cannot_write_outside_bank_0(self) -> None:
        started = time.monotonic()
        tally = run_invariant(ITERATIONS, SEED)
        elapsed = time.monotonic() - started
        if tally.violations:
            line, env, found, commit = tally.violations[0]
            self.fail("seed %d: %d violations; the first: %r (commit %s) -> %s"
                      % (SEED, len(tally.violations), line, commit, found))
        # The run must reach the interesting part: lines were allowed, executed,
        # and some of them erased, wrote and saved.
        self.assertGreaterEqual(tally.lines, ITERATIONS)
        self.assertGreater(tally.allowed, ITERATIONS // 100, "seed %d" % SEED)
        self.assertGreater(tally.flash_ops, ITERATIONS // 200, "seed %d" % SEED)
        self.assertGreater(tally.env_changes, ITERATIONS // 200, "seed %d" % SEED)
        self.assertGreater(tally.refused, tally.allowed)
        self.assertLess(elapsed, 20, "the mutation run must stay well under 20 seconds")

    def test_the_seed_lines_cover_every_environment(self) -> None:
        self.assertGreater(len(environments()), 25)
        self.assertGreater(len(seed_cases()), 133 + 25 * 20)

    def control(self, label: str, patches: list, expect: str) -> None:
        """A guard broken on purpose must let the invariant be violated."""
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)
        tally = run_invariant(6000, SEED, stop_at_first=True)
        self.assertTrue(tally.violations, "%s must produce violations" % label)
        self.assertTrue(any(item.startswith(expect) for found in tally.violations
                            for item in found[2]),
                        "%s: %r not among %r" % (label, expect, tally.violations[0][2]))

    def test_negative_control_without_the_range_rule(self) -> None:
        self.control("no range rule",
                     [mock.patch.object(guard, "check_range", lambda offset, length: None)],
                     expect="erase")

    def test_negative_control_with_any_run_allowed(self) -> None:
        self.control("any run",
                     [mock.patch.object(guard, "RUN_MACROS", AnyName()),
                      mock.patch.object(guard, "preflight_macro", lambda name, variables: None)],
                     expect="erase")

    def test_negative_control_with_the_boot_chain_unchecked(self) -> None:
        self.control("boot chain unchecked",
                     [mock.patch.object(guard, "preflight_boot", lambda variables: None)],
                     expect="erase")

    def test_negative_control_with_an_off_by_one_range_rule(self) -> None:
        def lenient(offset: int, length: int):
            end = offset + length
            if length < 1 or end > guard.FLASH_SIZE:
                raise Refusal("protected-range", "x")
            for part in guard.PROTECTED:
                if offset < part.end and end > part.start + 1:
                    raise Refusal("protected-range", part.name)

        self.control("off by one", [mock.patch.object(guard, "check_range", lenient)],
                     expect="erase")

    def test_negative_control_with_parentheses_accepted_in_every_macro_part(self) -> None:
        def anywhere(text: str, variables, bootargs: bool) -> str:
            expanded = guard.expand(text, variables, undefined_empty=bootargs)
            guard._check_expanded(expanded.replace("(", "").replace(")", ""))
            return expanded

        self.control("parentheses in every part",
                     [mock.patch.object(guard, "_expand_part", anywhere)], expect="go")

    def test_negative_control_without_the_aperture_rule(self) -> None:
        anywhere = {"mw.l": lambda words, variables, session: None,
                    "loady": lambda words, variables, session: None}
        self.control("no aperture rule",
                     [mock.patch.object(guard, "_check_aperture", lambda words: None),
                      mock.patch.dict(guard._FORMS, anywhere)],
                     expect="store")


if __name__ == "__main__":
    unittest.main()
