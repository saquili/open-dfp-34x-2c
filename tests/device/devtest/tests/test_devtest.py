# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The runner, the record and the parsing of the cases, against canned replies."""

from __future__ import annotations

import contextlib
import dataclasses
import io
import json
import re
import shlex
import subprocess
import unittest
import unittest.mock

from devtest import cases_data, cases_net, cases_pon, cases_system
from devtest.__main__ import main
from devtest.access import Reply, Settings, bench_remote, redact, stick_remote
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


class Sequenced(Canned):
    """A Canned whose answer may be a list: each match takes the next item."""

    def run(self, command, *, timeout=60.0, check=False, stdin=None):
        for i, (pattern, code, out) in enumerate(self.answers):
            if re.search(pattern, command) and isinstance(out, list):
                self.answers[i] = (pattern, code, out[1:] or out)
                saved, self.answers = self.answers, [(pattern, code, out[0])]
                try:
                    return super().run(command, timeout=timeout, check=check, stdin=stdin)
                finally:
                    self.answers = saved
            if re.search(pattern, command):
                break
        return super().run(command, timeout=timeout, check=check, stdin=stdin)


def context(stick=(), bench=()) -> Context:
    trail: list[str] = []
    return Context(SETTINGS, Canned("stick", list(stick), trail), Canned("bench", list(bench), trail),
                   trail)


