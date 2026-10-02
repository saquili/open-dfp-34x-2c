#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

set -u
# Tests of style_gate.sh: seeded files that it must pass or fail.
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
gate=$here/style_gate.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/style-gate-test.XXXXXX") || exit 2
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

lacks() {
    checks=$((checks + 1))
    if grep -F -q -- "$2" "$1"; then
        failures=$((failures + 1))
        printf '  FAIL %s: "%s" is in the output\n' "$3" "$2"
    fi
}

cat > "$tmp/cf" << 'EOF'
#!/bin/sh
case $1 in --version) echo "stub clang-format version 0"; exit 0 ;; esac
rc=0
for a in "$@"; do
    case $a in -*) continue ;; esac
    if grep -q -e 'STUB-FORMAT-BAD' -e '){return' "$a"; then
        n=$(grep -n -e 'STUB-FORMAT-BAD' -e '){return' "$a" | head -n 1 | cut -d: -f1)
        echo "$a:$n:1: error: code should be clang-formatted [-Wclang-format-violations]"
        rc=1
    fi
done
exit $rc
EOF
cat > "$tmp/cp" << 'EOF'
#!/bin/sh
f=
types=
for a in "$@"; do
    f=$a
    case $a in LONG_LINE) types=$a ;; esac
done
if [ -n "$types" ]; then
    awk -v f="$f" '
        length($0) > 100 && $0 !~ /^\/\// { printf "%s:%d: CHECK:LONG_LINE: line length of %d exceeds 100 columns\n", f, NR, length($0); n++ }
        END { if (n) printf "total: 0 errors, 0 warnings, %d checks, %d lines checked\n", n, NR; exit n ? 1 : 0 }' "$f"
    exit
fi
awk -v f="$f" '
    /STUB:[A-Z_0-9]+/ { match($0, /STUB:[A-Z_0-9]+/); t = substr($0, RSTART + 5, RLENGTH - 5)
                        printf "%s:%d: WARNING:%s: stub finding\n", f, NR, t; n++ }
    /^    /          { printf "%s:%d: WARNING:LEADING_SPACE: please, no spaces at the start of a line\n", f, NR; n++ }
    END             { if (n) printf "total: 0 errors, %d warnings, %d lines checked\n", n, NR; exit n ? 1 : 0 }' "$f"
EOF
printf '#!/bin/sh\nexit 0\n' > "$tmp/silent"
printf '#!/bin/sh\nexit 0\n' > "$tmp/cf-silent"
printf '#!/bin/sh\ncase $1 in --version) echo stub; exit 0 ;; esac\necho "boom" >&2\nexit 7\n' > "$tmp/cf-broken"
printf '#!/bin/sh\nf=\nfor a in "$@"; do f=$a; done\ncase $f in *canary-ll*) echo "$f:1: CHECK:LONG_LINE: x"; exit 1 ;; *canary*) echo "$f:1: WARNING:LEADING_SPACE: x"; exit 1 ;; esac\nexit 1\n' > "$tmp/cp-lies-1"
printf '#!/bin/sh\nf=\nfor a in "$@"; do f=$a; done\ncase $f in *canary-ll*) echo "$f:1: CHECK:LONG_LINE: x"; exit 1 ;; *canary*) echo "$f:1: WARNING:LEADING_SPACE: x"; exit 1 ;; esac\necho "$f:1: WARNING:SPACING: x"\nexit 0\n' > "$tmp/cp-lies-2"
printf '#!/bin/sh\nf=\nfor a in "$@"; do f=$a; done\ncase $f in *canary-ll*) echo "$f:1: CHECK:LONG_LINE: x"; exit 1 ;; *canary*) echo "$f:1: WARNING:LEADING_SPACE: x"; exit 1 ;; esac\necho "perl: broken" >&2\nexit 255\n' > "$tmp/cp-crashes"
printf '#!/bin/sh\nf=\ntypes=\nfor a in "$@"; do f=$a; case $a in LONG_LINE) types=$a ;; esac; done\n[ -n "$types" ] && exit 0\ncase $f in *canary*) echo "$f:1: WARNING:LEADING_SPACE: x"; exit 1 ;; esac\nexit 0\n' > "$tmp/silent-ll"
chmod +x "$tmp/silent-ll" "$tmp/cf" "$tmp/cp" "$tmp/silent" "$tmp/cf-silent" "$tmp/cf-broken" "$tmp/cp-lies-1" "$tmp/cp-lies-2" "$tmp/cp-crashes"

