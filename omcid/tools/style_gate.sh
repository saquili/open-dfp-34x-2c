#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

set -u
# style_gate.sh: run clang-format and checkpatch over the files given after
# "--" and fail on any difference or finding, except the findings that the
# file given with --open lets through.

PROG=style_gate.sh
cf=
cp=
open_file=
jobs=${STYLE_JOBS:-4}

die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 2; }

while [ $# -gt 0 ]; do
    case $1 in
        --clang-format) [ $# -ge 2 ] || die "--clang-format needs an argument"; cf=$2; shift 2 ;;
        --checkpatch)   [ $# -ge 2 ] || die "--checkpatch needs an argument"; cp=$2; shift 2 ;;
        --open)         [ $# -ge 2 ] || die "--open needs an argument"; open_file=$2; shift 2 ;;
        --jobs)         [ $# -ge 2 ] || die "--jobs needs an argument"; jobs=$2; shift 2 ;;
        --)             shift; break ;;
        -*)             die "unknown option $1" ;;
        *)              break ;;
    esac
done

[ -n "$cf" ] || die "no clang-format: give it with CLANG_FORMAT=... (there is no default, so that the gate cannot pass by not running)"
[ -n "$cp" ] || die "no checkpatch: give it with CHECKPATCH=... (there is no default, so that the gate cannot pass by not running)"
[ $# -gt 0 ] || die "no files to check: the gate measured nothing"
case $jobs in ''|*[!0-9]*|0) die "--jobs wants a positive number, not '$jobs'" ;; esac

tool_ok() { set -- $1; command -v "$1" > /dev/null 2>&1; }
tool_ok "$cf" || die "clang-format is not there: $cf"
tool_ok "$cp" || die "checkpatch is not there: $cp"
[ -f .clang-format ] || die "no .clang-format in $(pwd): the gate runs in the root of the tree"
for f in "$@"; do
    case $f in *[[:space:]]*) die "a file name with a blank in it: $f" ;; esac
    [ -f "$f" ] || die "no such file: $f"
done

tmp=$(mktemp -d "${TMPDIR:-/tmp}/style-gate.XXXXXX") || die "cannot make a temporary directory"
trap 'rm -rf "$tmp"' EXIT INT TERM

