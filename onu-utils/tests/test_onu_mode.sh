#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-mode and scripts/dhcp-wan, against stand-ins for the
# programs they run, in a scratch root: each stand-in writes its name and its
# arguments to a log, and nft also what it reads.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
MODE_SCRIPT=$HERE/scripts/onu-mode
WAN_SCRIPT=$HERE/scripts/dhcp-wan
REAL_SLEEP=$(command -v sleep)
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

logged() {
	grep -q -- "$1" "$LOG"
}

setup() {
	T=$(mktemp -d)
	LOG=$T/log
	export ONU_ROOT=$T/root ONU_MODE_NO_WATCH=1 LOG
	mkdir -p "$ONU_ROOT/run" "$ONU_ROOT/etc" "$ONU_ROOT/proc/sys/net/ipv4" \
		"$ONU_ROOT/sys/class/net/sfp" "$ONU_ROOT/sys/class/net/pon" \
		"$ONU_ROOT/sys/class/net/gem4095" "$ONU_ROOT/sys/class/net/gem1024" \
		"$ONU_ROOT/sys/class/net/gem256" "$T/bin"
	echo 0 >"$ONU_ROOT/proc/sys/net/ipv4/ip_forward"
	: >"$LOG"
	for prog in ip tc omcid killall udhcpd udhcpc logger kill; do
		printf '#!/bin/sh\necho "%s $*" >>"$LOG"\n' "$prog" >"$T/bin/$prog"
	done
	printf '#!/bin/sh\necho "nft $*" >>"$LOG"\n[ "$2" = - ] && cat >>"$LOG"\nexit 0\n' >"$T/bin/nft"
	printf '#!/bin/sh\necho "pppoed $*" >>"$LOG"\n%s\n%s 2\n' \
		'echo "up ppp0 local 192.0.2.2 peer 192.0.2.1 session 17 dns 198.51.100.53 -"' \
		"$REAL_SLEEP" >"$T/bin/pppoed"
	printf '#!/bin/sh\nexit 0\n' >"$T/bin/sleep"
	chmod +x "$T/bin"/*
	PATH=$T/bin:$PATH
}

teardown() {
	ONU_MODE_NO_WATCH=1 "$MODE_SCRIPT" stop 2>/dev/null
	rm -rf "$T"
}

conf() {
	printf '%s\n' "$@" >"$ONU_ROOT/etc/onu.conf"
}

echo "bridge mode"
setup
conf MODE=bridge
"$MODE_SCRIPT" start 2>/dev/null
check 'logged "^omcid $"'
check '! logged "omcid -r"'
check '! logged "^nft"'
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ]'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/ip_forward")" = 0 ]'
teardown

echo "router mode, DHCP on the WAN"
setup
conf MODE=router WAN_PROTO=dhcp LAN_ADDRESS=198.51.100.1/24 \
	LAN_DHCP_START=198.51.100.10 LAN_DHCP_END=198.51.100.20
"$MODE_SCRIPT" start 2>/dev/null
check 'logged "^ip link del omcibr0"'
check 'logged "^tc qdisc del dev sfp clsact"'
check 'logged "^omcid -r"'
check 'logged "^ip address add 198.51.100.1/24 dev sfp"'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/ip_forward")" = 1 ]'
check 'grep -q "^opt subnet 255.255.255.0" "$ONU_ROOT/run/udhcpd.conf"'
check 'grep -q "^opt router 198.51.100.1" "$ONU_ROOT/run/udhcpd.conf"'
check 'grep -q "^start 198.51.100.10" "$ONU_ROOT/run/udhcpd.conf"'
check 'logged "^udhcpd $ONU_ROOT/run/udhcpd.conf"'
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^ip link set gem256 up"'
check 'logged "^udhcpc -i gem256 "'
check 'logged "devices = { sfp, gem256 }"'
check 'logged "flowtable ft {"'
check 'logged "flow add @ft"'
check 'logged "oifname \"gem256\" masquerade"'
check '[ "$(cat "$ONU_ROOT/run/onu-wan")" = "gem256 gem256 gem256" ]'
check '"$MODE_SCRIPT" status | grep -q "^wan gem256 on gem256"'
teardown

echo "router mode, PPPoE in a VLAN, on a named GEM port"
setup
conf MODE=router WAN_PROTO=pppoe WAN_VLAN=410 WAN_GEM=1024
echo "user secret" >"$ONU_ROOT/etc/onu-pppoe"
"$MODE_SCRIPT" start 2>/dev/null
check '[ "$(ls -l "$ONU_ROOT/run/onu-pppoe" | cut -c1-10)" = "-rw-------" ]'
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^ip link add link gem1024 name wan type vlan id 410"'
check 'logged "oifname \"ppp0\" masquerade"'
check 'logged "devices = { sfp, gem1024 }"'
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$ONU_ROOT/run/onu-dns" ] && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^pppoed up -i wan -f $ONU_ROOT/run/onu-pppoe -D"'
check '[ "$(cat "$ONU_ROOT/run/onu-dns")" = 198.51.100.53 ]'
"$MODE_SCRIPT" tick 2>/dev/null
check 'grep -q "^opt dns 198.51.100.53" "$ONU_ROOT/run/udhcpd.conf"'
teardown

echo "router mode, a static WAN"
setup
conf MODE=router WAN_PROTO=static WAN_ADDRESS=203.0.113.2/30 WAN_GATEWAY=203.0.113.1
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^ip address add 203.0.113.2/30 dev gem256"'
check 'logged "^ip route replace default via 203.0.113.1 dev gem256"'
teardown

echo "a NAT table that nft refuses"
setup
printf '#!/bin/sh\necho "nft $*" >>"$LOG"\n[ "$2" = - ] || exit 0\ncat >/dev/null\necho "Error: syntax error" >&2\nexit 1\n' >"$T/bin/nft"
conf MODE=router WAN_PROTO=static WAN_ADDRESS=203.0.113.2/30 WAN_GATEWAY=203.0.113.1
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^logger -t onu-mode no NAT: nft refused the table: Error: syntax error"'
teardown

echo "a PPPoE session without name servers"
setup
printf '#!/bin/sh\necho "pppoed $*" >>"$LOG"\n%s\n%s 2\n' \
	'echo "up ppp0 local 192.0.2.2 peer 192.0.2.1 session 17 dns - -"' \
	"$REAL_SLEEP" >"$T/bin/pppoed"
conf MODE=router WAN_PROTO=pppoe
echo "user secret" >"$ONU_ROOT/etc/onu-pppoe"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	logged "WAN up on ppp0" && break
	"$REAL_SLEEP" 0.2
done
check 'logged "WAN up on ppp0"'
check '[ ! -e "$ONU_ROOT/run/onu-dns" ]'
check '"$MODE_SCRIPT" status >/dev/null'
check '! "$MODE_SCRIPT" status | grep -q "^dns"'
teardown

echo "from router back to bridge"
setup
conf MODE=router LAN_ADDRESS=198.51.100.65/24
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
: >"$LOG"
printf 'MODE=bridge\n' >"$ONU_ROOT/run/onu.conf"
"$MODE_SCRIPT" apply 2>/dev/null
check 'logged "^nft delete table inet onu-router"'
check 'logged "^ip address del 198.51.100.65/24 dev sfp"'
check 'logged "^omcid $"'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/ip_forward")" = 0 ]'
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ]'
teardown

echo "settings that are not values"
setup
printf 'MODE=router;reboot\nWAN_VLAN=$(reboot)\nOTHER=x\n' >"$ONU_ROOT/etc/onu.conf"
"$MODE_SCRIPT" start 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ]'
check '! logged "reboot"'
teardown

echo "the netmask of a prefix"
check '[ "$("$MODE_SCRIPT" netmask 24)" = 255.255.255.0 ]'
check '[ "$("$MODE_SCRIPT" netmask 20)" = 255.255.240.0 ]'
check '[ "$("$MODE_SCRIPT" netmask 32)" = 255.255.255.255 ]'
check '[ "$("$MODE_SCRIPT" netmask 0)" = 0.0.0.0 ]'

echo "the DHCP client's script"
setup
interface=wan ip=203.0.113.6 mask=30 router="203.0.113.5 203.0.113.9" \
	dns="198.51.100.1 198.51.100.2" "$WAN_SCRIPT" bound
check 'logged "^ip address add 203.0.113.6/30 dev wan"'
check 'logged "^ip route replace default via 203.0.113.5 dev wan"'
check '[ "$(cat "$ONU_ROOT/run/onu-dns")" = "198.51.100.1 198.51.100.2" ]'
interface=wan "$WAN_SCRIPT" deconfig
check '[ ! -e "$ONU_ROOT/run/onu-dns" ]'
teardown

echo "test_onu_mode: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
