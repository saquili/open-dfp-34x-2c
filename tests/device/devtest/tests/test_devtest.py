# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The runner, the record and the parsing of the cases, against canned replies."""

from __future__ import annotations

import json
import re
import unittest

from devtest import cases_data, cases_net, cases_pon, cases_system
from devtest.access import Reply, Settings, redact
from devtest.core import FAIL, PASS, SKIP, Case, Context, failed, passed, run_case, select
from devtest.report import LOG_LINES, as_json, cell, counts, log, page

SETTINGS = Settings(bench="bench.example.org", stick="192.0.2.2", pond_token="s3cr3t-token")


class Canned:
    """A remote end that answers commands from a table of regular expressions."""

    def __init__(self, name: str, answers: list[tuple[str, int, str]], trail: list[str]):
        self.name = name
        self.answers = answers
        self.trail = trail
        self.asked: list[str] = []

    def run(self, command, *, timeout=60.0, check=False, stdin=None):
        self.asked.append(command)
        for pattern, code, out in self.answers:
            if re.search(pattern, command):
                break
        else:
            code, out = 127, ""
        self.trail.append(f"{self.name}$ {command}\n{out}")
        if check and code:
            from devtest.access import RemoteError
            raise RemoteError(f"{command!r} exited {code}")
        return Reply([command], code, out, "", 0.01)

    def text(self, command, *, timeout=60.0):
        return self.run(command, timeout=timeout, check=True).out.strip()


def context(stick=(), bench=()) -> Context:
    trail: list[str] = []
    return Context(SETTINGS, Canned("stick", list(stick), trail), Canned("bench", list(bench), trail),
                   trail)


class Redaction(unittest.TestCase):
    def test_macs_serials_names_and_the_token_are_taken_out(self):
        text = ("sfp 00:00:5e:00:53:01 serial ABCD12345678 via bench.example.org to 192.0.2.2 "
                "Bearer s3cr3t-token")
        self.assertEqual(redact(text, SETTINGS),
                         "sfp <mac> serial <serial> via <bench> to <stick> Bearer <token>")

    def test_hex_alone_and_short_words_stay(self):
        self.assertEqual(redact("frame 00012345 state 5 ONU-ID 1"), "frame 00012345 state 5 ONU-ID 1")


class Runner(unittest.TestCase):
    def test_an_exception_is_a_failure_with_its_message(self):
        def broken(ctx):
            raise ValueError("no frame line")
        result = run_case(Case("x.broken", "X", "breaks", broken), context())
        self.assertEqual((result.status, result.evidence), (FAIL, "ValueError: no frame line"))

    def test_a_destructive_case_is_skipped_unless_asked_for(self):
        ran = []
        c = Case("x.reset", "X", "resets", lambda ctx: ran.append(1) or passed("reset"), True)
        self.assertEqual(run_case(c, context()).status, SKIP)
        ctx = context()
        ctx.destructive = True
        self.assertEqual(run_case(c, ctx).status, PASS)
        self.assertEqual(ran, [1])

    def test_the_log_of_a_case_is_its_own_and_redacted(self):
        ctx = context(stick=[("uname -r", 0, "6.18.48 at 00:00:5e:00:53:01\n")])
        ctx.log.append("before\n")
        c = Case("x.uname", "X", "uname", lambda ctx: passed(ctx.stick.text("uname -r")))
        result = run_case(c, ctx)
        self.assertNotIn("before", result.log)
        self.assertIn("<mac>", result.log)
        self.assertEqual(result.evidence, "6.18.48 at <mac>")

    def test_select_takes_cases_and_areas(self):
        self.assertEqual([c.id for c in select(["gpon.activation"])], ["gpon.activation"])
        self.assertTrue(all(c.id.startswith("system.") for c in select(["system"])))
        with self.assertRaises(SystemExit):
            select(["nothing"])


