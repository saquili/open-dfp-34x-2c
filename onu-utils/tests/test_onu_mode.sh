#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-mode and scripts/dhcp-wan, against stand-ins for the
# programs they run, in a scratch root: each stand-in writes its name and its
# arguments to a log.

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
	mkdir -p "$ONU_ROOT/proc/sys/net/ipv6/conf/all" "$ONU_ROOT/proc/sys/net/ipv6/conf/lan"
	echo 0 >"$ONU_ROOT/proc/sys/net/ipv6/conf/all/forwarding"
	echo 1 >"$ONU_ROOT/proc/sys/net/ipv6/conf/lan/disable_ipv6"
	mkdir -p "$ONU_ROOT/proc/sys/net/ipv4/conf/lan" "$ONU_ROOT/proc/sys/net/ipv4/conf/sfp"
	echo 0 >"$ONU_ROOT/proc/sys/net/ipv4/conf/lan/rp_filter"
	echo 0 >"$ONU_ROOT/proc/sys/net/ipv4/conf/sfp/rp_filter"
	: >"$LOG"
	for prog in killall udhcpd udhcpc logger kill onu-store dhcp6d; do
		printf '#!/bin/sh\necho "%s $*" >>"$LOG"\n' "$prog" >"$T/bin/$prog"
	done
	# tc shows, on the ingress of sfp and pon, the boot's filter of the
	# management VLAN at priority 1 and two of omcid's VLAN rules.
	printf '#!/bin/sh\necho "tc $*" >>"$LOG"\n[ "$1 $2" = "filter show" ] || exit 0\n%s\n' \
		'printf "filter protocol 802.1Q pref 1 flower chain 0 \nfilter protocol 802.1Q pref 1 flower chain 0 handle 0x1 \nfilter protocol all pref 1000 flower chain 0 \nfilter protocol all pref 1000 flower chain 0 handle 0x1 \nfilter protocol 802.1Q pref 1001 flower chain 0 \n"' \
		>"$T/bin/tc"
	# ip makes the interfaces it is told to add, and takes away those it is
	# told to delete, as the kernel would.
	printf '#!/bin/sh\necho "ip $*" >>"$LOG"\n%s\n%s\n' \
		'[ "$1 $2 $3 $5" = "link add link name" ] && mkdir -p "$ONU_ROOT/sys/class/net/$6"' \
		'[ "$1 $2" = "link del" ] && rm -rf "$ONU_ROOT/sys/class/net/$3"; exit 0' \
		>"$T/bin/ip"
	# omcid runs from its first start on, as on the board.
	printf '#!/bin/sh\necho "omcid $*" >>"$LOG"\n: >"%s/omcid.running"\n' "$T" >"$T/bin/omcid"
	printf '#!/bin/sh\n[ "$1" = omcid ] && [ -e "%s/omcid.running" ]\n' "$T" >"$T/bin/pidof"
	printf '#!/bin/sh\necho "devlink $*" >>"$LOG"\n[ "$1 $2" = "dev show" ] && echo platform/switch\nexit 0\n' \
		>"$T/bin/devlink"
	# nft stays a stand-in, so that a call to it would show in the log: the
	# stick has no packet filter, and onu-mode must not need one.
	printf '#!/bin/sh\necho "nft $*" >>"$LOG"\n' >"$T/bin/nft"
	# pppoed opens IPv4, or IPv6 alone when told -6, and keeps it two seconds.
	printf '#!/bin/sh\necho "pppoed $*" >>"$LOG"\ncase " $* " in\n%s\n%s\nesac\n%s 2\n' \
		'*" -6 "*) echo "up6 ppp1 local fe80::2 peer fe80::1 session 18" ;;' \
		'*) echo "up ppp0 local 192.0.2.2 peer 192.0.2.1 session 17 dns 198.51.100.53 -" ;;' \
		"$REAL_SLEEP" >"$T/bin/pppoed"
	printf '#!/bin/sh\nexit 0\n' >"$T/bin/sleep"
	printf '#!/bin/sh\n[ "$1" = gem ] || exit 1\n%s\n' \
		'printf "gem4095 port 4095 down multicast\ngem256 port 256 both tcont 0\ngem1024 port 1024 both tcont 1\n"' \
		>"$T/bin/pon"
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
# A boot applies what the store gave and saves nothing.
check '! logged "^onu-store"'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/ip_forward")" = 0 ]'
teardown