newtree() {
    rm -rf "$1"
    mkdir -p "$1/src"
    printf 'BasedOnStyle: LLVM\n' > "$1/.clang-format"
    printf '# the types that may stay\nBLOCK_COMMENT_STYLE comments are replaced later\n' > "$1/open.txt"
}

run() {
    d=$1; shift
    (cd "$d" && sh "$gate" --clang-format "$tmp/cf" --checkpatch "$tmp/cp" --open open.txt "$@" > "$tmp/o" 2>&1)
}

t=$tmp/t
newtree "$t"
printf '// clean\nint f(void)\n{\n\treturn 0;\n}\n' > "$t/src/clean.c"
printf '// clean too\nint g(void)\n{\n\treturn 1;\n}\n' > "$t/src/clean2.c"

run "$t" -- src/clean.c src/clean2.c
expect $? 0 "a clean tree"
has "$tmp/o" "style gate: OK" "a clean tree says OK"
has "$tmp/o" "style gate: 2 files" "the gate counts the files it looked at"
has "$tmp/o" "stub clang-format version 0" "the gate prints the version of clang-format"
has "$tmp/o" "every file is as it formats it" "a clean tree: clang-format"
has "$tmp/o" "0 finding(s), 0 of them in an open type, 0 not" "a clean tree: checkpatch"
has "$tmp/o" "open type BLOCK_COMMENT_STYLE: 0 finding(s) -- comments are replaced later" "an open type is printed with its reason and its count"

run "$t" --jobs 1 -- src/clean.c src/clean2.c
expect $? 0 "a clean tree, one job"
has "$tmp/o" "style gate: OK" "a clean tree, one job, says OK"

printf '// bad\nint h(void){ return 2; } /* STUB-FORMAT-BAD */\n' > "$t/src/bad_format.c"
run "$t" -- src/clean.c src/bad_format.c
expect $? 1 "a file clang-format would change"
has "$tmp/o" "style gate: FAIL" "a bad format says FAIL"
has "$tmp/o" "src/bad_format.c:2:1: error: code should be clang-formatted" "the place that clang-format would change is printed"
has "$tmp/o" "1 place(s) in 1 file(s) that it would change" "the count of places and files"
lacks "$tmp/o" "src/clean.c:" "a clean file is not named"

printf '// finding\nint k(void)\n{\n\treturn 3; /* STUB:SPACING stub */\n}\n' > "$t/src/finding.c"
run "$t" -- src/clean.c src/finding.c
expect $? 1 "a file with a checkpatch finding"
has "$tmp/o" "src/finding.c:4: WARNING:SPACING: stub finding" "the finding is printed"
has "$tmp/o" "1 finding(s), 0 of them in an open type, 1 not" "the finding is counted as one that is not let through"
has "$tmp/o" "1  SPACING" "the findings are counted by type"
lacks "$tmp/o" "SPACING  (open)" "a type that is not open is not marked"

printf '// open\nint m(void)\n{\n\treturn 4; /* STUB:BLOCK_COMMENT_STYLE stub */\n}\n' > "$t/src/open_type.c"
run "$t" -- src/clean.c src/open_type.c
expect $? 0 "a finding of an open type is let through"
has "$tmp/o" "1 finding(s), 1 of them in an open type, 0 not" "the open finding is counted"
has "$tmp/o" "open type BLOCK_COMMENT_STYLE: 1 finding(s)" "the open type is counted"
has "$tmp/o" "1  BLOCK_COMMENT_STYLE  (open)" "the findings by type say which type is open"
has "$tmp/o" "style gate: OK" "an open finding does not fail the gate"

run "$t" -- src/open_type.c src/finding.c
expect $? 1 "an open type and a type that is not"
has "$tmp/o" "2 finding(s), 1 of them in an open type, 1 not" "one let through and one not"

printf '// two\nint q(void)\n{\n\treturn 5; /* STUB:BLOCK_COMMENT_STYLE a */\n\t/* STUB:LONG_LINE b */\n}\n' > "$t/src/two.c"
run "$t" -- src/two.c
expect $? 1 "two findings in a file, one of them open"
has "$tmp/o" "2 finding(s), 1 of them in an open type, 1 not" "two findings, one open"
has "$tmp/o" "src/two.c:5: WARNING:LONG_LINE: stub finding" "the finding of the type that is not open is printed"
lacks "$tmp/o" "  src/two.c:4:" "the finding of the open type is not printed as one to fix"

