#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-mgmt, with stand-ins for ip, tc, dropbear, kill,
# onu-store and logger, which log what they are asked to do.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
MGMT=$HERE/scripts/onu-mgmt
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
	export ONU_ROOT=$T/root PENDING_LIMIT=600 LOG=$T/log
	mkdir -p "$ONU_ROOT/run" "$ONU_ROOT/proc/sys/net/ipv6/conf/mgmt100" "$T/bin"
	: >"$LOG"
	echo "500.25 400.00" >"$ONU_ROOT/proc/uptime"
	printf 'VLAN=4094\nADDRESS=198.51.100.2/30\nIFACE=mgmt\nPREF=1\nDATA_ADDRESS=\n' \
		>"$ONU_ROOT/run/onu-mgmt"
	# The ssh server of the boot: a process to be stopped.
	sleep 60 &
	OLD_SSH=$!
	echo "$OLD_SSH" >"$ONU_ROOT/run/dropbear.pid"
	for c in tc logger; do
		printf '#!/bin/sh\necho "%s $*" >>"$LOG"\n' "$c" >"$T/bin/$c"
	done
	# ip refuses a VLAN that is in use, as Linux does a second interface of one.
	printf '#!/bin/sh\necho "ip $*" >>"$LOG"\n[ "$1 $2" = "link add" ] && [ -e "%s/vlan-in-use" ] && exit 2\nexit 0\n' \
		"$T" >"$T/bin/ip"
	printf '#!/bin/sh\necho "dropbear $*" >>"$LOG"\nwhile [ $# -gt 0 ]; do [ "$1" = -P ] && echo 22 >"$2"; shift; done\n' \
		>"$T/bin/dropbear"
	printf '#!/bin/sh\n[ "$1" = system ] && cat >>"%s/stored"\n' "$T" >"$T/bin/onu-store"
	chmod +x "$T/bin"/*
	PATH=$T/bin:$PATH
}

teardown() {
	kill "$OLD_SSH" 2>/dev/null
	rm -rf "$T"
}

run() {
	out=$("$MGMT" "$@" 2>/dev/null)
	rc=$?
}

logged() {
	grep -q -x -F -- "$1" "$LOG"
}

echo "the pair in use"
setup
run status
check '[ $rc -eq 0 ]'
check 'echo "$out" | grep -qx VLAN=4094'
check 'echo "$out" | grep -qx ADDRESS=198.51.100.2/30'
check '! echo "$out" | grep -q PENDING_'
teardown

echo "a new VLAN beside the old one, then committed"
setup
run add 100 192.0.2.6/30
check '[ $rc -eq 0 ]'
check 'logged "ip link add link sfp name mgmt100 type vlan id 100"'
check 'logged "tc filter add dev sfp ingress pref 2 protocol 802.1Q flower skip_sw vlan_id 100 action trap"'
check 'logged "tc filter add dev pon ingress pref 2 protocol 802.1Q flower skip_sw vlan_id 100 action drop"'
check 'logged "ip address add 192.0.2.6/30 dev mgmt100"'
check 'grep -q -- "-p 192.0.2.6:22" "$LOG"'
check '! grep -q "ip link del mgmt$" "$LOG"'
run status
check 'echo "$out" | grep -qx PENDING_VLAN=100'
check 'echo "$out" | grep -qx PENDING_UNTIL=1100'
check 'echo "$out" | grep -qx VLAN=4094'
run add 200 192.0.2.10/30
check '[ $rc -ne 0 ]'
run commit
check '[ $rc -eq 0 ]'
check 'grep -qx MGMT_VLAN=100 "$T/stored" && grep -qx MGMT_ADDRESS=192.0.2.6/30 "$T/stored"'
check 'logged "tc filter del dev sfp ingress pref 1 protocol 802.1Q"'
check 'logged "ip link del mgmt"'
check '! kill -0 "$OLD_SSH" 2>/dev/null'
check 'grep -q "dropbear .*-P $ONU_ROOT/run/dropbear.pid -p 192.0.2.6:22" "$LOG"'
run status
check 'echo "$out" | grep -qx VLAN=100 && echo "$out" | grep -qx IFACE=mgmt100'
check '! echo "$out" | grep -q PENDING_'
check 'grep -qx PREF=2 "$ONU_ROOT/run/onu-mgmt"'
teardown

echo "a new address in the same VLAN, then cancelled"
setup
run add 4094 198.51.100.6/30
check '[ $rc -eq 0 ]'
check 'logged "ip address add 198.51.100.6/30 dev mgmt"'
check '! grep -q "ip link add" "$LOG"'
run cancel
check 'logged "ip address del 198.51.100.6/30 dev mgmt"'
check '! grep -q "ip link del" "$LOG"'
check '[ ! -e "$ONU_ROOT/run/onu-mgmt-pending" ] && [ ! -e "$T/stored" ]'
check 'grep -qx ADDRESS=198.51.100.2/30 "$ONU_ROOT/run/onu-mgmt"'
teardown

echo "a change not committed goes by itself; an earlier watcher leaves a later change"
setup
export PENDING_LIMIT=1
run add 100 192.0.2.6/30
sleep 3
check '[ ! -e "$ONU_ROOT/run/onu-mgmt-pending" ]'
check 'logged "ip link del mgmt100"'
check 'logged "tc filter del dev pon ingress pref 2 protocol 802.1Q"'
export PENDING_LIMIT=600
run add 100 192.0.2.6/30
run expire 1
check '[ -e "$ONU_ROOT/run/onu-mgmt-pending" ]'
teardown

echo "what is not a pair, or a VLAN in use, changes nothing"
setup
for bad in "0 192.0.2.6/30" "4095 192.0.2.6/30" "100 192.0.2.6" "100 192.0.2.300/30" \
	"100 192.0.2.6/32" "abc 192.0.2.6/30"; do
	# shellcheck disable=SC2086
	run add $bad
	check '[ $rc -eq 2 ]'
done
check '! grep -q -e "^ip " -e "^tc " -e "^dropbear " "$LOG"'
: >"$T/vlan-in-use"
run add 36 192.0.2.6/30
check '[ $rc -ne 0 ] && [ ! -e "$ONU_ROOT/run/onu-mgmt-pending" ]'
check '! grep -q "tc filter add" "$LOG"'
run commit
check '[ $rc -ne 0 ]'
teardown

echo "test_onu_mgmt: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
