# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The forbidden-word and unit-identifier rules, one by one.

Each rule is shown to fire on what it refuses and to stay quiet on the
ordinary technical use of the same word or shape. Refused text is assembled
at run time (see support.py).
"""

from __future__ import annotations

import unittest

from .support import Seeds, scan, word

NARRATIVE = word("ven", "dor")
KIT = word("S", "DK")
PACKAGE = word("B", "SP")


class ForbiddenWords(unittest.TestCase):

    def fires(self, text: str, rule: str, path: str = "notes.txt") -> None:
        self.assertIn(("forbidden-words", rule), scan(text, path), text)

    def quiet(self, text: str, path: str = "notes.txt") -> None:
        self.assertEqual(scan(text, path), set(), text)

    def test_tool_words(self) -> None:
        for text in (word("de", "compiled from it"), word("the dis", "assembly"),
                     word("Ghi", "dra"), word("rad", "are2"), word("reverse", "-engineered"),
                     word("reverse eng", "ineering"), word("binary ", "ninja"),
                     word("ID", "A Pro"), word("ID", "APython"), word("opened in ", "ID", "A")):
            self.fires(text, "re-tool")

    def test_the_kernel_id_allocator_is_not_a_tool(self) -> None:
        self.quiet("Minor numbers come from an IDA.")
        self.quiet("The IDA-allocated minor is freed on release.")
        self.quiet("static DEFINE_IDA(pon_ids); id = ida_alloc(&pon_ids, GFP_KERNEL);", "pon.c")

    def test_automatic_names(self) -> None:
        for text in (word("FU", "N_0012ab34"), word("sub", "_0040ab12"), word("fc", "n.00401000")):
            self.fires(text, "re-autoname")

    def test_narrative(self) -> None:
        for text in ("The %s sets it." % NARRATIVE, "as the %s's driver does" % NARRATIVE,
                     "%ss disagree" % NARRATIVE.capitalize(), "the %s kernel" % NARRATIVE):
            self.fires(text, "narrative")

    def test_standards_terms_after_vendor(self) -> None:
        for text in ("the vendor ID of the serial number", "vendor-specific data",
                     "vendor name, vendor OUI, vendor PN, vendor rev, vendor SN",
                     "the date code", "vendor-prefixes.yaml", "a device-tree vendor prefix",
                     "vendor product code", "vendor class identifier"):
            self.quiet(text)

    def test_development_kit(self) -> None:
        for text in ("from the %s tables" % KIT, "%s_PATH = /opt/kit" % KIT,
                     "r" + KIT.lower(), "the %ss" % KIT):
            self.fires(text, "dev-kit")

    def test_the_build_system_target_is_not_a_reference(self) -> None:
        self.quiet("Run ``make sdk`` for a relocatable toolchain.")
        kit = KIT.lower()
        self.quiet(".PHONY: image %s\n\n%s:\n\t@true\n" % (kit, kit), "Makefile")
        self.quiet("%s:\n\t@true\n" % kit, "external.mk")

    def test_the_target_exception_is_narrow(self) -> None:
        kit = KIT.lower()
        self.fires("\t%s-tool --build\n" % kit, "dev-kit", "Makefile")
        self.fires("%s := ../kit\n" % kit, "dev-kit", "Makefile")
        self.fires("%s:\n\t@true\n" % kit, "dev-kit", "notes.txt")

    def test_board_package(self) -> None:
        self.fires("the header of that %s" % PACKAGE, "board-package")
        self.fires(word("bs", "p_board_init"), "board-package")

    def test_nonpublic_material(self) -> None:
        for text in (word("the lea", "ked ", KIT), word("a lea", "ked datasheet"),
                     word("from the lea", "ked source"), word("the GitHub le", "ak")):
            self.fires(text, "nonpublic-material")

    def test_technical_leaks_are_not_material(self) -> None:
        for text in ("fix a memory leak in the error path", "a way to leak them",
                     "the per-queue leaky bucket", "leak the skb on failure",
                     "the skb is leaked too", "kmemleak reports it",
                     "fixes a leak in the source port lookup"):
            self.quiet(text)

    def test_source_collection(self) -> None:
        self.fires(word("the source cor", "pus"), "source-collection")
        self.quiet("the fuzz corpus")

    def test_code_address(self) -> None:
        self.fires("jumps to %s" % Seeds().code_address(), "code-address")
        self.fires(word("0x80", "12ab34UL"), "code-address")
        self.quiet("0x8012345 has seven digits; 0x801234567 has nine")

    def test_issue_references(self) -> None:
        for text in (word("see ", "#", "203"), word("(", "#", "145)"), word("(", "#", "194/", "#202)"),
                     word("G", "H-12")):
            self.fires(text, "issue-reference")
        self.quiet('fill="#000000"; color: #333333; &#123; https://example.org/page#12')
        self.quiet("Issue a CCTL operation; the driver issues 3 reads.")
        self.quiet("\tmov r0, #1\n\tadd sp, sp,#16\n", "entry.S")

    def test_lab_paths(self) -> None:
        for text in ("/" + word("Us", "ers/someone"), word("/ho", "me/someone/git/tree"),
                     word("~/", "git/tree"),
                     "/" + word("private/", "tmp/x"), "/" + word("Vol", "umes/Lab")):
            self.fires(text, "lab-path")
        self.quiet("the overlay installs /home/ftp and /root")

    def test_internal_references(self) -> None:
        self.fires(word("the sibling ", "repo"), "internal-reference")
        self.fires(word("in the lab ", "session"), "internal-reference")

    def test_other_software(self) -> None:
        for text in (word("the orig", "inal firmware sets it"),
                     word("as the sto", "ck driver does"), word("the OE", "M bootloader"),
                     word("factory ", "software"),
                     word("what the previous ", "driver left"), word("the old ", "drivers")):
            self.fires(text, "narrative")

    def test_facts_of_the_hardware_are_not_narrative(self) -> None:
        self.quiet("cleared the entries left by the previous kernel")
        self.quiet("the identity from the unit's factory configuration")
        self.quiet("the original value is written back")


class UnitIdentifiers(unittest.TestCase):

    def fires(self, text: str, rule: str, path: str = "notes.txt") -> None:
        self.assertIn(("unit-ids", rule), scan(text, path), text)

    def quiet(self, text: str, path: str = "notes.txt") -> None:
        self.assertEqual(scan(text, path), set(), text)

    def setUp(self) -> None:
        self.seeds = Seeds()

    def test_gpon_serial(self) -> None:
        serial = self.seeds.gpon_serial()
        self.fires("serial %s" % serial, "gpon-serial")
        self.fires("serial %s-%s" % (serial[:4], serial[4:]), "gpon-serial")
        hexed = serial[:4].encode().hex() + serial[4:]
        self.fires("serial 0x%s" % hexed, "gpon-serial")
        self.quiet("the value ABCD12345678 and 0x4142434412345678")

    def test_device_serial(self) -> None:
        self.fires("S/N: 7%s" % self.seeds.hexdigits(9), "device-serial")
        self.fires('serial_number = "5%s"' % self.seeds.hexdigits(8), "device-serial", "x.c")
        self.quiet("serial: <your serial>; sn = read_sn(dev);", "x.c")
        self.quiet("serial: abcdefgh")
        self.quiet(word("s", "n = board42 + 1;"), "x.c")  # an expression, not a literal

    def test_mac(self) -> None:
        mac = self.seeds.mac()
        self.fires("address %s" % mac, "mac-address")
        self.fires("address %s" % mac.replace(":", "-").upper(), "mac-address")
        dotted = mac.replace(":", "")
        self.fires("address %s.%s.%s" % (dotted[:4], dotted[4:8], dotted[8:]), "mac-address")
        self.quiet("00:00:5e:00:53:01 01:00:5e:00:00:01 ff:ff:ff:ff:ff:ff 00:00:00:00:00:00")

    def test_ipv4(self) -> None:
        self.fires("host %s" % self.seeds.private_ipv4(), "ipv4-address")
        self.fires("host %s/24" % self.seeds.private_ipv4(), "ipv4-address")
        self.fires("resolver %s" % ".".join(["9", "9", "9", "9"]), "ipv4-address")
        self.quiet("192.0.2.1 198.51.100.7 203.0.113.9 127.0.0.1 255.255.255.0 224.0.0.1")
        self.quiet("The private blocks are 10.0.0.0/8, 172.16.0.0/12 and 192.168.0.0/16.")
        self.quiet("uClibc 0.9.30.3 and version 6.18.48")
        self.quiet("not an address: 999.1.2.3")
        for cite in ("G.984.3 9.2.3.1", "G.988 clause 9.3.11.1", "clause 10.2.1.1",
                     "IEEE 802.3-2022 45.2.1.1", "§ 9.2.3.1", "SFF-8472 section 8.2.1.1",
                     "RFC 7042 2.1.2.1", "Table 9.2.3.1"):
            self.quiet("see %s for the layout" % cite)
        self.fires("see %s for the layout" % self.seeds.private_ipv4(), "ipv4-address")

    def test_ipv6(self) -> None:
        self.fires("host %s" % ":".join(["fd12", "3456", "789a", "", "1"]), "ipv6-address")
        self.quiet("2001:db8::1 ff02::1 ::1 fe80::1 std::string a::b abcd::ef01::1")

    def test_passwords(self) -> None:
        self.fires("password: %s" % self.seeds.hexdigits(10), "password")
        self.fires("PLOAM_PW='%s'" % self.seeds.hexdigits(10), "password", "board.conf")
        secret = word("pass", "word")
        self.quiet("%s: <secret>; %s = cfg->pw;" % (secret, secret), "x.c")
        self.quiet("%s: xxxxxxxx" % secret)

    def test_host_names_under_private_top_level_names(self) -> None:
        self.fires("the share on %s" % word("nas.l", "an"), "host-name")
        self.fires("ssh root@%s" % word("build.home.ar", "pa"), "host-name")
        self.fires('url = "https://%s/x";' % word("wiki.int", "ernal"), "host-name", "x.c")
        self.fires('fetch("http://%s/api")' % word("stick.l", "ocal"), "host-name", "api.ts")
        self.quiet(word("cfg.lo", "cal = 1; p->priv.inter", "nal = 0;"), "x.c")
        self.quiet(word("const lan = s.pass.l", "an;"), "switch.ts")
        self.quiet("mDNS answers for names under .local")

    def test_host_names_in_settings_and_remote_commands(self) -> None:
        host = word("stick.bench", "lab.net")
        self.fires('target: "https://%s",' % host, "host-name", "vite.config.ts")
        self.fires('const target = process.env.POND_TARGET ?? "https://%s";' % host, "host-name",
                   "vite.config.ts")
        self.fires("POND_TARGET=https://%s npm run dev" % host, "host-name")
        self.fires("proxy = http://user@%s:3128" % host, "host-name", "x.conf")
        self.fires("scp image root@%s:/tmp" % host, "host-name")
        self.quiet('target: "https://stick.example", proxy = http://proxy.example.com:3128')
        self.quiet("POND_TARGET=https://192.0.2.1 npm run dev")
        self.quiet("ssh root@192.0.2.1; scp x root@stick.test:/tmp")
        self.quiet("Link: https://git.kernel.org/x and the host the board boots from")


if __name__ == "__main__":
    unittest.main()
