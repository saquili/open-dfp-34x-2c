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

from bootscript import BUILTINS, DROPBEAR, PACKAGES, commands

HERE = os.path.dirname(os.path.realpath(__file__))
BOARD = os.path.join(os.path.dirname(os.path.dirname(HERE)), "board", "dfp-34x-2c3")
OVERLAY = os.path.join(BOARD, "overlay")
RCS = os.path.join(OVERLAY, "etc", "init.d", "rcS")
INIT = os.path.join(OVERLAY, "sbin", "init")
INITTAB = os.path.join(OVERLAY, "etc", "inittab")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
TCPRIO = os.path.join(REPO, "omcid", "target", "tcprio.h")
ONU_MGMT = os.path.join(REPO, "onu-utils", "scripts", "onu-mgmt")
ONU_APPLY = os.path.join(REPO, "onu-utils", "scripts", "onu-apply")
SHELLS = [s for s in (shutil.which("sh"), shutil.which("dash")) if s]

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
        for script in (RCS, INIT):
            unknown = []
            for word in sorted(set(commands(read(script)))):
                if word in BUILTINS or word in DROPBEAR or word in PACKAGES:
                    continue
                if SYMBOL.get(word, word.upper()) not in symbols:
                    unknown.append(word)
            self.assertEqual(unknown, [], "not applets of busybox.config: " + script)

    def test_the_programs_it_needs_are_the_ones_it_calls(self):
        # If a script stops calling one of these, the list of what the image
        # needs is out of date: take the applet out of the configuration.
        for script, words in ((RCS, ("hostname", "ip", "sed", "uname")),
                              (INIT, ("mount", "mkdir", "ln", "grep"))):
            called = set(commands(read(script)))
            for word in words:
                self.assertIn(word, called, script)

    def test_roots_password_is_in_place_before_the_ssh_server_starts(self):
        # The ssh server's flags are onu-run's, which onu-utils tests.
        text = read(RCS)
        self.assertLess(text.index("\nonu-config load "), text.index("onu-svc start ssh "))

    def test_the_programs_are_services_of_procd_in_their_order(self):
        # The log first; netifd once its configuration is written; the servers
        # once the management address is there.
        text = read(RCS)
        order = ["onu-svc start syslogd -- /sbin/syslogd -n ", "onu-svc start klogd -- ",
                 'onu-mgmt start "$MGMT_VLAN"', "onu-svc start netifd -- /usr/sbin/netifd ",
                 'onu-net wait mgmt "$MGMT_ADDRESS" 10',
                 "onu-svc start ssh -- /usr/lib/onu/onu-run ssh",
                 "onu-svc start trials -- /usr/lib/onu/onu-change watch",
                 "onu-svc start act -- /usr/lib/onu/onu-act",
                 "onu-svc start pond -- /usr/lib/onu/onu-run pond",
                 "onu-svc start ntp -l -- /usr/lib/onu/onu-run ntp"]
        places = [text.index(what) for what in order]
        self.assertEqual(places, sorted(places))

    def test_the_scripts_take_their_tc_priorities_from_the_registry(self):
        # omcid/target/tcprio.h is the one list of the priorities of the filters
        # on sfp and pon. The two scripts that write numbers of it say the same.
        registry = {name: int(value) for name, value in
                    re.findall(r"#define (TC_PRIO_\w+) (\d+)u", read(TCPRIO))}
        mgmt = read(ONU_MGMT)
        first, second = (int(re.search(rf"^PREF_{x}=(\d+)$", mgmt, re.M).group(1)) for x in "AB")
        self.assertEqual((first, second), (registry["TC_PRIO_MGMT"], registry["TC_PRIO_MGMT"] + 1))
        self.assertEqual(registry["TC_PRIO_MGMT_N"], 2)
        apply = read(ONU_APPLY)
        self.assertEqual(int(re.search(r"^VLAN_PREF=(\d+)$", apply, re.M).group(1)),
                         registry["TC_PRIO_VLAN"])
        # The ranges are in the order the stick's rules look at a frame, and the
        # session interfaces' own drops stay below the management VLAN's range.
        self.assertLess(int(re.search(r"^GUARD_PREFS=(\d+)$", apply, re.M).group(1)),
                        registry["TC_PRIO_DISCARD"])
        # The boot script writes none: onu-mgmt is the only writer of the first two.
        self.assertNotIn("onu-nl filter", read(RCS))

    def test_netifd_tells_onu_apply_of_each_interface(self):
        # A PPPoE session's address is handed on when netifd says it is up.
        text = read(RCS)
        self.assertIn("onu-svc start netifd -- /usr/sbin/netifd -c /run/config "
                      "-r /run/resolv.conf.auto -h /usr/sbin/onu-apply || {", text)

    def test_the_log_goes_to_a_server_once_the_management_address_is_up(self):
        # Only when the settings name one, and with a copy kept on the stick.
        text = read(RCS)
        remote = 'syslogd -n -O /run/messages -s 64 -L -R "$SYS_LOG_SERVER"'
        self.assertIn(remote, text)
        self.assertLess(text.index('onu-net wait mgmt "$MGMT_ADDRESS" 10'), text.index(remote))
        self.assertLess(text.index("SYS_LOG_SERVER=$(sed -n 's/^LOG_SERVER=//p' /run/onu-system)"),
                        text.index(remote))
        self.assertIn('if [ -n "$SYS_LOG_SERVER" ]; then', text)
        # The ntpd of the image is a client alone: the stick serves no time.
        self.assertNotIn("FEATURE_NTPD_SERVER", enabled_applets())

    def test_without_procd_the_ssh_server_and_netifd_still_start(self):
        text = read(RCS)
        self.assertIn("\tonu-run ssh &\n", text)
        self.assertIn("\tnetifd -c /run/config -r /run/resolv.conf.auto -h /usr/sbin/onu-apply &\n", text)

    def test_it_ends_with_the_ready_line_that_the_bench_waits_for(self):
        # procd keeps the script's output for its log: the bench reads the
        # console, so the last lines go there.
        lines = read(RCS).rstrip().splitlines()
        self.assertEqual(lines[-1], "} >/dev/console")
        self.assertEqual(lines[-2], '\techo "open-dfp-34x-2c: ready"')

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

    def test_a_bench_can_name_the_versions_the_olt_reads(self):
        text = read(RCS)
        start, end = text.index("onu-config load"), text.index("umask 022")
        for key, name in (("sw_version", "OMCI_SW_VERSION"),
                          ("pack_version", "OMCI_PACK_VERSION")):
            with self.subTest(key=key):
                line = f'echo "{key}=${name}" >>/run/onu-identity'
                self.assertIn(line, text)
                self.assertTrue(start < text.index(line) < end)


