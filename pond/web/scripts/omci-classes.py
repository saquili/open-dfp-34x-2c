#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""omci-classes.py: write src/lib/omci-classes.ts, the managed entity classes
omcid serves, from its table of G.988 (omcid/data/g988-me-min.tsv): their
names, and of each attribute its name, its size, and whether it is a table.
The classes pond reads (src/prov_omci.c) come first, in its order; the panel
reads the others' frames in the conversation with the OLT. Run from
pond/web."""

import re
import sys

TSV = "../../omcid/data/g988-me-min.tsv"
OUT = "src/lib/omci-classes.ts"


def classes_pond_reads():
    text = open("../src/prov_omci.c").read()
    table = text[text.index("g_classes[] = {"):]
    table = table[: table.index("};")]
    return [int(n) for n in re.findall(r"^\s*(\d+),", table, re.M)]


def main():
    names, attrs = {}, {}
    for line in open(TSV):
        if line.startswith("#class\t"):
            f = line.rstrip("\n").split("\t")
            names[int(f[1])] = f[2]
            continue
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("\t")
        cls = int(f[0])
        names[cls] = f[1]
        attrs.setdefault(cls, []).append((int(f[2]), f[3], int(f[4]), f[7] == "yes", "C" in f[5]))
    read = classes_pond_reads()
    wanted = read + sorted(c for c in names if c not in read)
    out = [
        # The tags are split so that REUSE does not read them as this script's.
        "// SPDX-" "License-Identifier: GPL-2.0-only",
        "// SPDX-" "FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>",
        "",
        "/*",
        " * The managed entity classes omcid serves, with their attributes as ITU-T",
        " * G.988 names them, the size of each in bytes (of a row, for a table), the",
        " * attributes that are tables and those a Create gives: made from",
        " * omcid/data/g988-me-min.tsv by scripts/omci-classes.py. Do not edit by hand.",
        " */",
        "",
        "export interface OmciClass {",
        "  name: string;",
        "  attrs: Record<number, string>;",
        "  /** Attribute n's size at n - 1. */",
        "  sizes: number[];",
        "  tables: number[];",
        "  /** The attributes a Create gives, in its order. */",
        "  create: number[];",
        "}",
        "",
        "export const CLASSES: Record<number, OmciClass> = {",
    ]
    for cls in wanted:
        if cls not in names:
            sys.exit(f"class {cls} is not in {TSV}")
        rows = sorted(attrs.get(cls, []))
        if [r[0] for r in rows] != list(range(1, len(rows) + 1)):
            sys.exit(f"class {cls}: the attributes are not numbered 1 to {len(rows)}")
        out += [f"  {cls}: {{", f'    name: "{names[cls]}",', "    attrs: {"]
        out += [f'      {n}: "{a}",' for n, a, _, _, _ in rows]
        out += ["    },"]
        out += [f"    sizes: [{', '.join(str(z) for _, _, z, _, _ in rows)}],"]
        out += [f"    tables: [{', '.join(str(n) for n, _, _, t, _ in rows if t)}],"]
        out += [f"    create: [{', '.join(str(n) for n, _, _, _, c in rows if c)}],"]
        out += ["  },"]
    out.append("};")
    open(OUT, "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
