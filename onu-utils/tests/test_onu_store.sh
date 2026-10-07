#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-store, with onucfg of the build on a store in a file,
# an erased one as the flash would give, and a stand-in for ponid.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
STORE_SCRIPT=$HERE/scripts/onu-store
BUILD=${BUILD:-$HERE/build}
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
	export ONU_ROOT=$T/root ONU_STORE=$T/store
	mkdir -p "$ONU_ROOT/run" "$T/bin"
	# 64 KiB of erased flash.
	dd if=/dev/zero bs=1024 count=64 2>/dev/null | tr '\000' '\377' >"$ONU_STORE"
	printf '#!/bin/sh\nexit 0\n' >"$T/bin/logger"
	# ponid gives the shipped firmware's identity, until it is told to fail.
	printf '#!/bin/sh\n[ -e "%s/ponid.fail" ] && exit 1\n%s\n' "$T" \
		'printf "serial=XXXXXXXXXXXX\nploam_password=00112233445566778899\nmodel=TEST-1\n"' \
		>"$T/bin/ponid"
	chmod +x "$T/bin"/*
	PATH=$T/bin:$BUILD:$PATH
}

teardown() {
	rm -rf "$T"
}

# The exit status of the last run is in rc, which check() would lose.
run() {
	"$STORE_SCRIPT" "$@" 2>/dev/null
	rc=$?
}

stored() {
	onucfg --store "$ONU_STORE" list | grep -q -x -F -e "$1"
}

settings() {
	printf '%s\n' MODE=passthrough WAN_VLAN=410 >"$ONU_ROOT/run/onu.conf"
	echo "user@example.net s3cret" >"$ONU_ROOT/run/onu-pppoe"
}

echo "an empty store: the identity is the shipped firmware's"
setup
run load
check '[ $rc -eq 0 ]'
check 'grep -q -x serial=XXXXXXXXXXXX "$ONU_ROOT/run/onu-identity"'
check '[ ! -e "$ONU_ROOT/run/onu.conf" ]'
check '[ ! -e "$ONU_ROOT/run/onu-pppoe" ]'
check '[ "$(cat "$ONU_ROOT/run/onu-store")" = defaults ]'
teardown

echo "the first save takes the identity into the store with the settings"
setup
settings
run save
check '[ $rc -eq 0 ]'
check 'stored ID_SERIAL=XXXXXXXXXXXX'
check 'stored ID_PLOAM_PASSWORD=00112233445566778899'
check 'stored ID_MODEL=TEST-1'
check 'stored MODE=passthrough'
check 'stored WAN_VLAN=410'
check 'stored PPPOE_USER=user@example.net'
check 'stored PPPOE_SECRET=s3cret'
check '[ "$(cat "$ONU_ROOT/run/onu-store")" = saved ]'
teardown

echo "load gives back what was saved, the identity no longer from ponid"
setup
settings
run save
rm -f "$ONU_ROOT/run/onu.conf" "$ONU_ROOT/run/onu-pppoe"
: >"$T/ponid.fail"
run load
check '[ $rc -eq 0 ]'
check '[ "$(sort "$ONU_ROOT/run/onu-identity" | tr "\n" " ")" = "model=TEST-1 ploam_password=00112233445566778899 serial=XXXXXXXXXXXX " ]'
check '[ "$(cat "$ONU_ROOT/run/onu.conf" | tr "\n" " ")" = "MODE=passthrough WAN_VLAN=410 " ]'
check '[ "$(cat "$ONU_ROOT/run/onu-pppoe")" = "user@example.net s3cret" ]'
check '[ "$(ls -l "$ONU_ROOT/run/onu-identity" | cut -c1-10)" = -rw------- ]'
check '[ "$(ls -l "$ONU_ROOT/run/onu-pppoe" | cut -c1-10)" = -rw------- ]'
check '[ "$(ls -l "$ONU_ROOT/run/onu.conf" | cut -c1-10)" = -rw-r--r-- ]'
check '[ "$(cat "$ONU_ROOT/run/onu-store")" = loaded ]'
teardown

echo "a later save keeps the identity and changes the settings"
setup
settings
run save
: >"$T/ponid.fail"
printf '%s\n' MODE=router WAN_VLAN=410 >"$ONU_ROOT/run/onu.conf"
run save
check '[ $rc -eq 0 ]'
check 'stored ID_SERIAL=XXXXXXXXXXXX'
check 'stored MODE=router'
check '! stored MODE=passthrough'
teardown

echo "a save that changes nothing writes nothing"
setup
settings
run save
cp "$ONU_STORE" "$T/before"
run save
check '[ $rc -eq 0 ]'
check 'cmp -s "$ONU_STORE" "$T/before"'
teardown

echo "without the shipped identity, a save keeps the settings alone"
setup
settings
: >"$T/ponid.fail"
run save
check '[ $rc -eq 0 ]'
check 'stored MODE=passthrough'
check '! onucfg --store "$ONU_STORE" list | grep -q "^ID_"'
teardown

echo "migrate needs the shipped identity, and writes nothing without it"
setup
settings
: >"$T/ponid.fail"
cp "$ONU_STORE" "$T/before"
run migrate
check '[ $rc -ne 0 ]'
check 'cmp -s "$ONU_STORE" "$T/before"'
check '[ "$(cat "$ONU_ROOT/run/onu-store")" = failed ]'
rm -f "$T/ponid.fail"
run migrate
check '[ $rc -eq 0 ]'
check 'stored ID_SERIAL=XXXXXXXXXXXX'
cp "$ONU_STORE" "$T/before"
run migrate
check '[ $rc -eq 0 ]'
check 'cmp -s "$ONU_STORE" "$T/before"'
teardown

echo "an import with a bad line writes nothing"
setup
settings
run save
cp "$ONU_STORE" "$T/before"
printf 'MODE=router\nnot a setting\n' | onucfg --store "$ONU_STORE" import 2>/dev/null
rc=$?
check '[ $rc -ne 0 ]'
check 'cmp -s "$ONU_STORE" "$T/before"'
teardown

echo "a removed account leaves the store, and does not come back at boot"
setup
settings
run save
rm -f "$ONU_ROOT/run/onu-pppoe"
run save
check '[ $rc -eq 0 ]'
check 'stored PPPOE_USER='
check '! stored PPPOE_USER=user@example.net'
rm -f "$ONU_ROOT/run/onu.conf"
run load
check '[ ! -e "$ONU_ROOT/run/onu-pppoe" ]'
check 'grep -q -x MODE=passthrough "$ONU_ROOT/run/onu.conf"'
teardown

echo "the sign-in's hash goes to the store and to a file for root"
setup
settings
run save
run auth 'pbkdf2-sha256$1000$00ff$abcd'
check '[ $rc -eq 0 ]'
check 'stored AUTH_HASH=pbkdf2-sha256\$1000\$00ff\$abcd'
check '[ "$(cat "$ONU_ROOT/run/onu-auth")" = "pbkdf2-sha256\$1000\$00ff\$abcd" ]'
check '[ "$(ls -l "$ONU_ROOT/run/onu-auth" | cut -c1-10)" = -rw------- ]'
rm -f "$ONU_ROOT/run/onu-auth" "$ONU_ROOT/run/onu.conf"
run load
check '[ "$(cat "$ONU_ROOT/run/onu-auth")" = "pbkdf2-sha256\$1000\$00ff\$abcd" ]'
check '! grep -q AUTH "$ONU_ROOT/run/onu.conf"'
cp "$ONU_STORE" "$T/before"
run auth 'not a hash'
check '[ $rc -ne 0 ]'
check 'cmp -s "$ONU_STORE" "$T/before"'
teardown

echo "an identity from the status daemon goes to the store and to the running files"
setup
settings
run save
printf 'SERIAL=XXXXXXXXXXXX\nVERSION=V2.0\n' | "$STORE_SCRIPT" identity 2>/dev/null
rc=$?
check '[ $rc -eq 0 ]'
check 'stored ID_VERSION=V2.0'
check 'stored ID_MODEL=TEST-1'
check 'grep -q -x version=V2.0 "$ONU_ROOT/run/onu-identity"'
check 'grep -q -x model=TEST-1 "$ONU_ROOT/run/onu-identity"'
check '[ "$(ls -l "$ONU_ROOT/run/onu-identity" | cut -c1-10)" = -rw------- ]'
teardown

echo "test_onu_store: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
