# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of compare-findings and of the list it holds the gate's findings to.

The gate's output in these tests is written out by hand in the form the gate
prints it, so they need no toolchain and no objdump.
"""

import importlib.machinery
import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
TOOL = os.path.join(HERE, "compare-findings")
KNOWN_FILE = os.path.join(HERE, "known-findings.txt")
_loader = importlib.machinery.SourceFileLoader("compare_findings", TOOL)
_spec = importlib.util.spec_from_loader("compare_findings", _loader)
cf = importlib.util.module_from_spec(_spec)
sys.modules["compare_findings"] = cf
_loader.exec_module(cf)

ROOT = "/build/toolchain"
LIBC = ROOT + "/mips/sysroot/usr/lib/libc.a"
SO = ROOT + "/mips/sysroot/lib/libuClibc-1.0.59.so"

KNOWN = """\
# a list of two groups
2 | mips/sysroot/usr/lib/libc.a | _test_and_set.os | _test_and_set | lwc0 swc0
3 | mips/sysroot/lib/libuClibc-1.0.59.so | - | __cxa_finalize | lwc0 swc0 .word=0xf
"""

GATE = """\
isa-gate: {libc}: _test_and_set.os: _test_and_set+0x0: lwc0 c0_entrylo,0(a0)
isa-gate: {libc}: _test_and_set.os: _test_and_set+0xc: swc0 $3,0(a0)
isa-gate: {so}: __cxa_finalize+0x8c: lwc0 c0_context,0(v0)
isa-gate: {so}: __cxa_finalize+0x9c: swc0 $3,0(v0)
isa-gate: {so}: __cxa_finalize+0xa8: .word 0xf
isa-gate: FAIL -- 2 files, 100 objects, 5000 instructions, 5 findings
""".format(libc=LIBC, so=SO)


def compare(known, gate, root=ROOT):
    """Run the comparison in this process; return (status, standard output, standard error)."""
    out, err = io.StringIO(), io.StringIO()
    with tempfile.TemporaryDirectory() as tmp:
        known_path = os.path.join(tmp, "known")
        gate_path = os.path.join(tmp, "gate")
        with open(known_path, "w") as handle:
            handle.write(known)
        with open(gate_path, "w") as handle:
            handle.write(gate)
        status = cf.main([known_path, root, gate_path], out=out, err=err)
    return status, out.getvalue(), err.getvalue()


class SameFindings(unittest.TestCase):
    def test_the_findings_of_the_list_pass(self):
        status, out, err = compare(KNOWN, GATE)
        self.assertEqual((status, err), (0, ""))
        self.assertIn("the 5 findings are the 2 groups of the list", out)

    def test_offsets_and_registers_do_not_matter(self):
        gate = GATE.replace("+0x8c", "+0x90").replace("c0_context,0(v0)", "c0_entrylo,0(a1)")
        self.assertEqual(compare(KNOWN, gate)[0], 0)

    def test_the_order_of_the_groups_does_not_matter(self):
        lines = GATE.splitlines()
        gate = "\n".join(lines[2:5] + lines[0:2] + lines[5:]) + "\n"
        self.assertEqual(compare(KNOWN, gate)[0], 0)

    def test_standard_input_is_read_without_a_file(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as handle:
            handle.write(KNOWN)
        self.addCleanup(os.remove, handle.name)
        out, err = io.StringIO(), io.StringIO()
        status = cf.main([handle.name, ROOT], stdin=io.StringIO(GATE), out=out, err=err)
        self.assertEqual(status, 0, err.getvalue())

    def test_text_that_is_not_the_gates_is_ignored(self):
        status, out, err = compare(KNOWN, "make: entering\n" + GATE + "done\n")
        self.assertEqual(status, 0, err)


class ChangedFindings(unittest.TestCase):
    def assertDiffers(self, gate, *words, known=KNOWN):
        status, out, err = compare(known, gate)
        self.assertEqual(status, 1, out + err)
        for word in words:
            self.assertIn(word, out)
        return out

    def test_one_finding_fewer_in_a_group(self):
        gate = GATE.replace("isa-gate: %s: _test_and_set.os: _test_and_set+0xc: swc0 $3,0(a0)\n" % LIBC,
                            "").replace("5 findings", "4 findings")
        self.assertDiffers(gate, "changed:", "_test_and_set", "known 2 (lwc0 swc0), found 1 (lwc0)")

    def test_one_finding_more_in_a_group(self):
        extra = "isa-gate: %s: _test_and_set.os: _test_and_set+0x10: swc0 $3,0(a0)\n" % LIBC
        gate = GATE.replace("isa-gate: FAIL", extra + "isa-gate: FAIL").replace("5 findings", "6 findings")
        self.assertDiffers(gate, "changed:", "found 3 (lwc0 swc0 swc0)")

    def test_a_new_function(self):
        extra = "isa-gate: %s: __speculation_barrier+0x0: .word 0xf\n" % SO
        gate = GATE.replace("isa-gate: FAIL", extra + "isa-gate: FAIL").replace("5 findings", "6 findings")
        self.assertDiffers(gate, "new:", "__speculation_barrier")

    def test_a_finding_in_another_file(self):
        extra = "isa-gate: %s/mips/lib/libnew.so: foo+0x0: sync\n" % ROOT
        gate = GATE.replace("isa-gate: FAIL", extra + "isa-gate: FAIL").replace("5 findings", "6 findings")
        self.assertDiffers(gate, "new:", "mips/lib/libnew.so")

    def test_a_group_that_is_gone(self):
        lines = GATE.splitlines()
        gate = "\n".join(lines[:2] + ["isa-gate: FAIL -- 2 files, 100 objects, 5000 instructions, "
                                     "2 findings"]) + "\n"
        out = self.assertDiffers(gate, "gone:", "__cxa_finalize")
        self.assertNotIn("new:", out)

    def test_no_finding_at_all_is_a_difference_from_a_list_that_expects_some(self):
        gate = "isa-gate: ok -- 2 files, 100 objects, 5000 instructions, 0 findings\n"
        self.assertDiffers(gate, "gone:")

    def test_another_instruction(self):
        gate = GATE.replace("swc0 $3,0(v0)", "sw $3,0(v0)")
        self.assertDiffers(gate, "changed:", "__cxa_finalize")

    def test_another_word_in_the_place_of_a_word(self):
        gate = GATE.replace(".word 0xf", ".word 0x10")
        self.assertDiffers(gate, "changed:", ".word=0x10")

    def test_a_different_member_of_the_archive(self):
        gate = GATE.replace("_test_and_set.os", "_test_and_set_v2.os")
        self.assertDiffers(gate, "new:", "gone:")

    def test_a_path_outside_the_root_is_not_the_known_path(self):
        status, out, err = compare(KNOWN, GATE, root="/elsewhere")
        self.assertEqual(status, 1)
        self.assertIn("new:", out)
        self.assertIn(LIBC, out)


class CannotCompare(unittest.TestCase):
    def assertCannot(self, known, gate, text):
        status, out, err = compare(known, gate)
        self.assertEqual(status, 2, out + err)
        self.assertIn(text, err)
        self.assertEqual(out, "")

    def test_no_summary_line(self):
        self.assertCannot(KNOWN, "\n".join(GATE.splitlines()[:-1]) + "\n", "no summary line")

    def test_findings_cut_off_by_the_limit(self):
        gate = GATE.replace("isa-gate: FAIL", "isa-gate: %s: ... and 7 more\nisa-gate: FAIL" % SO)
        self.assertCannot(KNOWN, gate, "--limit")

    def test_a_summary_that_does_not_count_the_findings_printed(self):
        self.assertCannot(KNOWN, GATE.replace("5 findings", "6 findings"),
                          "the summary says 6 findings and 5 were printed")

    def test_two_summaries(self):
        self.assertCannot(KNOWN, GATE + GATE, "more than one summary")

    def test_a_line_of_a_form_the_tool_does_not_know(self):
        gate = GATE.replace("isa-gate: FAIL", "isa-gate: %s: something odd\nisa-gate: FAIL" % SO)
        self.assertCannot(KNOWN, gate, "a line of a form this tool does not know")

    def test_an_empty_list(self):
        self.assertCannot("# nothing\n", GATE, "list of known findings is empty")

    def test_a_line_of_the_list_with_the_wrong_number_of_fields(self):
        self.assertCannot("2 | a | - | f\n", GATE, "line 1")

    def test_a_count_that_is_not_the_number_of_instructions_listed(self):
        self.assertCannot("3 | a | - | f | lwc0 swc0\n", GATE, "the count is 3 and 2 instructions")

    def test_a_group_listed_twice(self):
        self.assertCannot("1 | a | - | f | sync\n1 | a | - | f | sync\n", GATE, "listed twice")

    def test_a_missing_file(self):
        out, err = io.StringIO(), io.StringIO()
        status = cf.main(["/no/such/list", ROOT, "/no/such/output"], out=out, err=err)
        self.assertEqual(status, 2)
        self.assertIn("/no/such/list", err.getvalue())

    def test_the_wrong_number_of_arguments(self):
        out, err = io.StringIO(), io.StringIO()
        self.assertEqual(cf.main([KNOWN_FILE], out=out, err=err), 2)


class HeaderFindings(unittest.TestCase):
    def test_header_findings_are_grouped_and_compared_like_the_rest(self):
        gate = ("isa-gate: %s/mips/bin/x: ELF header: ELF flags name mips32r2, not mips1\n"
                "isa-gate: %s: member y.o: ELF flags name mips32r2, not mips1\n"
                "isa-gate: FAIL -- 2 files, 2 objects, 10 instructions, 2 findings\n" % (ROOT, LIBC))
        known = ("1 | mips/bin/x | - | ELF header | ELF_flags_name_mips32r2,_not_mips1\n"
                 "1 | mips/sysroot/usr/lib/libc.a | y.o | ELF header | ELF_flags_name_mips32r2,_not_mips1\n")
        self.assertEqual(compare(known, gate)[0], 0)
        self.assertEqual(compare(known, gate.replace("mips32r2", "mips32"))[0], 1)


class PrintKnown(unittest.TestCase):
    def test_what_it_prints_is_a_list_that_the_same_findings_pass(self):
        out, err = io.StringIO(), io.StringIO()
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as handle:
            handle.write(GATE)
        self.addCleanup(os.remove, handle.name)
        self.assertEqual(cf.main(["--print-known", ROOT, handle.name], out=out, err=err), 0)
        printed = out.getvalue()
        self.assertTrue(printed.startswith("# findings | file"))
        self.assertIn("2 | mips/sysroot/usr/lib/libc.a | _test_and_set.os | _test_and_set | lwc0 swc0",
                      printed)
        self.assertEqual(compare(printed, GATE)[0], 0)

    def test_it_refuses_a_run_with_no_finding(self):
        out, err = io.StringIO(), io.StringIO()
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as handle:
            handle.write("isa-gate: ok -- 1 files, 1 objects, 1 instructions, 0 findings\n")
        self.addCleanup(os.remove, handle.name)
        self.assertEqual(cf.main(["--print-known", ROOT, handle.name], out=out, err=err), 2)


class AsAProgram(unittest.TestCase):
    def run_tool(self, known, gate):
        with tempfile.TemporaryDirectory() as tmp:
            for name, text in (("known", known), ("gate", gate)):
                with open(os.path.join(tmp, name), "w") as handle:
                    handle.write(text)
            return subprocess.run([sys.executable, TOOL, os.path.join(tmp, "known"), ROOT,
                                   os.path.join(tmp, "gate")], capture_output=True, text=True)

    def test_exit_statuses(self):
        self.assertEqual(self.run_tool(KNOWN, GATE).returncode, 0)
        self.assertEqual(self.run_tool(KNOWN, GATE.replace("swc0", "sw")).returncode, 1)
        self.assertEqual(self.run_tool(KNOWN, "").returncode, 2)


class TheList(unittest.TestCase):
    """known-findings.txt: the findings in the toolchain's own libraries, as measured."""

    def groups(self):
        with open(KNOWN_FILE, encoding="utf-8") as handle:
            return cf.parse_known(handle.readlines())

    def test_it_parses(self):
        self.assertGreater(len(self.groups()), 0)

    def test_it_holds_the_two_functions_and_no_other(self):
        functions = {key[2] for key in self.groups()}
        self.assertEqual(functions, {"__speculation_barrier", "_test_and_set"})

    def test_each_function_has_the_count_the_documentation_gives_per_file(self):
        counts = {}
        for (path, _member, function), what in self.groups().items():
            counts.setdefault(function, []).append((os.path.basename(path), len(what)))
        self.assertEqual(sorted(counts["__speculation_barrier"]),
                         [("libgcc.a", 10), ("libgcc_s.so.1", 10), ("libgcc_s.so.1", 10)])
        self.assertEqual(sorted(counts["_test_and_set"]),
                         [("libc.a", 2), ("libuClibc-1.0.59.so", 2)])

    def test_the_total_is_thirty_four(self):
        self.assertEqual(sum(len(what) for what in self.groups().values()), 34)

    def test_every_path_is_relative_to_the_toolchain_directory(self):
        for path, _member, _function in self.groups():
            self.assertFalse(path.startswith("/"), path)


if __name__ == "__main__":
    unittest.main()
