# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The guard's verdicts as data: one row per line, macro, word and sequence.

Each row has an identifier; a test that iterates over the rows fails with
that identifier. A line vector is judged from a fresh session with commit
mode off. Environments are named: ``E0`` knows no variables; ``E1`` is the
board's layout with the two kernel window addresses, ``sw_tryactive`` 2,
``sw_commit`` 0 and a file size; ``E2`` changes a layout value; ``E3`` has a
bank 1 trial pending; ``E4`` holds a value that is a second command; ``E5``
holds a value that names another variable; ``E6`` is the board's shape for
the macro rows: the macros and the boot chain with partition tables that hold
parentheses, ``rst2dfl_flg`` undefined.
"""

from __future__ import annotations

from dataclasses import dataclass, field

LAYOUT = {
    "fl_boot_sz": "40000",
    "fl_env": "40000",
    "fl_env2": "42000",
    "fl_env_sz": "2000",
    "fl_cfgfs": "44000",
    "fl_cfgfs_sz": "3c000",
    "fl_kernel1": "80000",
    "fl_kernel1_sz": "14c000",
    "fl_rootfs1": "1cc000",
    "fl_rootfs1_sz": "274000",
    "fl_kernel2": "440000",
    "fl_kernel2_sz": "14c000",
    "fl_rootfs2": "58c000",
    "fl_rootfs2_sz": "274000",
}

E1 = dict(LAYOUT, img0_kernel="94080000", img1_kernel="94440000", sw_tryactive="2",
          sw_commit="0", filesize="5f41a")

ENVIRONMENTS: dict[str, dict[str, str]] = {
    "E0": {},
    "E1": dict(E1),
    "E2": dict(E1, fl_kernel1="440000"),
    "E3": dict(E1, sw_tryactive="1"),
    "E4": dict(E1, evil="440000; sf erase 0 +40000"),
    "E5": dict(E1, a="$b", b="440000"),
}

# The macros of the preflight rows. The first rows let ``mtdparts`` hold any
# text; the rows of E6 hold the board's partition tables, parentheses included.
SDSUP = "mw.l bb000100 1; mw.l bb000104 2"
RAMBOOT = ("run sdsup; tftpboot 0x81000000 uImage; setenv bootargs ${bootargs_base} "
           "${mtdparts0} ${rst2dfl_flg}; bootm 0x81000000")
B0 = "setenv bootargs ${bootargs_base} ${mtdparts0}; bootm ${img0_kernel}"
B1 = "setenv bootargs ${bootargs_base} ${mtdparts1}; bootm ${img1_kernel}"
BOOTCMD = ("if itest.s ${sw_tryactive} == 2; then run boot_by_commit; "
           "else run boot_by_tryactive; fi")
BOOT_BY_COMMIT = "if itest.s ${sw_commit} == 0; then run b0; else run b1; fi"
BOOT_BY_TRYACTIVE = "setenv sw_tryactive 2; saveenv; run en_wdt; run b0"
EN_WDT = "mw b8003268 e7c00000"

RUN_BASE = dict(E1, bootargs_base="console=ttyS0,115200", mtdparts0="bank0 root=31:5",
                sdsup=SDSUP, ramboot=RAMBOOT, b0=B0)
BOOT_BASE = dict(RUN_BASE, mtdparts1="bank1 root=31:7", bootcmd=BOOTCMD,
                 boot_by_commit=BOOT_BY_COMMIT, boot_by_tryactive=BOOT_BY_TRYACTIVE,
                 en_wdt=EN_WDT, b1=B1)

# E6, the board's shape: the base environment and the chain, with partition
# tables as the kernel takes them (they hold parentheses and ``=``).
MTDPARTS_BANK0 = ("mtdparts=spi_nor:256K(boot),8K(env),8K(env2),240K(config),"
                  "1328K(k0)ro,2512K(r0)ro,1328K(k1),2512K(r1) root=31:5")
MTDPARTS_BANK1 = MTDPARTS_BANK0[:-len("root=31:5")] + "root=31:7"
E6 = dict(BOOT_BASE, mtdparts0=MTDPARTS_BANK0, mtdparts1=MTDPARTS_BANK1)

# The same, with the macros as long as the board's: a ``sdsup`` of several
# register writes and a boot chain that records the active bank.
SET_ACT0 = "setenv sw_active 0; saveenv"
SET_ACT1 = "setenv sw_active 1; saveenv"
BOARD_SDSUP = ("mw.l bb000100 1; mw.l bb000104 2; mw.l bb000108 3; mw.l bb00010c 4; "
               "mw.l bb000110 5")
BOARD = dict(
    E6, sdsup=BOARD_SDSUP, set_act0=SET_ACT0, set_act1=SET_ACT1,
    boot_by_commit=("if itest.s ${sw_commit} == 0; then run set_act0; run b0; "
                    "else run set_act1; run b1; fi"),
    boot_by_tryactive="setenv sw_tryactive 2; saveenv; run set_act0; run en_wdt; run b0")


@dataclass(frozen=True)
class LineVector:
    """A line, the environment it is judged in and the guard's verdict."""

    id: str
    line: str
    env: str
    verdict: str  # "allow" or "refuse"
    code: str = ""
    detail: str | None = None  # None where the sheet fixes no detail


