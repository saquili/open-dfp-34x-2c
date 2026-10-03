# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The runner, the record and the parsing of the cases, against canned replies."""

from __future__ import annotations

import dataclasses
import json
import re
import unittest

from devtest import cases_data, cases_net, cases_pon, cases_router, cases_system
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

    def test_the_bench_s_addresses_are_taken_out(self):
        text = ("inet 10.11.12.13/30 via 172.16.1.1, doc 192.0.2.1, mask 255.255.255.252, "
                "any 0.0.0.0, kernel 6.18.48, public 8.8.8.8")
        self.assertEqual(redact(text), "inet <address>/30 via <address>, doc 192.0.2.1, "
                                       "mask 255.255.255.252, any 0.0.0.0, kernel 6.18.48, "
                                       "public <address>")
        self.assertEqual(redact("ssh 192.0.2.2 ok", SETTINGS), "ssh <stick> ok")

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

    def test_the_destructive_cases_come_last(self):
        everything = select(None)
        first = next(i for i, c in enumerate(everything) if c.destructive)
        self.assertTrue(all(c.destructive for c in everything[first:]))
        self.assertEqual(select(["system"])[-1].id, "system.watchdog-reset")


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


PON = """pon  GPON  O5 operation  ONU-ID 3  Alloc-ID 3  OMCC 3
   alarms none  GEM ports 2  T-CONTs 1  serial number set (-s shows it)
    PLOAM messages             120
    bandwidth maps             80000
    bandwidth map CRC errors   0
    T-CONT 0  Alloc-ID 1024
"""


