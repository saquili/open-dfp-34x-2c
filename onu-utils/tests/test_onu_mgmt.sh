#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-mgmt, with stand-ins for onu-nl, dropbear, onu-net,
# onu-config and logger, which log what they are asked to do. The stand-in of
# onu-net keeps the part "mgmt" of netifd's configuration in $T/part, and its
# wait fails while $T/no-address is there.

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
	mkdir -p "$ONU_ROOT/run" "$ONU_ROOT/proc/net/vlan" "$T/bin"
	: >"$LOG"
	echo "500.25 400.00" >"$ONU_ROOT/proc/uptime"
	printf 'VLAN Dev name\t | VLAN ID\nName-Type: VLAN_NAME_TYPE_RAW_PLUS_VID_NO_PAD\nmgmt           | 4094  | sfp\n' \
		>"$ONU_ROOT/proc/net/vlan/config"
	printf 'VLAN=4094\nADDRESS=198.51.100.2/30\nIFACE=mgmt\nNAME=mgmt\nPREF=1\nDATA_ADDRESS=\n' \
		>"$ONU_ROOT/run/onu-mgmt"
	# The ssh server of the boot: a process to be stopped.
	sleep 60 &
	OLD_SSH=$!
	echo "$OLD_SSH" >"$ONU_ROOT/run/dropbear.pid"
	# timed, with the router of the boot's link for its time server and the
	# link's interface for PTP.
	sleep 60 &
	OLD_TIME=$!
	echo "$OLD_TIME" >"$ONU_ROOT/run/timed.pid"
	for c in onu-nl logger onu-apply; do
		printf '#!/bin/sh\necho "%s $*" >>"$LOG"\n' "$c" >"$T/bin/$c"
	done
	printf '#!/bin/sh\necho "onu-net $*" >>"$LOG"\ncase $1 in\nset) cat >"%s/part" ;;\nwait) [ ! -e "%s/no-address" ] ;;\nesac\n' \
		"$T" "$T" >"$T/bin/onu-net"
	printf '#!/bin/sh\necho "dropbear $*" >>"$LOG"\nwhile [ $# -gt 0 ]; do [ "$1" = -P ] && echo 22 >"$2"; shift; done\n' \
		>"$T/bin/dropbear"
	printf '#!/bin/sh\n[ "$1" = system ] && cat >>"%s/stored"\n' "$T" >"$T/bin/onu-config"
	chmod +x "$T/bin"/*
	PATH=$T/bin:$PATH
}

teardown() {
	kill "$OLD_SSH" "$OLD_TIME" 2>/dev/null
	rm -rf "$T"
}

run() {
	out=$("$MGMT" "$@" 2>/dev/null)
	rc=$?
}

logged() {
	grep -q -x -F -- "$1" "$LOG"
}

# The interfaces and the devices of the part, one a line: "interface NAME
# DEVICE ADDRESS" and "device NAME VID".
part() {
	awk '/^config interface/ { kind = "interface"; name = $3 }
		/^config device/ { kind = "device"; name = "" }
		/option name/ { name = $3 }
		/option device/ { dev = $3 }
		/option vid/ { vid = $3 }
		/option ipaddr/ { print kind, name, dev, $3 }
		/option ipv6/ { print kind, name, vid }' "$T/part" | tr -d "'"
}

in_part() {
	part | grep -q -x -F -- "$1"
}

echo "the pair of the boot"
setup
rm -f "$ONU_ROOT/run/onu-mgmt"
run start 4094 198.51.100.2/30 192.0.2.2/30
check '[ $rc -eq 0 ]'
check 'grep -qx IFACE=mgmt "$ONU_ROOT/run/onu-mgmt" && grep -qx NAME=mgmt "$ONU_ROOT/run/onu-mgmt"'
check 'grep -qx DATA_ADDRESS=192.0.2.2/30 "$ONU_ROOT/run/onu-mgmt"'
check 'in_part "device mgmt 4094"'
check 'in_part "interface mgmt mgmt 198.51.100.2/30"'
# The address on the SFP port is netifd's too, as the interface "data".
check 'in_part "interface data sfp 192.0.2.2/30"'
check '[ "$(part | wc -l)" -eq 3 ]'
# The sessions keep their packets off the pair's networks.
check 'logged "onu-apply guard"'
# The switch keeps the VLAN from the PON, at the first priority: this script
# is the only writer of it, the boot script no longer is.
check 'logged "onu-nl clsact add sfp" && logged "onu-nl clsact add pon"'
check 'logged "onu-nl filter add sfp pref 1 vlan 4094 hw trap"'
check 'logged "onu-nl filter add pon pref 1 vlan 4094 hw drop"'
# A second start leaves what the first did, and does not stack rules.
: >"$LOG"
run start 4094 198.51.100.2/30 192.0.2.2/30
check 'logged "onu-nl filter del sfp pref 1" && logged "onu-nl filter del pon pref 1"'
check '[ "$(grep -c "^onu-nl filter add sfp pref 1 " "$LOG")" -eq 1 ]'
check 'grep -q "option ipv6 .1." "$T/part"'
run status
check 'echo "$out" | grep -qx VLAN=4094'
check 'echo "$out" | grep -qx ADDRESS=198.51.100.2/30'
check '! echo "$out" | grep -q PENDING_'
teardown

echo "what is not a pair does not start"
setup
for bad in "0 198.51.100.2/30" "4094 198.51.100.2" "x y"; do
	# shellcheck disable=SC2086
	run start $bad
	check '[ $rc -eq 2 ]'
done
check '[ ! -e "$T/part" ] && grep -qx VLAN=4094 "$ONU_ROOT/run/onu-mgmt"'
teardown

echo "a new VLAN beside the old one, then committed"
setup
run add 100 192.0.2.6/30
check '[ $rc -eq 0 ]'
name=$(sed -n 's/^NAME=//p' "$ONU_ROOT/run/onu-trial.d/mgmt")
check '[ -n "$name" ] && [ "$name" != mgmt ]'
check 'in_part "device mgmt 4094" && in_part "interface mgmt mgmt 198.51.100.2/30"'
check 'in_part "device mgmt100 100" && in_part "interface $name mgmt100 192.0.2.6/30"'
check 'logged "onu-net wait mgmt100 192.0.2.6/30 10"'
check 'logged "onu-nl filter add sfp pref 2 vlan 100 hw trap"'
check 'logged "onu-nl filter add pon pref 2 vlan 100 hw drop"'
check 'grep -q -- "-p 192.0.2.6:22" "$LOG"'
# Root's password as well as keys, as the boot's ssh server (ADR 0012).
check '! grep -q "^dropbear .*-[sB]\b" "$LOG"'
run status
check 'echo "$out" | grep -qx PENDING_VLAN=100'
check 'echo "$out" | grep -qx PENDING_UNTIL=1100'
check 'echo "$out" | grep -qx VLAN=4094'
run add 200 192.0.2.10/30
check '[ $rc -ne 0 ]'
run commit
check '[ $rc -eq 0 ]'
check '[ "$(grep -c -x "onu-apply guard" "$LOG")" -ge 2 ]'
check 'grep -qx MGMT_VLAN=100 "$T/stored" && grep -qx MGMT_ADDRESS=192.0.2.6/30 "$T/stored"'
check 'logged "onu-nl filter del sfp pref 1"'
check '[ "$(part)" = "$(printf "device mgmt100 100\ninterface $name mgmt100 192.0.2.6/30")" ]'
# The server of the boot ends, and procd starts it again on the new pair;
# onu-mgmt starts none of its own but the pending pair's.
check '! kill -0 "$OLD_SSH" 2>/dev/null'
check '[ "$(grep -c "^dropbear " "$LOG")" -eq 1 ]'
check '! grep -q "^dropbear .*-[sB]\b" "$LOG"'
# timed ends as well, and procd starts it again for the new link.
check '! kill -0 "$OLD_TIME" 2>/dev/null'
run status
check 'echo "$out" | grep -qx VLAN=100 && echo "$out" | grep -qx IFACE=mgmt100'
check '! echo "$out" | grep -q PENDING_'
check 'grep -qx PREF=2 "$ONU_ROOT/run/onu-mgmt" && grep -qx "NAME=$name" "$ONU_ROOT/run/onu-mgmt"'
teardown

echo "a VLAN the settings give a session is no management VLAN"
setup
printf 'SESSION2_VLAN=410\nSESSION1_HOST_VLAN=36\n' >"$ONU_ROOT/run/onu.conf"
run add 410 192.0.2.6/30
check '[ $rc -ne 0 ] && [ ! -e "$ONU_ROOT/run/onu-trial.d/mgmt" ]'
run add 36 192.0.2.6/30
check '[ $rc -ne 0 ] && [ ! -e "$ONU_ROOT/run/onu-trial.d/mgmt" ]'
run add 37 192.0.2.6/30
check '[ $rc -eq 0 ]'
run cancel
teardown

echo "the pending pair's ssh server takes keys only while root's password is not known"
setup
echo '!' >"$ONU_ROOT/run/onu-auth"
run add 100 192.0.2.6/30
check '[ $rc -eq 0 ] && grep -q "^dropbear -s .* -p 192.0.2.6:22$" "$LOG"'
run cancel
teardown

echo "a new address in the same VLAN, as an interface of its own, then cancelled"
setup
run add 4094 198.51.100.6/30
check '[ $rc -eq 0 ]'
name=$(sed -n 's/^NAME=//p' "$ONU_ROOT/run/onu-trial.d/mgmt")
check 'in_part "interface mgmt mgmt 198.51.100.2/30" && in_part "interface $name mgmt 198.51.100.6/30"'
check '[ "$(part | grep -c "^device")" -eq 1 ]'
check '! grep -q "^onu-nl " "$LOG"'
run cancel
check '[ "$(part)" = "$(printf "device mgmt 4094\ninterface mgmt mgmt 198.51.100.2/30")" ]'
check '[ ! -e "$ONU_ROOT/run/onu-trial.d/mgmt" ] && [ ! -e "$T/stored" ]'
check 'grep -qx ADDRESS=198.51.100.2/30 "$ONU_ROOT/run/onu-mgmt"'
# The link stays as it was, and so do its servers and timed.
check 'kill -0 "$OLD_SSH" 2>/dev/null && kill -0 "$OLD_TIME" 2>/dev/null'
teardown

echo "a new address in the same VLAN, committed: the new one's interface stays as it was"
setup
run add 4094 198.51.100.6/30
name=$(sed -n 's/^NAME=//p' "$ONU_ROOT/run/onu-trial.d/mgmt")
before=$(grep -A4 "^config interface '$name'" "$T/part")
run commit
check '[ $rc -eq 0 ]'
check '[ "$(part)" = "$(printf "device mgmt 4094\ninterface $name mgmt 198.51.100.6/30")" ]'
check '[ "$(grep -A4 "^config interface '"'"'$name'"'"'" "$T/part")" = "$before" ]'
check '! grep -q "^onu-nl " "$LOG"'
teardown

echo "a change not committed goes when the keeper says so; a call for an earlier one leaves a later change"
setup
run add 100 192.0.2.6/30
token=$(sed -n 's/^TOKEN=//p' "$ONU_ROOT/run/onu-trial.d/mgmt")
run expire "$token"
check '[ ! -e "$ONU_ROOT/run/onu-trial.d/mgmt" ]'
check '[ "$(part)" = "$(printf "device mgmt 4094\ninterface mgmt mgmt 198.51.100.2/30")" ]'
check 'logged "onu-nl filter del pon pref 2"'
run add 100 192.0.2.6/30
run expire 1
check '[ -e "$ONU_ROOT/run/onu-trial.d/mgmt" ]'
teardown

echo "an address netifd does not bring up is taken back"
setup
: >"$T/no-address"
run add 100 192.0.2.6/30
check '[ $rc -ne 0 ] && [ ! -e "$ONU_ROOT/run/onu-trial.d/mgmt" ]'
check '[ "$(part)" = "$(printf "device mgmt 4094\ninterface mgmt mgmt 198.51.100.2/30")" ]'
check 'logged "onu-nl filter del sfp pref 2"'
check '! grep -q "^dropbear " "$LOG"'
teardown

echo "what is not a pair, or a VLAN in use, changes nothing"
setup
for bad in "0 192.0.2.6/30" "4095 192.0.2.6/30" "100 192.0.2.6" "100 192.0.2.300/30" \
	"100 192.0.2.6/32" "abc 192.0.2.6/30"; do
	# shellcheck disable=SC2086
	run add $bad
	check '[ $rc -eq 2 ]'
done
check '! grep -q -e "^onu-net " -e "^onu-nl " -e "^dropbear " "$LOG"'
echo "lan36          | 36  | sfp" >>"$ONU_ROOT/proc/net/vlan/config"
run add 36 192.0.2.6/30
check '[ $rc -ne 0 ] && [ ! -e "$ONU_ROOT/run/onu-trial.d/mgmt" ]'
check '! grep -q "onu-nl filter add" "$LOG" && [ ! -e "$T/part" ]'
run commit
check '[ $rc -ne 0 ]'
teardown

echo "test_onu_mgmt: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
