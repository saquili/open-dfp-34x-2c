#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-trial, with stand-ins for onu-update, pon, nc, pidof,
# reboot and logger, each told by a file what to answer.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
TRIAL_SCRIPT=$HERE/scripts/onu-trial
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
	export ONU_ROOT=$T/root TRIAL_LIMIT=2 TRIAL_STEP=1 LOG=$T/log
	mkdir -p "$ONU_ROOT/run" "$T/bin"
	: >"$LOG"
	# onu-update gives the status in $T/status and logs a commit.
	printf '#!/bin/sh\necho "onu-update $*" >>"$LOG"\ncase $1 in\nstatus) cat "%s/status" ;;\ncommit) [ ! -e "%s/commit.fail" ] ;;\nesac\n' \
		"$T" "$T" >"$T/bin/onu-update"
	printf '#!/bin/sh\ncat "%s/pon"\n' "$T" >"$T/bin/pon"
	printf '#!/bin/sh\ncat >/dev/null\n[ -e "%s/pond.up" ] && printf "HTTP/1.1 200 OK\\r\\n\\r\\n{\\"ok\\":true}"\nexit 0\n' \
		"$T" >"$T/bin/nc"
	# pidof gives omcid's PID, of $T/omcid.up, 100 when the file is empty.
	printf '#!/bin/sh\n[ -e "%s/omcid.up" ] || exit 1\n[ -s "%s/omcid.up" ] && cat "%s/omcid.up" || echo 100\n' \
		"$T" "$T" "$T" >"$T/bin/pidof"
	printf '#!/bin/sh\necho "reboot $*" >>"$LOG"\n' >"$T/bin/reboot"
	# The wait for procd's restart is no wait here; the looks' are real.
	printf '#!/bin/sh\n[ "$1" = 30 ] && exit 0\nexec %s "$@"\n' "$(command -v sleep)" >"$T/bin/sleep"
	printf '#!/bin/sh\nexit 0\n' >"$T/bin/logger"
	printf '#!/bin/sh\nexit 0\n' >"$T/bin/sync"
	chmod +x "$T/bin"/*
	PATH=$T/bin:$PATH
	status RUNNING=1 COMMITTED=0
	pon "pon  GPON  O5 operation  ONU-ID 1" "     alarms none  GEM ports up to 128"
	: >"$T/omcid.up"
	: >"$T/pond.up"
}

teardown() {
	rm -rf "$T"
}

status() {
	printf '%s\n' "$@" >"$T/status"
}

pon() {
	printf '%s\n' "$@" >"$T/pon"
}

logged() {
	grep -q -- "$1" "$LOG"
}

trial() {
	"$TRIAL_SCRIPT" 192.0.2.1 2>/dev/null
	rc=$?
	result=$(cat "$ONU_ROOT/run/onu-trial" 2>/dev/null)
}

echo "a start from the committed bank is no trial"
setup
status RUNNING=0 COMMITTED=0
trial
check '[ $rc -eq 0 ] && [ -z "$result" ]'
check '! logged commit && ! logged reboot'
teardown

echo "a start from memory is no trial"
setup
status RUNNING=-1 COMMITTED=1
trial
check '[ -z "$result" ] && ! logged commit && ! logged reboot'
teardown

echo "a tried bank that comes up is committed"
setup
trial
check '[ "$result" = committed ]'
check 'logged "^onu-update commit"'
check '! logged reboot'
teardown

echo "without light the PON is not waited for"
setup
pon "pon  GPON  O1 initial  ONU-ID -" "     alarms LOS  GEM ports up to 128"
trial
check '[ "$result" = committed ] && ! logged reboot'
teardown

echo "with light but no O5, the stick restarts into the committed bank"
setup
pon "pon  GPON  O3 serial number  ONU-ID -" "     alarms none  GEM ports up to 128"
trial
check '[ "$result" = failed ]'
check 'logged reboot && ! logged "^onu-update commit"'
teardown

echo "a status daemon that does not answer, or no omcid, fails the trial"
setup
rm "$T/pond.up"
trial
check '[ "$result" = failed ] && logged reboot'
teardown
setup
rm "$T/omcid.up"
trial
check '[ "$result" = failed ] && logged reboot'
teardown

echo "an omcid that procd starts again and again fails the trial"
setup
printf '#!/bin/sh\nn=$(cat "%s/pid" 2>/dev/null || echo 100)\necho $((n + 1)) >"%s/pid"\necho $n\n' \
	"$T" "$T" >"$T/bin/pidof"
trial
check '[ "$result" = failed ] && ! logged "^onu-update commit" && logged "^reboot -f"'
teardown

echo "without an address, the one of onu-mgmt, once it is there"
setup
"$TRIAL_SCRIPT" 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-trial")" = failed ]'
teardown
setup
printf 'VLAN=4094\nADDRESS=198.51.100.2/30\nDATA_ADDRESS=192.0.2.2/30\n' >"$ONU_ROOT/run/onu-mgmt"
printf '#!/bin/sh\necho "nc $*" >>"$LOG"\ncat >/dev/null\nprintf "HTTP/1.1 200 OK\\r\\n\\r\\n{\\"ok\\":true}"\n' >"$T/bin/nc"
"$TRIAL_SCRIPT" 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-trial")" = committed ] && logged "^nc 192.0.2.2 80$"'
teardown

echo "a commit that fails is a failed trial"
setup
: >"$T/commit.fail"
trial
check '[ "$result" = failed ] && logged reboot'
teardown

echo "test_onu_trial: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
