#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""omci-classes.py: write src/lib/omci-classes.ts, the names of the managed
entity classes pond reads (src/prov_omci.c) and of their attributes, from
omcid's table of G.988 (omcid/data/g988-me-min.tsv). Run from pond/web."""

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
    wanted = classes_pond_reads()
    names, attrs = {}, {}
    for line in open(TSV):
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("\t")
        cls = int(f[0])
        if cls in wanted:
            names[cls] = f[1]
            attrs.setdefault(cls, []).append((int(f[2]), f[3]))
    out = [
        "// SPDX-License-Identifier: GPL-2.0-only",
        "// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>",
        "",
        "/*",
        " * The managed entity classes pond reads from omcid, with their attributes as",
        " * ITU-T G.988 names them: made from omcid/data/g988-me-min.tsv by",
        " * scripts/omci-classes.py. Do not edit by hand.",
        " */",
        "",
        "export const CLASSES: Record<number, { name: string; attrs: Record<number, string> }> = {",
    ]
    for cls in wanted:
        if cls not in names:
            sys.exit(f"class {cls} is not in {TSV}")
        out += [f"  {cls}: {{", f'    name: "{names[cls]}",', "    attrs: {"]
        out += [f'      {n}: "{a}",' for n, a in sorted(attrs[cls])]
        out += ["    },", "  },"]
    out.append("};")
    open(OUT, "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