echo "router mode, DHCP on the WAN"
setup
conf MODE=router WAN_PROTO=dhcp LAN_ADDRESS=198.51.100.1/24 \
	LAN_DHCP_START=198.51.100.10 LAN_DHCP_END=198.51.100.20
"$MODE_SCRIPT" start 2>/dev/null
check 'logged "^ip link del omcibr0"'
check 'logged "^tc filter del dev sfp ingress pref 1000"'
check 'logged "^tc filter del dev pon ingress pref 1001"'
check '! logged "^tc filter del dev [a-z]* ingress pref 1$"'
check '! logged "^tc qdisc del dev \(sfp\|pon\) clsact"'
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
check '[ "$(cat "$ONU_ROOT/run/onu-wan")" = "gem256 gem256 gem256" ]'
check '! logged "^nft"'
# Every other GEM port hands its frames on, the downstream-only one and one
# that also sends, as an IEEE 802.1p mapper's GEM port of other priorities.
check 'logged "^tc filter add dev gem4095 ingress matchall action mirred ingress redirect dev gem256"'
check 'logged "^tc filter add dev gem1024 ingress matchall action mirred ingress redirect dev gem256"'
check '! logged "^tc filter add dev gem256 "'
check '"$MODE_SCRIPT" status | grep -q "^wan gem256 on gem256"'
teardown

echo "router mode, PPPoE in a VLAN, on a named GEM port"
setup
conf MODE=router WAN_PROTO=pppoe WAN_VLAN=410 WAN_GEM=1024
echo "user secret" >"$ONU_ROOT/etc/onu-pppoe"
"$MODE_SCRIPT" start 2>/dev/null
check '[ "$(ls -l "$ONU_ROOT/run/onu-pppoe1" | cut -c1-10)" = "-rw-------" ]'
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^ip link add link gem1024 name wan type vlan id 410"'
check 'logged "^ip link set pon mtu 1508"'
check 'logged "^ip link set gem1024 mtu 1508"'
check 'logged "^ip link set wan mtu 1508"'
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$ONU_ROOT/run/onu-dns" ] && break
	"$REAL_SLEEP" 0.2
done
check '[ "$(grep -n "ip link set wan mtu 1508" "$LOG" | cut -d: -f1)" -lt "$(grep -n "^pppoed up" "$LOG" | head -1 | cut -d: -f1)" ]'
check 'logged "^pppoed up -i wan -f $ONU_ROOT/run/onu-pppoe1 -D"'
check '[ "$(cat "$ONU_ROOT/run/onu-dns")" = 198.51.100.53 ]'
# The WAN in a VLAN takes the frames of that VLAN alone from the other ports.
check 'logged "^tc filter add dev gem4095 ingress protocol 802.1Q flower vlan_id 410 action mirred ingress redirect dev gem1024"'
check 'logged "^tc filter add dev gem256 ingress protocol 802.1Q flower vlan_id 410 action mirred ingress redirect dev gem1024"'
check '! logged "matchall"'
"$MODE_SCRIPT" tick 2>/dev/null
check 'grep -q "^opt dns 198.51.100.53" "$ONU_ROOT/run/udhcpd.conf"'
"$MODE_SCRIPT" stop 2>/dev/null
check 'logged "^ip link set gem1024 mtu 1500"'
check '[ "$(grep -n "ip link set gem1024 mtu 1500" "$LOG" | cut -d: -f1)" -lt "$(grep -n "ip link set pon mtu 1500" "$LOG" | cut -d: -f1)" ]'
teardown

echo "router mode, a static WAN"
setup
conf MODE=router WAN_PROTO=static WAN_ADDRESS=203.0.113.2/30 WAN_GATEWAY=203.0.113.1
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^ip address add 203.0.113.2/30 dev gem256"'
check 'logged "^ip route replace default via 203.0.113.1 dev gem256"'
teardown

