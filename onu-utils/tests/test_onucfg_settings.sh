#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of "onucfg check" and "onucfg settings", the settings of the data path
# as the status daemon asks for them. BUILD names the directory of onucfg.

set -u
ONUCFG=${BUILD:-build}/onucfg
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

run() {
	printf "$2" | "$ONUCFG" "$1" >"$T/out" 2>"$T/err"
	rc=$?
}

echo "check: the settings in full, judged, without a store"
unset ONUCFG_STORE
run check 'PPPOE1_USER=bench\nPPPOE1_SECRET=not-a-real-one\nSESSION1_VLAN=410\n'
check '[ $rc -eq 0 ] && [ ! -s "$T/err" ]'
check 'grep -qx "LAN_ADDRESS=192.168.1.1/24" "$T/out" && grep -qx "SESSION1_VLAN=410" "$T/out"'
check '! grep -q -e bench -e not-a-real-one -e PPPOE "$T/out"'
check '[ "$(grep -c "^SESSION1_" "$T/out")" -eq 6 ] && ! grep -q "^SESSION2_" "$T/out"'

echo "check refuses, says why, and prints no settings"
run check 'WAN_VLAN=410\nWAN_VLAN=4095\n'
check '[ $rc -eq 1 ] && [ ! -s "$T/out" ] && grep -qx "onucfg: WAN_VLAN: not a value of it" "$T/err"'
run check 'PPPOE2_USER=bench\n'
check '[ $rc -eq 1 ] && grep -q "PPPOE2_SECRET" "$T/err"'
run check 'PPPOE2_USER=bench\nPPPOE2_SECRET=xyzzy plugh\n'
check '[ $rc -eq 1 ] && grep -q "PPPOE2_SECRET" "$T/err" && ! grep -q -e xyzzy -e plugh "$T/err"'

echo "settings: the same, a line that is not taken left out and said"
run settings 'WAN_VLAN=410\nWAN_VLAN=4095\nPASS_GATEWAY=169.254.0.1\nLAN_DHCP=on\n'
check '[ $rc -eq 0 ] && grep -qx "WAN_VLAN=410" "$T/out" && grep -qx "LAN_DHCP=on" "$T/out"'
check 'grep -qx "onucfg: WAN_VLAN: not a value of it; left out" "$T/err"'
check 'grep -qx "onucfg: PASS_GATEWAY: no such setting; left out" "$T/err"'
# It judges nothing: an account without a secret is no reason to fail.
run settings 'PPPOE2_USER=bench\nLAN_DHCP_START=203.0.113.9\n'
check '[ $rc -eq 0 ] && grep -qx "LAN_DHCP_START=203.0.113.9" "$T/out" && grep -q "^SESSION2_GEM=auto" "$T/out"'

echo "more than it takes, and what is not one of its words"
awk 'BEGIN { for (i = 0; i < 2000; i++) print "LAN_DHCP=on" }' >"$T/long"
"$ONUCFG" check <"$T/long" >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 1 ] && grep -q "too long" "$T/err"'
"$ONUCFG" check extra </dev/null >/dev/null 2>&1
rc=$?
check '[ $rc -eq 2 ]'

echo "test_onucfg_settings: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