class Reading(unittest.TestCase):
    """The reading of a script that the tests above rest on."""

    def test_operators_substitutions_and_tests_start_commands(self):
        self.assertEqual(commands('a x || b y && c\nd $(e f) | g\n[ -n "$x" ] && h\n'),
                         ["a", "b", "c", "d", "e", "g", "h"])

    def test_assignments_and_case_patterns_are_not_commands(self):
        script = ("x=1\ncase $x in\n*[!0-9]*) ;;\n"
                  "*) [ ${#x} -eq 2 ] && x=0x$x ;;\nesac\ny z\n")
        self.assertEqual(commands(script), ["case", "esac", "y"])


class FirstProgram(unittest.TestCase):
    """/sbin/init: the file systems, then procd as process 1."""

    def test_it_is_executable_and_starts_with_its_shell(self):
        self.assertTrue(os.stat(INIT).st_mode & stat.S_IXUSR)
        self.assertEqual(read(INIT).splitlines()[0], "#!/bin/sh")

    def test_it_parses_in_every_posix_shell_there_is(self):
        for shell in SHELLS:
            with self.subTest(shell=shell):
                done = subprocess.run([shell, "-n", INIT], capture_output=True, text=True)
                self.assertEqual(done.returncode, 0, done.stderr)

    def test_it_mounts_what_the_kernel_leaves_to_it(self):
        text = read(INIT)
        for what in ("-t proc proc /proc", "-t sysfs sysfs /sys", "devtmpfs", "/dev/pts",
                     "tmpfs", "/tmp", "/run", "/var", "ln -s ../run /var/run"):
            self.assertIn(what, text)

    def test_procd_takes_its_place_once_the_file_systems_are_there(self):
        lines = read(INIT).rstrip().splitlines()
        self.assertEqual(lines[-1], "exec /sbin/procd")
        self.assertNotIn("mount", read(RCS).replace("mounted", ""))

    def test_procd_finds_ubusd_and_the_boot_script_where_it_looks(self):
        # procd starts /sbin/ubusd, and runs /etc/rc.d/S* at sysinit.
        self.assertEqual(os.readlink(os.path.join(OVERLAY, "sbin", "ubusd")), "../usr/sbin/ubusd")
        self.assertEqual(os.readlink(os.path.join(OVERLAY, "etc", "rc.d", "S10boot")),
                         "../init.d/rcS")


    def test_the_library_procd_preloads_is_where_lib_trim_leaves_it(self):
        # procd preloads /lib/libsetlbf.so into a service whose output it logs.
        # No program needs it, so in lib or usr/lib lib-trim would remove it:
        # it lies in a directory of its own, with a link where procd looks.
        recipe = read(os.path.join(os.path.dirname(BOARD), "..", "package", "procd", "procd.mk"))
        self.assertIn("PROCD_MAKE_OPTS = procd setlbf\n", recipe)
        self.assertIn("$(@D)/libsetlbf.so $(TARGET_DIR)/usr/lib/procd/libsetlbf.so", recipe)
        self.assertIn("ln -sf ../usr/lib/procd/libsetlbf.so $(TARGET_DIR)/lib/libsetlbf.so",
                      recipe)
        trim = read(os.path.join(os.path.dirname(BOARD), "..", "..", "tools", "lib-trim",
                                 "lib-trim"))
        self.assertIn('SEARCH = ("lib", "usr/lib")', trim)