: > "$tmp/open.types"
if [ -n "$open_file" ]; then
    [ -f "$open_file" ] || die "no such open file: $open_file"
    n=0
    while read -r type rest || [ -n "$type" ]; do
        n=$((n + 1))
        case $type in ''|'#'*) continue ;; esac
        case $type in *[!A-Z0-9_]*) die "$open_file:$n: '$type' is not a checkpatch type" ;; esac
        pat=
        case $rest in
            /*)
                pat=${rest%% *}
                case $pat in /*/) ;; *) die "$open_file:$n: the pattern of $type does not end with a /" ;; esac
                if [ "$pat" = "$rest" ]; then rest=; else rest=${rest#* }; fi
                pat=${pat#/}
                pat=${pat%/}
                [ -n "$pat" ] || die "$open_file:$n: the pattern of $type is empty"
                RE="$pat" awk 'BEGIN { "" ~ ENVIRON["RE"] }' 2> /dev/null || die "$open_file:$n: the pattern of $type is not a regular expression"
                ;;
        esac
        [ -n "$rest" ] || die "$open_file:$n: $type has no reason: an open type says why it is open"
        printf '%s\t%s\t%s\n' "$type" "$pat" "$rest" >> "$tmp/open.types"
    done < "$open_file"
fi

printf 'int main(void){return 0;}\n' > "$tmp/canary.c"
$cf --style=file:.clang-format --dry-run --Werror "$tmp/canary.c" > "$tmp/canary.cf" 2>&1
rc=$?
if [ "$rc" -eq 0 ] || ! grep -q 'clang-format-violations' "$tmp/canary.cf"; then
    die "clang-format said nothing about a file it must reformat (status $rc): the command measures nothing: $cf"
fi
printf '    int x;\n' > "$tmp/canary-cp.c"
$cp --no-tree --terse --show-types -f "$tmp/canary-cp.c" > "$tmp/canary.cp" 2>&1
if ! grep -q ':LEADING_SPACE:' "$tmp/canary.cp"; then
    die "checkpatch did not report the spaces at the start of a line of the canary: the command measures nothing: $cp"
fi
awk 'BEGIN { printf "int f(int a"; for (i = 0; i < 30; i++) printf ", int b%d", i; printf ");\n" }' > "$tmp/canary-ll.c"
$cp --no-tree --strict --terse --show-types --types LONG_LINE -f "$tmp/canary-ll.c" > "$tmp/canary.ll" 2>&1
if ! grep -q ':LONG_LINE:' "$tmp/canary.ll"; then
    die "checkpatch did not report the line of 200 columns of the canary: the command measures nothing for the length of lines: $cp"
fi

cf_version=$($cf --version 2>&1 | head -n 1)
$cf --style=file:.clang-format --dry-run --Werror "$@" > "$tmp/cf.out" 2>&1
cf_rc=$?
grep 'code should be clang-formatted' "$tmp/cf.out" > "$tmp/cf.places"
cf_places=$(grep -c . "$tmp/cf.places")
cf_files=$(sed -n 's/^\([^:]*\):[0-9][0-9]*:[0-9][0-9]*: .*/\1/p' "$tmp/cf.places" | sort -u | grep -c .)
if [ "$cf_rc" -ne 0 ] && [ "$cf_places" -eq 0 ]; then
    sed 's/^/    | /' "$tmp/cf.out" >&2
    die "clang-format failed (status $cf_rc) without a violation to show: $cf"
fi
if [ "$cf_rc" -eq 0 ] && [ "$cf_places" -ne 0 ]; then
    die "clang-format returned 0 and printed $cf_places violation(s): its status and its output disagree"
fi

mkdir "$tmp/cp"
cat > "$tmp/run1.sh" << 'EOF'
#!/bin/sh
$STYLE_GATE_CP --no-tree --terse --show-types -f "$3" > "$1/$2.out" 2>&1
echo $? > "$1/$2.rc"
$STYLE_GATE_CP --no-tree --strict --terse --show-types --types LONG_LINE -f "$3" > "$1/$2.ll" 2>&1
echo $? > "$1/$2.llrc"
EOF
STYLE_GATE_CP=$cp
export STYLE_GATE_CP
n=0
for f in "$@"; do
    n=$((n + 1))
    printf '%s\t%s\n' "$n" "$f"
done > "$tmp/cp.list"
if [ "$jobs" -gt 1 ] && xargs -P 1 true < /dev/null > /dev/null 2>&1; then
    awk -F'\t' -v dir="$tmp/cp" '{ print dir, $1, $2 }' "$tmp/cp.list" | xargs -P "$jobs" -n 3 sh "$tmp/run1.sh"
else
    while IFS="$(printf '\t')" read -r n f; do
        sh "$tmp/run1.sh" "$tmp/cp" "$n" "$f"
    done < "$tmp/cp.list"
fi

cp_findings=0
: > "$tmp/cp.all"
FINDING='^[^ ]+:[0-9]+: (WARNING|ERROR|CHECK):[A-Z0-9_]+:'
while IFS="$(printf '\t')" read -r n f; do
    for kind in "" ll; do
        if [ -z "$kind" ]; then out=$tmp/cp/$n.out; rcf=$tmp/cp/$n.rc; what="checkpatch"; else out=$tmp/cp/$n.ll; rcf=$tmp/cp/$n.llrc; what="checkpatch (the length of lines)"; fi
        [ -f "$rcf" ] || die "$what did not run on $f"
        rc=$(cat "$rcf")
        found=$(grep -c -E "$FINDING" "$out")
        case $rc in
            0) [ "$found" -eq 0 ] || die "$what returned 0 for $f and printed $found finding(s): its status and its output disagree" ;;
            1) [ "$found" -gt 0 ] || die "$what returned 1 for $f and printed no finding: its status and its output disagree" ;;
            *) sed 's/^/    | /' "$out" >&2; die "$what failed on $f (status $rc)" ;;
        esac
        cp_findings=$((cp_findings + found))
        grep -E "$FINDING" "$out" >> "$tmp/cp.all"
    done
