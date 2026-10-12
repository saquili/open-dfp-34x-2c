# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of the cases that look at the stick's sessions, its store, its logs, its loader, the
kill of its programs and its updates, on remote ends that answer from a table or that play a
stick: nothing here needs the stick."""

from __future__ import annotations

import dataclasses
import json
import os
import tempfile
import unittest

from devtest import cases_net, cases_system, cases_update
from devtest.access import Reply
from devtest.core import FAIL, PASS, SKIP, Context
from devtest.tests.test_devtest import SETTINGS, Canned, Sequenced, context


class Clock:
    """A clock the sleeps of a case move on."""

    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class Loader(unittest.TestCase):
    def test_the_multicast_listener_must_be_off(self):
        for value, want in (("0", PASS), ("1", FAIL), ("", FAIL)):
            ctx = context(stick=[("onu-update env mupgrade_en", 0, f"mupgrade_en={value}\n"
                                  if value else "")])
            self.assertEqual(cases_system.loader_listener(ctx).status, want, value)


MOUNTS = "mtd:config /config jffs2 ro,nosuid,nodev,noexec,relatime 0 0\n"


class ConfigStore(unittest.TestCase):
    def ctx(self, mounts=MOUNTS, count="2\n", ours=True, sums=("a 1\nb 2\n", "a 1\nb 2\n"), size="1059\n"):
        trail: list[str] = []
        stick = Sequenced("stick", [("cat /proc/mounts", 0, mounts), ("wc -l", 0, count),
                                    (r"\[ -f /config/", 0 if ours else 1, ""),
                                    ("sha256sum", 0, list(sums)), ("wc -c <", 0, size),
                                    ("onu-config save", 0, "")], trail)
        return Context(SETTINGS, stick, Canned("bench", [], trail), trail)

    def test_the_shipped_files_must_be_as_they_were_after_a_save(self):
        good = cases_system.config_store(self.ctx())
        self.assertEqual(good.status, PASS)
        self.assertIn("jffs2 (ro); the shipped firmware's 2 files as they were after a save", good.evidence)
        self.assertIn("this system's file 1059 B of 65536", good.evidence)
        changed = cases_system.config_store(self.ctx(sums=("a 1\nb 2\n", "a 1\nb 3\n")))
        self.assertEqual(changed.status, FAIL)
        self.assertIn("CHANGED", changed.evidence)

    def test_it_wants_jffs2_this_systems_file_under_its_limit_and_the_shipped_ones(self):
        self.assertEqual(cases_system.config_store(self.ctx(mounts=MOUNTS.replace("jffs2", "ext4"))).status, FAIL)
        self.assertEqual(cases_system.config_store(self.ctx(ours=False)).status, FAIL)
        self.assertEqual(cases_system.config_store(self.ctx(size="70000\n")).status, FAIL)
        none = cases_system.config_store(self.ctx(count="0\n"))
        self.assertEqual((none.status, none.evidence), (FAIL, "/config holds no file of the shipped firmware"))
        self.assertEqual(cases_system.config_store(self.ctx(mounts="tmpfs /run tmpfs rw 0 0\n")).status, FAIL)

    def test_the_files_are_found_by_find_not_named(self):
        ctx = self.ctx()
        cases_system.config_store(ctx)
        self.assertTrue(any(a.startswith("find /config -type f ! -name open-dfp-34x-2c.conf -exec sha256sum")
                            for a in ctx.stick.asked))


GOOD_MOUNTS = ("tmpfs /tmp tmpfs rw,relatime,size=2048k 0 0\n"
               "tmpfs /run tmpfs rw,nosuid,nodev,relatime,size=4096k,mode=755 0 0\n"
               "tmpfs /var tmpfs rw,relatime,size=1024k,mode=755 0 0\n")


class LogLimits(unittest.TestCase):
    def outcome(self, mounts=GOOD_MOUNTS, current="40000", older="65000", omcid="4000", rotated=True):
        trail: list[str] = []
        stick = Canned("stick", [("cat /proc/mounts", 0, mounts), ("awk", 0, ""),
                                 (r"messages\.0", 0 if rotated else 1, older),
                                 (r"/run/messages$", 0, current), ("omcid.log", 0, omcid)], trail)
        ctx = Context(SETTINGS, stick, Canned("bench", [], trail), trail)
        real = cases_system.time
        cases_system.time = Clock()
        try:
            return cases_system.log_limits(ctx)
        finally:
            cases_system.time = real

    def test_the_memory_has_limits_and_the_logs_stay_under_theirs(self):
        good = self.outcome()
        self.assertEqual(good.status, PASS)
        self.assertIn("after 2000 lines the log is 39 KiB and a rotated copy 63 KiB", good.evidence)
        self.assertEqual(self.outcome(mounts=GOOD_MOUNTS.replace("size=4096k,", "")).status, FAIL)
        self.assertEqual(self.outcome(mounts=GOOD_MOUNTS.replace("size=1024k", "size=9999k")).status, FAIL)
        self.assertEqual(self.outcome(current="90000").status, FAIL)
        self.assertEqual(self.outcome(rotated=False).status, FAIL)
        self.assertEqual(self.outcome(omcid="300000").status, FAIL)


STATE = ("mode=passthrough\npass.address=192.0.2.9\npass.peer_gateway=192.0.2.1\n"
         "store=loaded\nsession.1.dev=wan410\nsession.1.gem=gem257\nsession.1.hands=ipv4\n"
         "session.2.dev=wan410\nsession.2.gem=gem257\nsession.2.hands=ipv6\n")
PREFIX = {"name": "ses2", "prefix": "2001:db8:1200:5f00::/56", "bound": True, "routed": True}


class Sessions(unittest.TestCase):
    def outcome(self, state=STATE, up=("true", "true"), dhcp=PREFIX):
        stick = [("onu-apply state", 0, state),
                 ("network.interface.ses1 status", 0, json.dumps({"up": up[0] == "true"})),
                 ("network.interface.ses2 status", 0, json.dumps({"up": up[1] == "true"})),
                 ("dhcp6d.ses2 status", 0 if dhcp is not None else 4, json.dumps(dhcp) if dhcp else "")]
        return cases_net.sessions(context(stick=stick))

    def test_two_sessions_up_with_the_address_and_the_prefix_handed_to_the_host(self):
        good = self.outcome()
        self.assertEqual(good.status, PASS)
        self.assertIn("session 1 on gem257, its address handed to the host", good.evidence)
        self.assertIn("session 2 on gem257, prefix 2001:db8:1200:5f00::/56 routed to the host", good.evidence)

    def test_what_is_missing_is_named(self):
        self.assertEqual(self.outcome(state="mode=bridge\n").status, SKIP)
        down = self.outcome(up=("true", "false"))
        self.assertEqual((down.status, "session 2 is not up" in down.evidence), (FAIL, True))
        no_gem = self.outcome(state=STATE.replace("session.1.gem=gem257", ""))
        self.assertIn("session 1 has no GEM port", no_gem.evidence)
        self.assertEqual(self.outcome(dhcp=dict(PREFIX, routed=False)).status, FAIL)
        self.assertEqual(self.outcome(dhcp=None).status, FAIL)
        no_address = self.outcome(state=STATE.replace("pass.address=192.0.2.9\n", ""))
        self.assertIn("no address handed to the host", no_address.evidence)
        self.assertEqual(self.outcome(state="mode=passthrough\n").status, FAIL)
        kept = self.outcome(state=STATE.replace("hands=ipv4", "hands=none"))
        self.assertIn("its address kept on the stick", kept.evidence)


class NoStore(unittest.TestCase):
    def outcome(self, code="423", had_file=True):
        trail: list[str] = []
        stick = Canned("stick", [("cp -p", 0, ""), (r"\[ -e /run/onu-auth.devtest \]", 0 if had_file else 1, ""),
                                 ("echo '!'", 0, ""), ("mv /run/onu-auth.devtest", 0, ""),
                                 ("rm -f /run/onu-auth", 0, "")], trail)
        ctx = Context(SETTINGS, stick, Canned("bench", [("curl", 0, code)], trail), trail)
        return cases_net.pond_no_store(ctx), stick.asked

    def test_every_sign_in_is_refused_with_423_and_the_file_comes_back(self):
        result, asked = self.outcome()
        self.assertEqual(result.status, PASS)
        self.assertTrue(any(a.startswith("mv /run/onu-auth.devtest") for a in asked))
        self.assertEqual(self.outcome(code="200")[0].status, FAIL)
        self.assertEqual(self.outcome(code="401")[0].status, FAIL)

    def test_a_stick_that_had_no_file_has_none_after(self):
        result, asked = self.outcome(had_file=False)
        self.assertEqual(result.status, PASS)
        self.assertIn("rm -f /run/onu-auth", asked)
        self.assertFalse(any(a.startswith("mv ") for a in asked))


class FakeProcd:
    """A stick whose programs procd starts again two reads after they are killed."""

    def __init__(self, instances, objects, pppoed=None, never=(), ubus_objects_back=True,
                 stuck_sessions=()):
        self.running = {k: [True, pid] for k, pid in instances.items()}
        self.objects = list(objects)
        self.pppoed = dict(pppoed or {})
        self.never = set(never)
        self.stuck_sessions = set(stuck_sessions)
        self.session_down = set()
        self.next_pid = 1000
        self.down = {}
        self.objects_back = ubus_objects_back
        self.killed: list[str] = []
        self.ubus_down = 0
        self.objects_present = True

    def new_pid(self):
        self.next_pid += 1
        return self.next_pid

    def tick(self):
        for key in list(self.down):
            self.down[key] -= 1
            if self.down[key] <= 0 and key not in self.never:
                del self.down[key]
                self.running[key] = [True, self.new_pid()]
        for n in list(self.pppoed):
            if self.pppoed[n] is None:
                self.pppoed[n] = self.new_pid()
        if self.ubus_down:
            self.ubus_down -= 1

    def run(self, command, *, timeout=60.0, check=False, stdin=None):
        def reply(out="", code=0):
            return Reply([command], code, out, "", 0.0)
        if command.startswith("kill -9 "):
            pid = int(command.split()[2])
            for key, (running, p) in self.running.items():
                if p == pid:
                    self.running[key] = [False, 0]
                    self.down[key] = 2
                    self.killed.append(key)
                    if key.startswith("ubus/"):
                        self.ubus_down = 2
                        self.objects_present = False
            for n, p in self.pppoed.items():
                if p == pid:
                    self.pppoed[n] = None
                    self.killed.append(f"pppoed {n}")
                    if n in self.stuck_sessions:
                        self.session_down.add(n)
            return reply()
        self.tick()
        if command == "ubus call service list":
            if self.ubus_down:
                return reply("", 4)
            body = {}
            for key, (running, pid) in self.running.items():
                svc, _, inst = key.partition("/")
                body.setdefault(svc, {"instances": {}})["instances"][inst] = (
                    {"running": True, "pid": pid} if running else {"running": False})
            return reply(json.dumps(body))
        if command == "ps":
            return reply("".join(f"  {p} pppoed 1364 S /usr/sbin/pppoed up -i wan410 -f /run/onu-pppoe{n} -M\n"
                                 for n, p in self.pppoed.items() if p))
        if command == "onu-apply state":
            return reply("".join(f"session.{n}.dev=wan410\n" for n in self.pppoed))
        if command.startswith("ubus call network.interface.ses"):
            n = command.split("network.interface.ses")[1].split()[0]
            return reply(json.dumps({"up": n not in self.session_down}))
        if command == "ubus list":
            if self.ubus_down:
                return reply("", 4)
            if not self.objects_present and self.objects_back:
                self.objects_present = True
            return reply("\n".join(self.objects) + "\n" if self.objects_present else "network\n")
        if command == "pon":
            return reply("pon  GPON  O5 operation  ONU-ID 1\n")
        return reply()

    def text(self, command, *, timeout=60.0):
        return self.run(command, timeout=timeout, check=True).out.strip()


class Restart(unittest.TestCase):
    def outcome(self, **kw):
        stick = FakeProcd({"klogd/klogd": 11, "onu/omcid": 12, "onu/dhcp6d2": 13, "pond/pond": 14,
                           "ubus/instance1": 15, "netifd/netifd": 16},
                          ["omcid", "dhcp6d.ses2"], pppoed={"1": 21, "2": 22}, **kw)
        trail: list[str] = []
        ctx = Context(SETTINGS, stick, Canned("bench", [], trail), trail)
        real = cases_system.time
        cases_system.time = Clock()
        try:
            return cases_system.restart(ctx), stick
        finally:
            cases_system.time = real

    def test_every_program_killed_in_the_order_the_others_hang_from(self):
        result, stick = self.outcome()
        self.assertEqual(result.status, PASS, result.evidence)
        self.assertEqual(stick.killed, ["klogd/klogd", "onu/dhcp6d2", "pond/pond", "onu/omcid",
                                        "pppoed 1", "pppoed 2", "netifd/netifd", "ubus/instance1"])
        self.assertIn("8 of 8 killed programs running again", result.evidence)
        self.assertIn("2 session(s) up again after", result.evidence)
        self.assertIn("dhcp6d.ses2, omcid on ubus after", result.evidence)

    def test_a_program_that_does_not_come_back_fails_the_case_and_is_named(self):
        result, _ = self.outcome(never={"onu/omcid"})
        self.assertEqual(result.status, FAIL)
        self.assertIn("not back: onu/omcid", result.evidence)

    def test_a_session_that_stays_pending_under_netifd_fails_the_case(self):
        result, _ = self.outcome(stuck_sessions={"2"})
        self.assertEqual(result.status, FAIL)
        self.assertIn("sessions NOT up again", result.evidence)

    def test_objects_that_do_not_come_back_on_ubus_fail_the_case(self):
        result, _ = self.outcome(ubus_objects_back=False)
        self.assertEqual(result.status, FAIL)
        self.assertIn("objects on ubus NOT back", result.evidence)

    def test_a_stick_whose_services_cannot_be_read_fails_at_once(self):
        trail: list[str] = []
        stick = Canned("stick", [("ubus call service list", 4, "")], trail)
        self.assertEqual(cases_system.restart(Context(SETTINGS, stick, Canned("bench", [], trail), trail)).status,
                         FAIL)


class FakeBanks:
    """A stick with two banks, the loader and onu-trial: a write, a try and a reboot."""

    def __init__(self, running="0", other_state="valid", fails=False, never_back=False):
        self.running, self.committed, self.next = running, running, None
        self.state = {"0": "valid", "1": other_state}
        self.version = {"0": "old", "1": "shipped-1"}
        self.fails, self.never_back = fails, never_back
        self.boot, self.gone = "boot-1", 0
        self.written: bytes | None = None
        self.asked: list[str] = []

    def run(self, command, *, timeout=60.0, check=False, stdin=None):
        self.asked.append(command)

        def reply(out="", code=0):
            return Reply([command], code, out, "", 0.0)
        if command.startswith("onu-update write - "):
            bank = command.split()[-1]
            self.written = stdin
            self.state[bank], self.version[bank] = "valid", "new"
            return reply()
        if command.startswith("onu-update try "):
            self.next = command.split()[-1]
            return reply()
        if command == "reboot":
            self.gone = 2
            if self.next is not None and not self.fails:
                self.running = self.next
            self.committed = self.committed
            return reply()
        if self.gone:
            self.gone -= 1
            if self.gone == 0:
                self.boot = f"boot-{int(self.boot.split('-')[1]) + 1}"
                if self.never_back:
                    self.gone = 10 ** 9
                else:
                    self.committed = self.running
            return reply("", 255)
        if "boot_id" in command:
            return reply(self.boot + "\n")
        if command == "onu-update status":
            text = (f"RUNNING={self.running}\nCOMMITTED={self.committed}\n"
                    + "".join(f"BANK{b}_STATE={self.state[b]}\nBANK{b}_VERSION={self.version[b]}\n" for b in "01"))
            return reply(text)
        return reply()

    def text(self, command, *, timeout=60.0):
        return self.run(command, timeout=timeout, check=True).out.strip()


class Updates(unittest.TestCase):
    def outcome(self, update=True, overwrite=False, **kw):
        stick = FakeBanks(**kw)
        trail: list[str] = []
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "update.bin")
            with open(path, "wb") as handle:
                handle.write(b"an update")
            settings = dataclasses.replace(SETTINGS, update=path if update else "", overwrite_shipped=overwrite)
            ctx = Context(settings, stick, Canned("bench", [], trail), trail)
            real = cases_update.time
            cases_update.time = Clock()
            try:
                return cases_update.trial_and_back(ctx), stick
            finally:
                cases_update.time = real

    def test_the_update_runs_on_trial_is_kept_and_the_stick_goes_back(self):
        result, stick = self.outcome()
        self.assertEqual(result.status, PASS, result.evidence)
        self.assertEqual(stick.written, b"an update")
        self.assertEqual([a for a in stick.asked if a.startswith("onu-update try")],
                         ["onu-update try 1", "onu-update try 0"])
        self.assertEqual((stick.running, stick.committed), ("0", "0"))
        self.assertIn("written into bank 1 (9 B, was valid)", result.evidence)
        self.assertIn("version new", result.evidence)

    def test_it_goes_the_other_way_from_bank_1(self):
        result, stick = self.outcome(running="1")
        self.assertEqual(result.status, PASS, result.evidence)
        self.assertIn("written into bank 0", result.evidence)

    def test_it_asks_before_it_writes_over_the_shipped_firmware(self):
        result, stick = self.outcome(other_state="shipped")
        self.assertEqual(result.status, SKIP)
        self.assertIn("--overwrite-shipped", result.evidence)
        self.assertIsNone(stick.written)
        result, stick = self.outcome(other_state="shipped", overwrite=True)
        self.assertEqual(result.status, PASS, result.evidence)
        self.assertIn("was shipped", result.evidence)

    def test_it_needs_an_update_file_and_a_bank_that_runs(self):
        self.assertEqual(self.outcome(update=False)[0].status, SKIP)
        self.assertEqual(self.outcome(running="-1")[0].status, SKIP)

    def test_a_bank_that_does_not_come_up_fails_the_case(self):
        result, _ = self.outcome(fails=True)
        self.assertEqual(result.status, FAIL)
        self.assertIn("on trial: the stick came back in bank 0", result.evidence)

    def test_a_stick_that_never_comes_back_fails_the_case(self):
        result, _ = self.outcome(never_back=True)
        self.assertEqual(result.status, FAIL)
        self.assertIn("never saw the stick come back", result.evidence)


if __name__ == "__main__":
    unittest.main()