def _g(n: int, line: str, env: str, verdict: str, code: str = "",
       detail: str | None = None) -> LineVector:
    return LineVector("GV-%d" % n, line, env, verdict, code, detail)


def _allow(n: int, line: str, env: str = "E0") -> LineVector:
    return _g(n, line, env, "allow")


def _refuse(n: int, line: str, code: str, detail: str | None = None,
            env: str = "E0") -> LineVector:
    return _g(n, line, env, "refuse", code, detail)


PR = "protected-range"
PV_ = "protected-variable"
FA = "flash-aperture"
NA = "not-allowed"
SY = "syntax"

LINE_VECTORS: tuple[LineVector, ...] = (
    # the lines the tool types
    _allow(1, ""),
    _allow(2, "printenv"),
    _allow(3, "printenv sw_commit"),
    _allow(4, "setenv sw_tryactive 0"),
    _allow(5, "mw.l b8003268 0"),
    _allow(6, "loady 80000000"),
    _allow(7, "sf erase 80000 +14c000"),
    _allow(8, "sf erase 1cc000 +274000"),
    _allow(9, "sf write 80000000 80000 5f41a"),
    _allow(10, "sf write 80000000 1cc000 274000"),
    _allow(11, "crc32 94440000 14c000"),
    _allow(12, "tftpboot 80000000 uImage"),
    _allow(13, "run ramboot"),
    _allow(14, "reset"),
    _allow(15, "boot", "E1"),
    # spellings of allowed lines
    _allow(16, "mw.l 0xB8003268 0x0"),
    _allow(17, "sf   erase  0x00080000\t+0x14c000"),
    _allow(18, "sf erase 0X80000 +14C000"),
    _allow(19, "crc32 bfc00000 40000"),
    _allow(20, "sf erase 43f000 +1000"),
    _refuse(21, "sf erase 1cb000 +2000", PR, "bank0-span"),
    # the other bank, directly
    _refuse(22, "sf erase 440000 +14c000", PR, "k1"),
    _refuse(23, "sf erase 58c000 +274000", PR, "r1"),
    _refuse(24, "sf write 80000000 440000 5f41a", PR, "k1"),
    _refuse(25, "sf write 80000000 58c000 274000", PR, "r1"),
    _refuse(26, "   sf erase 440000 +14c000", PR, "k1"),
    # boot, environment and configuration partitions
    _refuse(27, "sf erase 0 +40000", PR, "boot"),
    _refuse(28, "sf write 80000000 0 40000", PR, "boot"),
    _refuse(29, "sf erase 40000 +2000", PR, "env"),
    _refuse(30, "sf erase 42000 +2000", PR, "env2"),
    _refuse(31, "sf erase 44000 +3c000", PR, "config"),
    _refuse(32, "sf erase 0 +800000", PR, "boot"),
    # ranges that cross a boundary
    _refuse(33, "sf erase 7f000 +2000", PR, "config"),
    _refuse(34, "sf erase 43f000 +1001", PR, "k1"),
    _refuse(35, "sf erase 1cc000 +274001", PR, "k1"),
    _refuse(36, "sf write 80000000 7ffff 2", PR, "config"),
    _refuse(37, "sf write 80000000 43ffff 2", PR, "k1"),
    # numbers that try to fool a parser
    _refuse(38, "sf erase 100440000 +1000", "overflow"),
    _refuse(39, "sf erase 80000 +ffffffffffffffff", "overflow"),
    _refuse(40, "sf erase 80000 +10000000000000000", SY),
    _refuse(41, "sf erase ffff0000 +20000", PR, "beyond"),
    _refuse(42, "sf erase 80000 +ffffffff", PR, "beyond"),
    _refuse(43, "sf erase 800000 +1000", PR, "beyond"),
    _refuse(44, "sf erase 4456448 +1000", PR, "beyond"),
    _refuse(45, "sf erase 80000 +0", PR, "zero length"),
    _refuse(46, "sf erase +80000 +1000", SY),
    _refuse(47, "sf erase -80000 +1000", SY),
    _refuse(48, "sf erase 80000 14c000", NA),
    _refuse(49, "sf erase 80000 + 14c000", NA),
    _refuse(50, "sf erase 8_0000 +1000", SY),
    _refuse(51, "sf erase 0440000 +14c000", PR, "k1"),
    _refuse(52, "sf erase 80000 +14c000 440000", NA),
    _refuse(53, "sf write 81000000 80000 100", NA),
    # spellings of commands
    _refuse(54, "SF ERASE 440000 +14c000", NA),
    _refuse(55, 's"f" erase 440000 +14c000', SY),
    _refuse(56, "sf er 440000 +14c000", NA),
    _refuse(57, "sf update 80000000 440000 14c000", NA),
    _refuse(58, "sf read 80000000 440000 14c000", NA),
    _refuse(59, "sf probe 0", NA),
    _refuse(60, "erase 94440000 +14c000", NA),
    _refuse(61, "protect off all", NA),
    # several commands on one line
    _refuse(62, "sf erase 80000 +14c000; sf erase 440000 +14c000", SY),
    _refuse(63, "sf erase 80000 +14c000 && sf erase 0 +40000", SY),
    _refuse(64, "printenv || sf erase 0 +40000", SY),
    _refuse(65, "printenv `sf erase 0 +40000`", SY),
    _refuse(66, "printenv $(sf erase 0 +40000)", SY),
    _refuse(67, "printenv\nsf erase 0 +40000", SY),
    _refuse(68, "if true; then sf erase 440000 +14c000; fi", SY),
    # variables that expand to protected offsets
    _allow(69, "sf erase ${fl_kernel1} +${fl_kernel1_sz}", "E1"),
    _allow(70, "sf write 80000000 ${fl_kernel1} ${filesize}", "E1"),
    _refuse(71, "sf erase ${fl_kernel2} +${fl_kernel2_sz}", PR, "k1", "E1"),
    _refuse(72, "sf erase $fl_rootfs2 +$fl_rootfs2_sz", PR, "r1", "E1"),
    _refuse(73, "sf erase ${fl_boot_sz} +${fl_env_sz}", PR, "env", "E1"),
    _refuse(74, "sf erase ${fl_cfgfs} +${fl_cfgfs_sz}", PR, "config", "E1"),
    _refuse(75, "sf erase ${fl_kernel1} +${fl_kernel1_sz}", PR, "k1", "E2"),
    _refuse(76, "sf write 80000000 ${fl_kernel1} ${filesize}", PR, "k1", "E2"),
    _refuse(77, "sf erase ${nosuch} +14c000", "unknown-variable", None, "E1"),
    _refuse(78, "sf erase ${evil} +14c000", SY, None, "E4"),
    _refuse(79, "sf erase ${a} +14c000", SY, None, "E5"),
    _refuse(80, "sf erase ${fl_kernel2 +14c000", SY, None, "E1"),
    # addresses in the memory-mapped flash window
    _refuse(81, "mw.l 94440000 0", FA, "94440000"),
    _refuse(82, "mw.l b4440000 0", FA, "b4440000"),
    _refuse(83, "mw.l 14440000 0", FA, "14440000"),
    _refuse(84, "mw.b 94000000 ff", FA, "94000000"),
    _refuse(85, "mw.l bfc00000 0", FA, "bfc00000"),
    _refuse(86, "mw.l 1fc10000 0", FA, "1fc10000"),
    _refuse(87, "cp.b 80000000 9fc00000 40000", FA, "9fc00000"),
    _refuse(88, "loady bfc00000", FA, "bfc00000"),
    _refuse(89, "cp.b 80000000 94440000 14c000", FA, "94440000"),
    _refuse(90, "loady 94440000", FA, "94440000"),
    _refuse(91, "tftpboot 94440000 uImage", FA, "94440000"),
    _refuse(92, "sf write 94080000 80000 100", FA, "94080000"),
    # other memory writes and loads
    _refuse(93, "mw.l b8003268 80000000", NA),
    _refuse(94, "mw.l 18003268 0", NA),
    _refuse(95, "mw.l bb000100 1", NA),
    _refuse(96, "cp.b 80000000 81000000 100", NA),
    _refuse(97, "loady 81000000", NA),
    _refuse(98, "crc32 94440000 14c000 80000000", NA),
    _refuse(99, "tftpboot 80000000 ../uImage", NA),
    # macros
    _refuse(100, "run yk", NA),
    _refuse(101, "run yr", NA),
    _refuse(102, "run yu", NA),
    _refuse(103, "run upb", NA),
    _refuse(104, "run upe", NA),
    _refuse(105, "run upk1", NA),
    _refuse(106, "run upr1", NA),
    _refuse(107, "run erase_env", NA),
    _refuse(108, "run erase_cfgfs", NA),
    _refuse(109, "run b1", NA),
    _refuse(110, "run ${evil}", SY, None, "E4"),
    _refuse(111, "run ramboot ramboot", NA),
    # the environment and booting
    _refuse(112, "setenv sw_tryactive 1", PV_, "sw_tryactive"),
    _refuse(113, "setenv sw_tryactive 00", PV_, "sw_tryactive"),
    _refuse(114, "setenv sw_tryactive", PV_, "sw_tryactive"),
    _refuse(115, "setenv sw_tryactive 0 1", PV_, "sw_tryactive"),
    _refuse(116, "setenv sw_commit 1", PV_, "sw_commit"),
    _refuse(117, "setenv sw_commit 0", PV_, "sw_commit"),
    _refuse(118, "setenv sw_active 1", PV_, "sw_active"),
    _refuse(119, "setenv fl_kernel1 440000", PV_, "fl_kernel1"),
    _refuse(120, "setenv img0_kernel 94440000", PV_, "img0_kernel"),
    _refuse(121, "setenv bootcmd run b1", PV_, "bootcmd"),
    _refuse(122, "setenv ramboot run yu", PV_, "ramboot"),
    _refuse(123, "saveenv", "state"),
    _refuse(124, "boot", "bank1-trial", None, "E3"),
    _refuse(125, "boot x", NA),
    _refuse(126, "go 94440000", NA),
    _refuse(127, "bootm 94440000", NA),
    # lengths and characters
    _refuse(128, "printenv " + "a" * 70, NA),
    _refuse(129, "printenv " + "a" * 300, SY),
    _refuse(130, "printenv sw_commit\x00", SY),
    _refuse(131, "printenv sw_commit\x03", SY),
    _refuse(132, "printenv sw_commit\r", SY),
    _refuse(133, "printenv ѕw_commit", SY),
)


