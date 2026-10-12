#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""test_acl.py REPO: the lists that ubusd holds for the programs that run as
accounts of their own, REPO/*/acl.json, give each the methods of onu-act that
it calls for itself and no others, and name only methods that are in onu-act's
table (src/act.c). pond's has its own test, which reads its sources.
"""

import json
import os
import re
import sys

# The account, its list, and the methods of onu-act it may call: its own
# alone, by which it tells onu-apply of a change (onu-call).
EXPECTED = {
    "dhcp6d": ("dhcp6d/acl.json", {"dhcp6d"}),
}


def main():
    repo = sys.argv[1]
    with open(os.path.join(repo, "onu-utils", "src", "act.c"), encoding="utf-8") as handle:
        table = set(re.findall(r'\.name = "([a-z_0-9]+)"', handle.read()))
    failures = 0
    checks = 0

    def check(ok, what):
        nonlocal failures, checks
        checks += 1
        if not ok:
            failures += 1
            print("  FAIL:", what)

    for account, (path, methods) in sorted(EXPECTED.items()):
        with open(os.path.join(repo, path), encoding="utf-8") as handle:
            acl = json.load(handle)
        listed = set(acl["access"]["onu-act"]["methods"])
        check(acl["user"] == account, "%s is the list of %s" % (path, account))
        check(listed == methods, "%s: onu-act's %s, got %s" % (path, sorted(methods), sorted(listed)))
        check(listed <= table, "%s: not in the table: %s" % (path, sorted(listed - table)))
        check(set(acl["access"]) == {"onu-act"}, "%s: it calls nothing else" % path)
        check(all(isinstance(p, str) and p for p in acl.get("publish", [])),
              "%s: what it publishes is names" % path)
    print("test_acl: %d checks, %d failed" % (checks, failures))
    sys.exit(1 if failures else 0)


main()
