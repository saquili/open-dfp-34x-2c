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
check 'grep -qx "SESSION1_GEM=auto" "$T/out" && grep -qx "SESSION1_VLAN=410" "$T/out"'
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
run settings 'PPPOE1_USER=a\nWAN_VLAN=410\nWAN_VLAN=4095\nPASS_GATEWAY=169.254.0.1\nLAN_DHCP=on\n'
# The VLAN of before is the first session's; the router's key goes.
check '[ $rc -eq 0 ] && grep -qx "SESSION1_VLAN=410" "$T/out" && ! grep -q "LAN_DHCP" "$T/out"'
check 'grep -qx "onu-config: WAN_VLAN: not a value of it; left out" "$T/err"'
check 'grep -qx "onu-config: PASS_GATEWAY: no such setting; left out" "$T/err"'
# It judges nothing: an account without a secret is no reason to fail.
run settings 'PPPOE2_USER=bench\nSESSION2_VLAN=40\n'
check '[ $rc -eq 0 ] && grep -qx "SESSION2_VLAN=40" "$T/out" && grep -q "^SESSION2_GEM=auto" "$T/out"'

echo "defaults: the table's default of every key, which a session with nothing set has"
"$ONUCFG" defaults </dev/null >"$T/dflt" 2>"$T/err"
rc=$?
check '[ $rc -eq 0 ] && [ ! -s "$T/err" ] && [ "$(wc -l <"$T/dflt")" -eq 50 ]'
check 'grep -qx "WAN_GEM=auto" "$T/dflt" && grep -qx "SESSION7_FAMILY=both" "$T/dflt"'
check 'grep -qx "SESSION4_PASS=on" "$T/dflt" && grep -qx "SESSION2_HOST_VLAN=" "$T/dflt"'
check 'grep -qx "FW_OUT=allow" "$T/dflt" && grep -qx "FW_BETWEEN=block" "$T/dflt"'
check 'grep -qx "FW_PING_IN=block" "$T/dflt" && grep -qx "FW_PING_LANS=block" "$T/dflt"'
# A session with an account and nothing else set has those defaults, and no
# other program needs to know them.
run settings 'PPPOE1_USER=a\n'
check 'grep "^SESSION1_" "$T/dflt" | cmp -s - "$T/out"'
"$ONUCFG" defaults extra </dev/null >/dev/null 2>&1
rc=$?
check '[ $rc -eq 2 ]'

echo "more than it takes, and what is not one of its words"
# More than the store holds, 64 KiB.
awk 'BEGIN { for (i = 0; i < 4000; i++) print "SESSION1_PASS=on" }' >"$T/long"
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
check '[ $rc -eq 0 ] && grep -qx "SESSION1_VLAN=410" "$T/out" && grep -qx "SESSION1_PASS=on" "$T/out"'
check 'grep -qx "PPPOE1_USER=bench" "$T/out" && grep -qx "PPPOE1_SECRET_SET=1" "$T/out"'
check '! grep -q not-a-real-one "$T/out" && ! grep -q "^PPPOE2" "$T/out"'
check 'grep -qx "onu-config: WAN_VLAN: not a value of it; left out" "$T/err"'

echo "change: over the settings in force, written aside until onu-apply try"
P=$R/run/onu-pending
# What onu-apply try does with it, under its lock (pending_take()).
take() {
	for f in "$P"/onu-pppoe*; do
		[ -e "$f" ] || continue
		if [ -s "$f" ]; then mv "$f" "$R/run/"; else rm -f "$f" "$R/run/${f##*/}"; fi
	done
	mv "$P/onu.conf" "$R/run/onu.conf"
	rm -rf "$P"
}
cp "$R/run/onu.conf" "$T/conf.in-force"
printf 'SESSION1_VLAN=36\n' | ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 0 ] && grep -qx "SESSION1_VLAN=36" "$P/onu.conf" && cmp -s "$R/run/onu.conf" "$T/conf.in-force"'
check '[ "$(stat -c %a "$P")" = 700 ] && [ "$(stat -c %a "$P/onu.conf")" = 644 ] && [ ! -e "$P.new" ]'
check '[ ! -e "$P/onu-pppoe1" ] && ! grep -q -e PPPOE -e bench "$P/onu.conf"'
# A second change before a try replaces the first whole.
printf 'SESSION1_VLAN=37\n' | ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
check 'grep -qx "SESSION1_VLAN=37" "$P/onu.conf"'
printf 'SESSION1_VLAN=36\n' | ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
take
check 'grep -qx "SESSION1_VLAN=36" "$R/run/onu.conf" && [ "$(cat "$R/run/onu-pppoe1")" = "bench not-a-real-one" ]'
printf 'PPPOE2_USER=second\nPPPOE2_SECRET=s2-secret\nSESSION2_VLAN=40\n' |
	ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 0 ] && [ "$(cat "$P/onu-pppoe2")" = "second s2-secret" ] && [ ! -e "$R/run/onu-pppoe2" ]'
