#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-change, against stand-ins for onu-apply, onu-lane and
# onu-mgmt, which log what they are asked and end their change as the real
# ones do.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
CHANGE=$HERE/scripts/onu-change
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

logged() {
	grep -q -x -F -- "$1" "$LOG"
}

setup() {
	T=$(mktemp -d)
	LOG=$T/log
	export ONU_ROOT=$T/root LOG
	DIR=$ONU_ROOT/run/onu-trial.d
	mkdir -p "$DIR" "$T/bin"
	: >"$LOG"
	for p in onu-apply:settings onu-lane:lane onu-mgmt:mgmt; do
		printf '#!/bin/sh\necho "%s $*" >>"$LOG"\nrm -f "%s/%s"\n' \
			"${p%%:*}" "$DIR" "${p#*:}" >"$T/bin/${p%%:*}"
	done
	printf '#!/bin/sh\nexit 0\n' >"$T/bin/logger"
	chmod +x "$T/bin"/*
	PATH=$T/bin:$PATH
	printf 'UNTIL=1264\nTOKEN=7\n' >"$DIR/settings"
	printf 'MODE=2500basex\nPREVIOUS=auto\nUNTIL=1270\nTOKEN=8\n' >"$DIR/lane"
	printf 'VLAN=100\nADDRESS=192.0.2.10/30\nIFACE=mgmt100\nUNTIL=1800\nTOKEN=9\n' >"$DIR/mgmt"
}

teardown() {
	rm -rf "$T"
}

echo "every change on trial, by its name"
setup
: >"$DIR/settings.new"
out=$("$CHANGE" status)
check '[ "$out" = "lane 1270
mgmt 1800 192.0.2.10/30
settings 1264" ]'
teardown

echo "a confirmation keeps them, the management pair's only from its new address"
setup
"$CHANGE" confirm 198.51.100.2 2>/dev/null
rc=$?
check '[ $rc -eq 0 ] && logged "onu-apply confirm" && logged "onu-lane keep"'
check '! grep -q "^onu-mgmt" "$LOG" && [ -e "$DIR/mgmt" ]'
: >"$LOG"
"$CHANGE" confirm 192.0.2.10 2>/dev/null
check 'logged "onu-mgmt commit" && [ "$(wc -l <"$LOG")" -eq 1 ]'
check '[ -z "$("$CHANGE" status)" ]'
teardown

echo "a confirmation without an address leaves the management pair"
setup
"$CHANGE" confirm 2>/dev/null
check '! grep -q "^onu-mgmt" "$LOG" && [ -e "$DIR/mgmt" ]'
teardown

echo "a reversal takes every change back"
setup
"$CHANGE" revert
rc=$?
check '[ $rc -eq 0 ] && logged "onu-apply revert" && logged "onu-lane revert" && logged "onu-mgmt cancel"'
check '[ -z "$(ls "$DIR")" ]'
teardown

echo "nothing on trial: nothing to take back, and a confirmation says it came too late"
setup
rm -rf "$DIR"
check '[ -z "$("$CHANGE" status)" ]'
"$CHANGE" revert
rc=$?
check '[ $rc -eq 0 ] && [ ! -s "$LOG" ]'
"$CHANGE" confirm 192.0.2.10 2>/dev/null
rc=$?
check '[ $rc -eq 3 ] && [ ! -s "$LOG" ]'
"$CHANGE" 2>/dev/null
rc=$?
check '[ $rc -eq 2 ]'
teardown

echo "the keeper takes back what is due, with its token, and waits for the nearest"
setup
mkdir -p "$ONU_ROOT/proc"
echo "1000.50 900.00" >"$ONU_ROOT/proc/uptime"
"$CHANGE" expire 2>/dev/null
check '[ ! -s "$LOG" ] && [ -e "$DIR/settings" ] && [ -e "$DIR/lane" ] && [ -e "$DIR/mgmt" ]'
echo "1264.00 900.00" >"$ONU_ROOT/proc/uptime"
"$CHANGE" expire 2>/dev/null
check 'logged "onu-apply expire 7" && [ ! -e "$DIR/settings" ]'
check '! grep -q "^onu-lane" "$LOG" && [ -e "$DIR/lane" ] && [ -e "$DIR/mgmt" ]'
echo "1271.00 900.00" >"$ONU_ROOT/proc/uptime"
: >"$LOG"
"$CHANGE" expire 2>/dev/null
check 'logged "onu-lane expire 8" && [ ! -e "$DIR/lane" ] && [ -e "$DIR/mgmt" ] && [ "$(wc -l <"$LOG")" -eq 1 ]'
echo "1801.00 900.00" >"$ONU_ROOT/proc/uptime"
: >"$LOG"
"$CHANGE" expire 2>/dev/null
check 'logged "onu-mgmt expire 9" && [ -z "$(ls "$DIR")" ]'
teardown

echo "a change that does not go back is tried again, and a file in the making is left alone"
setup
mkdir -p "$ONU_ROOT/proc"
echo "1264.00 900.00" >"$ONU_ROOT/proc/uptime"
printf '#!/bin/sh\necho "onu-apply $*" >>"%s"\nexit 1\n' "$LOG" >"$T/bin/onu-apply"
: >"$DIR/settings.new"
"$CHANGE" expire 2>/dev/null
"$CHANGE" expire 2>/dev/null
check '[ "$(grep -c "^onu-apply expire 7" "$LOG")" -eq 2 ] && [ -e "$DIR/settings" ]'
check '! grep -q "settings.new" "$LOG"'
teardown

echo "the keeper waits for the nearest deadline, at most the idle time, and ends nothing early"
setup
mkdir -p "$ONU_ROOT/proc"
echo "1000.00 900.00" >"$ONU_ROOT/proc/uptime"
cat >"$T/bin/sleep" <<EOF2
#!/bin/sh
echo "sleep \$1" >>"$LOG"
echo "1264.00 900.00" >"$ONU_ROOT/proc/uptime"
EOF2
chmod +x "$T/bin/sleep"
# The first wait is the idle time (the nearest deadline is 264 s away); the
# stand-in sleep moves the clock to the settings' deadline, where the second
# pass takes the change back and the loop is stopped by the stand-in onu-apply.
printf '#!/bin/sh\necho "onu-apply $*" >>"%s"\nrm -f "%s/settings"\nkill -TERM $PPID\n' "$LOG" "$DIR" >"$T/bin/onu-apply"
"$CHANGE" watch 2>/dev/null
check 'sed -n 1p "$LOG" | grep -qx "sleep 5"'
check 'logged "onu-apply expire 7" && [ ! -e "$DIR/settings" ]'
teardown

echo "test_onu_change: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
