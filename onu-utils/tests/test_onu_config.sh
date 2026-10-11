#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-config.sh, with onu-config of the build on a store in a
# file, empty at first, and stand-ins for ponid and for BusyBox's cryptpw.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
STORE_SCRIPT=$HERE/scripts/onu-config.sh
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
	: >"$ONU_STORE"
	printf '#!/bin/sh\nexit 0\n' >"$T/bin/logger"
	# ponid gives the shipped firmware's identity, until it is told to fail.
	printf '#!/bin/sh\n[ -e "%s/ponid.fail" ] && exit 1\n%s\n' "$T" \
		'printf "serial=XXXXXXXXXXXX\nploam_password=00112233445566778899\nmodel=TEST-1\n"' \
		>"$T/bin/ponid"
	# cryptpw -m sha256 -S SALT: "$5$SALT$" and the password's bytes in hex,
	# which shows what it was given.
	cat >"$T/bin/cryptpw" <<-'EOF'
	#!/bin/sh
	[ "$1 $2 $3" = "-m sha256 -S" ] || exit 2
	IFS= read -r p
	printf '$5$%s$%s\n' "$4" "$(printf %s "$p" | od -An -tx1 | tr -d ' \n')"
	EOF
	chmod +x "$T/bin"/*
	PATH=$T/bin:$BUILD:$PATH
	mkdir -p "$ONU_ROOT/etc"
	printf 'root::19000:0:99999:7:::\ndaemon:*:::::::\n' >"$ONU_ROOT/etc/shadow.image"
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
	onu-config --store "$ONU_STORE" list | grep -q -x -F -e "$1"
}

settings() {
	printf '%s\n' MODE=passthrough WAN_VLAN=410 >"$ONU_ROOT/run/onu.conf"
	echo "user@example.net s3cret" >"$ONU_ROOT/run/onu-pppoe1"
}

echo "an empty store: the identity is the shipped firmware's"
setup
run load
check '[ $rc -eq 0 ]'
check 'grep -q -x serial=XXXXXXXXXXXX "$ONU_ROOT/run/onu-identity"'
check '[ ! -e "$ONU_ROOT/run/onu.conf" ]'
check '[ ! -e "$ONU_ROOT/run/onu-pppoe1" ]'
check '[ "$(cat "$ONU_ROOT/run/onu-store")" = defaults ]'
check '[ "$(cat "$ONU_ROOT/run/onu-identity-source")" = factory ]'
teardown

echo "a save takes the settings and the accounts into the store, never the identity"
setup
settings
run save
check '[ $rc -eq 0 ]'
check 'stored MODE=passthrough'
check 'stored WAN_VLAN=410'
check 'stored PPPOE1_USER=user@example.net'
check 'stored PPPOE1_SECRET=s3cret'
check '! stored PPPOE2_USER='
check '! stored PPPOE_USER='
check '! onu-config --store "$ONU_STORE" list | grep -q "^ID_"'
check '[ "$(cat "$ONU_ROOT/run/onu-store")" = saved ]'
check '[ "$(ls -l "$ONU_STORE" | cut -c1-10)" = -rw------- ]'
teardown

echo "load gives back what was saved, and the identity of the shipped firmware"
setup
settings
run save
rm -f "$ONU_ROOT/run/onu.conf" "$ONU_ROOT/run/onu-pppoe1"
run load
check '[ $rc -eq 0 ]'
check '[ "$(sort "$ONU_ROOT/run/onu-identity" | tr "\n" " ")" = "model=TEST-1 ploam_password=00112233445566778899 serial=XXXXXXXXXXXX " ]'
check '[ "$(cat "$ONU_ROOT/run/onu.conf" | tr "\n" " ")" = "MODE=passthrough WAN_VLAN=410 " ]'
check '[ "$(cat "$ONU_ROOT/run/onu-pppoe1")" = "user@example.net s3cret" ]'
check '[ "$(ls -l "$ONU_ROOT/run/onu-identity" | cut -c1-10)" = -rw------- ]'
check '[ "$(ls -l "$ONU_ROOT/run/onu-pppoe1" | cut -c1-10)" = -rw------- ]'
check '[ "$(ls -l "$ONU_ROOT/run/onu.conf" | cut -c1-10)" = -rw-r--r-- ]'
check '[ "$(cat "$ONU_ROOT/run/onu-store")" = loaded ]'
check '[ "$(cat "$ONU_ROOT/run/onu-identity-source")" = factory ]'
teardown

echo "the store's ID_ keys go over the shipped firmware's identity"
setup
printf '%s\n' ID_SERIAL=YYYYYYYYYYYY ID_VERSION=V2.0 | onu-config --store "$ONU_STORE" import
run load
check '[ $rc -eq 0 ]'
check '[ "$(sort "$ONU_ROOT/run/onu-identity" | tr "\n" " ")" = "model=TEST-1 ploam_password=00112233445566778899 serial=YYYYYYYYYYYY version=V2.0 " ]'
check '[ "$(cat "$ONU_ROOT/run/onu-identity-source")" = store ]'
# Without the shipped firmware's files, the store's alone.
: >"$T/ponid.fail"
run load
check '[ $rc -eq 0 ]'
check '[ "$(sort "$ONU_ROOT/run/onu-identity" | tr "\n" " ")" = "serial=YYYYYYYYYYYY version=V2.0 " ]'
# A save keeps them.
settings
run save
check 'stored ID_SERIAL=YYYYYYYYYYYY'
check 'stored MODE=passthrough'
teardown

echo "with no identity anywhere, load says so"
setup
: >"$T/ponid.fail"
run load
check '[ $rc -ne 0 ]'
check '[ ! -s "$ONU_ROOT/run/onu-identity" ]'
teardown

echo "a later save changes the settings"
setup
settings
run save
printf '%s\n' MODE=router WAN_VLAN=410 >"$ONU_ROOT/run/onu.conf"
run save
check '[ $rc -eq 0 ]'
check 'stored MODE=router'
check '! stored MODE=passthrough'
teardown

echo "a setting no longer in force leaves the store, and nothing else does"
setup
settings
printf '%s\n' LAN_VLAN=36 PASS_GATEWAY=192.0.2.1 >>"$ONU_ROOT/run/onu.conf"
run save
check 'stored LAN_VLAN=36'
printf '%s\n' ID_SERIAL=XXXXXXXXXXXX SYS_HOSTNAME=stick PD2_HOST=host AUTH_HASH=hash |
	onu-config --store "$ONU_STORE" import
settings
run save
check '[ $rc -eq 0 ]'
check '! onu-config --store "$ONU_STORE" list | grep -q -e "^LAN_VLAN=" -e "^PASS_GATEWAY="'
check 'stored MODE=passthrough'
check 'stored WAN_VLAN=410'
check 'stored ID_SERIAL=XXXXXXXXXXXX'
check 'stored SYS_HOSTNAME=stick'
check 'stored PD2_HOST=host'
check 'stored AUTH_HASH=hash'
check 'stored PPPOE1_USER=user@example.net'
# Without the settings in force, a save takes none of the stored ones away.
: >"$ONU_ROOT/run/onu.conf"
run save
check 'stored MODE=passthrough'
rm -f "$ONU_ROOT/run/onu.conf"
run save
check '[ $rc -eq 0 ]'
check 'stored WAN_VLAN=410'
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

echo "an import with a bad line writes nothing"
setup
settings
run save
cp "$ONU_STORE" "$T/before"
printf 'MODE=router\nnot a setting\n' | onu-config --store "$ONU_STORE" import 2>/dev/null
rc=$?
check '[ $rc -ne 0 ]'
check 'cmp -s "$ONU_STORE" "$T/before"'
teardown

echo "an import removes the key of a -KEY line, and refuses a bad one"
setup
settings
run save
printf -- '-WAN_VLAN\nMODE=router\n' | onu-config --store "$ONU_STORE" import
rc=$?
check '[ $rc -eq 0 ]'
check '! onu-config --store "$ONU_STORE" list | grep -q "^WAN_VLAN="'
check 'stored MODE=router'
cp "$ONU_STORE" "$T/before"
printf -- '-MODE\n-not a key\n' | onu-config --store "$ONU_STORE" import 2>/dev/null
rc=$?
check '[ $rc -ne 0 ]'
check 'cmp -s "$ONU_STORE" "$T/before"'
teardown

echo "a removed account leaves the store, and does not come back at boot"
setup
settings
run save
rm -f "$ONU_ROOT/run/onu-pppoe1"
run save
check '[ $rc -eq 0 ]'
check 'stored PPPOE1_USER='
check '! stored PPPOE1_USER=user@example.net'
rm -f "$ONU_ROOT/run/onu.conf"
run load
check '[ ! -e "$ONU_ROOT/run/onu-pppoe1" ]'
check 'grep -q -x MODE=passthrough "$ONU_ROOT/run/onu.conf"'
teardown

echo "two sessions' accounts: each to its own file for root, and one removed"
setup
settings
echo "user@example.net/ipv6 s3cret6" >"$ONU_ROOT/run/onu-pppoe2"
printf '%s\n' SESSION1_FAMILY=ipv4 SESSION2_FAMILY=ipv6 >>"$ONU_ROOT/run/onu.conf"
run save
check 'stored PPPOE2_USER=user@example.net/ipv6'
check 'stored PPPOE2_SECRET=s3cret6'
check 'stored SESSION2_FAMILY=ipv6'
rm -f "$ONU_ROOT/run/onu-pppoe1" "$ONU_ROOT/run/onu-pppoe2"
run load
check '[ "$(cat "$ONU_ROOT/run/onu-pppoe2")" = "user@example.net/ipv6 s3cret6" ]'
check '[ "$(ls -l "$ONU_ROOT/run/onu-pppoe2" | cut -c1-10)" = -rw------- ]'
rm -f "$ONU_ROOT/run/onu-pppoe2"
run save
check 'stored PPPOE2_USER='
check 'stored PPPOE1_USER=user@example.net'
teardown

echo "the single account of before becomes the first session's"
setup
printf '%s\n' ID_SERIAL=XXXXXXXXXXXX PPPOE_USER=user@example.net PPPOE_SECRET=s3cret \
	MODE=passthrough | onu-config --store "$ONU_STORE" import
run load
check '[ "$(cat "$ONU_ROOT/run/onu-pppoe1")" = "user@example.net s3cret" ]'
check '! grep -q PPPOE "$ONU_ROOT/run/onu.conf"'
run save
check 'stored PPPOE1_USER=user@example.net'
check 'stored PPPOE1_SECRET=s3cret'
check 'stored PPPOE_USER='
check '! stored PPPOE_USER=user@example.net'
rm -f "$ONU_ROOT/run/onu-pppoe1"
run load
check '[ "$(cat "$ONU_ROOT/run/onu-pppoe1")" = "user@example.net s3cret" ]'
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

DEFAULT_HASH=$(sed -n "s/^DEFAULT_HASH='\(.*\)'$/\1/p" "$STORE_SCRIPT")
ROUNDS=$(sed -n 's/^ROUNDS=//p' "$STORE_SCRIPT")

root_hash() {
	awk -F: '$1 == "root" { print $2 }' "$ONU_ROOT/run/shadow"
}

echo "root's password: the default until one is set, in /run/shadow for login and ssh"
setup
run load
check '[ "$(root_hash)" = "$DEFAULT_HASH" ]'
check 'grep -q -x "daemon:\*:::::::" "$ONU_ROOT/run/shadow"'
check 'grep -q -x "root:$DEFAULT_HASH:19000:0:99999:7:::" "$ONU_ROOT/run/shadow"'
check '[ "$(ls -l "$ONU_ROOT/run/shadow" | cut -c1-10)" = -rw------- ]'
check '[ ! -e "$ONU_ROOT/run/onu-auth" ]'
check 'case $DEFAULT_HASH in "\$5\$rounds=$ROUNDS\$"*) true ;; *) false ;; esac'
teardown

echo "a new password is hashed into the store, /run/onu-auth and /run/shadow"
setup
printf '%s\n' 'correct horse' | "$STORE_SCRIPT" password 2>/dev/null
rc=$?
check '[ $rc -eq 0 ]'
hash=$(cat "$ONU_ROOT/run/onu-auth" 2>/dev/null)
check 'case $hash in "\$5\$rounds=$ROUNDS\$"????????????????"\$636f727265637420686f727365") true ;; *) false ;; esac'
check 'stored "AUTH_HASH=$hash"'
check '[ "$(root_hash)" = "$hash" ]'
check '[ "$(ls -l "$ONU_ROOT/run/onu-auth" | cut -c1-10)" = -rw------- ]'
rm -f "$ONU_ROOT/run/onu-auth" "$ONU_ROOT/run/shadow"
run load
check '[ "$(cat "$ONU_ROOT/run/onu-auth")" = "$hash" ] && [ "$(root_hash)" = "$hash" ]'
# A second one has a salt of its own.
printf '%s\n' 'correct horse' | "$STORE_SCRIPT" password 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-auth")" != "$hash" ]'
# Too short, too long, or with a control character: nothing changes.
cp "$ONU_STORE" "$T/before"
cp "$ONU_ROOT/run/shadow" "$T/shadow.before"
printf '%s\n' 'short' | "$STORE_SCRIPT" password 2>/dev/null
rc=$?
check '[ $rc -ne 0 ] && cmp -s "$ONU_STORE" "$T/before"'
printf '%0129d\n' 0 | "$STORE_SCRIPT" password 2>/dev/null
rc=$?
check '[ $rc -ne 0 ] && cmp -s "$ONU_STORE" "$T/before"'
printf 'tab\tinside it\n' | "$STORE_SCRIPT" password 2>/dev/null
rc=$?
check '[ $rc -ne 0 ] && cmp -s "$ONU_STORE" "$T/before"'
check 'cmp -s "$ONU_ROOT/run/shadow" "$T/shadow.before"'
teardown

echo "a hash of the panel's older form leaves root with keys only on ssh and no console password"
setup
run auth 'pbkdf2-sha256$1000$00ff$abcd'
check '[ "$(root_hash)" = "*" ]'
rm -f "$ONU_ROOT/run/shadow"
run load
check '[ "$(root_hash)" = "*" ]'
teardown

# The default hash is crypt()'s for "admin", where Perl's crypt() knows the
# SHA-256 form (the C library of Linux).
if perl -e 'exit(crypt("a", q($5$rounds=1000$ab)) =~ /^\$5\$/ ? 0 : 1)' 2>/dev/null; then
	echo "the default hash is the one of admin"
	check '[ "$(perl -e "print crypt(q(admin), \$ARGV[0])" "$DEFAULT_HASH")" = "$DEFAULT_HASH" ]'
fi

echo "an identity from the status daemon goes to the store and to the running files"
setup
settings
run save
printf 'SERIAL=XXXXXXXXXXXX\nVERSION=V2.0\n' | "$STORE_SCRIPT" identity 2>/dev/null
rc=$?
check '[ $rc -eq 0 ]'
check 'stored ID_VERSION=V2.0'
check 'stored ID_SERIAL=XXXXXXXXXXXX'
check '! stored ID_MODEL=TEST-1'
check 'grep -q -x version=V2.0 "$ONU_ROOT/run/onu-identity"'
check 'grep -q -x model=TEST-1 "$ONU_ROOT/run/onu-identity"'
check '[ "$(cat "$ONU_ROOT/run/onu-identity-source")" = store ]'
check '[ "$(ls -l "$ONU_ROOT/run/onu-identity" | cut -c1-10)" = -rw------- ]'
teardown

echo "the stick's own settings go to the store, to /run/onu-system, and come back at boot"
setup
settings
run save
printf 'HOSTNAME=stick-7\nMGMT_VLAN=100\nMGMT_ADDRESS=192.0.2.6/30\n' |
	"$STORE_SCRIPT" system 2>/dev/null
rc=$?
check '[ $rc -eq 0 ]'
check 'stored SYS_HOSTNAME=stick-7'
check 'stored SYS_MGMT_ADDRESS=192.0.2.6/30'
check 'grep -q -x HOSTNAME=stick-7 "$ONU_ROOT/run/onu-system"'
check 'grep -q -x MGMT_VLAN=100 "$ONU_ROOT/run/onu-system"'
check '[ "$(ls -l "$ONU_ROOT/run/onu-system" | cut -c1-10)" = -rw-r--r-- ]'
# A save of the data path keeps them, and they are no line of onu.conf.
run save
check 'stored SYS_HOSTNAME=stick-7'
rm -f "$ONU_ROOT/run/onu-system" "$ONU_ROOT/run/onu.conf"
run load
check 'grep -q -x MGMT_ADDRESS=192.0.2.6/30 "$ONU_ROOT/run/onu-system"'
check '! grep -q -e SYS_ -e HOSTNAME "$ONU_ROOT/run/onu.conf"'
# An empty value takes a setting away; a value of the wrong shape, or a key
# the boot script does not know, changes nothing.
printf 'HOSTNAME=\n' | "$STORE_SCRIPT" system 2>/dev/null
check '! grep -q HOSTNAME "$ONU_ROOT/run/onu-system"'
check 'grep -q -x MGMT_VLAN=100 "$ONU_ROOT/run/onu-system"'
cp "$ONU_STORE" "$T/before"
printf 'HOSTNAME=-bad\n' | "$STORE_SCRIPT" system 2>/dev/null
rc=$?
check '[ $rc -ne 0 ] && cmp -s "$ONU_STORE" "$T/before"'
printf 'MGMT_ADDRESS=192.0.2.6\n' | "$STORE_SCRIPT" system 2>/dev/null
rc=$?
check '[ $rc -ne 0 ] && cmp -s "$ONU_STORE" "$T/before"'
printf 'MODE=router\n' | "$STORE_SCRIPT" system 2>/dev/null
rc=$?
check '[ $rc -ne 0 ] && cmp -s "$ONU_STORE" "$T/before"'
# The time server and the server of the system log: an address or a name, the
# latter with a port; nothing that ntpd or syslogd would read as an option.
printf 'NTP_SERVER=192.0.2.123\nLOG_SERVER=[2001:db8::1]:5514\n' | "$STORE_SCRIPT" system 2>/dev/null
check 'grep -q -x NTP_SERVER=192.0.2.123 "$ONU_ROOT/run/onu-system"'
check 'grep -q -x "LOG_SERVER=\[2001:db8::1\]:5514" "$ONU_ROOT/run/onu-system"'
for good in NTP_SERVER=time.example.com NTP_SERVER=2001:db8::123 LOG_SERVER=192.0.2.9 \
	LOG_SERVER=192.0.2.9:5514 LOG_SERVER=logs.example.com:514 LOG_SERVER=2001:db8::9; do
	printf '%s\n' "$good" | "$STORE_SCRIPT" system 2>/dev/null
	check 'grep -q -x -F "$good" "$ONU_ROOT/run/onu-system"'
done
cp "$ONU_STORE" "$T/before"
for bad in NTP_SERVER=-d "NTP_SERVER=192.0.2.1 -d" 'NTP_SERVER=$(reboot)' NTP_SERVER=a/b \
	LOG_SERVER=-x "LOG_SERVER=192.0.2.9 514" "LOG_SERVER=[2001:db8::9" 'LOG_SERVER=a;b'; do
	printf '%s\n' "$bad" | "$STORE_SCRIPT" system 2>/dev/null
	rc=$?
	check '[ $rc -ne 0 ] && cmp -s "$ONU_STORE" "$T/before"'
done
printf 'NTP_SERVER=\n' | "$STORE_SCRIPT" system 2>/dev/null
check '! grep -q NTP_SERVER "$ONU_ROOT/run/onu-system"'
teardown
# A store without them at all leaves no file behind at boot.
setup
run load
check '[ ! -e "$ONU_ROOT/run/onu-system" ]'
teardown

echo "the host a session delegates its IPv6 prefix to, kept across a restart"
setup
B='000300010000005e0053ff 1 fe80::12 2001:db8:1200::/56'
echo "$B" >"$ONU_ROOT/run/onu-pd2"
run pd 2
check '[ $rc -eq 0 ]'
check "stored 'PD2_HOST=$B'"
# A save of the settings keeps it, and it is no line of onu.conf.
echo "MODE=router" >"$ONU_ROOT/run/onu.conf"
run save
check "stored 'PD2_HOST=$B'"
rm -f "$ONU_ROOT/run/onu-pd2" "$ONU_ROOT/run/onu.conf"
run load
check '[ "$(cat "$ONU_ROOT/run/onu-pd2")" = "$B" ]'
check '! grep -q PD "$ONU_ROOT/run/onu.conf"'
check '[ ! -e "$ONU_ROOT/run/onu-pd1" ]'
# The same again writes nothing.
cp "$ONU_STORE" "$T/before"
run pd 2
check '[ $rc -eq 0 ] && cmp -s "$ONU_STORE" "$T/before"'
# No file: the host is forgotten.
rm "$ONU_ROOT/run/onu-pd2"
run pd 2
check '[ $rc -eq 0 ] && ! onu-config --store "$ONU_STORE" list | grep -q ^PD2_'
# Not a binding, or not a session: nothing changes.
cp "$ONU_STORE" "$T/before"
echo 'x; reboot' >"$ONU_ROOT/run/onu-pd2"
run pd 2
check '[ $rc -ne 0 ] && cmp -s "$ONU_STORE" "$T/before"'
run pd 8
check '[ $rc -ne 0 ] && cmp -s "$ONU_STORE" "$T/before"'
teardown

echo "the store is a file of KEY=VALUE lines, at most 64 KiB, replaced whole"
setup
rm -f "$ONU_STORE"
check '[ -z "$(onu-config --store "$ONU_STORE" list)" ]'
onu-config --store "$ONU_STORE" set MODE router
check 'stored MODE=router'
check '[ "$(ls -l "$ONU_STORE" | cut -c1-10)" = -rw------- ] && [ ! -e "$ONU_STORE.new" ]'
# A last line without its newline is still a line.
printf 'MODE=router\nWAN_VLAN=410' >"$ONU_STORE"
onu-config --store "$ONU_STORE" set LAN_DHCP on
check '[ "$(cat "$ONU_STORE" | tr "\n" " ")" = "MODE=router WAN_VLAN=410 LAN_DHCP=on " ]'
# Not lines of keys: refused, and left as it is.
printf 'MODE=router\nsomething else\n' >"$ONU_STORE"
cp "$ONU_STORE" "$T/before"
onu-config --store "$ONU_STORE" list >/dev/null 2>&1
rc=$?
check '[ $rc -ne 0 ]'
onu-config --store "$ONU_STORE" set MODE bridge 2>/dev/null
rc=$?
check '[ $rc -ne 0 ] && cmp -s "$ONU_STORE" "$T/before"'
printf 'MODE=router\n\000\n' >"$ONU_STORE"
onu-config --store "$ONU_STORE" list >/dev/null 2>&1
rc=$?
check '[ $rc -ne 0 ]'
# A device is not a store.
onu-config --store /dev/null list >/dev/null 2>&1
rc=$?
check '[ $rc -ne 0 ]'
# 64 KiB at most: a change past it is refused, and a bigger file is.
: >"$ONU_STORE"
awk 'BEGIN { for (i = 0; i < 70; i++) { printf "K%03d=", i; for (j = 0; j < 1000; j++) printf "x"; print "" } }' |
	onu-config --store "$ONU_STORE" import 2>/dev/null
rc=$?
check '[ $rc -ne 0 ] && [ ! -s "$ONU_STORE" ]'
awk 'BEGIN { for (i = 0; i < 60; i++) { printf "K%03d=", i; for (j = 0; j < 1000; j++) printf "x"; print "" } }' |
	onu-config --store "$ONU_STORE" import
rc=$?
check '[ $rc -eq 0 ] && [ "$(wc -c <"$ONU_STORE")" -le 65536 ]'
awk 'BEGIN { for (i = 0; i < 70; i++) { printf "K%03d=", i; for (j = 0; j < 1000; j++) printf "x"; print "" } }' >"$ONU_STORE"
onu-config --store "$ONU_STORE" list >/dev/null 2>&1
rc=$?
check '[ $rc -ne 0 ]'
teardown

echo "without a store, load still gives the identity, and root's password is not known"
setup
rm -f "$ONU_STORE"
ONU_STORE= "$STORE_SCRIPT" load 2>/dev/null
rc=$?
check '[ $rc -eq 0 ]'
check '[ "$(cat "$ONU_ROOT/run/onu-store")" = none ]'
check 'grep -q -x serial=XXXXXXXXXXXX "$ONU_ROOT/run/onu-identity"'
check '[ "$(cat "$ONU_ROOT/run/onu-auth")" = "!" ]'
check '[ "$(ls -l "$ONU_ROOT/run/onu-auth" | cut -c1-10)" = -rw------- ]'
# The console's login alone takes the default password then.
check '[ "$(root_hash)" = "$DEFAULT_HASH" ]'
settings
ONU_STORE= "$STORE_SCRIPT" save 2>/dev/null
rc=$?
check '[ $rc -ne 0 ] && [ "$(cat "$ONU_ROOT/run/onu-store")" = failed ]'
teardown

echo "a store that cannot be read leaves root's password not known, not the default"
setup
printf 'AUTH_HASH=$5$rounds=1000$salt$hash\n' >"$ONU_STORE"
run load
check '[ "$(cat "$ONU_ROOT/run/onu-auth")" = "\$5\$rounds=1000\$salt\$hash" ]'
# A store over its limit is refused as a whole.
awk 'BEGIN { for (i = 0; i < 70; i++) { printf "K%03d=", i; for (j = 0; j < 1000; j++) printf "x"; print "" } }' >"$ONU_STORE"
run load
check '[ $rc -eq 0 ] && [ "$(cat "$ONU_ROOT/run/onu-auth")" = "!" ]'
check '[ "$(cat "$ONU_ROOT/run/onu-store")" = none ]'
check '[ "$(root_hash)" = "$DEFAULT_HASH" ]'
# Read again, the store gives its hash back.
printf 'AUTH_HASH=$5$rounds=1000$salt$hash\n' >"$ONU_STORE"
run load
check '[ "$(cat "$ONU_ROOT/run/onu-auth")" = "\$5\$rounds=1000\$salt\$hash" ]'
# And one without a hash is the default again, with no file.
: >"$ONU_STORE"
run load
check '[ ! -e "$ONU_ROOT/run/onu-auth" ] && [ "$(root_hash)" = "$DEFAULT_HASH" ]'
teardown

# The boot's own store: the partition "config", of /proc/mtd, mounted on
# /config. mountpoint and mount stand in: a mount leaves a mark in the
# directory, and fails while $T/mount.fail is there.
boot_setup() {
	setup
	mkdir -p "$ONU_ROOT/proc" "$ONU_ROOT/dev" "$ONU_ROOT/config"
	echo 'mtd4: 00010000 00001000 "config"' >"$ONU_ROOT/proc/mtd"
	printf '#!/bin/sh\n[ "$1" = -q ] && shift\n[ -e "$1/.mounted" ]\n' >"$T/bin/mountpoint"
	cat >"$T/bin/mount" <<-EOF
	#!/bin/sh
	[ -e "$T/mount.fail" ] && exit 32
	echo "\$*" >>"$T/mount.args"
	for d; do :; done
	: >"\$d/.mounted"
	EOF
	chmod +x "$T/bin/mountpoint" "$T/bin/mount"
	dd if=/dev/zero bs=1024 count=64 2>/dev/null | tr '\000' '\377' >"$T/erased"
	BOOT_STORE=$ONU_ROOT/config/open-dfp-34x-2c.conf
}

boot_load() {
	ONU_STORE= "$STORE_SCRIPT" load 2>/dev/null
	rc=$?
}

echo "at boot: the store of before goes into the file, and the partition is mounted"
boot_setup
printf '%s\n' ID_SERIAL=YYYYYYYYYYYY MODE=passthrough WAN_VLAN=410 PPPOE1_USER=user@example.net \
	PPPOE1_SECRET=s3cret AUTH_HASH=hash | mkslots "$ONU_ROOT/dev/mtd4" 0x10000
boot_load
check '[ $rc -eq 0 ]'
check 'cmp -s "$ONU_ROOT/dev/mtd4" "$T/erased"'
check 'grep -q -x "\-t jffs2 -o nosuid,nodev,noexec mtd:config $ONU_ROOT/config" "$T/mount.args"'
check 'onu-config --store "$BOOT_STORE" list | grep -q -x MODE=passthrough'
check 'onu-config --store "$BOOT_STORE" list | grep -q -x ID_SERIAL=YYYYYYYYYYYY'
check '[ "$(ls -l "$BOOT_STORE" | cut -c1-10)" = -rw------- ]'
check '[ ! -e "$ONU_ROOT/run/onu-store.before" ]'
check '[ "$(cat "$ONU_ROOT/run/onu.conf" | tr "\n" " ")" = "MODE=passthrough WAN_VLAN=410 " ]'
check '[ "$(cat "$ONU_ROOT/run/onu-pppoe1")" = "user@example.net s3cret" ]'
check '[ "$(cat "$ONU_ROOT/run/onu-auth")" = hash ]'
check 'grep -q -x serial=YYYYYYYYYYYY "$ONU_ROOT/run/onu-identity"'
check 'grep -q -x model=TEST-1 "$ONU_ROOT/run/onu-identity"'
check '[ "$(cat "$ONU_ROOT/run/onu-store")" = loaded ]'
# The next boot mounts the partition as it is, and finds the file.
rm -f "$ONU_ROOT/config/.mounted" "$ONU_ROOT/run/onu.conf" "$T/mount.args"
boot_load
check '[ $rc -eq 0 ] && [ "$(wc -l <"$T/mount.args")" -eq 1 ]'
check 'grep -q -x MODE=passthrough "$ONU_ROOT/run/onu.conf"'
# A save writes the file.
printf '%s\n' MODE=router >"$ONU_ROOT/run/onu.conf"
ONU_STORE= "$STORE_SCRIPT" save 2>/dev/null
check 'onu-config --store "$BOOT_STORE" list | grep -q -x MODE=router'
teardown

echo "at boot: a partition without the store of before is mounted as it is"
boot_setup
printf 'not a store of before' >"$ONU_ROOT/dev/mtd4"
dd if=/dev/zero bs=1024 count=63 2>/dev/null >>"$ONU_ROOT/dev/mtd4"
cp "$ONU_ROOT/dev/mtd4" "$T/before"
boot_load
check '[ $rc -eq 0 ] && [ "$(cat "$ONU_ROOT/run/onu-store")" = defaults ]'
check 'cmp -s "$ONU_ROOT/dev/mtd4" "$T/before"'
check '[ -e "$ONU_ROOT/config/.mounted" ]'
check 'grep -q -x serial=XXXXXXXXXXXX "$ONU_ROOT/run/onu-identity"'
# erase-slots refuses it.
onu-config erase-slots "$ONU_ROOT/dev/mtd4" 2>/dev/null
rc=$?
check '[ $rc -eq 3 ] && cmp -s "$ONU_ROOT/dev/mtd4" "$T/before"'
teardown

echo "at boot: a partition that cannot be mounted leaves no store, and the identity"
boot_setup
cp "$T/erased" "$ONU_ROOT/dev/mtd4"
: >"$T/mount.fail"
boot_load
check '[ $rc -eq 0 ] && [ "$(cat "$ONU_ROOT/run/onu-store")" = none ]'
check 'grep -q -x serial=XXXXXXXXXXXX "$ONU_ROOT/run/onu-identity"'
teardown

echo "at boot: when the store of before cannot be mounted after its erase, it stays in /run"
boot_setup
printf '%s\n' MODE=passthrough | mkslots "$ONU_ROOT/dev/mtd4" 0x10000
: >"$T/mount.fail"
boot_load
check '[ "$(cat "$ONU_ROOT/run/onu-store.before")" = MODE=passthrough ]'
check '[ "$(ls -l "$ONU_ROOT/run/onu-store.before" | cut -c1-10)" = -rw------- ]'
check '[ "$(cat "$ONU_ROOT/run/onu-store")" = none ]'
teardown

echo "the panel's key: made once, kept in the store, back at boot, never a setting"
setup
run tls
check '[ $rc -eq 0 ]'
key=$(cat "$ONU_ROOT/run/onu-tls-key" 2>/dev/null)
check 'echo "$key" | grep -q -x "[0-9a-f]\{64\}"'
check '[ "$(ls -l "$ONU_ROOT/run/onu-tls-key" | cut -c1-10)" = -rw------- ]'
check 'stored "TLS_KEY=$key"'
run tls
check '[ $rc -eq 0 ] && [ "$(cat "$ONU_ROOT/run/onu-tls-key")" = "$key" ]'
rm -f "$ONU_ROOT/run/onu-tls-key"
run load
check '[ "$(cat "$ONU_ROOT/run/onu-tls-key")" = "$key" ]'
check '[ "$(ls -l "$ONU_ROOT/run/onu-tls-key" | cut -c1-10)" = -rw------- ]'
check '! grep -q "^TLS_" "$ONU_ROOT/run/onu.conf" 2>/dev/null'
settings
run save
check '[ $rc -eq 0 ] && stored "TLS_KEY=$key" && stored MODE=passthrough'
# The order of the curve is no key; neither is 0: a new one replaces it.
echo ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551 >"$ONU_ROOT/run/onu-tls-key"
run tls
new=$(cat "$ONU_ROOT/run/onu-tls-key")
check '[ $rc -eq 0 ] && [ "$new" != ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551 ]'
check 'stored "TLS_KEY=$new" && ! stored "TLS_KEY=$key"'
printf '%064d\n' 0 >"$ONU_ROOT/run/onu-tls-key"
run tls
check '[ $rc -eq 0 ] && [ "$(cat "$ONU_ROOT/run/onu-tls-key")" != "$(printf %064d 0)" ]'
# A store that lost the key takes the file of before away at boot.
onu-config --store "$ONU_STORE" unset TLS_KEY
run load
check '[ ! -e "$ONU_ROOT/run/onu-tls-key" ]'
teardown

echo "without a store, the panel's key lasts until the next start"
setup
rm -f "$ONU_STORE"
ONU_STORE= "$STORE_SCRIPT" tls 2>"$T/err"
rc=$?
check '[ $rc -eq 0 ] && grep -q -x "[0-9a-f]\{64\}" "$ONU_ROOT/run/onu-tls-key"'
check 'grep -q "not kept: no store" "$T/err"'
teardown

echo "test_onu_config: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