echo "from bridge to router, with omcid running"
setup
conf MODE=bridge
"$MODE_SCRIPT" start 2>/dev/null
: >"$LOG"
printf 'MODE=router\n' >"$ONU_ROOT/run/onu.conf"
"$MODE_SCRIPT" apply 2>/dev/null
check 'logged "^killall -USR1 omcid"'
check '! logged "^omcid"'
check '[ "$(grep -n "killall -USR1 omcid" "$LOG" | cut -d: -f1)" -lt "$(grep -n "ip link del omcibr0" "$LOG" | cut -d: -f1)" ]'
# A change is kept in the store before it is applied.
check '[ "$(grep -n "^onu-store save" "$LOG" | cut -d: -f1)" = 1 ]'
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
	logged "IPv4 up on ppp0" && break
	"$REAL_SLEEP" 0.2
done
check 'logged "IPv4 up on ppp0"'
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
check '! logged "^nft"'
check 'logged "^ip address del 198.51.100.65/24 dev sfp"'
check 'logged "^killall -USR2 omcid"'
check '! logged "^omcid"'
check 'logged "^tc qdisc del dev gem4095 clsact"'
check 'logged "^ip -4 address flush dev gem256"'
check '[ ! -e "$ONU_ROOT/run/onu-feeds" ]'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/ip_forward")" = 0 ]'
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ]'
teardown

echo "an account: the stick holds the session, in a VLAN, the LAN link in another"
setup
mkdir -p "$ONU_ROOT/sys/class/net/ppp0"
echo 1492 >"$ONU_ROOT/sys/class/net/ppp0/mtu"
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 LAN_VLAN=36 \
	PASS_GATEWAY=192.0.2.254 SESSION1_MAC=00:00:5e:00:53:01
echo "user secret" >"$ONU_ROOT/etc/onu-pppoe"
"$MODE_SCRIPT" start 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = passthrough ]'
# omcid bridges the rest of what the OLT provisions, without the session's VLAN.
check 'logged "^omcid $"'
check '! logged "omcid -r"'
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans")" = 410 ]'
check 'logged "^ip link del omcibr0"'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/ip_forward")" = 1 ]'
check 'logged "^ip link add link sfp name lan type vlan id 36"'
check 'logged "^ip address add 192.0.2.254/32 dev lan"'
# The LAN link forwards only the handed address's packets (BCP 38).
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/conf/lan/rp_filter")" = 1 ]'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/conf/sfp/rp_filter")" = 0 ]'
check '! logged "^udhcpd"'
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^ip link add link gem256 name wan410 type vlan id 410"'
check 'logged "^ip link set wan410 address 00:00:5e:00:53:01"'
check '[ "$(grep -n "ip link set wan410 address" "$LOG" | cut -d: -f1)" -lt "$(grep -n "^ip link set wan410 up" "$LOG" | cut -d: -f1)" ]'
check 'logged "^ip link set wan410 mtu 1508"'
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$ONU_ROOT/run/onu-pass" ] && logged "^udhcpd" && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe1 -D -4 -P"'
check '[ "$(cat "$ONU_ROOT/run/session/1/dev")" = wan410 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/gem")" = gem256 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/ipv4")" = 192.0.2.2 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/ppp")" = ppp0 ]'
check '[ -s "$ONU_ROOT/run/pppoed1.pid" ]'
check '[ "$(cat "$ONU_ROOT/run/onu-pass")" = 192.0.2.2 ]'
check 'logged "^ip route replace 192.0.2.2 dev lan"'
check 'grep -q "^interface lan" "$ONU_ROOT/run/udhcpd.conf"'
check 'grep -q "^start 192.0.2.2" "$ONU_ROOT/run/udhcpd.conf"'
check 'grep -q "^end 192.0.2.2" "$ONU_ROOT/run/udhcpd.conf"'
check 'grep -q "^opt subnet 255.255.255.255" "$ONU_ROOT/run/udhcpd.conf"'
check 'grep -q "^opt router 192.0.2.254" "$ONU_ROOT/run/udhcpd.conf"'
check 'grep -q "^opt staticroutes 192.0.2.254/32 0.0.0.0, 0.0.0.0/0 192.0.2.254" "$ONU_ROOT/run/udhcpd.conf"'
check 'grep -q "^opt dns 198.51.100.53" "$ONU_ROOT/run/udhcpd.conf"'
check 'grep -q "^opt mtu 1492" "$ONU_ROOT/run/udhcpd.conf"'
check 'logged "^udhcpd $ONU_ROOT/run/udhcpd.conf"'
check '"$MODE_SCRIPT" status | grep -q "^pass 192.0.2.2 on lan"'
check '"$MODE_SCRIPT" status | grep -q "^session 1 on wan410 (ipv4), up on ppp0, 192.0.2.2"'
# The stand-in session ends after two seconds: the address goes with it.
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	logged "^ip route del 192.0.2.2 dev lan" && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^ip route del 192.0.2.2 dev lan"'
"$MODE_SCRIPT" stop 2>/dev/null
check '[ ! -e "$ONU_ROOT/run/session/1" ]'
check 'logged "^ip link del wan410"'
teardown

