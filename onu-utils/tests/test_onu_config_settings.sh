#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of "onu-config check" and "onu-config settings", the settings of the data path
# as the status daemon asks for them. BUILD names the directory of onu-config.

set -u
ONUCFG=${BUILD:-build}/onu-config
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
unset ONU_STORE
run check 'PPPOE1_USER=bench\nPPPOE1_SECRET=not-a-real-one\nSESSION1_VLAN=410\n'
check '[ $rc -eq 0 ] && [ ! -s "$T/err" ]'
check 'grep -qx "LAN_ADDRESS=192.168.1.1/24" "$T/out" && grep -qx "SESSION1_VLAN=410" "$T/out"'
check '! grep -q -e bench -e not-a-real-one -e PPPOE "$T/out"'
check '[ "$(grep -c "^SESSION1_" "$T/out")" -eq 6 ] && ! grep -q "^SESSION2_" "$T/out"'

echo "check refuses, says why, and prints no settings"
run check 'WAN_VLAN=410\nWAN_VLAN=4095\n'
check '[ $rc -eq 1 ] && [ ! -s "$T/out" ] && grep -qx "onu-config: WAN_VLAN: not a value of it" "$T/err"'
run check 'PPPOE2_USER=bench\n'
check '[ $rc -eq 1 ] && grep -q "PPPOE2_SECRET" "$T/err"'
run check 'PPPOE2_USER=bench\nPPPOE2_SECRET=xyzzy plugh\n'
check '[ $rc -eq 1 ] && grep -q "PPPOE2_SECRET" "$T/err" && ! grep -q -e xyzzy -e plugh "$T/err"'

echo "settings: the same, a line that is not taken left out and said"
run settings 'WAN_VLAN=410\nWAN_VLAN=4095\nPASS_GATEWAY=169.254.0.1\nLAN_DHCP=on\n'
check '[ $rc -eq 0 ] && grep -qx "WAN_VLAN=410" "$T/out" && grep -qx "LAN_DHCP=on" "$T/out"'
check 'grep -qx "onu-config: WAN_VLAN: not a value of it; left out" "$T/err"'
check 'grep -qx "onu-config: PASS_GATEWAY: no such setting; left out" "$T/err"'
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

echo "the files' verbs go to onu-config.sh of ONU_LIB, the store with them"
mkdir -p "$T/lib"
printf '#!/bin/sh\necho "$0 $* [${ONU_STORE:-}]" >"%s/ran"\n' "$T" >"$T/lib/onu-config.sh"
chmod +x "$T/lib/onu-config.sh"
ONU_LIB=$T/lib "$ONUCFG" load </dev/null
check '[ "$(cat "$T/ran")" = "$T/lib/onu-config.sh load []" ]'
ONU_LIB=$T/lib "$ONUCFG" --store /run/x pd 2 </dev/null
check '[ "$(cat "$T/ran")" = "$T/lib/onu-config.sh pd 2 [/run/x]" ]'
ONU_LIB=$T/nowhere "$ONUCFG" save </dev/null 2>/dev/null
rc=$?
check '[ $rc -eq 127 ]'

echo "current: the settings in force, each account without its secret"
R=$T/root
mkdir -p "$R/run"
printf '# a remark\nSESSION1_VLAN=410\nWAN_VLAN=4095\n' >"$R/run/onu.conf"
printf 'bench not-a-real-one\n' >"$R/run/onu-pppoe1"
chmod 600 "$R/run/onu-pppoe1"
ONU_ROOT=$R "$ONUCFG" current </dev/null >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 0 ] && grep -qx "SESSION1_VLAN=410" "$T/out" && grep -qx "LAN_DHCP=on" "$T/out"'
check 'grep -qx "PPPOE1_USER=bench" "$T/out" && grep -qx "PPPOE1_SECRET_SET=1" "$T/out"'
check '! grep -q not-a-real-one "$T/out" && ! grep -q "^PPPOE2" "$T/out"'
check 'grep -qx "onu-config: WAN_VLAN: not a value of it; left out" "$T/err"'

echo "change: over the settings in force, written to their files"
printf 'SESSION1_VLAN=36\n' | ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 0 ] && grep -qx "SESSION1_VLAN=36" "$R/run/onu.conf"'
check '[ "$(cat "$R/run/onu-pppoe1")" = "bench not-a-real-one" ] && ! grep -q -e PPPOE -e bench "$R/run/onu.conf"'
check '[ ! -e "$R/run/onu.conf.new" ] && [ "$(stat -c %a "$R/run/onu.conf")" = 644 ]'
printf 'PPPOE2_USER=second\nPPPOE2_SECRET=s2-secret\nSESSION2_VLAN=40\n' |
	ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 0 ] && [ "$(cat "$R/run/onu-pppoe2")" = "second s2-secret" ]'
check '[ "$(stat -c %a "$R/run/onu-pppoe2")" = 600 ] && grep -qx "SESSION2_VLAN=40" "$R/run/onu.conf"'
check '! grep -q s2-secret "$R/run/onu.conf" "$T/out" "$T/err"'
printf 'PPPOE2_USER=\n' | ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 0 ] && [ ! -e "$R/run/onu-pppoe2" ] && ! grep -q "^SESSION2_" "$R/run/onu.conf"'
check '[ -e "$R/run/onu-pppoe1" ] && grep -qx "SESSION1_VLAN=36" "$R/run/onu.conf"'

echo "change refuses, says why, and writes nothing"
cp "$R/run/onu.conf" "$T/conf.before"
printf 'WAN_VLAN=4095\n' | ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 1 ] && grep -qx "onu-config: WAN_VLAN: not a value of it" "$T/err" && cmp -s "$R/run/onu.conf" "$T/conf.before"'
printf 'PPPOE3_USER=third\n' | ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 1 ] && grep -q PPPOE3_SECRET "$T/err" && [ ! -e "$R/run/onu-pppoe3" ] && cmp -s "$R/run/onu.conf" "$T/conf.before"'
"$ONUCFG" current extra </dev/null >/dev/null 2>&1
rc=$?
check '[ $rc -eq 2 ]'

echo "test_onu_config_settings: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
