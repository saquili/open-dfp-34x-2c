#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Fail unless the tests ran every line of src/.

Reads the counters of a coverage build (through gcovr) or a gcovr JSON
report, prints the line and branch coverage of each file, and fails when a
line never ran, when a source file was not measured at all, or when a
source holds a marker that would hide lines from the count. Branches are
reported and not judged.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

MARKERS = re.compile(r"(GCOVR|LCOV|GCOV)_EXCL")

def collapse(numbers):
    """Write line numbers as ranges: 3, 7-9."""
    out = []
    start = prev = None
    for n in sorted(numbers):
        if start is None:
            start = prev = n
        elif n == prev + 1:
            prev = n
        else:
            out.append((start, prev))
            start = prev = n
    if start is not None:
        out.append((start, prev))
    return ", ".join(str(a) if a == b else "%d-%d" % (a, b) for a, b in out)

def stale_counters(build):
    """True when counters older than the objects lie in the build."""
    gcda = [os.path.getmtime(p) for p in glob.glob(build + "/**/*.gcda", recursive=True)]
    gcno = [os.path.getmtime(p) for p in glob.glob(build + "/**/*.gcno", recursive=True)]
    return bool(gcda) and bool(gcno) and min(gcda) < max(gcno)

def run_gcovr(root, build):
    """Run gcovr on the build: the report, or a string that says why there is none."""
    try:
        done = subprocess.run(
            ["gcovr", "--root", root, "--filter", "src/", "--no-markers", "--json", "-o", "-", build],
            capture_output=True, text=True, check=True, cwd=root)
    except FileNotFoundError:
        return "gcovr is not installed"
    except subprocess.CalledProcessError as err:
        return "gcovr failed: " + err.stderr.strip()
    try:
        return json.loads(done.stdout)
    except ValueError as err:
        return "gcovr wrote no JSON: %s" % err

def read_report(path):
    try:
        with open(path) as fh:
            return json.load(fh)
    except (OSError, ValueError) as err:
        return "cannot read %s: %s" % (path, err)

def under_src(name):
    name = os.path.normpath(name)
    return name if name.startswith("src" + os.sep) else None

def judge(report, root):
    """Return the exit status and the text of the verdict on a report."""
    out = []
    files = {}
    for entry in report.get("files", []) if isinstance(report, dict) else []:
        name = under_src(entry.get("file", ""))
        if name is not None:
            files[name] = entry.get("lines", [])

    if not any(files.values()):
        return 2, "coverage gate: nothing measured under src/ -- a gate that measured nothing has proved nothing\n"

    on_disk = sorted(
        os.path.relpath(p, root) for p in glob.glob(os.path.join(root, "src", "**", "*.c"), recursive=True))
    unmeasured = [p for p in on_disk if not files.get(p)]
    markers = []
    for p in sorted(glob.glob(os.path.join(root, "src", "**", "*.[ch]"), recursive=True)):
        with open(p, errors="replace") as fh:
            for n, text in enumerate(fh, 1):
                if MARKERS.search(text):
                    markers.append("%s:%d" % (os.path.relpath(p, root), n))

    out.append("%-34s %15s %9s   %15s %9s\n" % ("file", "lines", "", "branches", "(not gated)"))
    tot_l = cov_l = tot_b = cov_b = 0
    never = {}
    for name in sorted(files):
        lines = files[name]
        nl = len(lines)
        cl = sum(1 for ln in lines if ln.get("count", 0) > 0)
        branches = [b for ln in lines for b in ln.get("branches", [])]
        nb = len(branches)
        cb = sum(1 for b in branches if b.get("count", 0) > 0)
        tot_l += nl
        cov_l += cl
        tot_b += nb
        cov_b += cb
        if cl != nl:
            never[name] = [ln["line_number"] for ln in lines if ln.get("count", 0) == 0]
        out.append("%-34s %7d/%-7d %8.1f%%   %7d/%-7d %8.1f%%\n" % (
            name, cl, nl, 100.0 * cl / nl if nl else 100.0, cb, nb, 100.0 * cb / nb if nb else 100.0))
    out.append("%-34s %7d/%-7d %8.1f%%   %7d/%-7d %8.1f%%\n" % (
        "TOTAL", cov_l, tot_l, 100.0 * cov_l / tot_l, cov_b, tot_b, 100.0 * cov_b / tot_b if tot_b else 100.0))

    out.append("\nlines that never ran: %d in %d file(s)\n" % (sum(len(v) for v in never.values()), len(never)))
    for name in sorted(never):
        out.append("  %s: %d line(s): %s\n" % (name, len(never[name]), collapse(never[name])))
    out.append("files under src/ that were not measured: %d\n" % len(unmeasured))
    for p in unmeasured:
        out.append("  %s\n" % p)
    out.append("exclusion markers in the sources: %d\n" % len(markers))
    for m in markers:
        out.append("  %s\n" % m)

    if never or unmeasured or markers:
        out.append("\ncoverage gate: FAILED\n")
        return 1, "".join(out)
    out.append("\ncoverage gate: OK\n")
    return 0, "".join(out)

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", required=True, help="the tree: the directory that holds src/")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--build", help="a coverage build whose tests have run")
    group.add_argument("--report", help="a gcovr JSON report")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    if args.build:
        if stale_counters(args.build):
            sys.stdout.write("coverage gate: %s holds counters older than the notes of its objects: the numbers "
                             "would be a union of several versions of the source; nothing measured\n" % args.build)
            return 2
        report = run_gcovr(root, os.path.abspath(args.build))
    else:
        report = read_report(args.report)
    if isinstance(report, str):
        sys.stdout.write("coverage gate: %s -- nothing measured\n" % report)
        return 2
    status, text = judge(report, root)
    sys.stdout.write(text)
    return status

if __name__ == "__main__":
    sys.exit(main())
