# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of the files of the board's overlay and of what they rely on.

The boot script runs on the board and cannot run here, so these tests read it:
it must parse, it may call only commands that the image holds, and the commands
that it needs must be in the BusyBox configuration. The inittab is read against
the format that BusyBox init takes.
"""

import os
import re
import shutil
import stat
import subprocess
import unittest

from bootscript import commands

HERE = os.path.dirname(os.path.realpath(__file__))
BOARD = os.path.join(os.path.dirname(os.path.dirname(HERE)), "board", "dfp-34x-2c3")
OVERLAY = os.path.join(BOARD, "overlay")
RCS = os.path.join(OVERLAY, "etc", "init.d", "rcS")
INITTAB = os.path.join(OVERLAY, "etc", "inittab")
NFTABLES = os.path.join(OVERLAY, "etc", "nftables.conf")
SHELLS = [s for s in (shutil.which("sh"), shutil.which("dash")) if s]

# Words that start a command in the boot script and are not programs.
BUILTINS = {"export", "echo", "warn", "[", "if", "then", "else", "elif", "fi", "do", "done",
            "case", "esac", ".", "exit", "set", "true", "false", ":", "umask"}
# Programs of the dropbear package and of this tree's packages; every other
# command is a BusyBox applet.
DROPBEAR = {"dropbear", "dropbearkey"}
PACKAGES = {"omcid", "ponid", "devlink", "pond", "nft", "onu-mode", "tc"}
# BusyBox applets by the name of their configuration symbol, where it is not the upper-case name.
SYMBOL = {"hostname": "HOSTNAME", "ip": "IP"}


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def enabled_applets():
    symbols = set()
    for line in read(os.path.join(BOARD, "busybox.config")).splitlines():
        match = re.match(r"^CONFIG_([A-Z0-9_]+)=y$", line)
        if match:
            symbols.add(match.group(1))
    return symbols


class BootScript(unittest.TestCase):
    def test_it_is_executable_and_starts_with_its_shell(self):
        self.assertTrue(os.stat(RCS).st_mode & stat.S_IXUSR)
        self.assertEqual(read(RCS).splitlines()[0], "#!/bin/sh")

    def test_it_parses_in_every_posix_shell_there_is(self):
        for shell in SHELLS:
            with self.subTest(shell=shell):
                done = subprocess.run([shell, "-n", RCS], capture_output=True, text=True)
                self.assertEqual(done.returncode, 0, done.stderr)

    def test_it_calls_only_programs_the_image_holds(self):
        symbols = enabled_applets()
        unknown = []
        for word in sorted(set(commands(read(RCS)))):
            if word in BUILTINS or word in DROPBEAR or word in PACKAGES:
                continue
            if SYMBOL.get(word, word.upper()) not in symbols:
                unknown.append(word)
        self.assertEqual(unknown, [], "not applets of busybox.config")

    def test_the_programs_it_needs_are_the_ones_it_calls(self):
        # If the script stops calling one of these, the list of what the image
        # needs is out of date: take the applet out of the configuration.
        called = set(commands(read(RCS)))
        for word in ("mount", "mkdir", "ln", "hostname", "ip", "grep", "uname", "dropbear",
                     "dropbearkey", "syslogd", "klogd", "watchdog"):
            self.assertIn(word, called)

    def test_it_mounts_what_the_kernel_leaves_to_it(self):
        text = read(RCS)
        for what in ("-t proc proc /proc", "-t sysfs sysfs /sys", "devtmpfs", "/dev/pts",
                     "tmpfs", "/tmp", "/run", "/var"):
            self.assertIn(what, text)

    def test_the_ssh_server_takes_keys_only_and_its_host_key_is_made_first(self):
        text = read(RCS)
        self.assertRegex(text, r"dropbear -s ")
        self.assertLess(text.index("dropbearkey -t ed25519"), text.index("dropbear -s "))

    def test_it_ends_with_the_ready_line_that_the_bench_waits_for(self):
        self.assertEqual(read(RCS).rstrip().splitlines()[-1],
                         'echo "open-dfp-34x-2c: ready"')

    def test_the_default_addresses_are_in_the_documentation_ranges(self):
        text = read(RCS)
        addresses = re.findall(r"^(?:DATA|MGMT)_ADDRESS=(\S+)$", text, re.M)
        self.assertEqual(addresses, ["192.0.2.2/30", "198.51.100.2/30"])

    def test_the_bench_defaults_are_the_addresses_it_tries_to_reach(self):
        # tools/bench/settings.py: board.control_address and board.data_address.
        settings = read(os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(BOARD))),
                                     "tools", "bench", "settings.py"))
        self.assertIn('"board.control_address": (str, "198.51.100.2")', settings)
        self.assertIn('"board.data_address": (str, "192.0.2.2")', settings)

    def test_a_bench_can_override_the_addresses(self):
        self.assertIn("[ -r /etc/board.conf ] && . /etc/board.conf", read(RCS))


class Reading(unittest.TestCase):
    """The reading of a script that the tests above rest on."""

    def test_operators_substitutions_and_tests_start_commands(self):
        self.assertEqual(commands('a x || b y && c\nd $(e f) | g\n[ -n "$x" ] && h\n'),
                         ["a", "b", "c", "d", "e", "g", "h"])

    def test_assignments_and_case_patterns_are_not_commands(self):
        script = ("x=1\ncase $x in\n*[!0-9]*) ;;\n"
                  "*) [ ${#x} -eq 2 ] && x=0x$x ;;\nesac\ny z\n")
        self.assertEqual(commands(script), ["case", "esac", "y"])


class Watchdog(unittest.TestCase):
    def test_the_daemon_feeds_it_well_inside_the_drivers_timeout(self):
        # The driver's timeout on this board is 44 seconds, as
        # /sys/class/watchdog/watchdog0/timeout reads.
        match = re.search(r"^watchdog -t (\d+) /dev/watchdog ", read(RCS), re.M)
        self.assertIsNotNone(match)
        self.assertLessEqual(int(match.group(1)), 15)


class PacketFilter(unittest.TestCase):
    def test_it_is_loaded_before_any_server_listens(self):
        text = read(RCS)
        load = text.index("nft -f /etc/nftables.conf")
        for server in ("dropbear -s ", "onu-mode start", "pond >"):
            self.assertLess(load, text.index(server), server)

    def test_it_starts_from_nothing_and_drops_what_it_does_not_accept(self):
        text = read(NFTABLES)
        self.assertIn("\nflush ruleset\n", text)
        self.assertEqual(len(re.findall(r"hook (?:input|forward) priority filter; policy drop;",
                                        text)), 2)

    def test_the_host_side_reaches_the_servers_of_the_stick(self):
        self.assertIn('iifname { "sfp", "mgmt" } tcp dport { 22, 8080 } accept', read(NFTABLES))

    def test_icmpv6_is_accepted_or_ipv6_would_not_work(self):
        self.assertIn("meta l4proto ipv6-icmp accept", read(NFTABLES))


class QuietTowardsThePon(unittest.TestCase):
    """Nothing of the stick's own leaves by the PON, and nothing from it is answered."""

    def test_the_pon_side_has_no_ipv6_before_the_pon_comes_up(self):
        text = read(RCS)
        for conf in ("default", "cpu", "pon"):
            line = f"echo 1 >/proc/sys/net/ipv6/conf/{conf}/disable_ipv6"
            with self.subTest(conf=conf):
                self.assertIn(line, text)
                # "default" covers the bridge and the GEM interfaces that
                # omcid makes, so it must be in force before omcid starts.
                for later in ("ip link set pon up", "onu-mode start"):
                    self.assertLess(text.index(line), text.index(later), later)

    def test_the_host_side_keeps_ipv6(self):
        text = read(RCS)
        self.assertNotIn("conf/sfp/disable_ipv6", text)
        made = text.index('name mgmt type vlan id "$MGMT_VLAN"')
        self.assertLess(made, text.index("echo 0 >/proc/sys/net/ipv6/conf/mgmt/disable_ipv6"))

    def test_arp_answers_only_on_the_interface_of_the_address(self):
        text = read(RCS)
        self.assertIn("echo 1 >/proc/sys/net/ipv4/conf/all/arp_ignore", text)
        self.assertIn("echo 2 >/proc/sys/net/ipv4/conf/all/arp_announce", text)
        self.assertLess(text.index("arp_ignore"), text.index("ip link set sfp up"))