awk 'BEGIN { print "// many"; for (i = 0; i < 120; i++) print "\t/* STUB:SPACING n */" }' > "$t/src/many.c"
run "$t" -- src/many.c
expect $? 1 "a file with 120 findings"
has "$tmp/o" "120 finding(s), 0 of them in an open type, 120 not" "120 findings are counted"
has "$tmp/o" "... and 20 more" "the list says how many lines it left out"
has "$tmp/o" "120  SPACING" "the count by type is the whole"

printf '// patterns\nint r(void)\n{\n\treturn 6; /* STUB:SPACING in the pattern */\n\treturn 7; /* STUB:SPACING outside */\n}\n' > "$t/src/pattern.c"
printf 'SPACING /pattern\\.c:4:/ a finding of one place is a debt\n' > "$t/open.txt"
run "$t" -- src/pattern.c
expect $? 1 "an open entry with a pattern, and a finding of its type outside it"
has "$tmp/o" "2 finding(s), 1 of them in an open type, 1 not" "one of the two is let through by the entry"
has "$tmp/o" "open type SPACING /pattern\\.c:4:/: 1 finding(s) -- a finding of one place is a debt" "the entry with its pattern and its count is printed"
has "$tmp/o" "src/pattern.c:5: WARNING:SPACING: stub finding" "the finding outside the pattern is named"
lacks "$tmp/o" "  src/pattern.c:4:" "the finding inside the pattern is not named as one to fix"
has "$tmp/o" "2  SPACING  (1 of them open)" "the type says how many of its findings are open"
printf 'SPACING /pattern\\.c:[45]:/ both places of one file are a debt\n' > "$t/open.txt"
run "$t" -- src/pattern.c
expect $? 0 "an open entry with a pattern that matches every finding"
has "$tmp/o" "2  SPACING  (open)" "all findings of the type are open"
printf 'SPACING /pattern.c/\n' > "$t/open.txt"
run "$t" -- src/clean.c
expect $? 2 "an entry with a pattern and no reason"
printf 'SPACING /pattern.c no closing slash\n' > "$t/open.txt"
run "$t" -- src/clean.c
expect $? 2 "a pattern with no closing slash"
printf 'SPACING /(/ not an expression\n' > "$t/open.txt"
run "$t" -- src/clean.c
expect $? 2 "a pattern that is not a regular expression"
printf '# the types that may stay\nBLOCK_COMMENT_STYLE comments are replaced later\n' > "$t/open.txt"