echo "two sessions in one VLAN: IPv4 handed on, IPv6 on the stick"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION2_VLAN=410 SESSION2_FAMILY=ipv6 \
	LAN_VLAN=36
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user/ipv6 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
# One session starts in a round: the second waits for the next.
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe1 "'
check '! logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe2 "'
check '[ ! -e "$ONU_ROOT/run/session/2" ]'
"$MODE_SCRIPT" tick 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$ONU_ROOT/run/session/2/ipv6" ] && [ -s "$ONU_ROOT/run/onu-pass" ] && break
	"$REAL_SLEEP" 0.2
done
check '[ "$(grep -c "^ip link add link gem256 name wan410" "$LOG")" = 1 ]'
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe1 -D -4 -P"'
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe2 -6$"'
check '[ "$(cat "$ONU_ROOT/run/session/2/ipv6")" = fe80::2 ]'
check '[ "$(cat "$ONU_ROOT/run/session/2/ppp")" = ppp1 ]'
# The IPv6 session hands its delegated prefix on, through dhcp6d.
for _ in 1 2 3 4 5 6 7 8 9 10; do
	logged "^dhcp6d" && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^dhcp6d -w ppp1 -l lan -m wan410 -o $ONU_ROOT/run/session/2$"'
check '[ "$(grep -c "^dhcp6d" "$LOG")" = 1 ]'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv6/conf/all/forwarding")" = 1 ]'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv6/conf/lan/disable_ipv6")" = 0 ]'
check '[ "$(cat "$ONU_ROOT/run/session/2/family")" = ipv6 ]'
check '[ "$(cat "$ONU_ROOT/run/onu-pass")" = 192.0.2.2 ]'
check '[ "$(cat "$ONU_ROOT/run/onu-pass-session")" = 1 ]'
# The IPv6 account goes: its session ends, the VLAN stays for the other.
: >"$LOG"
rm "$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" tick 2>/dev/null
check '[ ! -e "$ONU_ROOT/run/session/2" ]'
check '[ -e "$ONU_ROOT/run/session/1" ]'
check '! logged "^ip link del wan410"'
check '! logged "^ip link set pon mtu 1500"'
check '[ ! -e "$ONU_ROOT/run/pppoed2.pid" ]'
check '[ ! -e "$ONU_ROOT/run/dhcp6d2.pid" ]'
"$MODE_SCRIPT" stop 2>/dev/null
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv6/conf/all/forwarding")" = 0 ]'
teardown