check '[ "$(stat -c %a "$P/onu-pppoe2")" = 600 ] && grep -qx "SESSION2_VLAN=40" "$P/onu.conf"'
check '! grep -q s2-secret "$P/onu.conf" "$T/out" "$T/err"'
take
check '[ "$(cat "$R/run/onu-pppoe2")" = "second s2-secret" ]'
printf 'PPPOE2_USER=\n' | ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 0 ] && [ -e "$P/onu-pppoe2" ] && [ ! -s "$P/onu-pppoe2" ] && ! grep -q "^SESSION2_" "$P/onu.conf"'
take
check '[ ! -e "$R/run/onu-pppoe2" ] && [ -e "$R/run/onu-pppoe1" ] && grep -qx "SESSION1_VLAN=36" "$R/run/onu.conf"'

echo "change refuses, says why, and writes nothing"
cp "$R/run/onu.conf" "$T/conf.before"
printf 'WAN_VLAN=4095\n' | ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 1 ] && grep -qx "onu-config: WAN_VLAN: not a value of it" "$T/err" && cmp -s "$R/run/onu.conf" "$T/conf.before" && [ ! -e "$P" ]'
printf 'PPPOE3_USER=third\n' | ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 1 ] && grep -q PPPOE3_SECRET "$T/err" && [ ! -e "$R/run/onu-pppoe3" ] && cmp -s "$R/run/onu.conf" "$T/conf.before"'
"$ONUCFG" current extra </dev/null >/dev/null 2>&1
rc=$?
check '[ $rc -eq 2 ]'
# The management VLAN in use, and one on trial, are no session's.
printf 'VLAN=4094\nADDRESS=198.51.100.2/30\n' >"$R/run/onu-mgmt"
mkdir -p "$R/run/onu-trial.d"
printf 'VLAN=100\nADDRESS=192.0.2.6/30\n' >"$R/run/onu-trial.d/mgmt"
printf 'SESSION1_VLAN=4094\n' | ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 1 ] && grep -q "session 1: VLAN 4094 is the management VLAN" "$T/err" && cmp -s "$R/run/onu.conf" "$T/conf.before"'
printf 'SESSION1_HOST_VLAN=100\n' | ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 1 ] && grep -q "VLAN 100 is the management VLAN" "$T/err"'
rm -rf "$R/run/onu-trial.d"
printf 'SESSION1_HOST_VLAN=100\n' | ONU_ROOT=$R "$ONUCFG" change >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 0 ] && grep -qx "SESSION1_HOST_VLAN=100" "$P/onu.conf"'

echo "password-state and password-check: root's hash, which they keep to themselves"
rm -f "$R/run/onu-auth"
check '[ "$(ONU_ROOT=$R "$ONUCFG" password-state </dev/null)" = default ]'
pwcheck() {
	printf '%s\n' "$1" | ONU_ROOT=$R "$ONUCFG" password-check >"$T/out" 2>&1
	rc=$?
}
pwcheck admin
check '[ $rc -eq 3 ] && [ ! -s "$T/out" ]'
pwcheck admin2
check '[ $rc -eq 1 ]'
perl -e 'print crypt($ARGV[0], q($5$rounds=1000$teststandinsalt)), "\n"' example-secret >"$R/run/onu-auth"
check '[ "$(ONU_ROOT=$R "$ONUCFG" password-state </dev/null)" = set ]'
pwcheck example-secret
check '[ $rc -eq 0 ] && [ ! -s "$T/out" ]'
pwcheck admin
check '[ $rc -eq 1 ]'
printf 'pbkdf2-sha256$1000$0102030405060708090a0b0c0d0e0f10$%s\n' \
	682b24383b17ce055ed70ead8c23895db05d4a3717dffd00133fc69d366d3627 >"$R/run/onu-auth"
pwcheck example-secret
check '[ $rc -eq 4 ]'
pwcheck example-secreT
check '[ $rc -eq 1 ]'
echo '!' >"$R/run/onu-auth"
check '[ "$(ONU_ROOT=$R "$ONUCFG" password-state </dev/null)" = unknown ]'
pwcheck admin
check '[ $rc -eq 5 ] && [ ! -s "$T/out" ]'
pwcheck example-secret
check '[ $rc -eq 5 ]'

echo "identity-current: the identity, without its passwords"
printf 'serial=000000000000\nploam_password=not-a-real-one\nloid_password=\nmodel=stick\n' >"$R/run/onu-identity"
printf 'store\n' >"$R/run/onu-identity-source"
ONU_ROOT=$R "$ONUCFG" identity-current </dev/null >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 0 ] && grep -qx "serial=000000000000" "$T/out" && grep -qx "model=stick" "$T/out"'
check 'grep -qx "ploam_set=1" "$T/out" && grep -qx "loid_password_set=0" "$T/out" && grep -qx "source=store" "$T/out"'
check '! grep -q -e not-a-real-one -e "^ploam_password" -e "^loid_password=" "$T/out"'
rm -f "$R/run/onu-identity" "$R/run/onu-identity-source"
ONU_ROOT=$R "$ONUCFG" identity-current </dev/null >"$T/out" 2>"$T/err"
rc=$?
check '[ $rc -eq 0 ] && [ "$(cat "$T/out")" = "$(printf "ploam_set=0\nloid_password_set=0\nsource=")" ]'

echo "test_onu_config_settings: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
