#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

set -u
# Tests of coverage_gate.py: made-up reports that it must pass or fail.
here=$(cd "$(dirname "$0")" && pwd)
gate=$here/coverage_gate.py
tmp=$(mktemp -d "${TMPDIR:-/tmp}/gate-test.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT INT TERM

checks=0
failures=0

expect() {
    checks=$((checks + 1))
    if [ "$1" != "$2" ]; then
        failures=$((failures + 1))
        printf '  FAIL %s: status %s, expected %s\n' "$3" "$1" "$2"
    fi
}

has() {
    checks=$((checks + 1))
    if ! grep -F -q -- "$2" "$1"; then
        failures=$((failures + 1))
        printf '  FAIL %s: no "%s" in the output:\n' "$3" "$2"
        sed 's/^/        | /' "$1"
    fi
}

hasre() {
    checks=$((checks + 1))
    if ! grep -E -q -- "$2" "$1"; then
        failures=$((failures + 1))
        printf '  FAIL %s: nothing matches "%s" in the output:\n' "$3" "$2"
        sed 's/^/        | /' "$1"
    fi
}

lacks() {
    checks=$((checks + 1))
    if grep -F -q -- "$2" "$1"; then
        failures=$((failures + 1))
        printf '  FAIL %s: "%s" is in the output\n' "$3" "$2"
    fi
}

report() {
    out=$1; shift
    python3 - "$out" "$@" <<'PY'
import json, sys
out, specs = sys.argv[1], sys.argv[2:]
files = []
for spec in specs:
    name, _, rest = spec.partition(":")
    lines = []
    for item in filter(None, rest.split(",")):
        num, _, count = item.partition("=")
        count, _, branches = count.partition("/")
        lines.append({"line_number": int(num), "count": int(count),
                      "branches": [{"count": int(b)} for b in filter(None, branches.split("+"))]})
    files.append({"file": name, "lines": lines, "functions": []})
json.dump({"files": files, "gcovr/format_version": "0.6"}, open(out, "w"))
PY
}

tree() {
    t=$1; shift
    rm -rf "$t"
    for f in "$@"; do
        mkdir -p "$t/$(dirname "$f")"
        printf 'int x;\nint y;\nint z;\n' > "$t/$f"
    done
}

run() { # tree report out: the gate over a report; the status is in $rc
    python3 "$gate" --root "$1" --report "$2" > "$3" 2>&1
    rc=$?
}

root=$tmp/root

tree "$root" src/a.c src/sub/b.c
report "$tmp/r" "src/a.c:1=3,2=1,3=7" "src/sub/b.c:1=1,2=1"
run "$root" "$tmp/r" "$tmp/o"
expect "$rc" 0 "all lines ran"
has "$tmp/o" "coverage gate: OK" "all lines ran"
hasre "$tmp/o" '^TOTAL +5/5 +100\.0%' "all lines ran: the total"
has "$tmp/o" "lines that never ran: 0 in 0 file(s)" "all lines ran"

report "$tmp/r" "src/a.c:1=3,2=0,3=7" "src/sub/b.c:1=1,2=1"
run "$root" "$tmp/r" "$tmp/o"
expect "$rc" 1 "one line never ran"
has "$tmp/o" "coverage gate: FAILED" "one line never ran"
has "$tmp/o" "src/a.c: 1 line(s): 2" "one line never ran"
lacks "$tmp/o" "src/sub/b.c: " "one line never ran: the covered file is not blamed"
has "$tmp/o" "lines that never ran: 1 in 1 file(s)" "one line never ran: the count"

report "$tmp/r" "src/a.c:1=0,2=0,3=0,10=0,12=0,13=4" "src/sub/b.c:7=0"
run "$root" "$tmp/r" "$tmp/o"
expect "$rc" 1 "ranges"
has "$tmp/o" "src/a.c: 5 line(s): 1-3, 10, 12" "ranges"
has "$tmp/o" "src/sub/b.c: 1 line(s): 7" "ranges: the second file"
has "$tmp/o" "lines that never ran: 6 in 2 file(s)" "ranges: the count"

report "$tmp/r" "src/a.c:1=1/1+0+0,2=5/0+0,3=2" "src/sub/b.c:1=1/3+0"
run "$root" "$tmp/r" "$tmp/o"
expect "$rc" 0 "branches are reported and not gated"
hasre "$tmp/o" '^src/a\.c +3/3 +100\.0% +1/5 +20\.0%' "branches: taken and total of the file, reported"
hasre "$tmp/o" '^TOTAL +4/4 +100\.0% +2/7 +28\.6%' "branches: the total"

report "$tmp/r" "src/a.c:1=0/4+4,2=1,3=1" "src/sub/b.c:1=1,2=1"
run "$root" "$tmp/r" "$tmp/o"
expect "$rc" 1 "a line with taken branches that never ran"

report "$tmp/r" "./src/a.c:1=1,2=1,3=1" "src/sub/b.c:1=1,2=1" "tests/t.c:1=0,2=0"
run "$root" "$tmp/r" "$tmp/o"
expect "$rc" 0 "./src/ prefix, tests/ ignored"
lacks "$tmp/o" "tests/t.c" "tests/ is not listed"

tree "$root" src/a.c src/h.h
report "$tmp/r" "src/a.c:1=1,2=1,3=1" "src/h.h:2=0"
run "$root" "$tmp/r" "$tmp/o"
expect "$rc" 1 "a header with a line that never ran"
has "$tmp/o" "src/h.h: 1 line(s): 2" "a header with a line that never ran"

tree "$root" src/a.c src/sub/b.c src/c.c
report "$tmp/r" "src/a.c:1=1,2=1,3=1" "src/sub/b.c:1=1"
run "$root" "$tmp/r" "$tmp/o"
expect "$rc" 1 "a file that was not measured"
has "$tmp/o" "files under src/ that were not measured: 1" "a file that was not measured"
has "$tmp/o" "  src/c.c" "a file that was not measured: named"

tree "$root" src/a.c src/sub/b.c
printf 'int x;\nint y; /* GCOVR_EXCL_LINE */\nint z;\n' > "$root/src/a.c"
printf 'int x;\n// LCOV_EXCL_START\nint z;\n' > "$root/src/sub/b.c"
report "$tmp/r" "src/a.c:1=1,2=1,3=1" "src/sub/b.c:1=1,2=1,3=1"
run "$root" "$tmp/r" "$tmp/o"
expect "$rc" 1 "exclusion markers"
has "$tmp/o" "exclusion markers in the sources: 2" "exclusion markers"
has "$tmp/o" "src/a.c:2" "exclusion markers: the first"
has "$tmp/o" "src/sub/b.c:2" "exclusion markers: the second"

tree "$root" src/a.c
printf '{"files": []}' > "$tmp/r"
run "$root" "$tmp/r" "$tmp/o"
expect "$rc" 2 "an empty report"
has "$tmp/o" "nothing measured" "an empty report"
lacks "$tmp/o" "coverage gate: OK" "an empty report is not a pass"

report "$tmp/r" "tests/t.c:1=1,2=1"
run "$root" "$tmp/r" "$tmp/o"
expect "$rc" 2 "a report with no file under src/"

report "$tmp/r" "src/a.c:"
run "$root" "$tmp/r" "$tmp/o"
expect "$rc" 2 "a file with no lines"

printf 'this is not json' > "$tmp/r"
run "$root" "$tmp/r" "$tmp/o"
expect "$rc" 2 "a report that is not JSON"
has "$tmp/o" "nothing measured" "a report that is not JSON"
run "$root" "$tmp/does-not-exist" "$tmp/o"
expect "$rc" 2 "a report that is not there"

mkdir -p "$tmp/build/obj"
: > "$tmp/build/obj/a.gcda"
: > "$tmp/build/obj/a.gcno"
touch -t 202001010000 "$tmp/build/obj/a.gcda"
python3 "$gate" --root "$root" --build "$tmp/build" > "$tmp/o" 2>&1
rc=$?
expect "$rc" 2 "stale counters"
has "$tmp/o" "counters older than the notes of its objects" "stale counters: said so"
has "$tmp/o" "nothing measured" "stale counters"

rm -rf "$tmp/empty"; mkdir -p "$tmp/empty"
python3 "$gate" --root "$root" --build "$tmp/empty" > "$tmp/o" 2>&1
rc=$?
expect "$rc" 2 "a build with no counters"
has "$tmp/o" "nothing measured" "a build with no counters"

python3 "$gate" --root "$root" > "$tmp/o" 2>&1
rc=$?
expect "$rc" 2 "neither --build nor --report"

if command -v gcc > /dev/null 2>&1 && command -v gcovr > /dev/null 2>&1; then
    real=$tmp/real
    rm -rf "$real"; mkdir -p "$real/src" "$real/build"
    printf 'int pick(int x)\n{\n    if (x)\n        return 1;\n    return 2;\n}\nint main(void)\n{\n    return pick(1) - 1;\n}\n' \
        > "$real/src/p.c"
    (cd "$real" && gcc --coverage -O0 -o build/p src/p.c && ./build/p)
    python3 "$gate" --root "$real" --build "$real/build" > "$tmp/o" 2>&1
    rc=$?
    expect "$rc" 1 "a real program with a line that never ran"
    has "$tmp/o" "src/p.c: 1 line(s): 5" "a real program: the line that never ran"
    hasre "$tmp/o" '^src/p\.c +5/6 ' "a real program: five of six lines"

    printf 'int pick(int x)\n{\n    if (x)\n        return 1;\n    return 2; /* GCOVR_EXCL_LINE */\n}\nint main(void)\n{\n    return pick(1) - 1;\n}\n' \
        > "$real/src/p.c"
    rm -rf "$real/build"; mkdir -p "$real/build"
    (cd "$real" && gcc --coverage -O0 -o build/p src/p.c && ./build/p)
    python3 "$gate" --root "$real" --build "$real/build" > "$tmp/o" 2>&1
    rc=$?
    expect "$rc" 1 "a real program with a marker"
    has "$tmp/o" "src/p.c: 1 line(s): 5" "a marker does not hide the line from the gate"
    has "$tmp/o" "src/p.c:5" "a marker is found"

    printf 'int pick(int x)\n{\n    if (x)\n        return 1;\n    return 2;\n}\nint main(void)\n{\n    return pick(1) + pick(0) - 3;\n}\n' \
        > "$real/src/p.c"
    rm -rf "$real/build"; mkdir -p "$real/build"
    (cd "$real" && gcc --coverage -O0 -o build/p src/p.c && ./build/p)
    python3 "$gate" --root "$real" --build "$real/build" > "$tmp/o" 2>&1
    rc=$?
    expect "$rc" 0 "a real program with every line run"
    hasre "$tmp/o" '^src/p\.c +6/6 ' "a real program: six of six lines"
else
    echo "  SKIP the case with a real program: gcc and gcovr are both needed and one is missing"
fi

printf 'check_coverage_gate: %d distinct assertions, %d executions, %d failures\n' "$checks" "$checks" "$failures"
[ "$failures" -eq 0 ]