echo "two sessions follow their GEM port when the OLT replaces it"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION2_VLAN=410 SESSION2_FAMILY=ipv6
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user/ipv6 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^ip link add link gem256 name wan410 type vlan id 410"'
check '[ "$(cat "$ONU_ROOT/run/session/2/gem")" = gem256 ]'
# The OLT provisions again: GEM port 256 goes, and the kernel takes wan410
# with it; the sessions move to the GEM port there is now.
rm -r "$ONU_ROOT/sys/class/net/gem256" "$ONU_ROOT/sys/class/net/wan410"
printf '#!/bin/sh\n[ "$1" = gem ] || exit 1\n%s\n' \
	'printf "gem4095 port 4095 down multicast\ngem1024 port 1024 both tcont 1\n"' \
	>"$T/bin/pon"
: >"$LOG"
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^ip link add link gem1024 name wan410 type vlan id 410"'
check '[ "$(cat "$ONU_ROOT/run/session/1/gem")" = gem1024 ]'
# The second session moves a round later, onto the interface the first made.
"$MODE_SCRIPT" tick 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/session/2/gem")" = gem1024 ]'
check '[ "$(grep -c "^ip link add link gem1024 name wan410" "$LOG")" = 1 ]'
teardown

echo "a session in no VLAN runs on the GEM interface, on the GEM port named"
setup
conf SESSION1_GEM=1024 SESSION1_FAMILY=both
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^pppoed up -i gem1024 -f $ONU_ROOT/run/onu-pppoe1 -D -P"'
check '! logged "type vlan id"'
# The session holds the whole GEM port: omcid leaves the OLT's bridge out, and
# every frame of the downstream ports goes to the session's GEM interface.
check 'logged "^omcid -r"'
check '[ ! -e "$ONU_ROOT/run/onu-held-vlans" ]'
check 'logged "^tc filter add dev gem4095 ingress matchall action mirred ingress redirect dev gem1024"'
"$MODE_SCRIPT" stop 2>/dev/null
check 'logged "^ip link set gem1024 mtu 1500"'
check 'logged "^ip link set pon mtu 1500"'
check '[ ! -e "$ONU_ROOT/run/session/1" ]'
teardown

echo "the settings of before: the WAN's VLAN and MAC are the first session's"
setup
conf MODE=passthrough WAN_VLAN=410 WAN_MAC=00:00:5e:00:53:01
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^ip link add link gem256 name wan410 type vlan id 410"'
check 'logged "^ip link set wan410 address 00:00:5e:00:53:01"'
teardown

echo "without an account there is no session: the OLT's bridge"
setup
conf MODE=passthrough SESSION1_VLAN=410
"$MODE_SCRIPT" start 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	logged "^omcid" && break
	"$REAL_SLEEP" 0.2
done
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ]'
check 'logged "^omcid $"'
teardown

echo "from router to passthrough, omcid left as it is"
setup
conf MODE=router LAN_ADDRESS=198.51.100.1/24 LAN_DHCP=off
"$MODE_SCRIPT" start 2>/dev/null
: >"$LOG"
printf 'MODE=\n' >"$ONU_ROOT/run/onu.conf"
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" apply 2>/dev/null
check '! logged "^killall -USR"'
check '! logged "^omcid"'
check 'logged "^ip address del 198.51.100.1/24 dev sfp"'
check 'logged "^ip address add 169.254.0.1/32 dev sfp"'
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = passthrough ]'
# The host link itself is the LAN link: it checks the sources, until the
# stick leaves passthrough with the account.
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/conf/sfp/rp_filter")" = 1 ]'
rm "$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" apply 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ]'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/conf/sfp/rp_filter")" = 0 ]'
teardown

