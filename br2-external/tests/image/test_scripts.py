# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of the shell scripts of the image against what the image holds.

The scripts run on the board under the BusyBox of the image. The tests of the
scripts in onu-utils run on a build machine with a GNU userland, so they pass
when a script calls an applet that the image does not have (``chgrp`` once did).
Here every command that a script runs must be a function of that script, a
builtin of the image's ash, a program that a package installs, or an applet that
busybox.config turns on. imagescripts.py reads the scripts; check-applets asks
the built tree the same question, and the usage of each applet for the options.
"""

import os
import re
import stat
import unittest

import imagescripts as scripts
from imagescripts import BOARD, OVERLAY, REPO, read


def kind(name, symbols, programs):
    """What the image holds under a name: "builtin", "program", "applet", or None."""
    if name in scripts.ASH_BUILTINS or scripts.ASH_GATED.get(name) in symbols:
        return "builtin"
    if name in programs:
        return "program"
    if scripts.applet_symbol(name) in symbols:
        return "applet"
    return None


def programs_of_the_image():
    defconfig = read(os.path.join(os.path.dirname(BOARD), "..", "configs", "odi_dfp_34x_2c3_defconfig"))
    programs = scripts.installed()
    for symbol, names in scripts.PACKAGE_PROGRAMS.items():
        if re.search(rf"^{symbol}=y$", defconfig, re.M):
            programs.update(names)
    return programs


def missing(label, text, symbols, programs):
    """The commands of a script text that the image does not hold, as "name (line)"."""
    found, unread = scripts.uses(label, text)
    lost = {f"{use.name} ({label}:{use.line})" for use in found if kind(use.name, symbols, programs) is None}
    return sorted(lost | {f"{word} ({label}:{line}, not read)" for line, word in unread})


class TheScripts(unittest.TestCase):
    def test_the_list_is_every_script_the_image_gets(self):
        listed = {os.path.realpath(path) for _, path in scripts.sources()}
        for _, path in scripts.sources():
            self.assertTrue(os.path.isfile(path), path)
        # Every file of onu-utils/scripts is installed by onu-utils.mk and so listed.
        directory = os.path.join(REPO, "onu-utils", "scripts")
        for name in os.listdir(directory):
            self.assertIn(os.path.realpath(os.path.join(directory, name)), listed, name)
        # Every regular file of the overlay that starts as a shell script is listed.
        for root, _, names in os.walk(OVERLAY):
            for name in names:
                path = os.path.join(root, name)
                if os.path.islink(path) or not os.path.isfile(path):
                    continue
                if read(path).startswith("#!/bin/sh") or "profile.d" in path:
                    self.assertIn(os.path.realpath(path), listed, path)

    def test_the_scripts_start_as_shell_scripts_that_run(self):
        for label, path in scripts.sources():
            if label == "board.sh":  # read by /etc/profile, not run
                continue
            self.assertEqual(read(path).splitlines()[0], "#!/bin/sh", path)
            if path.startswith(OVERLAY):  # the others are installed with -m 0755
                self.assertTrue(os.stat(path).st_mode & stat.S_IXUSR, path)

    def test_they_run_only_what_the_image_holds(self):
        symbols, programs = scripts.enabled(), programs_of_the_image()
        for label, path in scripts.sources():
            with self.subTest(script=label):
                self.assertEqual(missing(label, read(path), symbols, programs), [],
                                 "not in the image: a function, a builtin of ash, a program of a "
                                 "package or an applet of busybox.config is needed")

    def test_an_applet_that_the_image_lacks_is_found(self):
        # The case that this test is for: chgrp passes on a GNU machine.
        symbols, programs = {"CHOWN", "ASH_ECHO"}, {"onu-nl"}
        text = "chgrp omcid /dev/x\nchown :omcid /dev/x && echo ok | onu-nl\nf() { :; }\nf\n"
        self.assertEqual(missing("t", text, symbols, programs), ["chgrp (t:1)"])
        self.assertEqual(missing("t", "echo a\n", {"CHOWN"}, programs), ["echo (t:1)"])

    def test_a_word_that_cannot_be_read_is_not_passed_over(self):
        self.assertEqual(missing("t", '"$X" a\n', set(), set()), ['"$X" (t:1, not read)'])

    def test_the_programs_a_launcher_starts_are_held_too(self):
        text = ("onu-svc start a -- /sbin/syslogd -n\nsvc_start b -- udhcpd -f x\nsvc_start() { :; }\n"
                'exec "$DROP" -c net_raw omcid ntpd -n\n')
        found, unread = scripts.uses("onu-run", text)
        self.assertEqual(unread, [])
        self.assertEqual([(use.name, use.options) for use in found if use.name not in scripts.ASH_BUILTINS],
                         [("onu-svc", []), ("syslogd", ["-n"]), ("udhcpd", ["-f"]),
                          ("onu-drop", ["-c"]), ("ntpd", ["-n"])])

    def test_a_package_program_needs_its_package_in_the_defconfig(self):
        defconfig = read(os.path.join(REPO, "br2-external", "configs", "odi_dfp_34x_2c3_defconfig"))
        for symbol in scripts.PACKAGE_PROGRAMS:
            self.assertRegex(defconfig, rf"(?m)^{symbol}=y$")

    def test_what_the_packages_install_is_read_from_their_mk_files(self):
        names = scripts.installed()
        for name in ("onu-apply", "onu-run", "omcid", "pond", "procd", "pppoed", "ponid"):
            self.assertIn(name, names)

    def test_every_dynamic_word_is_in_its_script_and_means_what_it_says(self):
        by_label = dict(scripts.sources())
        for (label, word), names in scripts.DYNAMIC.items():
            text = read(by_label[label])
            commands, _ = scripts.shellwords.commands(text)
            self.assertIn(word, [c.name for c in commands if c.dynamic], (label, word))
            for name in names:
                if label == "onu-run":
                    self.assertIn(f"DROP=$LIB/{name}\n", text)
                else:
                    # verbs(): the program that made a change, the first word of its line.
                    self.assertRegex(text, rf'(?m)^\t[a-z]+\) echo "{name} ')

    def test_the_applets_the_scripts_need_are_in_the_configuration(self):
        # The names that the boot script and the helpers rely on stay in
        # busybox.config as long as a script calls them: this is the list that
        # tells which to keep.
        symbols = scripts.enabled()
        called = {use.name for label, path in scripts.sources() for use in scripts.uses(label, read(path))[0]}
        for name in ("hostname", "sed", "uname", "mount", "mkdir", "ln", "grep", "chown", "chmod",
                     "syslogd", "klogd", "ntpd", "udhcpd", "getty", "login", "cryptpw"):
            self.assertIn(name, called)
            self.assertIn(scripts.applet_symbol(name), symbols, name)


if __name__ == "__main__":
    unittest.main()