class Cases(unittest.TestCase):
    def test_the_kernel_version_is_read_from_the_tree(self):
        self.assertRegex(cases_system.kernel_version(), r"^\d+\.\d+\.\d+$")

    def test_a_clean_log_passes_and_a_warning_fails(self):
        ok = context(stick=[("grep -E", 0, ""), ("wc -l", 0, "312\n")])
        self.assertEqual(cases_system.log(ok).evidence, "0 of 312 lines")
        bad = context(stick=[("grep -E", 0, "[ 1.0] WARNING: at x\n"), ("wc -l", 0, "312\n")])
        self.assertEqual(cases_system.log(bad).status, FAIL)

    def test_the_report_of_the_pon_tool_is_read(self):
        self.assertEqual(cases_pon.onu_state(PON), 5)
        self.assertEqual(cases_pon.field(PON, r"bandwidth maps\s+(\d+)"), 80000)
        self.assertEqual(cases_pon.alarms(PON), "none")
        self.assertEqual(cases_pon.alarms(PON.replace("alarms none", "alarms LOS LOF")),
                         "LOS LOF")
        with self.assertRaises(ValueError):
            cases_pon.onu_state("pon  GPON")

    def test_the_downstream_lock_needs_maps_without_errors(self):
        def outcome(second):
            ctx = context(stick=[("^pon$", 0, PON)])
            reads = iter([PON, second])
            real = cases_pon.pon, cases_pon.time.sleep
            cases_pon.pon, cases_pon.time.sleep = (lambda _ctx: next(reads)), (lambda _s: None)
            try:
                return cases_pon.downstream(ctx)
            finally:
                cases_pon.pon, cases_pon.time.sleep = real
        more = PON.replace("bandwidth maps             80000", "bandwidth maps             88000")
        self.assertEqual(outcome(more).status, PASS)
        self.assertEqual(outcome(PON).status, FAIL)
        errors = more.replace("bandwidth map CRC errors   0", "bandwidth map CRC errors   2")
        self.assertEqual(outcome(errors).status, FAIL)

    def test_activation_reads_the_state(self):
        ctx = context(stick=[("^pon$", 0, PON)])
        outcome = cases_pon.activation(ctx)
        self.assertEqual((outcome.status, outcome.evidence), (PASS, "O5, ONU-ID 3"))
        ctx = context(stick=[("^pon$", 0, PON.replace("O5 operation", "O3 serial number"))])
        self.assertEqual(cases_pon.activation(ctx).status, FAIL)

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

    def test_the_root_is_read_only_from_flash_or_the_initramfs_of_a_ram_boot(self):
        memory = "tmpfs /tmp tmpfs rw 0 0\ntmpfs /run tmpfs rw 0 0\ntmpfs /var tmpfs rw 0 0\n"
        flash = context(stick=[("mounts", 0, "/dev/root / squashfs ro 0 0\n" + memory)])
        self.assertEqual(cases_system.filesystems(flash).status, PASS)
        ram = context(stick=[("mounts", 0, "rootfs / rootfs rw 0 0\n" + memory)])
        outcome = cases_system.filesystems(ram)
        self.assertEqual((outcome.status, outcome.evidence),
                         (PASS, "/ the initramfs of a RAM boot; /tmp tmpfs, /run tmpfs, /var tmpfs"))
        written = context(stick=[("mounts", 0, "/dev/mtdblock5 / jffs2 rw 0 0\n" + memory)])
        self.assertEqual(cases_system.filesystems(written).status, FAIL)
        no_tmp = context(stick=[("mounts", 0, "/dev/root / squashfs ro 0 0\n")])
        self.assertEqual(cases_system.filesystems(no_tmp).status, FAIL)

    def test_the_daemon_reports_the_kernel_as_its_version_line(self):
        def values(kernel):
            body = json.dumps({"kernel": kernel, "onu": {"state": "o5"},
                               "optics": {"vcc_v": 3.3}})
            return cases_net.pond_values(context(bench=[("curl", 0, body + "\n200")]))
        self.assertEqual(values("Linux version 6.18.48 (builder)").status, PASS)
        self.assertEqual(values({"release": "6.18.48"}).status, FAIL)
        self.assertEqual(values(None).status, FAIL)

    def test_a_reset_is_the_stick_gone_and_back_with_a_new_boot(self):
        class Clock:
            now = 0.0

            def monotonic(self):
                return self.now

            def sleep(self, seconds):
                self.now += seconds

        class Stick:
            def __init__(self, boots):
                self.boots = iter(boots)

            def text(self, command, *, timeout=60.0):
                return "44" if "timeout" in command else "first"

            def run(self, command, *, timeout=60.0, check=False, stdin=None):
                if "boot_id" not in command:
                    return Reply([command], 0, "", "", 0.0)
                boot = next(self.boots)
                return Reply([command], 0 if boot else 255, boot or "", "", 0.0)

        def outcome(boots):
            real = cases_system.time
            cases_system.time = Clock()
            try:
                ctx = context()
                ctx.stick = Stick(boots)
                return cases_system.watchdog_reset(ctx)
            finally:
                cases_system.time = real

        back = ["first"] * 8 + [None] * 100 + ["second"]
        self.assertEqual(outcome(back).status, PASS)
        self.assertEqual(outcome(["first"] * 300).status, FAIL)
        self.assertEqual(outcome(["first"] * 8 + [None] * 300).status, FAIL)
        late = ["first"] * 20 + [None] * 10 + ["second"]
        self.assertEqual(outcome(late).status, FAIL)

    def test_the_filter_must_drop_in_both_chains(self):
        two = "policy drop;\npolicy drop;\npolicy accept;\n"
        self.assertEqual(cases_net.filter_loaded(context(stick=[("nft list", 0, two)])).status, PASS)
        one = "policy drop;\npolicy accept;\n"
        self.assertEqual(cases_net.filter_loaded(context(stick=[("nft list", 0, one)])).status, FAIL)

    def test_the_ssh_server_must_offer_keys_only(self):
        def outcome(err):
            seen = []
            real = cases_net.local_run
            cases_net.local_run = lambda argv, timeout: seen.append(argv) or (255, err)
            try:
                result = cases_net.ssh_keys_only(context())
            finally:
                cases_net.local_run = real
            self.assertIn("-J", seen[0])
            self.assertIn("PubkeyAuthentication=no", seen[0])
            return result
        self.assertEqual(outcome("root@x: Permission denied (publickey).\n").status, PASS)
        self.assertEqual(outcome("root@x: Permission denied (publickey,password).\n").status, FAIL)

    def test_router_mode_runs_only_when_asked_for(self):
        for fn in (cases_router.static, cases_router.pppoe, cases_router.bridge_again):
            self.assertEqual(fn(context()).status, SKIP)

    def test_the_pppoe_secret_goes_from_the_bench_s_file_to_the_daemon(self):
        trail: list[str] = []
        settings = dataclasses.replace(SETTINGS, router=True, olt_port="eth1",
                                       subscriber_port="eth2", pppoe_secrets="/etc/ppp/secrets")
        bench = Canned("bench", [("is-active", 0, ""), ("curl", 0, '{"ok":true}\n200')], trail)
        stick = Canned("stick", [("onu-mode status", 0, "mode router\nwan gem256 on gem256, "
                                                        "out by ppp0\n"),
                                 ("ppp0", 0, "192.0.2.77/32\n"), ("onu-dns", 0, "192.0.2.53\n")],
                       trail)
        outcome = cases_router.pppoe(Context(settings, stick, bench, trail))
        self.assertEqual(outcome.status, PASS)
        post = next(c for c in bench.asked if "curl" in c)
        self.assertTrue(post.startswith("sudo -n sh -c "))
        self.assertIn("/etc/ppp/secrets", post)
        self.assertIn("MODE=router", post)
        self.assertEqual(outcome.evidence,
                         "session up in 0 s; ppp0 192.0.2.77/32; name servers 192.0.2.53; "
                         "MSS not checked without the LAN host of router.static")

    def test_a_pppoe_session_must_clamp_the_lan_host_s_mss(self):
        def outcome(seen):
            trail: list[str] = []
            settings = dataclasses.replace(SETTINGS, router=True, olt_port="eth1",
                                           subscriber_port="eth2",
                                           pppoe_secrets="/etc/ppp/secrets")
            bench = Canned("bench", [("is-active", 0, ""), ("curl", 0, '{"ok":true}\n200'),
                                     ("ip -4 -o addr", 0, "pppoe0\n"),
                                     ("python3 - pppoe0", 0, seen + "\n")], trail)
            stick = Canned("stick", [("onu-mode status", 0, "mode router\nwan gem256 on "
                                                            "gem256, out by ppp0\n"),
                                     ("ppp0", 0, "192.0.2.77\n"),
                                     ("onu-dns", 0, "192.0.2.53\n")], trail)
            ctx = Context(settings, stick, bench, trail)
            ctx.memo["namespaces"] = True
            return cases_router.pppoe(ctx)
        self.assertEqual(outcome("[1452, 1452]").status, PASS)
        self.assertIn("SYN MSS 1452, 1452 through it", outcome("[1452, 1452]").evidence)
        self.assertEqual(outcome("[1460]").status, FAIL)
        self.assertEqual(outcome("[]").status, FAIL)

    def test_the_data_path_is_skipped_without_ports(self):
        self.assertEqual(cases_data.downstream(context()).status, SKIP)

    def test_a_burst_that_never_left_the_bench_is_the_bench_s_fault(self):
        trail: list[str] = []
        settings = dataclasses.replace(SETTINGS, olt_port="eth1", subscriber_port="eth2")
        stick = Canned("stick", [("dsa/tagging", 0, "cpu\n"), ("statistics", 0, "100\n"),
                                 ("ethtool", 1, "")], trail)
        bench = Canned("bench", [("command -v", 0, "/usr/bin/python3\n"),
                                 ("python3 -", 0, "0.500000\n"),
                                 ("address", 0, "00:00:5e:00:53:01\n"),
                                 ("statistics", 0, "100\n")], trail)
        cases_data.time.sleep, real = (lambda _s: None), cases_data.time.sleep
        try:
            outcome = cases_data.downstream(Context(settings, stick, bench, trail))
        finally:
            cases_data.time.sleep = real
        self.assertEqual(outcome.status, FAIL)
        self.assertEqual(outcome.evidence, "the bench: eth1 sent 0 of 400000 frames")
        self.assertEqual(sum("python3 -" in c for c in bench.asked), 3)


if __name__ == "__main__":
    unittest.main()
