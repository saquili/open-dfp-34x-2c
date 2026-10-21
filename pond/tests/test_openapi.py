#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""test_openapi.py: the API's description (openapi.json) and the daemon's
sources agree. Every route the sources answer (src/pond.c, the table of
src/keys.c) is described, and every route described is answered; every value
of the table (src/table.c) that reads has its /api/get/ID, every one that
changes its /api/set/ID, and no other value is described. Every reference of
the description leads to a schema of its own. The panel's tests check the
mock stick's answers against the schemas (web/src/mocks/openapi.test.ts).
Run from pond's directory.
"""

import json
import re
import sys


def main():
    spec = json.load(open("openapi.json"))
    paths = set(spec["paths"])
    pond = open("src/pond.c").read()
    keys = open("src/keys.c").read()
    table = open("src/table.c").read()

    # The routes of pond.c, but its wildcards, and those of the keys' table.
    routes = set(re.findall(r'mg_str\("(/api/[a-z/_-]+)"\)', pond))
    routes |= set(re.findall(r'\{ "(/api/[a-z/_-]+)", "[a-z_]+", K_[A-Z_]+ \}', keys))
    # A value: its id, then whether it reads and whether it changes.
    values = {}
    for m in re.finditer(r'\{ "([a-z_0-9]+)", "[^"]+",(.*?)\},\n', table, re.S):
        fns = re.findall(r'\b(prov_[a-z_0-9]+|NULL)\b', m.group(2))
        if len(fns) >= 2 and m.group(1) not in values:
            values[m.group(1)] = (fns[0] != "NULL", fns[1] != "NULL")
    described = {p for p in paths if not re.match(r"/api/(get|set)/", p)}
    gets = {p[len("/api/get/"):] for p in paths if p.startswith("/api/get/")}
    sets = {p[len("/api/set/"):] for p in paths if p.startswith("/api/set/")}
    # Two changes of pond.c of their own, not values of the table.
    special = {"mgmt", "changes"}

    failures = 0
    checks = 0

    def check(ok, what):
        nonlocal failures, checks
        checks += 1
        if not ok:
            failures += 1
            print("  FAIL:", what)

    check(len(routes) > 20 and len(values) > 30, "the sources' routes and values are found")
    check(not routes - paths, "answered but not described: %s" % sorted(routes - paths))
    check(not described - routes, "described but not answered: %s" % sorted(described - routes))
    reads = {v for v, (r, _) in values.items() if r}
    writes = {v for v, (_, w) in values.items() if w}
    check(not reads - gets, "values that read without /api/get: %s" % sorted(reads - gets))
    check(not gets - reads, "/api/get of no value that reads: %s" % sorted(gets - reads))
    check(not writes - sets, "values that change without /api/set: %s" % sorted(writes - sets))
    check(not sets - writes - special, "/api/set of no value that changes: %s" % sorted(sets - writes - special))

    schemas = spec["components"]["schemas"]
    refs = set(re.findall(r'"\$ref": "#/components/([a-zA-Z]+)/([A-Za-z0-9]+)"', json.dumps(spec, indent=1)))
    missing = {"%s/%s" % r for r in refs if r[1] not in spec["components"].get(r[0], {})}
    check(not missing, "references to nothing: %s" % sorted(missing))
    used = {n for kind, n in refs if kind == "schemas"}
    check(not set(schemas) - used, "schemas no route uses: %s" % sorted(set(schemas) - used))

    print("test_openapi: %d checks, %d failed" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