class Inittab(unittest.TestCase):
    ACTIONS = {"sysinit", "wait", "once", "respawn", "askfirst", "shutdown", "restart",
               "ctrlaltdel"}

    def entries(self):
        for line in read(INITTAB).splitlines():
            if line.strip() and not line.startswith("#"):
                yield line

    def test_every_entry_has_the_four_fields_and_a_known_action(self):
        for line in self.entries():
            fields = line.split(":", 3)
            self.assertEqual(len(fields), 4, line)
            self.assertIn(fields[2], self.ACTIONS, line)

    def test_the_boot_script_is_run_at_sysinit(self):
        self.assertIn("::sysinit:/etc/init.d/rcS", list(self.entries()))

    def test_the_console_shell_is_a_login_shell_that_is_started_again(self):
        self.assertIn("::respawn:-/bin/sh", list(self.entries()))

    def test_the_boot_script_it_names_is_in_the_overlay(self):
        self.assertTrue(os.path.exists(RCS))

    def test_init_and_the_applets_it_runs_are_built(self):
        symbols = enabled_applets()
        for symbol in ("INIT", "FEATURE_USE_INITTAB", "REBOOT", "UMOUNT", "ASH", "SH_IS_ASH"):
            self.assertIn(symbol, symbols)


class Layout(unittest.TestCase):
    def test_os_release_names_the_image(self):
        text = read(os.path.join(OVERLAY, "etc", "os-release"))
        self.assertIn('PRETTY_NAME="open-dfp-34x-2c on the ODI DFP-34X-2C3"', text)
        self.assertNotIn("Buildroot", text)

    def test_var_exists_as_the_place_a_temporary_file_system_goes_on(self):
        self.assertTrue(os.path.isdir(os.path.join(OVERLAY, "var")))

    def test_no_key_and_no_secret_is_part_of_the_overlay(self):
        for top, _dirs, files in os.walk(OVERLAY):
            for name in files:
                self.assertNotIn(name, ("authorized_keys", "shadow", "board.conf"), top)
        self.assertFalse(os.path.exists(os.path.join(OVERLAY, "root")))


if __name__ == "__main__":
    unittest.main()
