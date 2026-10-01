# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of isa-gate: the parsers without objdump, the whole gate with it."""

import contextlib
import importlib.machinery
import importlib.util
import io
import os
import shutil
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
_loader = importlib.machinery.SourceFileLoader("isa_gate", os.path.join(os.path.dirname(HERE),
                                                                         "isa-gate"))
_spec = importlib.util.spec_from_loader("isa_gate", _loader)
gate = importlib.util.module_from_spec(_spec)
sys.modules["isa_gate"] = gate  # dataclasses look their module up here
_loader.exec_module(gate)

OBJDUMP = os.environ.get("OBJDUMP") or os.environ.get("CROSS_COMPILE", "mips-linux-gnu-") + "objdump"
HAVE_OBJDUMP = shutil.which(OBJDUMP) is not None

# Instruction words, big-endian MIPS.
ALLOWED_WORDS = {
    "lw": 0x8C880000, "addu": 0x01084821, "lwl": 0x888A0000, "lwr": 0x988A0003,
    "swl": 0xA8AA0000, "swr": 0xB8AA0003, "mult": 0x01090018, "mflo": 0x00005812,
    "jr": 0x03E00008, "nop": 0x00000000,
}
FORBIDDEN_WORDS = {
    "ll": 0xC08C0000, "sc": 0xE08C0000, "clz": 0x710D6820, "mul": 0x71097002,
    "movz": 0x0109780A, "movn": 0x0109780B, "ext": 0x7D1818C0, "seb": 0x7C08CC20,
    "rotr": 0x00284102, "sync": 0x0000000F, "teq": 0x01090034, "beql": 0x51090001,
    "pref": 0xCC800000,
}


def make_elf(words, flags=0, big=True, machine=8):
    """A relocatable ELF32 whose only code section holds the words."""
    end = ">" if big else "<"
    code = b"".join(struct.pack(end + "I", word) for word in words)
    shstr = b"\0.text\0.shstrtab\0"
    text_off = 52
    str_off = text_off + len(code)
    shoff = str_off + len(shstr)
    shoff += -shoff % 4
    ident = b"\x7fELF" + bytes([1, 2 if big else 1, 1, 0]) + bytes(8)
    header = ident + struct.pack(end + "HHIIIIIHHHHHH", 1, machine, 1, 0, 0, shoff, flags,
                                 52, 0, 0, 40, 3, 2)
    null = bytes(40)
    text = struct.pack(end + "IIIIIIIIII", 1, 1, 6, 0, text_off, len(code), 0, 0, 4, 0)
    strtab = struct.pack(end + "IIIIIIIIII", 7, 3, 0, 0, str_off, len(shstr), 0, 0, 1, 0)
    body = header + code + shstr
    body += bytes(shoff - len(body))
    return body + null + text + strtab


def make_archive(members):
    """An ar archive; a member is (name, bytes). Names longer than 15 go through the name table."""
    names = b""
    entries = []
    for name, body in members:
        if len(name) > 15:
            entries.append(("/%d" % len(names), body))
            names += name.encode() + b"/\n"
        else:
            entries.append((name + "/", body))
    out = b"!<arch>\n"
    if names:
        out += b"%-16s%-12s%-6s%-6s%-8s%-10d`\n" % (b"//", b"0", b"0", b"0", b"0", len(names))
        out += names + (b"\n" if len(names) & 1 else b"")
    for name, body in entries:
        out += b"%-16s%-12s%-6s%-6s%-8s%-10d`\n" % (name.encode(), b"0", b"0", b"0", b"100644",
                                                       len(body))
        out += body + (b"\n" if len(body) & 1 else b"")
    return out


