#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-run, with stand-ins for dropbear, dropbearkey, pond,
# getty, login, omcid and sleep, which log what they are asked to do.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
RUNNER=$HERE/scripts/onu-run
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
	export ONU_ROOT=$T/root LOG=$T/log
	mkdir -p "$ONU_ROOT/run" "$T/bin"
	: >"$LOG"
	printf 'VLAN=4094\nADDRESS=198.51.100.2/30\nIFACE=mgmt\nNAME=mgmt\nPREF=1\nDATA_ADDRESS=192.0.2.2/30\n' \
		>"$ONU_ROOT/run/onu-mgmt"
	for c in dropbear pond getty login omcid sleep; do
		printf '#!/bin/sh\necho "%s $*" >>"$LOG"\n' "$c" >"$T/bin/$c"
	done
	printf '#!/bin/sh\necho "dropbearkey $*" >>"$LOG"\nwhile [ $# -gt 0 ]; do [ "$1" = -f ] && echo key >"$2"; shift; done\n' \
		>"$T/bin/dropbearkey"
	chmod +x "$T/bin"/*
	PATH=$T/bin:$PATH
}

teardown() {
	rm -rf "$T"
}

run() {
	"$RUNNER" "$@" 2>/dev/null
	rc=$?
}

logged() {
	grep -q -x -F -- "$1" "$LOG"
}

echo "the ssh server, in the foreground, on both addresses, with a host key made first"
setup
run ssh
check '[ $rc -eq 0 ]'
check '[ "$(head -n 1 "$LOG")" = "dropbearkey -t ed25519 -f $ONU_ROOT/run/dropbear/host_key" ]'
check 'logged "dropbear -F -r $ONU_ROOT/run/dropbear/host_key -P $ONU_ROOT/run/dropbear.pid -p 198.51.100.2:22 -p 192.0.2.2:22"'
# Root's password as well as keys (ADR 0012).
check '! grep -q "^dropbear .*-[sB]\\b" "$LOG"'
: >"$LOG"
run ssh
check '! grep -q "^dropbearkey" "$LOG"'
teardown

echo "the status daemon on the same addresses"
setup
run pond
check '[ $rc -eq 0 ]'
check 'logged "pond -l http://198.51.100.2:8080 -l http://192.0.2.2:8080"'
teardown

echo "without an address on the host link, the management address alone"
setup
sed -i.bak 's/^DATA_ADDRESS=.*/DATA_ADDRESS=/' "$ONU_ROOT/run/onu-mgmt"
run ssh
check 'logged "dropbear -F -r $ONU_ROOT/run/dropbear/host_key -P $ONU_ROOT/run/dropbear.pid -p 198.51.100.2:22"'
run pond
check 'logged "pond -l http://198.51.100.2:8080"'
teardown

echo "the pair in use after a change"
setup
printf 'VLAN=100\nADDRESS=192.0.2.6/30\nIFACE=mgmt100\nNAME=mgmt7\nPREF=2\nDATA_ADDRESS=\n' \
	>"$ONU_ROOT/run/onu-mgmt"
run ssh
check 'grep -q "^dropbear .* -p 192.0.2.6:22$" "$LOG"'
teardown

echo "without a management address, a wait and a failure, and nothing started"
setup
rm "$ONU_ROOT/run/onu-mgmt"
run ssh
check '[ $rc -eq 1 ] && logged "sleep 5"'
check '! grep -q -e "^dropbear" -e "^pond" "$LOG"'
teardown

echo "the console asks for root's password, without an address too"
setup
rm "$ONU_ROOT/run/onu-mgmt"
run console
check '[ $rc -eq 0 ] && logged "getty -L console 0 vt100"'
check '! grep -q -e "^login" -e "^sleep" "$LOG"'
teardown

echo "a bench may have root's shell on the console without the password"
setup
mkdir -p "$ONU_ROOT/etc"
printf 'DATA_ADDRESS=192.0.2.2/30\nCONSOLE_AUTOLOGIN=root\n' >"$ONU_ROOT/etc/board.conf"
run console
check 'logged "login -f root" && ! grep -q "^getty" "$LOG"'
: >"$LOG"
for other in "CONSOLE_AUTOLOGIN=admin" "CONSOLE_AUTOLOGIN=" "#CONSOLE_AUTOLOGIN=root" \
	"CONSOLE_AUTOLOGIN=root; reboot"; do
	printf '%s\n' "$other" >"$ONU_ROOT/etc/board.conf"
	run console
done
check '! grep -q "^login" "$LOG" && [ "$(grep -c "^getty" "$LOG")" -eq 4 ]'
teardown

echo "omcid in the mode onu-mode gave it, without an address too"
setup
rm "$ONU_ROOT/run/onu-mgmt"
run omcid
check '[ $rc -eq 0 ] && logged "omcid "'
echo router >"$ONU_ROOT/run/omcid.mode"
run omcid
check 'logged "omcid -r"'
echo bridge >"$ONU_ROOT/run/omcid.mode"
run omcid
check '[ "$(grep -c "^omcid $" "$LOG")" -eq 2 ] && ! grep -q "^sleep" "$LOG"'
teardown

echo "what is not a server"
setup
run sshd
check '[ $rc -eq 2 ]'
run
check '[ $rc -eq 2 ]'
teardown

echo "test_onu_run: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