done < "$tmp/cp.list"

shown() {
    sed -n '1,100p' "$1" | sed 's/^/  /'
    total=$(grep -c . "$1")
    [ "$total" -le 100 ] || printf '  ... and %s more\n' "$((total - 100))"
}

printf 'style gate: %s files\n' "$#"
printf 'clang-format: %s\n' "$cf_version"
if [ "$cf_rc" -eq 0 ]; then
    printf 'clang-format: every file is as it formats it\n'
else
    printf 'clang-format: %s place(s) in %s file(s) that it would change:\n' "$cf_places" "$cf_files"
    shown "$tmp/cf.places"
fi
if [ -f .checkpatch.conf ]; then
    printf 'checkpatch: .checkpatch.conf holds: %s\n' "$(grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' .checkpatch.conf | tr '\n' ' ')"
fi
: > "$tmp/cp.bad"
: > "$tmp/cp.types"
cp_open=$(awk -F'\t' -v opens="$tmp/open.types" -v bad="$tmp/cp.bad" -v types="$tmp/cp.types" -v counts="$tmp/cp.counts" -v ctypes="$tmp/cp.counts.types" '
    BEGIN {
        while ((getline line < opens) > 0) {
            split(line, f, "\t")
            n++
            otype[n] = f[1]
            opat[n] = f[2]
        }
    }
    {
        t = "?"
        if (match($0, / (WARNING|ERROR|CHECK):[A-Z0-9_]+:/)) {
            t = substr($0, RSTART + 1, RLENGTH - 2)
            sub(/^[A-Z]+:/, "", t)
        }
        print t >> types
        taken = 0
        for (k = 1; k <= n && !taken; k++)
            if (otype[k] == t && (opat[k] == "" || $0 ~ opat[k])) {
                taken = 1
                count[k]++
                typeopen[t]++
            }
        if (taken) open++
        else print $0 >> bad
    }
    END {
        for (k = 1; k <= n; k++) print k "\t" count[k] + 0 > counts
        for (t in typeopen) print t "\t" typeopen[t] > ctypes
        print open + 0
    }' "$tmp/cp.all")
: >> "$tmp/cp.counts"
: >> "$tmp/cp.counts.types"
cp_bad=$(grep -c . "$tmp/cp.bad")
printf 'checkpatch: %s finding(s), %s of them in an open type, %s not\n' "$cp_findings" "$cp_open" "$cp_bad"
if [ -s "$tmp/open.types" ]; then
    awk -F'\t' -v counts="$tmp/cp.counts" '
        BEGIN { while ((getline line < counts) > 0) { split(line, f, "\t"); count[f[1]] = f[2] } }
        {
            where = ($2 == "") ? "" : " /" $2 "/"
            printf "  open type %s%s: %d finding(s) -- %s\n", $1, where, count[NR] + 0, $3
        }' "$tmp/open.types"
fi
if [ "$cp_findings" -gt 0 ]; then
    echo "checkpatch: findings by type:"
    sort "$tmp/cp.types" | uniq -c | sort -rn | awk -v od="$tmp/cp.counts.types" '
        BEGIN { while ((getline line < od) > 0) { split(line, f, "\t"); open[f[1]] = f[2] } }
        {
            mark = ""
            if (($2 in open) && open[$2] == $1) mark = "  (open)"
            else if ($2 in open) mark = "  (" open[$2] " of them open)"
            printf "  %7d  %s%s\n", $1, $2, mark
        }'
fi
if [ "$cp_bad" -gt 0 ]; then
    echo "checkpatch: the findings that are not let through:"
    shown "$tmp/cp.bad"
fi
if [ "$cf_rc" -eq 0 ] && [ "$cp_bad" -eq 0 ]; then
    echo "style gate: OK"
    exit 0
fi
echo "style gate: FAIL"
exit 1
