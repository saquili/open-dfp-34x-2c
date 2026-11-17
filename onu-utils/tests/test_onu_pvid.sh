#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-pvid, with stand-ins for onu-nl and onu-config, which
# log what they are asked to do, and logger.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
PVID=$HERE/scripts/onu-pvid
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

setup() {
	T=$(mktemp -d)
	export ONU_ROOT=$T/root TRIAL_LIMIT=60 LOG=$T/log
	mkdir -p "$ONU_ROOT/run" "$ONU_ROOT/proc/net/vlan" "$T/bin"
	: >"$LOG"
	echo "1000.50 900.00" >"$ONU_ROOT/proc/uptime"
	printf 'VLAN=4094\nADDRESS=198.51.100.2/30\nIFACE=mgmt\n' >"$ONU_ROOT/run/onu-mgmt"
	printf 'VLAN Dev name\t | VLAN ID\nName-Type: VLAN_NAME_TYPE_RAW_PLUS_VID_NO_PAD\nmgmt           | 4094  | sfp\nlan10          | 10  | sfp\n' \
		>"$ONU_ROOT/proc/net/vlan/config"
	printf '#!/bin/sh\necho "onu-nl $*" >>"%s"\n[ -e "%s/nl.fail" ] && [ "$2" = add ] && exit 1\nexit 0\n' \
		"$LOG" "$T" >"$T/bin/onu-nl"
	printf '#!/bin/sh\n[ "$1" = system ] && cat >>"%s/stored"\n' "$T" >"$T/bin/onu-config"
	printf '#!/bin/sh\nexit 0\n' >"$T/bin/logger"
	chmod +x "$T/bin"/*
	PATH=$T/bin:$PATH
}

teardown() {
	rm -rf "$T"
}

run() {
	out=$("$PVID" "$@" 2>/dev/null)
	rc=$?
}

logged() {
	grep -q -x -F -- "$1" "$LOG"
}

state() {
	sed -n 's/^PVID=//p' "$ONU_ROOT/run/onu-pvid"
}

echo "at boot: the store's PVID, 1 without one"
setup
run apply
check '[ $rc -eq 0 ] && [ "$(state)" = 1 ] && ! grep -q "filter add" "$LOG"'
check 'logged "onu-nl filter del sfp pref 3" && logged "onu-nl filter del sfp egress pref 3"'
: >"$LOG"
echo SFP_PVID=4094 >"$ONU_ROOT/run/onu-system"
run apply
check '[ $rc -eq 0 ] && [ "$(state)" = 4094 ]'
check 'logged "onu-nl filter add sfp pref 4 untagged sw push 4094"'
check 'logged "onu-nl filter add sfp pref 5 vlan 0 sw modify 4094"'
check 'logged "onu-nl filter add sfp egress pref 3 vlan 4094 sw pop"'
check 'logged "onu-nl filter add sfp pref 3 untagged hw trap"'
# The software's rules come before the switch's trap, and the trap goes first.
check '[ "$(grep -n "sw push" "$LOG" | cut -d: -f1)" -lt "$(grep -n "hw trap" "$LOG" | cut -d: -f1)" ]'
check '[ "$(grep -n "del sfp pref 3" "$LOG" | cut -d: -f1)" -lt "$(grep -n "del sfp pref 4" "$LOG" | cut -d: -f1)" ]'
run status
check '[ "$out" = PVID=4094 ]'
echo SFP_PVID=x >"$ONU_ROOT/run/onu-system"
run apply
check '[ "$(state)" = 1 ]'
teardown

echo "a VLAN device of the stick's on sfp is its own; a service's VLAN is omcid's"
setup
run set 10
check '[ $rc -eq 0 ] && [ "$(state)" = 10 ] && logged "onu-nl filter add sfp pref 4 untagged sw push 10"'
check 'grep -qx SFP_PVID=10 "$T/stored"'
: >"$LOG"
run set 35
check '[ $rc -eq 0 ] && [ "$(state)" = 35 ] && ! grep -q "filter add" "$LOG"'
run set 0
check '[ $rc -eq 2 ] && [ "$(state)" = 35 ]'
run set 4095
check '[ $rc -eq 2 ]'
teardown

echo "a PVID on trial, kept or gone back"
setup
run apply
run try 4094
check '[ $rc -eq 0 ] && [ "$(state)" = 4094 ] && [ -e "$ONU_ROOT/run/onu-trial.d/pvid" ]'
check 'grep -qx UNTIL=1060 "$ONU_ROOT/run/onu-trial.d/pvid" && grep -qx PREVIOUS=1 "$ONU_ROOT/run/onu-trial.d/pvid"'
run status
check 'echo "$out" | grep -qx TRIAL_PVID=4094 && echo "$out" | grep -qx TRIAL_PREVIOUS=1 && echo "$out" | grep -qx TRIAL_UNTIL=1060'
run try 35
check '[ $rc -eq 1 ]'
run keep
check '[ $rc -eq 0 ] && [ ! -e "$ONU_ROOT/run/onu-trial.d/pvid" ] && grep -qx SFP_PVID=4094 "$T/stored"'
run try 1
run revert
check '[ $rc -eq 0 ] && [ "$(state)" = 4094 ] && [ ! -e "$ONU_ROOT/run/onu-trial.d/pvid" ]'
check '[ "$(grep -c SFP_PVID "$T/stored")" -eq 1 ]'
# Not kept in time: the keeper of the deadlines takes it back.
ln -s "$PVID" "$T/bin/onu-pvid"
run try 35
echo "1061.00 950.00" >"$ONU_ROOT/proc/uptime"
"$CHANGE" expire 2>/dev/null
check '[ "$(state)" = 4094 ] && [ ! -e "$ONU_ROOT/run/onu-trial.d/pvid" ]'
# The same PVID is no trial.
run try 4094
check '[ $rc -eq 0 ] && [ ! -e "$ONU_ROOT/run/onu-trial.d/pvid" ]'
teardown

echo "a trial whose rules the switch refuses goes back at once"
setup
run apply
touch "$T/nl.fail"
run try 4094
check '[ $rc -ne 0 ] && [ "$(state)" = 1 ] && [ ! -e "$ONU_ROOT/run/onu-trial.d/pvid" ]'
teardown

echo "test_onu_pvid: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