class Record(unittest.TestCase):
    def results(self):
        ctx = context()
        return [run_case(Case("a.one", "A", "one", lambda c: passed("fine * `x`")), ctx),
                run_case(Case("a.two", "A", "two", lambda c: failed("bad | worse")), ctx)]

    def test_a_cell_is_one_line_without_markup(self):
        self.assertEqual(cell("a *b*\n`c` |d| e_f\\"), "a \\*b\\* \\`c\\` \\|d\\| e\\_f\\\\")
        self.assertEqual(cell(""), "-")

    def test_the_page_has_a_row_per_case_and_the_count(self):
        results = self.results()
        text = page("Device tests", {"Date": "today"}, results, "2026 Someone")
        self.assertIn(":Result: 1 passed, 1 failed, 0 skipped, of 2", text)
        self.assertIn("   * - ``a.one``", text)
        self.assertIn("     - **FAIL**", text)
        self.assertIn("     - bad \\| worse", text)
        self.assertTrue(text.startswith(".. SPDX-License-Identifier: GPL-2.0-only\n"))
        self.assertEqual(counts(results), "1 passed, 1 failed, 0 skipped, of 2")

    def test_the_json_leaves_the_log_out(self):
        record = json.loads(as_json({"Date": "today"}, self.results()))
        self.assertEqual([r["status"] for r in record["results"]], [PASS, FAIL])
        self.assertNotIn("log", record["results"][0])

    def test_a_long_log_is_cut(self):
        result = self.results()[0]
        result.log = "\n".join(str(i) for i in range(LOG_LINES + 5))
        self.assertIn("... 5 more lines", log([result]))


GPON = """top   00000000
ds    00000000  los 0 lof 0 lom 0 fec 0
onu   00000005  id 3 state 5
frame 00080000  superframes (8000/s when locked)
"""


class Cases(unittest.TestCase):
    def test_the_kernel_version_is_read_from_the_tree(self):
        self.assertRegex(cases_system.kernel_version(), r"^\d+\.\d+\.\d+$")

    def test_a_clean_log_passes_and_a_warning_fails(self):
        ok = context(stick=[("grep -E", 0, ""), ("wc -l", 0, "312\n")])
        self.assertEqual(cases_system.log(ok).evidence, "0 of 312 lines")
        bad = context(stick=[("grep -E", 0, "[ 1.0] WARNING: at x\n"), ("wc -l", 0, "312\n")])
        self.assertEqual(cases_system.log(bad).status, FAIL)

    def test_the_gpon_fields_are_read(self):
        self.assertEqual(cases_pon.gpon_field(GPON, "onu", "state"), 5)
        self.assertEqual(cases_pon.gpon_field(GPON, "ds", "lof"), 0)
        self.assertEqual(cases_pon.superframes(GPON), 80000)
        with self.assertRaises(ValueError):
            cases_pon.gpon_field(GPON, "onu", "alloc")

    def test_activation_reads_the_state(self):
        ctx = context(stick=[("/proc/mounts", 0, ""), ("cat .*gpon", 0, GPON)])
        outcome = cases_pon.activation(ctx)
        self.assertEqual((outcome.status, outcome.evidence), (PASS, "O5, ONU-ID 3"))

    def test_the_gem_ports_must_be_in_the_bridge_with_the_host_link(self):
        links = ("7: gem256@pon: <BROADCAST,UP> mtu 1500\n"
                 "8: gem4095@pon: <BROADCAST,UP> mtu 1500\n")
        ctx = context(stick=[("type gem", 0, links), ("brif", 0, "gem256 sfp\n")])
        outcome = cases_pon.gem_bridge(ctx)
        self.assertEqual(outcome.status, PASS)
        self.assertEqual(outcome.evidence, "2 GEM interface(s); omcibr0 holds gem256 sfp")
        alone = context(stick=[("type gem", 0, links), ("brif", 0, "sfp\n")])
        self.assertEqual(cases_pon.gem_bridge(alone).status, FAIL)

    def test_optical_power_is_shown_in_dbm(self):
        self.assertEqual(cases_pon.dbm(1000), "0.0 dBm")
        self.assertEqual(cases_pon.dbm(20), "-17.0 dBm")
        self.assertEqual(cases_pon.dbm(0), "dark")

    def test_the_filter_must_drop_in_both_chains(self):
        two = "policy drop;\npolicy drop;\npolicy accept;\n"
        self.assertEqual(cases_net.filter_loaded(context(stick=[("nft list", 0, two)])).status, PASS)
        one = "policy drop;\npolicy accept;\n"
        self.assertEqual(cases_net.filter_loaded(context(stick=[("nft list", 0, one)])).status, FAIL)

    def test_the_ssh_server_must_offer_keys_only(self):
        def outcome(err):
            ctx = context()
            ctx.bench.run = lambda command, **kw: Reply([command], 255, "", err, 0.1)
            return cases_net.ssh_keys_only(ctx)
        self.assertEqual(outcome("root@x: Permission denied (publickey).\n").status, PASS)
        self.assertEqual(outcome("root@x: Permission denied (publickey,password).\n").status, FAIL)

    def test_the_data_path_is_skipped_without_ports(self):
        self.assertEqual(cases_data.downstream(context()).status, SKIP)


if __name__ == "__main__":
    unittest.main()