echo "an account in a VLAN beside a running omcid: its bridge stays, without that VLAN"
setup
conf MODE=bridge
"$MODE_SCRIPT" start 2>/dev/null
: >"$LOG"
printf 'SESSION1_VLAN=1410\n' >"$ONU_ROOT/run/onu.conf"
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" apply 2>/dev/null
check 'logged "^killall -USR2 omcid"'
check '! logged "^killall -USR1"'
check '[ "$(grep -n "ip link del omcibr0" "$LOG" | cut -d: -f1)" -lt "$(grep -n "killall -USR2 omcid" "$LOG" | cut -d: -f1)" ]'
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans")" = 1410 ]'
"$MODE_SCRIPT" tick 2>/dev/null
# The other ports hand on the frames of the session's VLAN alone, and the
# session's own port none.
check 'logged "^tc filter add dev gem4095 ingress protocol 802.1Q flower vlan_id 1410 action mirred ingress redirect dev gem256"'
check 'logged "^tc filter add dev gem1024 ingress protocol 802.1Q flower vlan_id 1410 action mirred ingress redirect dev gem256"'
check '! logged "^tc qdisc add dev gem256 "'
check '! logged "matchall"'
# A second session in another VLAN: omcid builds its bridge again without both.
: >"$LOG"
printf 'SESSION1_VLAN=1410\nSESSION2_VLAN=410\n' >"$ONU_ROOT/run/onu.conf"
echo "user/ipv6 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" apply 2>/dev/null
check 'logged "^killall -USR2 omcid"'
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans" | tr "\n" " ")" = "410 1410 " ]'
# The same sessions again: omcid is left as it is.
: >"$LOG"
"$MODE_SCRIPT" apply 2>/dev/null
check '! logged "omcid"'
teardown

echo "a session in the router's VLAN runs in the VLAN the OLT translates it into"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
# Before the OLT's translations the session runs in the VLAN it was given.
check 'logged "^ip link add link gem256 name wan410 type vlan id 410"'
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans")" = 410 ]'
: >"$LOG"
printf '410 1410\n420 1430\n' >"$ONU_ROOT/run/omcid-xlate"
"$MODE_SCRIPT" tick 2>/dev/null
# With them, omcid leaves 1410 out, and the session moves into it.
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans")" = 1410 ]'
check 'logged "^killall -USR2 omcid"'
check 'logged "^ip link del wan410"'
check 'logged "^ip link add link gem256 name wan1410 type vlan id 1410"'
check 'logged "^pppoed up -i wan1410 "'
check 'logged "^tc filter add dev gem4095 ingress protocol 802.1Q flower vlan_id 1410 action mirred ingress redirect dev gem256"'
# The next round changes nothing.
: >"$LOG"
"$MODE_SCRIPT" tick 2>/dev/null
check '! logged "omcid"'
check '! logged "^ip link"'
teardown

echo "the account removed: from passthrough back to bridge"
setup
conf LAN_VLAN=36
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
: >"$LOG"
rm "$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" apply 2>/dev/null
check 'logged "^ip address del 169.254.0.1/32 dev lan"'
check 'logged "^ip link del lan"'
check 'logged "^killall -USR2 omcid"'
check '[ ! -e "$ONU_ROOT/run/onu-held-vlans" ]'
check '[ ! -e "$ONU_ROOT/run/onu-lan" ]'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/ip_forward")" = 0 ]'
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

echo "a new identity: the PON goes down, takes it, comes up, and omcid starts over"
setup
printf 'serial=XXXXXXXXXXXX\nploam_password=00112233445566778899\n' >"$ONU_ROOT/run/onu-identity"
echo passthrough >"$ONU_ROOT/run/onu-mode"
: >"$T/omcid.running"
"$MODE_SCRIPT" reidentify 2>/dev/null
check 'logged "^ip link set pon down"'
check 'logged "gpon_serial cmode runtime value XXXXXXXXXXXX"'
check 'logged "gpon_password cmode runtime value 0x00112233445566778899"'
check '[ "$(grep -n "ip link set pon down" "$LOG" | cut -d: -f1)" -lt "$(grep -n "gpon_serial" "$LOG" | cut -d: -f1)" ]'
check '[ "$(grep -n "gpon_serial" "$LOG" | cut -d: -f1)" -lt "$(grep -n "ip link set pon up" "$LOG" | cut -d: -f1)" ]'
check 'logged "^killall omcid"'
teardown

echo "without a serial number the PON stays down"
setup
: >"$ONU_ROOT/run/onu-identity"
"$MODE_SCRIPT" identity 2>/dev/null
rc=$?
check '[ $rc -ne 0 ]'
check '! logged "ip link set pon up"'
teardown

echo "test_onu_mode: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