class Temp(unittest.TestCase):
    def setUp(self):
        handle = tempfile.TemporaryDirectory()
        self.addCleanup(handle.cleanup)
        self.tmp = handle.name

    def write(self, name, data):
        path = os.path.join(self.tmp, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(data)
        return path

    def run_gate(self, *args):
        out, err = io.StringIO(), io.StringIO()
        status = gate.run(list(args), environ={}, out=out, err=err)
        return status, out.getvalue(), err.getvalue()


class ElfHeader(unittest.TestCase):
    def test_a_mips1_big_endian_object_is_accepted(self):
        self.assertIsNone(gate.elf_flags_finding(make_elf([0])))

    def test_every_later_architecture_is_named(self):
        for arch, name in gate.ARCH_NAMES.items():
            if arch == 0:
                continue
            with self.subTest(arch=name):
                reason = gate.elf_flags_finding(make_elf([0], flags=arch << 28))
                self.assertIn(name, reason)

    def test_other_machines_and_byte_orders_are_findings(self):
        self.assertIn("not MIPS", gate.elf_flags_finding(make_elf([0], machine=40)))
        self.assertIn("big-endian", gate.elf_flags_finding(make_elf([0], big=False)))
        self.assertIn("truncated", gate.elf_flags_finding(b"\x7fELF"))
        wide = bytearray(make_elf([0]))
        wide[4] = 2
        self.assertIn("32-bit", gate.elf_flags_finding(bytes(wide)))


class Archive(unittest.TestCase):
    def test_members_come_out_with_their_names_and_the_symbol_table_is_skipped(self):
        data = make_archive([("a.o", b"AAA"), ("a-very-long-member-name.o", b"BBBB")])
        symtab = (b"%-16s%-12s%-6s%-6s%-8s%-10d`\n" % (b"/", b"0", b"0", b"0", b"0", 4)) + b"\0\0\0\0"
        data = data.replace(b"!<arch>\n", b"!<arch>\n" + symtab, 1)
        self.assertEqual(list(gate.archive_members(data)),
                         [("a.o", b"AAA"), ("a-very-long-member-name.o", b"BBBB")])

    def test_a_damaged_archive_is_an_error(self):
        data = make_archive([("a.o", b"AAA")])
        with self.assertRaises(gate.GateError):
            list(gate.archive_members(data[:8] + b"x" * 60))


class Parsing(unittest.TestCase):
    TEXT = """
t.o:     file format elf32-tradbigmips



00000000 <f>:
   0:\tlw\tt0,0(a0)
   4:\taddu\tt1,t0,t0
   8:\tlwc0\tc0_sr,0(a0)
   c:\t.word\t0x710d6820
  10:\tnop
00000014 <g>:
  14:\tjr\tra
  18:\tnop
"""

    def test_instructions_are_counted_and_the_unknown_ones_named_with_their_place(self):
        count, found = gate.scan_lines("t.o", iter(self.TEXT.splitlines(True)), gate.ALLOWED)
        self.assertEqual(count, 7)
        self.assertEqual([str(f) for f in found],
                         ["t.o: f+0x8: lwc0 c0_sr,0(a0)", "t.o: f+0xc: .word 0x710d6820"])

    def test_a_member_of_an_archive_is_named(self):
        text = "a.o:     file format elf32-tradbigmips\n\n00000000 <f>:\n   0:\tsync\n"
        _count, found = gate.scan_lines("lib.a", iter(text.splitlines(True)), gate.ALLOWED)
        self.assertEqual(str(found[0]), "lib.a: a.o: f+0x0: sync")

    def test_allow_extends_the_set(self):
        allowed = gate.ALLOWED | {"lwc0"}
        _count, found = gate.scan_lines("t.o", iter(self.TEXT.splitlines(True)), allowed)
        self.assertEqual(len(found), 1)

    def test_the_allowed_set_is_the_mips1_integer_set(self):
        for mnemonic in ("lw", "sw", "lwl", "swr", "mult", "div", "jalr", "bgezal", "syscall"):
            self.assertIn(mnemonic, gate.ALLOWED)
        for mnemonic in ("ll", "sc", "lwc0", "swc0", "clz", "mul", "movz", "movn", "ext", "ins",
                         "rotr", "seb", "seh", "wsbh", "sync", "pref", "cache", "beql", "teq",
                         "mfc0", "lwc1", "eret", ".word"):
            self.assertNotIn(mnemonic, gate.ALLOWED)


class Usage(Temp):
    def test_nothing_to_scan_is_an_error_not_a_pass(self):
        self.write("script.sh", b"#!/bin/sh\n")
        status, _out, err = self.run_gate("--objdump", sys.executable, self.tmp)
        self.assertEqual(status, 2)
        self.assertIn("proves nothing", err)

    def test_a_missing_objdump_is_an_error(self):
        path = self.write("x.o", make_elf([0]))
        status, _out, err = self.run_gate("--objdump", os.path.join(self.tmp, "nope"), path)
        self.assertEqual(status, 2)
        self.assertIn("not installed", err)

    def test_a_missing_path_and_a_non_object_are_errors(self):
        status, _out, err = self.run_gate("--objdump", sys.executable, os.path.join(self.tmp, "none"))
        self.assertEqual((status, "no such file" in err), (2, True))
        text = self.write("a.txt", b"hello")
        status, _out, err = self.run_gate("--objdump", sys.executable, text)
        self.assertEqual((status, "neither" in err), (2, True))

    def test_the_options_are_checked(self):
        with contextlib.redirect_stderr(io.StringIO()):
            status, _out, _err = self.run_gate("--no-such-option")
        self.assertEqual(status, 2)


@unittest.skipUnless(HAVE_OBJDUMP, "no MIPS objdump (binutils-mips-linux-gnu)")
class EndToEnd(Temp):
    def gate_words(self, words, *args, flags=0):
        path = self.write("t.o", make_elf(words, flags))
        return self.run_gate("--objdump", OBJDUMP, *args, path)

    def test_the_allowed_instructions_pass(self):
        status, out, _err = self.gate_words(list(ALLOWED_WORDS.values()))
        self.assertEqual(status, 0, out)
        self.assertIn("ok -- 1 files, 1 objects, 10 instructions, 0 findings", out)

    def test_every_forbidden_instruction_fails_the_gate(self):
        for name, word in FORBIDDEN_WORDS.items():
            with self.subTest(instruction=name):
                status, out, _err = self.gate_words([0x8C880000, word, 0x03E00008, 0])
                self.assertEqual(status, 1, out)
                self.assertIn("FAIL", out)
                self.assertIn("+0x4: ", out, "the finding names the offset")

    def test_the_gate_can_fail_and_an_allowance_lifts_exactly_one_mnemonic(self):
        words = [FORBIDDEN_WORDS["ll"], FORBIDDEN_WORDS["clz"], 0]
        status, out, _err = self.gate_words(words)
        self.assertEqual(status, 1)
        self.assertIn("2 findings", out)
        status, out, _err = self.gate_words(words, "--allow", "lwc0")
        self.assertEqual((status, "1 findings" in out), (1, True))
        status, out, _err = self.gate_words(words, "--allow", "lwc0", "--allow", ".word")
        self.assertEqual((status, "0 findings" in out), (0, True))

    def test_an_object_that_names_a_later_architecture_fails_by_its_header(self):
        status, out, _err = self.gate_words([0x8C880000, 0], flags=7 << 28)
        self.assertEqual(status, 1)
        self.assertIn("ELF header: ELF flags name mips32r2", out)
        status, out, _err = self.gate_words([0x8C880000, 0], "--no-flags", flags=7 << 28)
        self.assertEqual(status, 0, out)

    def test_a_directory_is_searched_and_archives_only_on_request(self):
        self.write("bin/ok", make_elf([0x8C880000]))
        self.write("lib/bad.a", make_archive([("m.o", make_elf([FORBIDDEN_WORDS["sync"]]))]))
        self.write("etc/passwd", b"root:x:0:0\n")
        os.symlink(os.path.join(self.tmp, "bin/ok"), os.path.join(self.tmp, "bin/link"))
        status, out, _err = self.run_gate("--objdump", OBJDUMP, self.tmp)
        self.assertEqual(status, 0, out)
        self.assertIn("1 files, 1 objects", out)
        status, out, _err = self.run_gate("--objdump", OBJDUMP, "--archives", self.tmp)
        self.assertEqual(status, 1)
        self.assertIn("bad.a: m.o: ", out)
        self.assertIn("2 files, 2 objects", out)

    def test_a_named_archive_is_scanned_member_by_member(self):
        archive = self.write("l.a", make_archive([
            ("good.o", make_elf([0x8C880000])), ("bad.o", make_elf([0, FORBIDDEN_WORDS["mul"]],
                                                                   flags=7 << 28))]))
        status, out, _err = self.run_gate("--objdump", OBJDUMP, archive)
        self.assertEqual(status, 1)
        self.assertIn("member bad.o: ELF flags name mips32r2", out)
        self.assertIn("l.a: bad.o: ", out)

    def test_the_printed_findings_are_limited_but_counted_in_full(self):
        status, out, _err = self.gate_words([FORBIDDEN_WORDS["sync"]] * 5, "--limit", "2")
        self.assertEqual(status, 1)
        self.assertIn("... and 3 more", out)
        self.assertIn("5 findings", out)
        status, out, _err = self.gate_words([FORBIDDEN_WORDS["sync"]] * 5, "-q")
        self.assertEqual(out.count("\n"), 1)

    def test_a_host_binary_in_a_target_tree_is_a_finding(self):
        self.write("bin/host", make_elf([0], big=False, machine=62))
        status, out, _err = self.run_gate("--objdump", OBJDUMP, self.tmp)
        self.assertEqual(status, 1, out)


if __name__ == "__main__":
    unittest.main()
