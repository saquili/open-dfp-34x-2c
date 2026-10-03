#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Generate src/me/table.h and src/me/table.c from data/g988-me-min.tsv.

The table lists the managed entity classes the daemon serves: their
attributes, action sets, alarms and the attributes whose changes are
announced. --write writes the two files; --check exits with 1 when they
differ from what the table gives. The table is checked first: attribute
numbers run from 1 in order, every value fits the frame that carries it,
and the action set has what the attributes need.
"""

import argparse
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "data", "g988-me-min.tsv")
OUT_H = os.path.join(ROOT, "src", "me", "table.h")
OUT_C = os.path.join(ROOT, "src", "me", "table.c")

MESSAGE_TYPES = {
    4: "Create", 6: "Delete", 8: "Set", 9: "Get", 11: "Get All Alarms", 12: "Get All Alarms Next",
    13: "MIB Upload", 14: "MIB Upload Next", 15: "MIB Reset", 18: "Test",
    19: "Start Software Download", 20: "Download Section", 21: "End Software Download",
    22: "Activate Software", 23: "Commit Software", 24: "Synchronize Time", 25: "Reboot",
    26: "Get Next", 28: "Get Current Data", 29: "Set Table",
}

MT_CREATE, MT_DELETE, MT_GET, MT_GET_NEXT = 4, 6, 9, 26

CREATE_BYTES = 32
GET_BYTES = 25
MASK_ATTRS = 16

ACCESS = {
    "R": "OMCI_ME_ACC_READ",
    "R,W": "OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE",
    "R,C": "OMCI_ME_ACC_READ | OMCI_ME_ACC_SET_BY_CREATE",
    "R,W,C": "OMCI_ME_ACC_READ | OMCI_ME_ACC_WRITE | OMCI_ME_ACC_SET_BY_CREATE",
}

# REUSE-IgnoreStart
SPDX_H = ("/* SPDX-License-Identifier: GPL-2.0-only */\n"
          "/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */\n")
SPDX_C = ("// SPDX-License-Identifier: GPL-2.0-only\n"
          "// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>\n")
# REUSE-IgnoreEnd

class TableError(Exception):
    pass

def c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'

def numbers(text, what, where):
    """A list of numbers separated by commas, or "-" for none."""
    if text == "-":
        return []
    try:
        return [int(n) for n in text.split(",")]
    except ValueError:
        raise TableError("%s: %s is not a list of numbers: %r" % (where, what, text))

def alarm_numbers(text, where):
    """The numbers of number:name pairs separated by semicolons, or "-" for none."""
    if text == "-":
        return []
    out = []
    for item in text.split(";"):
        num, _, name = item.partition(":")
        if not num.isdigit() or not name:
            raise TableError("%s: alarm %r is not number:name" % (where, item))
        out.append(int(num))
    return out

def read_table(path):
    """Read and check the table; return its classes in order and its SHA-256."""
    with open(path, "rb") as fh:
        raw = fh.read()
    digest = hashlib.sha256(raw).hexdigest()

    classes, attrs = {}, {}
    for n, line in enumerate(raw.decode("utf-8").split("\n"), 1):
        if not line or line.startswith("# "):
            continue
        where = "%s:%d" % (os.path.basename(path), n)
        f = line.split("\t")
        if len(f) != 9:
            raise TableError("%s: %d fields, expected 9" % (where, len(f)))
        if f[0] == "#class":
            cid = int(f[1])
            if cid in classes:
                raise TableError("%s: class %d twice" % (where, cid))
            if f[3] not in ("ONU", "OLT"):
                raise TableError("%s: class %d is created by %r, not ONU or OLT" % (where, cid, f[3]))
            classes[cid] = dict(
                id=cid, name=f[2], creator=f[3], where=where,
                actions=numbers(f[4], "actions", where),
                alarms=alarm_numbers(f[5], where),
                avc=numbers(f[6], "avc", where),
                attrs=[])
        elif f[0].startswith("#"):
            raise TableError("%s: unknown row kind %r" % (where, f[0]))
        else:
            cid, no = int(f[0]), int(f[2])
            if f[5] not in ACCESS:
                raise TableError("%s: access %r" % (where, f[5]))
            if f[6] not in ("mandatory", "optional") or f[7] not in ("yes", "no"):
                raise TableError("%s: mandatory/table flags %r %r" % (where, f[6], f[7]))
            attrs.setdefault(cid, []).append(dict(
                no=no, name=f[3], size=int(f[4]), access=f[5],
                mandatory=f[6] == "mandatory", table=f[7] == "yes", where=where, class_name=f[1]))

    for cid, rows in attrs.items():
        if cid not in classes:
            raise TableError("attributes of class %d, which has no class row" % cid)
        classes[cid]["attrs"] = rows

    for cid in sorted(classes):
        check_class(classes[cid])
    return [classes[cid] for cid in sorted(classes)], digest

def check_class(c):
    where = "%s (class %d)" % (c["where"], c["id"])
    rows = c["attrs"]
    if not rows:
        raise TableError("%s: no attributes" % where)
    if [a["no"] for a in rows] != list(range(1, len(rows) + 1)):
        raise TableError("%s: attribute numbers do not run 1..%d in order" % (where, len(rows)))
    if len(rows) > MASK_ATTRS:
        raise TableError("%s: %d attributes, a 16-bit mask names %d" % (where, len(rows), MASK_ATTRS))
    for a in rows:
        if a["class_name"] != c["name"]:
            raise TableError("%s: attribute %d names the class %r" % (where, a["no"], a["class_name"]))
        if a["size"] < 1:
            raise TableError("%s: attribute %d has size %d" % (where, a["no"], a["size"]))
        if not a["table"] and a["size"] > GET_BYTES:
            raise TableError("%s: attribute %d (%d bytes) can never be read by a Get" %
                             (where, a["no"], a["size"]))
        if "R" not in a["access"].split(","):
            raise TableError("%s: attribute %d is not readable" % (where, a["no"]))
    for mt in c["actions"]:
        if mt not in MESSAGE_TYPES:
            raise TableError("%s: message type %d is not in the baseline set" % (where, mt))
    if len(set(c["actions"])) != len(c["actions"]):
        raise TableError("%s: a message type twice in the action set" % where)
    numbers_ = {a["no"]: a for a in rows}
    for no in c["avc"]:
        if no not in numbers_:
            raise TableError("%s: the announcing attribute %d does not exist" % (where, no))
        if numbers_[no]["table"]:
            raise TableError("%s: attribute %d is a table and cannot announce a change" % (where, no))
    for alarm in c["alarms"]:
        if alarm > 31:
            raise TableError("%s: alarm %d does not fit the alarm mask" % (where, alarm))

    created = [a for a in rows if "C" in a["access"].split(",")]
    if created and MT_CREATE not in c["actions"]:
        raise TableError("%s: set-by-create attributes in a class without Create" % where)
    if sum(a["size"] for a in created) > CREATE_BYTES:
        raise TableError("%s: the Create does not fit one baseline frame" % where)
    if c["creator"] == "OLT" and (MT_CREATE not in c["actions"] or MT_DELETE not in c["actions"]):
        raise TableError("%s: created by the OLT and missing Create or Delete" % where)
    if any(a["table"] for a in rows) and MT_GET_NEXT not in c["actions"]:
        raise TableError("%s: a table attribute and no Get Next" % where)
    if MT_GET not in c["actions"]:
        raise TableError("%s: no Get" % where)

def mask(numbers_):
    out = 0
    for n in numbers_:
        out |= 1 << n
    return out

def banner(digest, classes):
    n_attrs = sum(len(c["attrs"]) for c in classes)
    return (
        " * Made by tools/gen_me_table.py from data/g988-me-min.tsv: change the\n"
        " * table, not this file, then run python3 tools/gen_me_table.py --write.\n"
        " *\n"
        " * The table follows ITU-T G.988 (11/2022). sha256 of the table:\n"
        " * %s\n"
        " * %d classes, %d attributes.\n" % (digest, len(classes), n_attrs))

HEADER = r'''%(spdx)s/*
 * The managed entity classes the daemon serves.
 *
%(banner)s *
 * An attribute is known by its number alone: attribute n of a class is
 * element n - 1 of its array, and bit n of an attribute mask, counted from
 * the most significant bit. The instance identifier comes before attribute
 * 1 and is not an attribute.
 */

#ifndef OMCI_ME_TABLE_H
#define OMCI_ME_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * enum omci_me_creator - Who makes the instances of a class.
 * @OMCI_ME_CREATED_BY_ONU: the ONU, on its own; a Create is refused
 * @OMCI_ME_CREATED_BY_OLT: the OLT, with Create and Delete
 */
enum omci_me_creator {
	OMCI_ME_CREATED_BY_ONU = 0,
	OMCI_ME_CREATED_BY_OLT = 1,
};

/**
 * enum omci_me_access - How the OLT may use an attribute.
 * @OMCI_ME_ACC_READ: read it with Get
 * @OMCI_ME_ACC_WRITE: write it with Set
 * @OMCI_ME_ACC_SET_BY_CREATE: give it in the Create
 */
enum omci_me_access {
	OMCI_ME_ACC_READ = 1u << 0,
	OMCI_ME_ACC_WRITE = 1u << 1,
	OMCI_ME_ACC_SET_BY_CREATE = 1u << 2,
};

/**
 * enum omci_me_attr_flag - What else is true of an attribute.
 * @OMCI_ME_ATTR_MANDATORY: the standard requires it
 * @OMCI_ME_ATTR_TABLE: it is a table: a Get gives its size and Get next
 *	its rows
 * @OMCI_ME_ATTR_AVC: a change the ONU makes to it is announced
 */
enum omci_me_attr_flag {
	OMCI_ME_ATTR_MANDATORY = 1u << 0,
	OMCI_ME_ATTR_TABLE = 1u << 1,
	OMCI_ME_ATTR_AVC = 1u << 2,
};

/**
 * struct omci_me_attr - An attribute of a class.
 * @name: its name in the standard
 * @size: its size in bytes; for a table, the size of a row
 * @access: an &enum omci_me_access mask
 * @flags: an &enum omci_me_attr_flag mask
 */
struct omci_me_attr {
	const char *name;
	uint16_t size;
	uint8_t access;
	uint8_t flags;
};

/**
 * struct omci_me_class - A managed entity class.
 * @actions: bit N set when the class accepts message type N
 * @alarms: bit N set when the class defines alarm N
 * @class_id: its identifier
 * @creator: an &enum omci_me_creator
 * @attr_count: its attributes, 1 to 16
 * @name: its name in the standard
 * @attrs: its attributes, attribute n at @attrs[n - 1]
 */
struct omci_me_class {
	uint32_t actions;
	uint32_t alarms;
	uint16_t class_id;
	uint8_t creator;
	uint8_t attr_count;
	const char *name;
	const struct omci_me_attr *attrs;
};

/* The classes, in ascending order of identifier, and their number. */
extern const struct omci_me_class omci_me_classes[];
extern const size_t omci_me_class_count;

/**
 * omci_me_class_by_id() - Find a class.
 * @class_id: its identifier
 *
 * Return: the class, or NULL when the daemon does not serve it.
 */
const struct omci_me_class *omci_me_class_by_id(uint16_t class_id);

/**
 * omci_me_attr_get() - Find an attribute of a class.
 * @cls: the class, or NULL
 * @attr_no: the attribute, from 1
 *
 * Return: the attribute, or NULL when the class has none with that number.
 */
const struct omci_me_attr *omci_me_attr_get(const struct omci_me_class *cls,
					    uint8_t attr_no);

/**
 * omci_me_attr_is_table() - Whether an attribute is a table.
 * @a: the attribute, or NULL
 *
 * Return: true for a table attribute.
 */
static inline bool omci_me_attr_is_table(const struct omci_me_attr *a)
{
	return a != NULL && (a->flags & OMCI_ME_ATTR_TABLE) != 0u;
}

#endif /* OMCI_ME_TABLE_H */
'''

SOURCE_TOP = r'''%(spdx)s/*
 * The managed entity classes of table.h.
 *
%(banner)s */

#include "me/table.h"
#include "omcid/array_size.h"

'''

SOURCE_BOTTOM = r'''
const size_t omci_me_class_count = ARRAY_SIZE(omci_me_classes);

const struct omci_me_class *omci_me_class_by_id(uint16_t class_id)
{
	for (size_t i = 0; i < omci_me_class_count; i++)
		if (omci_me_classes[i].class_id == class_id)
			return &omci_me_classes[i];
	return NULL;
}

const struct omci_me_attr *omci_me_attr_get(const struct omci_me_class *cls,
					    uint8_t attr_no)
{
	if (cls == NULL || attr_no == 0u || attr_no > cls->attr_count)
		return NULL;
	return &cls->attrs[attr_no - 1u];
}
'''

COLUMNS = 80
TAB = 8

def row_lines(fields, tail):
    """Lay out the fields of an attribute in lines of at most 80 columns."""
    lines = []
    cur = "\t{ " + fields[0]
    for k, field in enumerate(fields[1:], 1):
        last = k == len(fields) - 1
        piece = field + (tail if last else "")
        width = len(cur.expandtabs(TAB)) + 1 + len(piece)
        if width <= COLUMNS:
            cur += " " + field
        else:
            lines.append(cur)
            cur = "\t  " + field
    lines.append(cur + tail)
    return lines

def gen_header(classes, digest):
    return HEADER % dict(spdx=SPDX_H, banner=banner(digest, classes))

def gen_source(classes, digest):
    out = [SOURCE_TOP % dict(spdx=SPDX_C, banner=banner(digest, classes))]
    for c in classes:
        out.append("/* Class %d: %s */\n" % (c["id"], c["name"]))
        out.append("static const struct omci_me_attr attrs_class_%d[] = {\n" % c["id"])
        for a in c["attrs"]:
            flags = []
            if a["mandatory"]:
                flags.append("OMCI_ME_ATTR_MANDATORY")
            if a["table"]:
                flags.append("OMCI_ME_ATTR_TABLE")
            if a["no"] in c["avc"]:
                flags.append("OMCI_ME_ATTR_AVC")
            fields = [c_string(a["name"]) + ",", "%d," % a["size"], ACCESS[a["access"]] + ",", " | ".join(flags) if flags else "0"]
            out.append("\n".join(row_lines(fields, " }, /* %d */" % a["no"])) + "\n")
        out.append("};\n\n")

    out.append("const struct omci_me_class omci_me_classes[] = {\n")
    for c in classes:
        acts = ", ".join(MESSAGE_TYPES[m] for m in c["actions"])
        out.append("\t{\n")
        out.append("\t\t.class_id = %d,\n" % c["id"])
        out.append("\t\t.name = %s,\n" % c_string(c["name"]))
        out.append("\t\t.creator = OMCI_ME_CREATED_BY_%s,\n" % c["creator"])
        out.append("\t\t/* %s */\n" % acts)
        out.append("\t\t.actions = 0x%08xu,\n" % mask(c["actions"]))
        out.append("\t\t.alarms = 0x%08xu,\n" % mask(c["alarms"]))
        out.append("\t\t.attr_count = %d,\n" % len(c["attrs"]))
        out.append("\t\t.attrs = attrs_class_%d,\n" % c["id"])
        out.append("\t},\n")
    out.append("};\n")
    out.append(SOURCE_BOTTOM)
    return "".join(out)

def main():
    ap = argparse.ArgumentParser()
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    args = ap.parse_args()

    try:
        classes, digest = read_table(SRC)
    except (TableError, OSError, ValueError) as exc:
        sys.exit("gen_me_table.py: %s" % exc)

    wanted = {OUT_H: gen_header(classes, digest), OUT_C: gen_source(classes, digest)}
    if args.write:
        for path, text in wanted.items():
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
        print("%d classes, %d attributes" % (len(classes), sum(len(c["attrs"]) for c in classes)))
        return 0

    bad = []
    for path, text in wanted.items():
        try:
            with open(path, encoding="utf-8", newline="") as fh:
                have = fh.read()
        except OSError:
            have = None
        if have != text:
            bad.append(os.path.relpath(path, ROOT))
    if bad:
        for path in bad:
            print("%s differs from what tools/gen_me_table.py writes from data/g988-me-min.tsv" % path,
                  file=sys.stderr)
        return 1
    print("check-gen: src/me/table.h and src/me/table.c are what data/g988-me-min.tsv produces")
    return 0

if __name__ == "__main__":
    sys.exit(main())