class Names(unittest.TestCase):
    def test_a_person_sees_only_the_programs_for_a_person(self):
        # ADR 0015: the helpers only programs run are in /usr/lib/onu.
        recipe = read(os.path.join(os.path.dirname(BOARD), "..", "package", "onu-utils",
                                   "onu-utils.mk"))
        installed = dict(re.findall(r"\$\(@D\)/(?:build|scripts)/(\S+) \$\(TARGET_DIR\)(\S+)",
                                    recipe))
        for helper in ("ponid", "onu-mgmt", "onu-net", "onu-run", "onu-lane", "onu-trial",
                       "onu-config.sh"):
            self.assertEqual(installed[helper], "/usr/lib/onu/" + helper)
        for program in ("onu-config", "onu-apply", "onu-update", "onu-svc", "pon"):
            self.assertEqual(installed[program], "/usr/sbin/" + program)
        self.assertIn(":/usr/lib/onu\n", read(RCS))


class Watchdog(unittest.TestCase):
    def test_procd_alone_feeds_it(self):
        # procd sets the driver's timeout to 30 s and feeds it every 5 s, from
        # its start; a second feeder would keep a hung procd alive.
        self.assertNotIn("WATCHDOG", enabled_applets())
        for script in (RCS, INIT, INITTAB):
            self.assertNotIn("watchdog", commands(read(script)))


class ServersOnTheHostSide(unittest.TestCase):
    """The stick has no packet filter: its servers listen on the host side alone."""

    def test_there_is_no_packet_filter(self):
        self.assertNotIn("nft", commands(read(RCS)))
        self.assertFalse(os.path.exists(os.path.join(OVERLAY, "etc", "nftables.conf")))

    def test_the_two_addresses_are_those_of_the_host_side(self):
        text = read(RCS)
        self.assertIn("\ndata=${DATA_ADDRESS%/*}\n", text)
        self.assertIn("\nmgmt=${MGMT_ADDRESS%/*}\n", text)

    def test_the_servers_listen_on_the_pair_the_script_records(self):
        # onu-run reads the addresses from /run/onu-mgmt, which onu-mgmt start
        # writes with the management address and the host link's.
        self.assertIn('onu-mgmt start "$MGMT_VLAN" "$MGMT_ADDRESS" "$DATA_ADDRESS"', read(RCS))

    def test_the_host_link_may_have_no_address(self):
        text = read(RCS)
        self.assertIn('[ -z "$DATA_ADDRESS" ] || onu-net wait sfp "$DATA_ADDRESS" 10', text)

    def test_the_trial_is_watched_from_the_first_step(self):
        # onu-trial starts before anything that could hang, and finds the
        # panel's address itself.
        text = read(RCS)
        trial = text.index("onu-trial >/run/onu-trial.log 2>&1 &")
        self.assertLess(trial, text.index("\nonu-config load"))
        self.assertLess(trial, text.index("\nonu-mgmt start"))

    def test_the_addresses_are_there_before_a_server_listens(self):
        text = read(RCS)
        start = text.index("onu-svc start ssh ")
        self.assertLess(text.index('onu-net wait sfp "$DATA_ADDRESS" 10'), start)
        self.assertLess(text.index('onu-net wait mgmt "$MGMT_ADDRESS" 10'), start)

    def test_netifd_alone_gives_the_addresses(self):
        # netifd takes away from a device it holds what it did not put
        # there (ADR 0014): onu-mgmt gives it both addresses, and the script
        # has no address of its own to set, nor a fallback of its own.
        text = read(RCS)
        self.assertNotIn("ip address add", text)
        self.assertNotIn("ip link add", text)