@dataclass(frozen=True)
class MacroVector:
    """A typed line whose macros the preflight reads, and the verdict.

    ``base`` names the environment (``run`` rows or ``boot`` rows); ``changes``
    maps a variable to its new value, or to ``None`` where it is removed.
    """

    id: str
    line: str
    base: str  # "run" or "boot"
    changes: dict[str, str | None] = field(default_factory=dict)
    verdict: str = "allow"
    code: str = ""


def _p(n: int, line: str, base: str, changes: dict[str, str | None] | None = None,
       verdict: str = "allow", code: str = "") -> MacroVector:
    return MacroVector("PV-%d" % n, line, base, dict(changes or {}), verdict, code)


MACRO_BASES = {"run": RUN_BASE, "boot": BOOT_BASE, "e6": E6}

MACRO_VECTORS: tuple[MacroVector, ...] = (
    _p(1, "run ramboot", "run"),
    _p(2, "run sdsup", "run"),
    _p(3, "run b0", "run"),
    _p(4, "run ramboot", "run",
       {"ramboot": RAMBOOT + "; sf erase ${fl_kernel2} +${fl_kernel2_sz}"}, "refuse", NA),
    _p(5, "run ramboot", "run", {"sdsup": SDSUP + "; mw.l 94440000 0"}, "refuse", FA),
    _p(6, "run ramboot", "run", {"sdsup": "mw.l 81000000 0"}, "refuse", NA),
    _p(7, "run ramboot", "run", {"ramboot": "run yu; bootm 0x81000000"}, "refuse", NA),
    _p(8, "run ramboot", "run", {"ramboot": RAMBOOT + "; saveenv"}, "refuse", NA),
    _p(9, "run b0", "run", {"img0_kernel": "94440000"}, "refuse", NA),
    _p(10, "run b0", "run", {"img0_kernel": "b4080000"}),
    _p(11, "run ramboot", "run", {"sdsup": None}, "refuse", "missing-macro"),
    _p(12, "run ramboot", "run", {"ramboot": "run sdsup", "sdsup": "run ramboot"},
       "refuse", "depth"),
    _p(13, "run ramboot", "run", {"ramboot": "if itest.s 1 == 1; then run sdsup; fi"},
       "refuse", NA),
    _p(14, "run ramboot", "run", {"ramboot": "tftpboot b4440000 uImage"}, "refuse", FA),
    _p(15, "run ramboot", "run", {"ramboot": "tftpboot 0x81000000 'uImage'"}, "refuse", SY),
    _p(16, "run ramboot", "run", {"ramboot": "tftpboot ${loadaddr} uImage"},
       "refuse", "unknown-variable"),
    _p(17, "run yk", "run", {"yk": "loady 80000000"}, "refuse", NA),
    _p(18, "run ramboot", "run", {"ramboot": "run sdsup || sf erase 0 +40000"}, "refuse", NA),
    _p(19, "run ramboot", "run",
       {"ramboot": "run sdsup; ${hide}", "hide": "sf erase 0 +40000"}, "refuse", NA),
    _p(20, "boot", "boot"),
    _p(21, "boot", "boot",
       {"boot_by_commit": BOOT_BY_COMMIT + "; sf erase ${fl_kernel2} +${fl_kernel2_sz}"},
       "refuse", NA),
    _p(22, "boot", "boot", {"en_wdt": "mw 94440000 0"}, "refuse", FA),
    _p(23, "boot", "boot",
       {"bootcmd": "run boot_by_commit; erase 94440000 +14c000"}, "refuse", NA),
    _p(24, "boot", "boot", {"img1_kernel": "94080123"}, "refuse", NA),
    _p(25, "boot", "boot",
       {"boot_by_tryactive": BOOT_BY_TRYACTIVE + "; setenv sw_commit 1"}, "refuse", NA),
    _p(26, "boot", "boot", {"bootcmd": None}, "refuse", "missing-macro"),
    _p(27, "boot", "boot", {"boot_by_commit": "run bootcmd"}, "refuse", "depth"),
    _p(28, "boot", "boot",
       {"boot_by_tryactive": BOOT_BY_TRYACTIVE + "; run yu", "yu": "sf erase 0 +40000"},
       "refuse", NA),
    _p(29, "boot", "boot",
       {"boot_by_tryactive": BOOT_BY_TRYACTIVE + "; while true; do run b1; done"},
       "refuse", NA),
    # the board's shape (E6): partition tables with parentheses
    _p(30, "run ramboot", "e6"),
    _p(31, "run b0", "e6"),
    _p(32, "boot", "e6"),
    _p(33, "run b0", "e6", {"mtdparts0": MTDPARTS_BANK0 + "; sf erase 0 +40000"}, "refuse", SY),
    _p(34, "run ramboot", "e6", {"mtdparts0": MTDPARTS_BANK0 + " {"}, "refuse", SY),
    _p(35, "run ramboot", "e6", {"ramboot": "setenv bootargs a(b)"}, "refuse", SY),
    _p(36, "run ramboot", "e6", {"ramboot": "tftpboot 0x81000000 ${mtdparts0}"}, "refuse", SY),
    _p(37, "run ramboot", "e6", {"ramboot": RAMBOOT + "; (sf erase 0 +40000)"}, "refuse", SY),
    _p(38, "run b0", "e6", {"mtdparts0": MTDPARTS_BANK0 + " $x"}, "refuse", SY),
    _p(39, "run ramboot", "e6", {"mtdparts0": MTDPARTS_BANK0 + ' x"y'}, "refuse", SY),
    _p(40, "run b0", "e6", {"mtdparts0": MTDPARTS_BANK0 + " `x`"}, "refuse", SY),
)


