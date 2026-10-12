#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""test_acl.py ONU_UTILS: ubusd's list for pond (acl.json) and onu-act's table
(ONU_UTILS/src/act.c) agree. A method of the table that pond's sources name
must be in the list, or ubusd refuses pond the call and the page shows nothing
for it; a method of the list must be in the table, or it is a typo that
allows nothing. Run from pond's directory.
"""

import glob
import json
import re
import sys


def main():
    onu_utils = sys.argv[1]
    table = set(re.findall(r'\.name = "([a-z_]+)"', open(onu_utils + "/src/act.c").read()))
    acl = json.load(open("acl.json"))["access"]["onu-act"]["methods"]
    listed = set(acl)
    sources = "".join(open(f).read() for f in glob.glob("src/*.c"))
    named = {m for m in table if re.search(r'"%s"' % m, sources)}
    failures = 0
    checks = 0

    def check(ok, what):
        nonlocal failures, checks
        checks += 1
        if not ok:
            failures += 1
            print("  FAIL:", what)

    check(len(acl) == len(listed), "no method is listed twice")
    check(not listed - table, "listed but not in the table: %s" % sorted(listed - table))
    check(not named - listed, "named by pond but not listed: %s" % sorted(named - listed))
    check("settings_state" in named, "pond asks for the state of the settings")
    print("test_acl: %d checks, %d failed" % (checks, failures))
    sys.exit(1 if failures else 0)


main()