class QuietTowardsThePon(unittest.TestCase):
    """Nothing of the stick's own leaves by the PON, and nothing from it is answered."""

    def test_the_pon_side_has_no_ipv6_before_the_pon_comes_up(self):
        text = read(RCS)
        for conf in ("default", "cpu", "pon"):
            line = f"echo 1 >/proc/sys/net/ipv6/conf/{conf}/disable_ipv6"
            with self.subTest(conf=conf):
                self.assertIn(line, text)
                # "default" covers the bridge and the GEM interfaces that
                # omcid makes, so it must be in force before omcid starts;
                # "onu-apply identity" is what brings the PON up.
                for later in ("onu-apply identity", "onu-apply start"):
                    self.assertLess(text.index(line), text.index(later), later)

    def test_the_host_side_keeps_ipv6(self):
        # sfp has it from before the default changes; mgmt is given it by
        # netifd, as onu-mgmt asks (test_onu_mgmt.sh).
        text = read(RCS)
        self.assertNotIn("conf/sfp/disable_ipv6", text)
        self.assertNotIn("conf/mgmt/disable_ipv6", text)

    def test_proxy_arp_answers_at_once(self):
        # onu-apply's links, which netifd makes later, take the default.
        text = read(RCS)
        for dev in ("default", "sfp"):
            line = f"echo 0 >/proc/sys/net/ipv4/neigh/{dev}/proxy_delay"
            with self.subTest(dev=dev):
                self.assertIn(line, text)
                self.assertLess(text.index(line), text.index("onu-svc start netifd "))

    def test_arp_answers_only_on_the_interface_of_the_address(self):
        text = read(RCS)
        self.assertIn("echo 1 >/proc/sys/net/ipv4/conf/all/arp_ignore", text)
        self.assertIn("echo 2 >/proc/sys/net/ipv4/conf/all/arp_announce", text)
        self.assertLess(text.index("arp_ignore"), text.index("ip link set sfp up"))


class Inittab(unittest.TestCase):
    # procd's actions (inittab.c).
    ACTIONS = {"sysinit", "shutdown", "askfirst", "askconsole", "respawn", "askconsolelate",
               "respawnlate"}

    def entries(self):
        for line in read(INITTAB).splitlines():
            if line.strip() and not line.startswith("#"):
                yield line

    def test_every_entry_has_the_four_fields_and_a_known_action(self):
        for line in self.entries():
            fields = line.split(":", 3)
            self.assertEqual(len(fields), 4, line)
            self.assertIn(fields[2], self.ACTIONS, line)

    def test_procd_reads_every_entry_whole(self):
        # procd reads 127 characters of a line and 7 words of a command
        # (LINE_LEN and MAX_ARGS of inittab.c); more are dropped silently.
        for line in self.entries():
            self.assertLessEqual(len(line), 127, line)
            self.assertLessEqual(len(line.split(":", 3)[3].split()), 7, line)

    def test_the_boot_script_is_run_at_sysinit(self):
        # procd runs /etc/rc.d/S* with "boot" (FirstProgram links the script).
        self.assertIn("::sysinit:/etc/init.d/rcS S boot", list(self.entries()))

    def test_a_restart_goes_on_past_the_shutdown_scripts(self):
        # procd moves from shutdown to halt when the shutdown entry's scripts
        # are done (rcdone in inittab.c): without the entry it never does.
        self.assertIn("::shutdown:/etc/init.d/rcS K shutdown", list(self.entries()))

    def test_the_programs_are_the_boot_scripts_services_not_its_own(self):
        # Only the console's login is a program of the inittab (BootScript).
        programs = [e for e in self.entries() if ":respawn" in e]
        self.assertEqual(programs, ["console::respawnlate:/usr/lib/onu/onu-run console"])

    def test_the_console_asks_for_root_and_its_password_again_after_each_session(self):
        entries = list(self.entries())
        # onu-run console runs getty, or on a bench that asks for it login -f.
        self.assertIn("console::respawnlate:/usr/lib/onu/onu-run console", entries)
        self.assertFalse([e for e in entries if e.endswith(":-/bin/sh")],
                         "a shell without a login on the console")

    def test_the_boot_script_it_names_is_in_the_overlay(self):
        self.assertTrue(os.path.exists(RCS))

    def test_the_applets_it_runs_are_built_and_init_is_procd(self):
        symbols = enabled_applets()
        for symbol in ("REBOOT", "HALT", "POWEROFF", "ASH", "SH_IS_ASH", "GETTY", "LOGIN",
                       "FEATURE_SHADOWPASSWDS", "SYSLOGD", "KLOGD", "FEATURE_REMOTE_LOG",
                       "NTPD"):
            self.assertIn(symbol, symbols)
        # procd is process 1; BusyBox's reboot, halt and poweroff signal it.
        self.assertNotIn("INIT", symbols)

    def test_login_checks_passwords_with_the_c_librarys_crypt(self):
        # The ssh server and the panel check root's password with crypt() of the
        # C library; login must take the same hash. BusyBox's own crypt and
        # shadow reader would be another implementation to keep in step.
        symbols = enabled_applets()
        for symbol in ("USE_BB_CRYPT", "USE_BB_SHADOW", "USE_BB_PWD_GRP", "PASSWD",
                       "FEATURE_SECURETTY"):
            self.assertNotIn(symbol, symbols)
        # onu-config hashes a new password with cryptpw.
        self.assertIn("CRYPTPW", symbols)


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