def macro_environment(vector: MacroVector) -> dict[str, str]:
    """The variables a preflight row is judged in."""
    env = dict(MACRO_BASES[vector.base])
    for name, value in vector.changes.items():
        if value is None:
            env.pop(name, None)
        else:
            env[name] = value
    return env


@dataclass(frozen=True)
class WordVector:
    """A target word or a bank number and the verdict."""

    id: str
    text: str
    verdict: str
    result: str = ""  # the target partition or the bank, where allowed
    code: str = ""
    detail: str | None = None


TARGET_VECTORS: tuple[WordVector, ...] = (
    WordVector("NV-1", "kernel", "allow", "k0"),
    WordVector("NV-2", "k0", "allow", "k0"),
    WordVector("NV-3", "rootfs", "allow", "r0"),
    WordVector("NV-4", "r0", "allow", "r0"),
    WordVector("NV-5", "k1", "refuse", code=PR, detail="k1"),
    WordVector("NV-6", "r1", "refuse", code=PR, detail="r1"),
    WordVector("NV-7", "boot", "refuse", code=PR, detail="boot"),
    WordVector("NV-8", "env", "refuse", code=PR, detail="env"),
    WordVector("NV-9", "env2", "refuse", code=PR, detail="env2"),
    WordVector("NV-10", "config", "refuse", code=PR, detail="config"),
    WordVector("NV-11", "linux", "refuse", code=NA),
    WordVector("NV-12", "K0", "refuse", code=NA),
    WordVector("NV-13", "all", "refuse", code=NA),
    WordVector("NV-14", "", "refuse", code=NA),
)

