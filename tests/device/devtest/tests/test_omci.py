# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of the stand-in OLT's messages and of the cases that inject them, on remote ends
that answer from a table: nothing here needs the stick."""

from __future__ import annotations

import json
import unittest

from devtest import cases_omci, omci
from devtest.core import FAIL, PASS, SKIP, Context
from devtest.tests.test_devtest import SETTINGS, Canned, Sequenced

# Two messages omcid took from the inject file of a stick (2026-10-21): a Create of an
# authentication security method, and a Set of its validation scheme.
TAKEN = ("7e01440a0094000100000000000000000000000000000000000000000000000000000000000000000000"
         "0028be06eabe",
         "7e02480a0094000180000100000000000000000000000000000000000000000000000000000000000000"
         "0028f6276b7d")


class Messages(unittest.TestCase):
    def test_the_check_is_the_crc_of_i_363_5(self):
        self.assertEqual(omci.crc32(b"123456789"), 0xFC891918)

    def test_the_messages_are_those_omcid_took(self):
        olt = omci.Olt()
        self.assertEqual(olt.create(148, 1).hex(), TAKEN[0])
        self.assertEqual(olt.set(148, 1, a1=1).hex(), TAKEN[1])

    def test_a_set_has_the_mask_of_its_attributes_and_their_values_in_order(self):
        msg = omci.Olt().set(157, 1, a2="sip", a1=1)
        self.assertEqual(len(msg), 48)
        self.assertEqual(msg[8:10], bytes.fromhex("c000"))
        self.assertEqual(msg[10:11], b"\x01")
        self.assertEqual(msg[11:36], b"sip" + bytes(22))

    def test_a_create_sets_what_a_create_sets_and_zero_the_rest(self):
        msg = omci.Olt().create(136, 1, a1=5060, a2=17)
        self.assertEqual(msg[2], omci.CREATE)
        self.assertEqual(msg[8:14], bytes.fromhex("13c4" "11" "00" "0000"))
        with self.assertRaisesRegex(ValueError, "does not set attribute 2"):
            omci.Olt().create(148, 1, a2="x")

    def test_what_does_not_fit_is_refused(self):
        olt = omci.Olt()
        with self.assertRaisesRegex(ValueError, "bytes of contents"):
            olt.set(148, 1, a2="x", a3="y")
        with self.assertRaisesRegex(ValueError, "does not fit"):
            olt.set(148, 1, a2="x" * 26)
        with self.assertRaisesRegex(ValueError, "has no attribute 9"):
            olt.set(148, 1, a9=1)
        with self.assertRaisesRegex(ValueError, "not one omcid serves"):
            olt.set(9999, 1, a1=1)

    def test_the_numbers_go_on_and_wrap(self):
        olt = omci.Olt(first=0xFFFF)
        self.assertEqual(olt.delete(148, 1)[:2], b"\x00\x00")
        self.assertEqual(omci.hexes([b"\x01", b"\xab"]), "01\nab\n")


def status(rejected: int) -> str:
    return json.dumps({"instances": 1, "frames": {"handled": 1, "rejected": rejected,
                                                   "answered": 1}})


PATH = "/sys/kernel/debug/pon/pon0omci0/inject\n"
EMPTY = json.dumps({"mes": [], "truncated": False})


def ctx(answers) -> Context:
    trail: list[str] = []
    return Context(SETTINGS, Sequenced("stick", answers, trail), Canned("bench", [], trail), trail)


class Login(unittest.TestCase):
    LOGIN = json.dumps({"user": "client@isp.example.org", "secret": "example-login-secret",
                        "realm": "isp.example.org"})

    def answers(self, after=0, gone="{}"):
        return [("ls /sys/kernel/debug", 0, PATH), ("omcid mib", 0, EMPTY),
                ("omcid status", 0, [status(0), status(after)]),
                ("omcid login", 0, [self.LOGIN, gone]), ("while read", 0, "")]

    def test_the_login_is_given_and_goes_with_its_instance(self):
        c = ctx(self.answers())
        out = cases_omci.olt_login(c)
        self.assertEqual(out.status, PASS, out.evidence)
        self.assertIn("after the Delete none", out.evidence)
        sent = [a for a in c.stick.asked if "while read" in a]
        self.assertEqual(len(sent), 2)

    def test_a_refused_message_or_a_login_left_behind_fails(self):
        self.assertEqual(cases_omci.olt_login(ctx(self.answers(after=1))).status, FAIL)
        self.assertEqual(cases_omci.olt_login(ctx(self.answers(gone=self.LOGIN))).status, FAIL)

    def test_without_the_inject_file_or_with_the_olts_own_login_it_is_skipped(self):
        out = cases_omci.olt_login(ctx([("ls /sys/kernel/debug", 2, "")]))
        self.assertEqual(out.status, SKIP)
        self.assertIn("--enable PON_OMCI_INJECT", out.evidence)
        own = json.dumps({"mes": [{"class": 148, "instance": 1, "attrs": {}}]})
        out = cases_omci.olt_login(ctx([("ls /sys/kernel/debug", 0, PATH),
                                        ("omcid mib", 0, own)]))
        self.assertEqual(out.status, SKIP)


class Television(unittest.TestCase):
    PORT = json.dumps({"mes": [{"class": 47, "instance": 1, "attrs": {"3": "01"}}]})
    POLICY = json.dumps({"policy": {"igmp_version": 3, "mode": "snoop", "fast_leave": True,
                                    "allowed": [{"from": "239.255.1.0", "to": "239.255.1.255"}]}})

    def answers(self, outside='{"error": "refused", "reason": "not_allowed"}'):
        return [("ls /sys/kernel/debug", 0, PATH),
                ("omcid mib.*\\[47\\]", 0, self.PORT), ("omcid mib", 0, EMPTY),
                ("omcid status", 0, [status(0), status(0)]),
                ("omcid iptv", 0, [self.POLICY, '{"policy": null}']),
                ("tv_test '{\"stop\"", 0, '{"running": false, "bytes": 0}'),
                ("tv_test '{\"group\": -268500731", 0, '{"ok": true}'),
                ("tv_test", 0, outside), ("while read", 0, "")]

    def test_the_policy_is_the_profiles_and_a_group_outside_it_is_refused(self):
        out = cases_omci.igmp_policy(ctx(self.answers()))
        self.assertEqual(out.status, PASS, out.evidence)
        self.assertIn("239.255.2.5 refused (not_allowed)", out.evidence)
        self.assertEqual(out.status, PASS)

    def test_a_group_outside_the_list_that_is_taken_fails(self):
        self.assertEqual(cases_omci.igmp_policy(ctx(self.answers(outside='{"ok": true}'))).status,
                         FAIL)

    def test_a_group_is_given_as_the_bits_of_a_signed_32_bit_number(self):
        self.assertEqual(cases_omci.group("239.255.1.5"), 0xEFFF0105 - (1 << 32))
        self.assertEqual(cases_omci.group("127.0.0.1"), 0x7F000001)

    def test_without_a_bridge_port_of_a_uni_it_is_skipped(self):
        out = cases_omci.igmp_policy(ctx([("ls /sys/kernel/debug", 0, PATH), ("omcid mib", 0, EMPTY)]))
        self.assertEqual(out.status, SKIP)


class Phone(unittest.TestCase):
    def test_it_sets_up_a_whole_line_and_the_ip_hosts_rule(self):
        sent = cases_omci.phone_messages(omci.Olt())
        kinds = {(m[4] << 8 | m[5], m[2]) for m in sent}
        for class_id in (157, 137, 136, 148, 150, 143, 58, 142, 153, 139, 171):
            self.assertIn((class_id, omci.CREATE), kinds, class_id)
        self.assertIn((138, omci.SET), kinds)
        self.assertTrue(all(len(m) == 48 for m in sent))

    def test_without_the_inject_file_it_is_skipped(self):
        self.assertEqual(cases_omci.phone(ctx([("ls /sys/kernel/debug", 2, "")])).status, SKIP)


if __name__ == "__main__":
    unittest.main()