awk 'BEGIN { printf "int w(int a"; for (i = 0; i < 30; i++) printf ", int b%d", i; printf ");\n" }' > "$t/src/wide.c"
run "$t" -- src/clean.c src/wide.c
expect $? 1 "a line of code of more than 100 columns"
has "$tmp/o" "src/wide.c:1: CHECK:LONG_LINE: line length of" "the line is named"
has "$tmp/o" "1  LONG_LINE" "the line is counted by type"
printf 'LONG_LINE /^src\\/wide\\.c:1:/ a table of strings that cannot be broken\n' > "$t/open.txt"
run "$t" -- src/wide.c
expect $? 0 "a line that is too long and listed"
has "$tmp/o" "open type LONG_LINE /^src\\/wide\\.c:1:/: 1 finding(s) -- a table of strings that cannot be broken" "the entry is printed with its count"
printf '# the types that may stay\nBLOCK_COMMENT_STYLE comments are replaced later\n' > "$t/open.txt"
printf '// a comment line of more than a hundred characters: it is not a finding, the length of comments is O6 and the comment is not code %s\n' xxxxxxxxxxxxxxxxxx > "$t/src/wide_comment.c"
run "$t" -- src/wide_comment.c
expect $? 0 "a comment of more than 100 columns is not a finding of the length check"
(cd "$t" && sh "$gate" --clang-format "$tmp/cf" --checkpatch "$tmp/silent-ll" -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "a checkpatch that does not report the long line of its canary"
has "$tmp/o" "the length of lines" "a checkpatch that does not measure the length of lines: the reason"

printf 'BLOCK_COMMENT_STYLE\n' > "$t/open.txt"
run "$t" -- src/clean.c
expect $? 2 "an open type with no reason"
has "$tmp/o" "BLOCK_COMMENT_STYLE has no reason" "an open type with no reason is named"
printf 'block_comment_style because\n' > "$t/open.txt"
run "$t" -- src/clean.c
expect $? 2 "an open type that is not a type"
printf '# nothing is open\n' > "$t/open.txt"
run "$t" -- src/open_type.c
expect $? 1 "no type is open"
rm -f "$t/open.txt"
run "$t" -- src/clean.c
expect $? 2 "an open file that is not there"
printf '# the types that may stay\nBLOCK_COMMENT_STYLE comments are replaced later\n' > "$t/open.txt"

run "$t" --
expect $? 2 "no files"
has "$tmp/o" "no files to check" "no files: the reason"
run "$t" -- src/clean.c src/nothing.c
expect $? 2 "a file that is not there"
has "$tmp/o" "no such file: src/nothing.c" "a file that is not there: the reason"
(cd "$t" && sh "$gate" --checkpatch "$tmp/cp" -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "no clang-format given"
has "$tmp/o" "no clang-format" "no clang-format: the reason"
(cd "$t" && sh "$gate" --clang-format "$tmp/cf" -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "no checkpatch given"
has "$tmp/o" "no checkpatch" "no checkpatch: the reason"
(cd "$t" && sh "$gate" --clang-format "" --checkpatch "$tmp/cp" -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "an empty clang-format"
(cd "$t" && sh "$gate" --clang-format "$tmp/no-such-tool" --checkpatch "$tmp/cp" -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "clang-format is not there"
has "$tmp/o" "clang-format is not there" "clang-format is not there: the reason"
(cd "$t" && sh "$gate" --clang-format "$tmp/cf" --checkpatch "$tmp/no-such-tool" -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "checkpatch is not there"
has "$tmp/o" "checkpatch is not there" "checkpatch is not there: the reason"
(cd "$t" && sh "$gate" --clang-format "$tmp/cf" --checkpatch "$tmp/cp" --jobs 0 -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "zero jobs"
(cd "$t" && sh "$gate" --clang-format "$tmp/cf" --checkpatch "$tmp/cp" --frobnicate -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "an option that is not one"

(cd "$t" && sh "$gate" --clang-format "$tmp/cf-silent" --checkpatch "$tmp/cp" -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "a clang-format that says nothing on the canary"
has "$tmp/o" "the command measures nothing" "a clang-format that says nothing: the reason"
(cd "$t" && sh "$gate" --clang-format "$tmp/cf" --checkpatch "$tmp/silent" -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "a checkpatch that says nothing on the canary"
has "$tmp/o" "the command measures nothing" "a checkpatch that says nothing: the reason"
(cd "$t" && sh "$gate" --clang-format "$tmp/cf-broken" --checkpatch "$tmp/cp" -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "a clang-format that fails"

(cd "$t" && sh "$gate" --clang-format "$tmp/cf" --checkpatch "$tmp/cp-lies-1" -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "a checkpatch that returns 1 and prints nothing"
has "$tmp/o" "its status and its output disagree" "status 1 and no finding: the reason"
(cd "$t" && sh "$gate" --clang-format "$tmp/cf" --checkpatch "$tmp/cp-lies-2" -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "a checkpatch that returns 0 and prints a finding"
(cd "$t" && sh "$gate" --clang-format "$tmp/cf" --checkpatch "$tmp/cp-crashes" -- src/clean.c > "$tmp/o" 2>&1)
expect $? 2 "a checkpatch that crashes"
has "$tmp/o" "checkpatch failed on src/clean.c (status 255)" "a crash is named"

rm -f "$t/.clang-format"
run "$t" -- src/clean.c
expect $? 2 "no .clang-format"
has "$tmp/o" "no .clang-format" "no .clang-format: the reason"
printf 'BasedOnStyle: LLVM\n' > "$t/.clang-format"

printf '# SPDX\n--ignore VOLATILE\n' > "$t/.checkpatch.conf"
run "$t" -- src/clean.c
expect $? 0 "a tree with a .checkpatch.conf"
has "$tmp/o" ".checkpatch.conf holds: --ignore VOLATILE" "the configuration of checkpatch is printed"
rm -f "$t/.checkpatch.conf"

make -s --no-print-directory -C "$root" check-style CLANG_FORMAT= CHECKPATCH= > "$tmp/o" 2>&1
expect $? 2 "make check-style with no tools given"
has "$tmp/o" "no clang-format" "make check-style with no tools: the reason"
make -s --no-print-directory -C "$root" check-style CLANG_FORMAT="$tmp/cf" CHECKPATCH= > "$tmp/o" 2>&1
expect $? 2 "make check-style with no checkpatch given"
has "$tmp/o" "no checkpatch" "make check-style with no checkpatch: the reason"

printf 'check_style_gate: %d distinct assertions, %d executions, %d failures\n' "$checks" "$checks" "$failures"
[ "$failures" -eq 0 ]