# ``text`` is what follows ``BANK=``.
BANK_VECTORS: tuple[WordVector, ...] = (
    WordVector("BV-1", "0", "allow", "0"),
    WordVector("BV-2", "1", "refuse", code=PR, detail="bank 1"),
    WordVector("BV-3", "2", "refuse", code=NA),
    WordVector("BV-4", "00", "refuse", code=NA),
    WordVector("BV-5", "", "refuse", code=NA),
)


@dataclass(frozen=True)
class SequenceVector:
    """Lines judged in order in one session, and a verdict for each."""

    id: str
    lines: tuple[str, ...]
    commit: bool
    verdicts: tuple[tuple[str, str], ...]  # (verdict, code) per line


def _s(n: int, lines: tuple[str, ...], commit: bool,
       verdicts: tuple[tuple[str, str], ...]) -> SequenceVector:
    return SequenceVector("SV-%d" % n, lines, commit, verdicts)


A = ("allow", "")
SEQUENCE_VECTORS: tuple[SequenceVector, ...] = (
    _s(1, ("saveenv",), False, (("refuse", "state"),)),
    _s(2, ("setenv sw_tryactive 0", "saveenv"), False, (A, A)),
    _s(3, ("setenv sw_tryactive 0", "saveenv", "saveenv"), False,
       (A, A, ("refuse", "state"))),
    _s(4, ("setenv sw_tryactive 2", "saveenv"), False, (A, A)),
    _s(5, ("setenv sw_tryactive 1", "saveenv"), False,
       (("refuse", PV_), ("refuse", "state"))),
    _s(6, ("setenv sw_commit 0", "saveenv"), True, (A, A)),
    _s(7, ("setenv sw_commit 1", "saveenv"), True,
       (("refuse", PV_), ("refuse", "state"))),
    _s(8, ("setenv sw_commit 0",), False, (("refuse", PV_),)),
)