class Access(unittest.TestCase):
    """How the stick and the bench host are reached, with the bench's namespace and without."""

    LAB = dataclasses.replace(SETTINGS, bench_netns="lab")

    def bench_run(self, settings, command):
        log: list[str] = []
        done = subprocess.CompletedProcess([], 0, "ok\n", "")
        with unittest.mock.patch("devtest.access.subprocess.run", return_value=done) as run:
            bench_remote(settings, log).run(command)
        return run.call_args[0][0][-1], log

    def test_without_a_namespace_the_stick_is_a_jump_away(self):
        argv = stick_remote(SETTINGS, []).ssh
        self.assertEqual(argv[-3:], ["-J", "bench.example.org", "root@192.0.2.2"])

    def test_with_a_namespace_the_bench_connects_to_the_stick_from_inside_it(self):
        argv = stick_remote(self.LAB, []).ssh
        self.assertNotIn("-J", argv)
        proxy = [a for a in argv if a.startswith("ProxyCommand=")]
        self.assertEqual(len(proxy), 1)
        self.assertTrue(proxy[0].endswith(" bench.example.org sudo -n ip netns exec lab nc %h %p"),
                        proxy[0])
        self.assertIn("BatchMode=yes", proxy[0])
        self.assertEqual(argv[-1], "root@192.0.2.2")

    def test_with_a_namespace_every_bench_command_runs_inside_it_and_is_logged_as_given(self):
        command = "curl -s -k 'https://192.0.2.2/'"
        sent, log = self.bench_run(self.LAB, command)
        self.assertEqual(sent, "sudo -n ip netns exec lab sh -c " + shlex.quote(command))
        self.assertTrue(log[0].startswith(f"bench$ {command}\n"), log[0])

    def test_without_one_a_bench_command_goes_as_it_is(self):
        sent, _ = self.bench_run(SETTINGS, "uptime")
        self.assertEqual(sent, "uptime")

    def test_a_namespace_must_be_a_plain_name(self):
        with contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit):
            main(["--bench", "bench.example.org", "--stick", "192.0.2.2",
                  "--bench-netns", "lab; reboot"])
        self.assertIn("not a namespace's name", err.getvalue())

    def test_the_ssh_login_case_goes_the_same_way_as_the_stick(self):
        seen = []
        trail: list[str] = []
        ctx = Context(self.LAB, Canned("stick", [], trail), Canned("bench", [], trail), trail)
        with unittest.mock.patch.object(cases_net, "local_run",
                                        lambda argv, timeout: seen.append(argv) or (255, "")):
            cases_net.ssh_login(ctx)
        self.assertNotIn("-J", seen[0])
        self.assertTrue(any(a.startswith("ProxyCommand=") and "netns exec lab" in a
                            for a in seen[0]))


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

    def test_a_bench_that_is_a_login_at_an_address_goes_whole(self):
        settings = Settings(bench="op@8.8.8.8", stick="192.0.2.2")
        self.assertEqual(redact("ssh -J op@8.8.8.8 root@192.0.2.2 true; then 8.8.8.8 alone", settings),
                         "ssh -J <bench> root@<stick> true; then <address> alone")

    def test_a_login_that_ssh_added_to_the_bench_goes_with_it(self):
        # The settings name the bench by its address; ssh's own configuration
        # puts a login in front of it.
        settings = Settings(bench="8.8.8.8", stick="192.0.2.2")
        self.assertEqual(redact("ssh -J op.name@8.8.8.8 root@192.0.2.2 true", settings),
                         "ssh -J <bench> root@<stick> true")
        self.assertEqual(redact("ssh -o X=1 -J op@bench.example.org root@192.0.2.2", settings),
                         "ssh -o X=1 -J <bench> root@<stick>")

    def test_a_login_at_an_address_that_was_taken_out_goes_too(self):
        self.assertEqual(redact("scp op.name@8.8.8.8:x ."), "scp <login>@<address>:x .")
        self.assertEqual(redact("root@192.0.2.2 and op@fe80::1"), "root@192.0.2.2 and <login>@<address6>")

    def test_ipv6_addresses_but_the_documentation_ones_are_taken_out(self):
        # A link-local address holds its interface's MAC address.
        text = ("sfp inet6 fe80::200:5eff:fe00:5301/64 scope link, global 2001:4860:4860::8888, "
                "doc 2001:db8::2, lo ::1/128, all nodes ff02::1, any ::, at 18:15:44")
        self.assertEqual(redact(text), "sfp inet6 <address6>/64 scope link, global <address6>, "
                                       "doc 2001:db8::2, lo ::1/128, all nodes ff02::1, any ::, "
                                       "at 18:15:44")

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

    def test_the_kernel_log_is_read_after_the_other_cases(self):
        everything = [c.id for c in select(None) if not c.destructive]
        self.assertEqual(everything[-1], "system.log")
        self.assertEqual([c.id for c in select(["system"])][-3:],
                         ["system.log", "system.restart", "system.watchdog-reset"])


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
        # REUSE-IgnoreStart
        self.assertTrue(text.startswith(".. SPDX-License-Identifier: GPL-2.0-only\n"))
        # REUSE-IgnoreEnd
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

    def test_a_switchdev_object_that_a_driver_failed_is_trouble(self):
        failed_del = ("[  284.1] gem256: Failed to del Host Multicast Database entry "
                      "(object id=3) with error: -2 (-2).")
        self.assertRegex(failed_del, cases_system.TROUBLE)
        self.assertNotRegex("[    0.7] rtl9602c-eth cpu: self-test passed", cases_system.TROUBLE)

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

    def test_the_omci_channel_forgives_what_the_olt_s_end_of_the_channel_took(self):
        def outcome(dropped, discarded):
            return cases_pon.channel(context(stick=[
                ("rx_messages", 0, "563\n"), ("rx_dropped", 0, f"{dropped}\n"),
                ("rx_discarded", 0, f"{discarded}\n"), ("tx_messages", 0, "588\n"),
                ("tx_errors", 0, "0\n")]))
        good = outcome(0, 1)
        self.assertEqual(good.status, PASS)
        self.assertIn("rx_discarded 1", good.evidence)
        self.assertEqual(outcome(1, 0).status, FAIL)

    def test_a_mapped_t_cont_must_hold_its_alloc_id(self):
        def outcome(line):
            return cases_pon.tcont_map(context(stick=[("mountpoint", 0, ""),
                                                       ("gpon", 0, line)]))
        good = outcome("tcont 0=256/256 1=300/- 2=-/- 3=-/400   (bound/mapped)\n")
        self.assertEqual((good.status, good.evidence),
                         (FAIL, "T-CONT 0 holds 256, mapped to 256, T-CONT 3 holds -, mapped to 400"))
        good = outcome("tcont 0=256/256 1=300/- 2=-/-   (bound/mapped)\n")
        self.assertEqual((good.status, good.evidence), (PASS, "T-CONT 0 holds 256, mapped to 256"))
        self.assertEqual(outcome("tcont 0=-/- 1=300/-   (bound/mapped)\n").evidence,
                         "no T-CONT mapped")
        self.assertEqual(outcome("aes   key 0\n").status, FAIL)

    def test_alarms_needs_the_power_level_and_no_alarm(self):
        line2 = "     alarms none  GEM ports up to 128  power level high  serial number set\n"

        def outcome(pon, log):
            return cases_pon.omci_alarms(context(stick=[("^pon$", 0, pon),
                                                        ("omcid.log", 0, log)]))
        good = outcome(PON + line2, "")
        self.assertEqual((good.status, good.evidence), (PASS, "power level high; no alarm"))
        bad = outcome(PON + line2,
                      "[INF] alarm.c:1: alarm: signal degrade declared (threshold 10^-9)\n"
                      "[INF] engine.c:2: alarm: notifying 263/32769, alarms 10 00, sequence 1\n")
        self.assertEqual((bad.status, bad.evidence),
                         (FAIL, "power level high; 2 alarm(s), the last: notifying 263/32769, "
                                "alarms 10 00, sequence 1"))
        self.assertEqual(outcome(PON, "").evidence, "no power level reported; no alarm")
        self.assertEqual(outcome(PON, "").status, FAIL)

    def test_activation_reads_the_state(self):
        ctx = context(stick=[("^pon$", 0, PON)])
        outcome = cases_pon.activation(ctx)
        self.assertEqual((outcome.status, outcome.evidence), (PASS, "O5, ONU-ID 3"))
        ctx = context(stick=[("^pon$", 0, PON.replace("O5 operation", "O3 serial number"))])
        self.assertEqual(cases_pon.activation(ctx).status, FAIL)

    GEMS = ("gem256 port 256 both tcont 0 queue 0 strict\n"
            "gem4095 port 4095 down multicast\n")

    def test_the_gem_ports_must_be_in_the_bridge_with_the_host_link(self):
        bridge = ("^pon gem", 0, self.GEMS), ("onu-apply state", 0, "mode=bridge\n")
        ctx = context(stick=[*bridge, ("brif", 0, "gem256 sfp\n")])
        outcome = cases_pon.gem_ports(ctx)
        self.assertEqual(outcome.status, PASS)
        self.assertEqual(outcome.evidence, "2 GEM interface(s); omcibr0 holds gem256 sfp")
        alone = context(stick=[*bridge, ("brif", 0, "sfp\n")])
        self.assertEqual(cases_pon.gem_ports(alone).status, FAIL)

    def test_with_sessions_each_must_run_on_a_gem_interface_that_exists(self):
        told = ("mode=passthrough\nsession.1.dev=wan410\nsession.1.gem=gem256\n"
                "session.2.gem=gem256\n")
        ctx = context(stick=[("^pon gem", 0, self.GEMS), ("onu-apply state", 0, told)])
        outcome = cases_pon.gem_ports(ctx)
        self.assertEqual(outcome.status, PASS)
        self.assertEqual(outcome.evidence,
                         "2 GEM interface(s); sessions: session 1 on gem256, session 2 on gem256")
        gone = told.replace("session.2.gem=gem256", "session.2.gem=gem300")
        ctx = context(stick=[("^pon gem", 0, self.GEMS), ("onu-apply state", 0, gone)])
        outcome = cases_pon.gem_ports(ctx)
        self.assertEqual(outcome.status, FAIL)
        self.assertIn("no interface for session 2", outcome.evidence)
        none = context(stick=[("^pon gem", 0, self.GEMS), ("onu-apply state", 0, "mode=passthrough\n")])
        self.assertEqual(cases_pon.gem_ports(none).status, FAIL)

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

    def test_a_reload_command_brings_the_stick_back_once_it_is_gone(self):
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

        def outcome(code):
            calls = []
            real = cases_system.time, cases_system.reload_stick
            cases_system.time = Clock()
            cases_system.reload_stick = lambda command: (calls.append(command) or
                                                         (code, "ramboot ok"))
            try:
                ctx = context()
                ctx.settings = dataclasses.replace(SETTINGS, reload="bring it back")
                ctx.stick = Stick(["first"] * 8 + [None] * 3 + ["second"])
                return cases_system.watchdog_reset(ctx), calls, ctx.log
            finally:
                cases_system.time, cases_system.reload_stick = real

        result, calls, trail = outcome(0)
        self.assertEqual(result.status, PASS)
        self.assertEqual(calls, ["bring it back"])
        self.assertIn("brought back by the reload command", result.evidence)
        self.assertIn("local$ the reload command\nramboot ok\n[exit 0]\n", trail)
        result, calls, _trail = outcome(2)
        self.assertEqual(result.status, PASS)
        self.assertIn("the reload command failed (exit 2)", result.evidence)

    def test_the_reload_command_runs_here_and_gives_its_last_line(self):
        self.assertEqual(cases_system.reload_stick("echo one; echo two"), (0, "two"))
        self.assertEqual(cases_system.reload_stick("echo gone >&2; exit 3"), (3, "gone"))
        self.assertEqual(cases_system.reload_stick("true"), (0, ""))
        real = cases_system.RELOAD_TIMEOUT
        cases_system.RELOAD_TIMEOUT = 0.2
        try:
            self.assertEqual(cases_system.reload_stick("exec sleep 5"),
                             (124, "no end within 0.2 s"))
        finally:
            cases_system.RELOAD_TIMEOUT = real

    NETSTAT = ("Active Internet connections (only servers)\n"
               "Proto Recv-Q Send-Q Local Address           Foreign Address         State\n"
               "tcp        0      0 192.0.2.1:22         0.0.0.0:*               LISTEN\n"
               "tcp        0      0 192.0.2.1:443        0.0.0.0:*               LISTEN\n"
               "tcp        0      0 192.0.2.5:22         0.0.0.0:*               LISTEN\n"
               "tcp        0      0 192.0.2.5:443        0.0.0.0:*               LISTEN\n")
    MGMT = "VLAN=4094\nADDRESS=192.0.2.1/30\nDATA_ADDRESS=192.0.2.5/30\n"

    def listening(self, netstat, probes):
        answers = iter(probes)
        real = cases_net.probe
        cases_net.probe = lambda ctx, port, wait=3.0: next(answers)
        try:
            return cases_net.listening(context(stick=[("cat /run/onu-mgmt", 0, self.MGMT),
                                                      ("netstat", 0, netstat)]))
        finally:
            cases_net.probe = real

    def test_the_servers_must_listen_on_the_two_host_side_addresses_only(self):
        good = [("open", 0.01), ("open", 0.01), ("refused", 0.01)]
        result = self.listening(self.NETSTAT, good)
        self.assertEqual(result.status, PASS)
        self.assertIn("22, 443 on 2 host-side addresses; on no other", result.evidence)
        wild = self.NETSTAT + "tcp        0      0 :::80                   :::*                    LISTEN\n"
        result = self.listening(wild, good)
        self.assertEqual(result.status, FAIL)
        self.assertIn("also :::80", result.evidence)
        anywhere = self.NETSTAT + "tcp   0   0 0.0.0.0:23   0.0.0.0:*   LISTEN\n"
        self.assertEqual(self.listening(anywhere, good).status, FAIL)

    def test_the_servers_must_answer_the_bench_and_a_closed_port_must_be_refused(self):
        self.assertEqual(self.listening(self.NETSTAT, [("timeout", 3.0), ("open", 0.01),
                                                       ("refused", 0.01)]).status, FAIL)
        self.assertEqual(self.listening(self.NETSTAT, [("open", 0.01), ("open", 0.01),
                                                       ("timeout", 3.0)]).status, FAIL)

    VLANS = ("VLAN Dev name    | VLAN ID\n"
             "Name-Type: VLAN_NAME_TYPE_RAW_PLUS_VID_NO_PAD\n"
             "mgmt           | 4094  | sfp\n"
             "net1           | 100  | sfp\n"
             "wan410         | 410  | gem256\n")
    ADDRS = ("1: lo    inet 127.0.0.1/8 scope host lo\n"
             "5: mgmt    inet 192.0.2.1/30 brd 192.0.2.3 scope global mgmt\n"
             "7: net1    inet 203.0.113.1/24 brd 203.0.113.255 scope global net1\n")

    def guarded(self, answers, subscriber_port="eth1"):
        """The case net.guard, the probe of each way answering from @answers in
        turn: the management VLAN's first."""
        seen = []
        answered = iter(answers)
        real = cases_net.probe, cases_net.guard_probe
        cases_net.probe = lambda ctx, port, wait=3.0: ("open", 0.01)
        cases_net.guard_probe = (lambda ctx, vid, mac, dst, src, arp=False:
                                 seen.append((vid, src, arp)) or next(answered))
        trail: list[str] = []
        settings = dataclasses.replace(SETTINGS, subscriber_port=subscriber_port)
        stick = Canned("stick", [("cat /run/onu-mgmt", 0, self.MGMT),
                                 ("sfp/address", 0, "00:00:5e:00:53:01"),
                                 ("ip -4 -o addr", 0, self.ADDRS),
                                 ("vlan/config", 0, self.VLANS),
                                 ("rp_filter", 0, "0")], trail)
        bench = Canned("bench", [("route get", 0, "192.0.2.1 dev eth1.4094 src 192.0.2.2 uid 0")],
                       trail)
        try:
            return cases_net.guarded(Context(settings, stick, bench, trail)), seen
        finally:
            cases_net.probe, cases_net.guard_probe = real

    def test_the_guard_keeps_the_servers_to_the_management_vlan(self):
        both, none = {22: True, 443: True}, {22: False, 443: False}
        result, seen = self.guarded([both, none, none])
        self.assertEqual(result.status, PASS)
        # The management VLAN, untagged from the bench's own address, and the
        # network's VLAN from an address of the network, whose ARP the bench answers.
        self.assertEqual(seen, [(4094, "192.0.2.2", False), (0, "192.0.2.2", False),
                                (100, "203.0.113.254", True)])
        self.assertIn("a SYN in VLAN 4094 answered on 22, 443", result.evidence)
        self.assertIn("net1 (VLAN 100, from 203.0.113.254 of 203.0.113.0/24): no answer",
                      result.evidence)
        self.assertIn("sfp (untagged, from the bench's address, rp_filter 0): no answer",
                      result.evidence)

    def test_a_server_reached_by_another_vlan_fails(self):
        both, none = {22: True, 443: True}, {22: False, 443: False}
        result, _ = self.guarded([both, none, {22: False, 443: True}])
        self.assertEqual(result.status, FAIL)
        self.assertIn("answered on 443", result.evidence)
        # Nor does a probe that the management VLAN does not answer prove anything.
        self.assertEqual(self.guarded([none, none, none])[0].status, FAIL)
        self.assertEqual(self.guarded([], subscriber_port="")[0].status, SKIP)

    def test_the_listeners_are_read_from_netstat(self):
        text = self.NETSTAT + "tcp        0      0 :::80                   :::*                    LISTEN\n"
        self.assertEqual(cases_net.listeners(text),
                         {("192.0.2.1", 22), ("192.0.2.1", 443), ("192.0.2.5", 22),
                          ("192.0.2.5", 443), ("::", 80)})
        self.assertEqual(cases_net.listeners("tcp 0 0 192.0.2.1:22 198.51.100.2:5000 ESTABLISHED\n"),
                         set())

    def test_the_ssh_server_must_offer_the_key_and_the_password(self):
        def outcome(err):
            seen = []
            real = cases_net.local_run
            cases_net.local_run = lambda argv, timeout: seen.append(argv) or (255, err)
            try:
                result = cases_net.ssh_login(context())
            finally:
                cases_net.local_run = real
            self.assertIn("-J", seen[0])
            self.assertIn("PubkeyAuthentication=no", seen[0])
            # Its own connection: one shared with the other cases is logged in.
            self.assertIn("ControlMaster=no", seen[0])
            self.assertIn("ControlPath=none", seen[0])
            return result
        self.assertEqual(outcome("root@x: Permission denied (publickey,password).\n").status, PASS)
        self.assertEqual(outcome("root@x: Permission denied (password,publickey).\n").status, PASS)
        self.assertEqual(outcome("root@x: Permission denied (publickey).\n").status, FAIL)
        self.assertEqual(outcome("root@x: Permission denied (publickey,password,keyboard-interactive).\n").status, FAIL)

    def test_the_watchdog_must_be_held_and_fed_by_procd(self):
        def outcome(comm="procd", held="1", state="active", up="120.5"):
            stick = Canned("stick", [("identity", 0, "Realtek watchdog timer\n"),
                                     ("watchdog0/timeout", 0, "30\n"),
                                     ("watchdog0/state", 0, state + "\n"),
                                     ("/proc/1/comm", 0, comm + "\n"),
                                     ("/proc/1/fd", 0, held + "\n"),
                                     ("/proc/uptime", 0, up + "\n")], [])
            ctx = Context(SETTINGS, stick, Canned("bench", [], []), [])
            return cases_system.watchdog(ctx)
        good = outcome()
        self.assertEqual(good.status, PASS)
        self.assertIn("procd (pid 1) holds /dev/watchdog; up 120 s", good.evidence)
        self.assertEqual(outcome(comm="init").status, FAIL)
        self.assertEqual(outcome(held="0").status, FAIL)
        self.assertEqual(outcome(state="inactive").status, FAIL)

    def test_a_young_stick_is_waited_for_before_the_watchdog_is_called_fed(self):
        class Clock:
            slept = 0.0

            def sleep(self, seconds):
                self.slept += seconds

        ups = iter(["12.0", "36.0"])

        class Stick:
            def text(self, command, *, timeout=60.0):
                if "uptime" in command:
                    return next(ups)
                return {"identity": "wdt", "timeout": "30", "state": "active",
                        "comm": "procd"}[next(k for k in ("identity", "timeout", "state", "comm")
                                              if k in command)]

            def run(self, command, *, timeout=60.0, check=False, stdin=None):
                return Reply([command], 0, "1\n", "", 0.0)

        clock, real = Clock(), cases_system.time
        cases_system.time = clock
        try:
            ctx = context()
            ctx.stick = Stick()
            result = cases_system.watchdog(ctx)
        finally:
            cases_system.time = real
        self.assertEqual(result.status, PASS)
        self.assertEqual(clock.slept, 30 + cases_system.WATCHDOG_MARGIN - 12.0)

    def test_the_data_path_is_skipped_without_ports(self):
        self.assertEqual(cases_data.downstream(context()).status, SKIP)

    def test_the_acl_case_wants_the_rule_of_the_lower_priority_to_decide(self):
        def outcome(added_first_decides: bool):
            trail: list[str] = []
            rules: list[tuple[int, str]] = []    # (priority, action), in the order added

            def decider():
                if not rules:
                    return None
                return rules[0] if added_first_decides else min(rules)

            def action():
                return decider()[1] if rules else "pass"

            class Stick(Canned):
                def run(self, command, **kw):
                    rules.extend((int(p), a) for p, a in re.findall(
                        r"dev pon ingress pref (\d+) protocol 0x88b5 flower skip_sw action (\w+)",
                        command))
                    for p in re.findall(r"filter del dev pon ingress pref (\d+)", command):
                        rules[:] = [r for r in rules if r[0] != int(p)]
                    hw = re.search(r"tc -s filter show dev pon ingress pref (\d+)", command)
                    if hw:
                        n = 200 if decider() and decider()[0] == int(hw.group(1)) else 0
                    else:
                        n = 200 if action() == "trap" else 0
                    self.answers[0] = ("tc -s filter show", 0, f"\tSent {n * 60} bytes {n} pkt\n")
                    return super().run(command, **kw)

            class Bench(Canned):
                def run(self, command, **kw):
                    seen = '{"untagged": 200}' if action() == "pass" else "{}"
                    self.answers[0] = ("python3 -", 0, seen + "\n")
                    return super().run(command, **kw)

            stick = Stick("stick", [("tc -s filter show", 0, ""), ("ls -d gem", 0, "gem256\n"),
                                    ("tc ", 0, "")], trail)
            bench = Bench("bench", [("python3 -", 0, ""),
                                    ("command -v", 0, "/usr/bin/python3\n")], trail)
            settings = dataclasses.replace(SETTINGS, olt_port="eth1", subscriber_port="eth2")
            ctx = Context(settings, stick, bench, trail)
            ctx.memo["primed"] = True
            return cases_data.acl(ctx), rules, stick.asked
        good, left, asked = outcome(added_first_decides=False)
        self.assertEqual(good.status, PASS)
        self.assertIn("trap over drop, added last: 0 arrived, 200 to the CPU, 0 counted at "
                      "49020, 200 counted at 49010", good.evidence)
        self.assertEqual(left, [])
        self.assertIn("tc qdisc del dev pon clsact; tc qdisc del dev gem256 clsact; true", asked)
        self.assertEqual(outcome(added_first_decides=True)[0].status, FAIL)

    def test_the_push_pop_case_wants_each_tag_operation_done(self):
        def outcome(pops: bool = True, s_pops: bool = True):
            trail: list[str] = []
            rules: set[str] = set()

            class Stick(Canned):
                def run(self, command, **kw):
                    for dev, text in re.findall(r"tc filter add dev (\w+) ingress pref \d+ (.*)",
                                                command):
                        rules.add(f"{dev}:{text}")
                    if "tc filter del" in command:
                        rules.clear()
                    return super().run(command, **kw)

            class Bench(Canned):
                def run(self, command, **kw):
                    if "python3 -" not in command:
                        return super().run(command, **kw)
                    spec = re.search(r"python3 - \S+ \S+ (\S+) \d+", command).group(1)
                    up = any("push id 3003" in r for r in rules)
                    down = pops and any(r.startswith("pon:protocol 802.1Q") and "pop" in r
                                        for r in rules)
                    qinq = any(r.startswith("sfp:") and "802.1ad" in r for r in rules)
                    unqinq = s_pops and any(r.startswith("pon:protocol 802.1ad") and "pop" in r
                                            for r in rules)
                    got = {"u": "c3003p5" if up else "u",
                           "c3003p5": "u" if down else "c3003p5",
                           "c3001p1": "s3004p3+c3001p1" if qinq else "c3001p1",
                           "s3004p3+c3001p1": "c3001p1" if unqinq else "s3004p3+c3001p1"}[spec]
                    self.answers[0] = ("python3 -", 0, json.dumps({got: 200}) + "\n")
                    return super().run(command, **kw)

            stick = Stick("stick", [("", 0, "")], trail)
            bench = Bench("bench", [("python3 -", 0, ""),
                                    ("command -v", 0, "/usr/bin/python3\n")], trail)
            settings = dataclasses.replace(SETTINGS, olt_port="eth1", subscriber_port="eth2")
            ctx = Context(settings, stick, bench, trail)
            ctx.memo["primed"] = True
            return cases_data.vlan_push_pop(ctx)
        good = outcome()
        self.assertEqual(good.status, PASS, good.evidence)
        self.assertIn("200 of c3001p1 upstream: 200 as s3004p3+c3001p1", good.evidence)
        self.assertIn("200 of s3004p3+c3001p1 downstream: 200 as c3001p1", good.evidence)
        bad = outcome(pops=False)
        self.assertEqual(bad.status, FAIL)
        self.assertIn("200 of c3003p5 downstream: 200 as c3003p5", bad.evidence)
        stuck = outcome(s_pops=False)
        self.assertEqual(stuck.status, FAIL)
        self.assertIn("200 of s3004p3+c3001p1 downstream: 200 as s3004p3+c3001p1",
                      stuck.evidence)

    def test_the_mgmt_case_wants_its_vlan_kept_from_the_pon_and_another_through(self):
        def outcome(isolated: bool = True, carried: bool = True):
            trail: list[str] = []

            class Bench(Canned):
                def run(self, command, **kw):
                    if "python3 -" not in command:
                        return super().run(command, **kw)
                    spec = re.search(r"python3 - \S+ \S+ (\S+) \d+", command).group(1)
                    if spec == "c4094":
                        got = {} if isolated else {"c4094p0": 200}
                    else:
                        got = {"c3005p0": 200} if carried else {}
                    self.answers[0] = ("python3 -", 0, json.dumps(got) + "\n")
                    return super().run(command, **kw)

            config = ("VLAN Dev name | VLAN ID\nName-Type: VLAN_NAME_TYPE_RAW_PLUS_VID_NO_PAD\n"
                      "mgmt           | 4094  | sfp\n")
            stick = Canned("stick", [("vlan/config", 0, config)], trail)
            bench = Bench("bench", [("python3 -", 0, ""),
                                    ("command -v", 0, "/usr/bin/python3\n")], trail)
            settings = dataclasses.replace(SETTINGS, olt_port="eth1", subscriber_port="eth2")
            ctx = Context(settings, stick, bench, trail)
            ctx.memo["primed"] = True
            return cases_data.mgmt_isolation(ctx)
        good = outcome()
        self.assertEqual(good.status, PASS, good.evidence)
        self.assertIn("200 of VLAN 4094 upstream: nothing, downstream: nothing", good.evidence)
        leaking = outcome(isolated=False)
        self.assertEqual(leaking.status, FAIL)
        self.assertIn("upstream: 200 as c4094p0", leaking.evidence)
        blocked = outcome(carried=False)
        self.assertEqual(blocked.status, FAIL)
        self.assertIn("200 of VLAN 3005 upstream: nothing", blocked.evidence)

    def test_the_silence_case_wants_nothing_answered_from_the_pon_and_the_host_answered(self):
        def outcome(ipv6: str = "1", answered: int = 0, reached: int = 5, host: int = 10):
            trail: list[str] = []

            class Bench(Canned):
                def run(self, command, **kw):
                    if "python3 -" not in command:
                        return super().run(command, **kw)
                    port = re.search(r"python3 - (\S+) ", command).group(1)
                    got = ({"arp": answered, "echo6": 0, "ns": 0} if port == "eth1" else
                           {"arp": host, "echo6": host, "ns": 0})
                    self.answers[0] = ("python3 -", 0, json.dumps(got) + "\n")
                    return super().run(command, **kw)

            config = ("VLAN Dev name | VLAN ID\nName-Type: VLAN_NAME_TYPE_RAW_PLUS_VID_NO_PAD\n"
                      "mgmt           | 4094  | sfp\n")
            shown = f"filter protocol ipv6 pref 49040\n Sent 900 bytes {reached} pkt\n"
            stick = Canned("stick", [
                ("vlan/config", 0, config),
                ("addr show dev mgmt", 0, "5: mgmt    inet 198.51.100.2/30 scope global mgmt\n"),
                ("mgmt/address", 0, "00:00:5e:00:53:02\n"),
                ("ls -d gem", 0, "gem256\ngem4095\n"),
                (r"^\[ -e ", 0, ""),
                ("disable_ipv6", 0, ipv6 + "\n"),
                ("ip -6 -o addr", 0, "2: sfp    inet6 fe80::1/64 scope link\n"),
                ("arp_ignore", 0, "1\n"),
                ("tc -s filter show", 0, shown * 4),
                ("^tc ", 0, ""),
            ], trail)
            bench = Bench("bench", [("python3 -", 0, ""),
                                    ("command -v", 0, "/usr/bin/python3\n")], trail)
            settings = dataclasses.replace(SETTINGS, olt_port="eth1", subscriber_port="eth2")
            return cases_data.pon_silence(Context(settings, stick, bench, trail))
        good = outcome()
        self.assertEqual(good.status, PASS, good.evidence)
        self.assertIn("20 reached the CPU, answered 0 ARP", good.evidence)
        self.assertIn("in VLAN 4094: 10 ARP, 10 echo", good.evidence)
        self.assertEqual(outcome(ipv6="0").status, FAIL)
        self.assertEqual(outcome(answered=1).status, FAIL)
        # A zero answer is worth nothing if the questions never reached the CPU.
        self.assertEqual(outcome(reached=0).status, FAIL)
        self.assertEqual(outcome(host=0).status, FAIL)

    def test_the_multicast_case_wants_every_report_through_and_unasked_groups_kept_out(self):
        def outcome(reports_to_a_group_pass: bool):
            trail: list[str] = []
            groups: dict[str, float] = {}
            clock = [0.0]

            class Bench(Canned):
                def run(self, command, **kw):
                    if "python3 -" not in command:
                        return super().run(command, **kw)
                    into, _, kind, group, count = re.search(
                        r"python3 - (\S+) (\S+) (\S+) (\S+) (\d+)", command).groups()
                    count = int(count)
                    up = into == "eth2"
                    if kind in ("join", "mldjoin2", "mldjoin1"):
                        got = count if reports_to_a_group_pass or group not in groups else 1
                        groups[group] = 260.0 + clock[0] if got == count else groups.get(group, 0.0)
                    elif kind == "leave":
                        got = count
                        groups.pop(group, None)
                    elif kind == "data" and not up:
                        got = count if group in groups or group.startswith("224.0.0.") else 0
                    else:
                        got = count
                    self.answers[0] = ("python3 -", 0, json.dumps({"sent": count, "got": got}) + "\n")
                    return super().run(command, **kw)

            class Stick(Canned):
                def run(self, command, **kw):
                    rows = "".join(f"dev omcibr0 port sfp grp {g} temp proto kernel offload "
                                   f"{t - clock[0]:.2f}\n" for g, t in groups.items())
                    self.answers[0] = ("bridge -s mdb show", 0, rows)
                    return super().run(command, **kw)

            stick = Stick("stick", [("bridge -s mdb show", 0, ""),
                                    ("multicast_snooping", 0, "1\n"), ("", 0, "")], trail)
            bench = Bench("bench", [("python3 -", 0, ""),
                                    ("command -v", 0, "/usr/bin/python3\n")], trail)
            settings = dataclasses.replace(SETTINGS, olt_port="eth1", subscriber_port="eth2")
            ctx = Context(settings, stick, bench, trail)
            ctx.memo["primed"] = True

            def sleep(seconds):
                clock[0] += seconds
            cases_data.time.sleep, real = sleep, cases_data.time.sleep
            try:
                return cases_data.igmp(ctx), stick.asked
            finally:
                cases_data.time.sleep = real
        good, asked = outcome(reports_to_a_group_pass=True)
        self.assertEqual(good.status, PASS, good.evidence)
        self.assertIn("join 3/3 to the network, group offloaded", good.evidence)
        self.assertIn("timer restarted", good.evidence)
        self.assertTrue(asked[-1].endswith("echo 1 > /sys/class/net/omcibr0/bridge/multicast_snooping"))
        bad = outcome(reports_to_a_group_pass=False)[0]
        self.assertEqual(bad.status, FAIL)
        self.assertIn("1/3 reports again, timer not restarted", bad.evidence)

    def test_the_mirror_case_wants_copies_both_ways_and_none_after(self):
        def outcome(copies: bool):
            trail: list[str] = []
            rules: set[str] = set()

            class Stick(Canned):
                def run(self, command, **kw):
                    for way in re.findall(r"filter add dev pon (\w+) pref 49040 matchall", command):
                        rules.add(way)
                    for way in re.findall(r"filter del dev pon (\w+) pref 49040", command):
                        rules.discard(way)
                    return super().run(command, **kw)

            class Bench(Canned):
                def run(self, command, **kw):
                    way = "ingress" if " eth1 eth2 " in command else "egress"
                    n = cases_data.MIRROR_COUNT if copies and way in rules else 0
                    self.answers[0] = ("python3 -", 0, f"{n}\n")
                    return super().run(command, **kw)

            stick = Stick("stick", [("sfp/address", 0, "00:00:5e:00:53:02\n"), ("tc ", 0, "")],
                          trail)
            bench = Bench("bench", [("python3 -", 0, ""), ("address", 0, "00:00:5e:00:53:01\n"),
                                    ("command -v", 0, "/usr/bin/python3\n")], trail)
            settings = dataclasses.replace(SETTINGS, olt_port="eth1", subscriber_port="eth2")
            ctx = Context(settings, stick, bench, trail)
            ctx.memo["primed"] = True
            return cases_data.mirror(ctx), rules, stick.asked
        good, left, asked = outcome(copies=True)
        self.assertEqual(good.status, PASS)
        self.assertIn("2000 of those from the network side", good.evidence)
        self.assertIn("after, 0 and 0", good.evidence)
        self.assertEqual(left, set())
        self.assertIn("tc qdisc del dev pon clsact; true", asked[-1])
        self.assertEqual(outcome(copies=False)[0].status, FAIL)

    def test_the_phc_case_counts_one_pulse_a_second(self):
        def outcome(last: str):
            trail: list[str] = []
            class Stick(Canned):
                def run(self, command, **kw):
                    if "/assert" in command:
                        self.answers[0] = ("/assert", 0, f"100.000000000#1\n{last}\n")
                    return super().run(command, **kw)

            stick = Stick("stick", [("/assert", 0, ""),
                                    ("ethtool -T", 0, "Hardware timestamp provider index: 0\n"
                                                      "\thardware-receive\n"
                                                      "\thardware-raw-clock\n"),
                                    ("clock_name", 0, "rtl9602c-ptp\n"),
                                    ("pps\\*", 0, "pps0\n"), ("pps_enable", 0, "")], trail)
            return cases_system.phc(Context(SETTINGS, stick, Canned("bench", [], trail), trail))
        good = outcome("104.000000100#5")
        self.assertEqual(good.status, PASS)
        self.assertIn("4 pulses 4.000 s apart", good.evidence)
        self.assertEqual(outcome("104.000000100#1").status, FAIL)

    def test_the_fdb_case_wants_learned_addresses_and_the_static_one_in_and_out(self):
        def outcome(learns: bool, takes_static: bool):
            trail: list[str] = []
            table = {"on": False}

            class Stick(Canned):
                def run(self, command, **kw):
                    if "fdb add" in command:
                        table["on"] = takes_static
                    if "fdb del" in command:
                        table["on"] = False
                    lines = ["00:00:5e:00:53:02 vlan 410 self"] if learns else []
                    if table["on"]:
                        lines.append(cases_data.FDB_STATIC + " self static")
                    self.answers[0] = ("fdb show", 0, "\n".join(lines) + "\n")
                    return super().run(command, **kw)

            stick = Stick("stick", [("fdb show", 0, ""), ("bridge fdb", 0, "")], trail)
            bench = Canned("bench", [("address", 0, "00:00:5e:00:53:02\n"),
                                     ("command -v", 0, "/usr/bin/python3\n")], trail)
            settings = dataclasses.replace(SETTINGS, olt_port="eth1", subscriber_port="eth2")
            ctx = Context(settings, stick, bench, trail)
            ctx.memo["primed"] = True
            cases_data.time.sleep, real = (lambda _s: None), cases_data.time.sleep
            try:
                return cases_data.fdb(ctx)
            finally:
                cases_data.time.sleep = real
        good = outcome(learns=True, takes_static=True)
        self.assertEqual(good.status, PASS)
        self.assertIn("then gone", good.evidence)
        self.assertEqual(outcome(learns=False, takes_static=True).status, FAIL)
        self.assertEqual(outcome(learns=True, takes_static=False).status, FAIL)

    def test_the_shaper_case_wants_the_rate_held_then_given_back(self):
        def outcome(held: float):
            trail: list[str] = []
            shaped = {"on": False}

            class Stick(Canned):
                def run(self, command, **kw):
                    if "tbf rate" in command:
                        shaped["on"] = True
                    if "qdisc del" in command:
                        shaped["on"] = False
                    return super().run(command, **kw)

            def measure(ctx, into, out_of, stick_in, stick_out, count=cases_data.COUNT):
                rate = held if shaped["on"] else 984.0
                return 400000, int(400000 * rate / 984), 60, rate, ""

            stick = Stick("stick", [("tc ", 0, "")], trail)
            bench = Canned("bench", [("command -v", 0, "/usr/bin/python3\n")], trail)
            settings = dataclasses.replace(SETTINGS, olt_port="eth1", subscriber_port="eth2")
            cases_data.measure, real = measure, cases_data.measure
            try:
                return cases_data.shaper(Context(settings, stick, bench, trail)), shaped["on"]
            finally:
                cases_data.measure = real
        good, left = outcome(held=203.0)
        self.assertEqual(good.status, PASS)
        self.assertFalse(left)
        self.assertEqual(outcome(held=984.0)[0].status, FAIL)

    def test_the_host_link_search_wants_both_speeds_and_gives_the_port_back(self):
        class Clock:
            now = 0.0

            def monotonic(self):
                return self.now

            def sleep(self, seconds):
                self.now += seconds

        def outcome(finds: bool):
            trail: list[str] = []
            port = {"speed": "2500", "an": "off", "managed": True, "kept": None}

            class Bench(Canned):
                def run(self, command, **kw):
                    if "systemd-run" in command:
                        return super().run(command, **kw)
                    for part in command.split(";"):
                        set_to = re.search(r"ethtool -s eth2 speed (\d+) duplex full autoneg (on|off)",
                                           part)
                        if set_to:
                            port["speed"], port["an"] = set_to.groups()
                        if "managed no" in part:
                            port["managed"] = False
                        if "managed yes" in part:
                            port["managed"], port["kept"] = True, (port["speed"], port["an"])
                    if command.startswith("nmcli -t"):
                        return Reply([command], 0, "eth2:connected\n", "", 0.01)
                    if re.search(r"ethtool eth2\b", command):
                        out = (f"\tSpeed: {port['speed']}Mb/s\n\tAuto-negotiation: {port['an']}\n"
                               "\tLink detected: yes\n")
                        return Reply([command], 0, out, "", 0.01)
                    return super().run(command, **kw)

            class Stick(Canned):
                def run(self, command, **kw):
                    if "sfp/speed" in command:
                        speed = port["speed"] if finds else "2500"
                        return Reply([command], 0, speed + "\n", "", 0.01)
                    return super().run(command, **kw)

            bench = Bench("bench", [], trail)
            stick = Stick("stick", [], trail)
            settings = dataclasses.replace(SETTINGS, olt_port="eth1", subscriber_port="eth2")
            cases_net.time, real = Clock(), cases_net.time
            try:
                return cases_net.host_link_search(Context(settings, stick, bench, trail)), port, \
                    bench.asked
            finally:
                cases_net.time = real
        good, port, asked = outcome(finds=True)
        self.assertEqual(good.status, PASS, good.evidence)
        self.assertTrue(port["managed"])
        self.assertEqual(port["kept"], ("2500", "off"))
        self.assertTrue(any("systemd-run" in c for c in asked))
        self.assertTrue(any("systemctl stop devtest-host-link.timer" in c for c in asked))
        bad, port, _ = outcome(finds=False)
        self.assertEqual(bad.status, FAIL)
        self.assertTrue(port["managed"])
        self.assertEqual(port["kept"], ("2500", "off"))

    def test_the_pcp_rewrite_case_wants_its_priority_rewritten_and_only_while_set(self):
        def outcome(rewrites: bool):
            trail: list[str] = []
            entry = {"on": False}

            class Stick(Canned):
                def run(self, command, **kw):
                    if "dcb rewr add" in command:
                        entry["on"] = True
                    if "dcb rewr del" in command:
                        entry["on"] = False
                    return super().run(command, **kw)

            class Bench(Canned):
                def run(self, command, **kw):
                    if kw.get("stdin") == cases_data.TAGGED:
                        pcp = int(command.split()[-2].strip("'"))
                        if entry["on"] and rewrites and pcp == 0:
                            pcp = cases_data.REWR_PCP
                        out = '{"%d/%d": 100}' % (cases_data.REWR_VID, pcp)
                        return Reply([command], 0, out + "\n", "", 0.01)
                    return super().run(command, **kw)

            stick = Stick("stick", [("dcb ", 0, "")], trail)
            bench = Bench("bench", [("command -v", 0, "/usr/bin/python3\n")], trail)
            settings = dataclasses.replace(SETTINGS, olt_port="eth1", subscriber_port="eth2")
            ctx = Context(settings, stick, bench, trail)
            ctx.memo["primed"] = True
            return cases_data.pcp_rewrite(ctx), entry["on"]
        good, left = outcome(rewrites=True)
        self.assertEqual(good.status, PASS, good.evidence)
        self.assertFalse(left)
        self.assertEqual(outcome(rewrites=False)[0].status, FAIL)

    def test_the_storm_case_wants_the_first_rule_to_police_and_the_acl_to_drop(self):
        def outcome(winner):
            trail: list[str] = []
            rules: dict[int, str] = {}

            class Stick(Canned):
                def run(self, command, **kw):
                    for part in command.split(";"):
                        m = re.search(r"tc filter (add|replace|del) dev sfp ingress pref (\d+)"
                                      r"(?:.* action (police pkts_rate \d+|drop))?", part)
                        if m and m.group(1) == "del":
                            rules.pop(int(m.group(2)), None)
                        elif m:
                            rules[int(m.group(2))] = m.group(3)
                    return super().run(command, **kw)

            class Bench(Canned):
                def run(self, command, **kw):
                    if kw.get("stdin") == cases_data.STORMED:
                        if "drop" in rules.values():
                            count = 0
                        elif rules:
                            pref = winner(rules)
                            count = int(rules[pref].split()[-1]) * 2 + 16
                        else:
                            count = 600
                        return Reply([command], 0, f"{count}\n", "", 0.01)
                    return super().run(command, **kw)

            stick = Stick("stick", [("tc ", 0, "")], trail)
            bench = Bench("bench", [("command -v", 0, "/usr/bin/python3\n")], trail)
            settings = dataclasses.replace(SETTINGS, olt_port="eth1", subscriber_port="eth2")
            ctx = Context(settings, stick, bench, trail)
            ctx.memo["primed"] = True
            return cases_data.storm(ctx), dict(rules)
        good, left = outcome(min)
        self.assertEqual(good.status, PASS, good.evidence)
        self.assertEqual(left, {})
        self.assertEqual(outcome(max)[0].status, FAIL)

    def test_the_cpu_limit_case_wants_the_flood_held_the_cpu_kept_and_the_limit_back(self):
        def outcome(took: int, loaded: str = "200.00 201.50"):
            trail: list[str] = []
            state = {"limit": 5000, "rx": 1000, "omci": [10, 7]}

            class Stick(Canned):
                def run(self, command, **kw):
                    self.asked.append(command)
                    out = ""
                    if "devlink dev show" in command:
                        out = "platform/1b000000.switch"
                    elif "param set" in command:
                        state["limit"] = int(command.split("value ")[1].split()[0])
                    elif "param show" in command:
                        out = f"      cmode runtime value {state['limit']}"
                    elif "sfp/address" in command:
                        out = "00:00:5e:00:53:01"
                    elif "dsa/tagging" in command:
                        out = "cpu"
                    elif "rx_packets" in command:
                        out = str(state["rx"])
                    elif "ponmac" in command:
                        out = "%d\n%d" % tuple(state["omci"])
                        state["omci"] = [n + 3 for n in state["omci"]]
                    elif "cat " + cases_data.CPU_PROBE in command:
                        out = loaded
                    elif command == cases_data.CPU_TIMED:
                        out = "100.00 101.20"
                    trail.append(f"stick$ {command}\n{out}")
                    return Reply([command], 0, out, "", 0.01)

            class Bench(Canned):
                def run(self, command, **kw):
                    if kw.get("stdin") == cases_data.FLOODER:
                        seconds = int(re.findall(r"\d+", command)[-1])
                        state["rx"] += took if seconds == cases_data.CPU_FLOOD else 30000 * seconds
                        return Reply([command], 0, "2600000\n", "", 0.01)
                    return super().run(command, **kw)

            stick = Stick("stick", [], trail)
            bench = Bench("bench", [("command -v", 0, "/usr/bin/python3\n")], trail)
            settings = dataclasses.replace(SETTINGS, olt_port="eth1", subscriber_port="eth2")
            ctx = Context(settings, stick, bench, trail)
            ctx.memo["primed"] = True
            cases_data.time.sleep, real = (lambda _s: None), cases_data.time.sleep
            try:
                return cases_data.cpu_limit(ctx), state["limit"]
            finally:
                cases_data.time.sleep = real
        good, limit = outcome(took=50400)
        self.assertEqual(good.status, PASS, good.evidence)
        self.assertEqual(limit, 5000)
        self.assertIn("the CPU took 50400", good.evidence)
        self.assertEqual(outcome(took=120000)[0].status, FAIL)
        self.assertEqual(outcome(took=50400, loaded="200.00 203.00")[0].status, FAIL)

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
