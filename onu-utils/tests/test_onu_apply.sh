#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-apply and scripts/dhcp-wan, against stand-ins for the
# programs they run, in a scratch root: each stand-in writes its name and its
# arguments to a log.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
MODE_SCRIPT=$HERE/scripts/onu-apply
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
	export ONU_ROOT=$T/root ONU_APPLY_NO_WATCH=1 LOG APPLY=$MODE_SCRIPT
	mkdir -p "$ONU_ROOT/run" "$ONU_ROOT/etc" "$ONU_ROOT/proc/sys/net/ipv4" \
		"$ONU_ROOT/sys/class/net/sfp" "$ONU_ROOT/sys/class/net/pon" \
		"$ONU_ROOT/sys/class/net/gem4095" "$ONU_ROOT/sys/class/net/gem1024" \
		"$ONU_ROOT/sys/class/net/gem256" "$T/bin"
	echo 00:00:5e:00:53:01 >"$ONU_ROOT/sys/class/net/sfp/address"
	echo 0 >"$ONU_ROOT/proc/sys/net/ipv4/ip_forward"
	mkdir -p "$ONU_ROOT/proc/sys/net/ipv6/conf/all"
	echo 0 >"$ONU_ROOT/proc/sys/net/ipv6/conf/all/forwarding"
	# The links to the host the tests make: the host link itself and VLANs.
	for lan in sfp lan410 lan36 lan37; do
		mkdir -p "$ONU_ROOT/proc/sys/net/ipv6/conf/$lan" \
			"$ONU_ROOT/proc/sys/net/ipv4/conf/$lan" \
			"$ONU_ROOT/proc/sys/net/ipv4/neigh/$lan"
		echo 1 >"$ONU_ROOT/proc/sys/net/ipv6/conf/$lan/disable_ipv6"
		echo 0 >"$ONU_ROOT/proc/sys/net/ipv4/conf/$lan/rp_filter"
		echo 80 >"$ONU_ROOT/proc/sys/net/ipv4/neigh/$lan/proxy_delay"
	done
	: >"$LOG"
	for prog in killall udhcpd udhcpc logger kill onu-config dhcp6d; do
		printf '#!/bin/sh\necho "%s $*" >>"$LOG"\n' "$prog" >"$T/bin/$prog"
	done
	# onu-svc stands in for procd: "sync onu DIR" runs, at once, the program
	# after "--" of each file of DIR that is new or changed, as procd starts
	# it ("procd start NAME LINE" in the log), and forgets the ones whose file
	# went ("procd stop NAME"), omcid's stand-in ending with its own. onu-run
	# is the real one.
	cat >"$T/bin/onu-svc" <<-EOF
	#!/bin/sh
	echo "onu-svc \$*" >>"\$LOG"
	[ "\$1 \$2" = "sync onu" ] || exit 0
	dir=\$3
	for f in "\$dir"/*; do
	 	[ -f "\$f" ] || continue
	 	n=\${f##*/}
	 	cmp -s "\$f" "$T/svc-\$n" && continue
	 	cp "\$f" "$T/svc-\$n"
	 	echo "procd start \$n \$(cat "\$f")" >>"\$LOG"
	 	set -- \$(cat "\$f")
	 	while [ \$# -gt 0 ] && [ "\$1" != -- ]; do shift; done
	 	shift
	 	"\$@"
	done
	for m in "$T"/svc-*; do
	 	[ -e "\$m" ] || continue
	 	n=\${m##*/svc-}
	 	[ -e "\$dir/\$n" ] && continue
	 	rm -f "\$m"
	 	[ "\$n" = omcid ] && rm -f "$T/omcid.running"
	 	echo "procd stop \$n" >>"\$LOG"
	done
	exit 0
	EOF
	ln -s "$HERE/scripts/onu-run" "$T/bin/onu-run"
	# onu-net stands in for netifd and its protocol "pppoed" as well.
	ln -s "$HERE/tests/netifd-standin.sh" "$T/bin/onu-net"
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
	# stick has no packet filter, and onu-apply must not need one.
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
	ONU_APPLY_NO_WATCH=1 "$MODE_SCRIPT" stop 2>/dev/null
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
check '! logged "^onu-config"'
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
check 'logged "^udhcpd -f $ONU_ROOT/run/udhcpd.conf"'
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
"$MODE_SCRIPT" reload 2>/dev/null
check 'logged "^killall -USR1 omcid"'
check '! logged "^omcid"'
check '[ "$(grep -n "killall -USR1 omcid" "$LOG" | cut -d: -f1)" -lt "$(grep -n "ip link del omcibr0" "$LOG" | cut -d: -f1)" ]'
# A change is kept in the store before it is applied.
check '[ "$(grep -n "^onu-config save" "$LOG" | cut -d: -f1)" = 1 ]'
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
"$MODE_SCRIPT" reload 2>/dev/null
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

echo "an account: the stick holds the session, in a VLAN, and hands it down in another"
setup
mkdir -p "$ONU_ROOT/sys/class/net/ppp0"
echo 1492 >"$ONU_ROOT/sys/class/net/ppp0/mtu"
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION1_HOST_VLAN=36 \
	SESSION1_MAC=00:00:5e:00:53:01
echo "user secret" >"$ONU_ROOT/etc/onu-pppoe"
"$MODE_SCRIPT" start 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = passthrough ]'
# omcid bridges the rest of what the OLT provisions, without the session's VLAN.
check 'logged "^omcid $"'
check '! logged "omcid -r"'
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans")" = 410 ]'
check 'logged "^ip link del omcibr0"'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/ip_forward")" = 1 ]'
check 'logged "^ip link add link sfp name lan36 type vlan id 36"'
check 'logged "^ip address add 169.254.0.1/32 dev lan36"'
# The link forwards only the handed address's packets (BCP 38).
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/conf/lan36/rp_filter")" = 1 ]'
# The stick is no hop: the switch routes with the TTL unchanged.
check 'logged "^devlink dev param set platform/switch name l3_ttl_decrement cmode runtime value false"'
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
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe1 -D -4 -P -M$"'
check '[ "$(cat "$ONU_ROOT/run/session/1/lan")" = lan36 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/hands")" = ipv4 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/dev")" = wan410 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/gem")" = gem256 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/ipv4")" = 192.0.2.2 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/ppp")" = ppp0 ]'
check '[ -s "$ONU_ROOT/run/network.d/ses1" ] && grep -q "option proto .pppoed." "$ONU_ROOT/run/network.d/ses1"'
check '[ "$(cat "$ONU_ROOT/run/onu-pass")" = 192.0.2.2 ]'
check '[ "$(cat "$ONU_ROOT/run/onu-lan")" = lan36 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/pass")" = 192.0.2.2 ]'
check 'logged "^ip route replace 192.0.2.2 dev lan36"'
# The stick's own way out: no table of its own.
check '! logged "^ip rule add"'
check 'grep -q "^interface lan36" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'grep -q "^start 192.0.2.2" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'grep -q "^end 192.0.2.2" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'grep -q "^opt subnet 255.255.255.255" "$ONU_ROOT/run/udhcpd-lan36.conf"'
# The gateway is the concentrator's address, as the router would have it with
# a session of its own; the stick answers ARP for it, at once, without
# taking it.
check 'grep -q "^opt router 192.0.2.1" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'grep -q "^opt staticroutes 192.0.2.1/32 0.0.0.0, 0.0.0.0/0 192.0.2.1" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'logged "^ip neigh add proxy 192.0.2.1 dev lan36"'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/neigh/lan36/proxy_delay")" = 0 ]'
check '! logged "^ip address add 192.0.2.1"'
check '[ "$(cat "$ONU_ROOT/run/session/1/gateway")" = 192.0.2.1 ]'
check 'grep -q "^opt dns 198.51.100.53" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'grep -q "^opt mtu 1492" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'logged "^udhcpd -f $ONU_ROOT/run/udhcpd-lan36.conf"'
check '"$MODE_SCRIPT" status | grep -q "^pass 192.0.2.2 on lan36 from session 1"'
check '"$MODE_SCRIPT" status | grep -q "^session 1 on wan410 (ipv4), up on ppp0, 192.0.2.2"'
# The stand-in session ends after two seconds: the address goes with it.
# The proxy ARP entry goes last, after the route.
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	logged "^ip neigh del proxy 192.0.2.1 dev lan36" && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^ip route del 192.0.2.2 dev lan36"'
check 'logged "^ip neigh del proxy 192.0.2.1 dev lan36"'
"$MODE_SCRIPT" stop 2>/dev/null
check '[ ! -e "$ONU_ROOT/run/session/1" ]'
check '[ ! -e "$ONU_ROOT/run/onu-pass" ]'
check 'logged "^ip link del wan410"'
teardown

echo "the settings of before: two sessions in one VLAN hand IPv4 and IPv6 down in LAN_VLAN"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION2_VLAN=410 SESSION2_FAMILY=ipv6 \
	LAN_VLAN=36 PASS_GATEWAY=192.0.2.254 PASS_PEER=off
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
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe2 -6 -M$"'
check '[ "$(cat "$ONU_ROOT/run/session/2/ipv6")" = fe80::2 ]'
check '[ "$(cat "$ONU_ROOT/run/session/2/ppp")" = ppp1 ]'
# The IPv6 session hands its delegated prefix on, through dhcp6d.
for _ in 1 2 3 4 5 6 7 8 9 10; do
	logged "^dhcp6d" && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^dhcp6d -w ppp1 -l lan36 -m wan410 -o $ONU_ROOT/run/session/2 -b $ONU_ROOT/run/onu-pd2$"'
check '[ "$(grep -c "^dhcp6d" "$LOG")" = 1 ]'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv6/conf/all/forwarding")" = 1 ]'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv6/conf/lan36/disable_ipv6")" = 0 ]'
check '[ "$(cat "$ONU_ROOT/run/session/2/family")" = ipv6 ]'
check '[ "$(cat "$ONU_ROOT/run/session/2/lan")" = lan36 ]'
check '[ "$(cat "$ONU_ROOT/run/onu-pass")" = 192.0.2.2 ]'
# The gateway the stick told of before is no setting any more: the
# concentrator's address is the gateway, from the stick's own address.
check 'logged "^ip address add 169.254.0.1/32 dev lan36"'
check 'grep -q "^opt router 192.0.2.1" "$ONU_ROOT/run/udhcpd-lan36.conf"'
# The IPv6 account goes: its session ends, the VLAN stays for the other.
: >"$LOG"
rm "$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" tick 2>/dev/null
check '[ ! -e "$ONU_ROOT/run/session/2" ]'
check '[ -e "$ONU_ROOT/run/session/1" ]'
check '! logged "^ip link del wan410"'
check '! logged "^ip link set pon mtu 1500"'
check 'logged "^onu-net del ses2$" && [ ! -e "$ONU_ROOT/run/network.d/ses2" ]'
check 'logged "^procd stop dhcp6d2$" && [ ! -e "$ONU_ROOT/run/onu-svc.d/dhcp6d2" ]'
# The host dhcp6d delegates to goes into the store once, and its going too.
echo "0003 1 fe80::2 2001:db8:1200::/56" >"$ONU_ROOT/run/onu-pd2"
: >"$LOG"
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^onu-config pd 2$"'
check '[ "$(grep -c "^onu-config pd" "$LOG")" = 1 ]'
check 'cmp -s "$ONU_ROOT/run/onu-pd2" "$ONU_ROOT/run/onu-pd2.kept"'
: >"$LOG"
"$MODE_SCRIPT" tick 2>/dev/null
check '! logged "^onu-config pd"'
rm "$ONU_ROOT/run/onu-pd2"
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^onu-config pd 2$"'
check '[ ! -e "$ONU_ROOT/run/onu-pd2.kept" ]'
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
check '[ "$(cat "$ONU_ROOT/run/session/1/hands")" = "ipv4 ipv6" ]'
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
"$MODE_SCRIPT" reload 2>/dev/null
check '! logged "^killall -USR"'
check '! logged "^omcid"'
check 'logged "^ip address del 198.51.100.1/24 dev sfp"'
check 'logged "^ip address add 169.254.0.1/32 dev sfp"'
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = passthrough ]'
# The host link itself is the LAN link: it checks the sources, until the
# stick leaves passthrough with the account.
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/conf/sfp/rp_filter")" = 1 ]'
rm "$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" reload 2>/dev/null
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
"$MODE_SCRIPT" reload 2>/dev/null
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
"$MODE_SCRIPT" reload 2>/dev/null
check 'logged "^killall -USR2 omcid"'
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans" | tr "\n" " ")" = "410 1410 " ]'
# The same sessions again: omcid is left as it is.
: >"$LOG"
"$MODE_SCRIPT" reload 2>/dev/null
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

echo "a session follows its service to the GEM port and the VLAN the OLT moves it to"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
printf '410 1410\n' >"$ONU_ROOT/run/omcid-xlate"
# The OLT lets 1410 through Port-ID 1024, not the lowest Port-ID, 256.
printf '1410 1024\n1430 256\n' >"$ONU_ROOT/run/omcid-gem-vlans"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^ip link add link gem1024 name wan1410 type vlan id 1410"'
check '[ "$(cat "$ONU_ROOT/run/session/1/gem")" = gem1024 ]'
: >"$LOG"
# The operator moves the service to 1600 on Port-ID 256, and keeps 410.
printf '410 1600\n' >"$ONU_ROOT/run/omcid-xlate"
printf '1600 256\n' >"$ONU_ROOT/run/omcid-gem-vlans"
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^ip link del wan1410"'
check 'logged "^ip link add link gem256 name wan1600 type vlan id 1600"'
check 'logged "^pppoed up -i wan1600 "'
check '[ "$(cat "$ONU_ROOT/run/session/1/gem")" = gem256 ]'
# A GEM port named by its Port-ID stays, whatever the OLT lets through it.
: >"$LOG"
printf '%s\n' SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION1_GEM=1024 >"$ONU_ROOT/run/onu.conf"
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^ip link add link gem1024 name wan1600 type vlan id 1600"'
teardown

echo "the account removed: from passthrough back to bridge"
setup
conf LAN_VLAN=36
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
: >"$LOG"
rm "$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" reload 2>/dev/null
check 'logged "^ip address del 169.254.0.1/32 dev lan36"'
check 'logged "^ip link del lan36"'
check 'logged "^devlink dev param set platform/switch name l3_ttl_decrement cmode runtime value true"'
check 'logged "^killall -USR2 omcid"'
check '[ ! -e "$ONU_ROOT/run/onu-held-vlans" ]'
check '[ ! -e "$ONU_ROOT/run/onu-lan" ]'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/ip_forward")" = 0 ]'
teardown

echo "a session told to keep its address: no link to the host, the address on the stick"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION1_PASS=off
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$ONU_ROOT/run/session/1/ipv4" ] && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe1 -4 -M$"'
check '! logged "^ip link add link sfp"'
check '! logged "^ip address add 169.254.0.1"'
check '[ ! -e "$ONU_ROOT/run/session/1/lan" ]'
check '[ -e "$ONU_ROOT/run/session/1/hands" ] && [ ! -s "$ONU_ROOT/run/session/1/hands" ]'
check '! logged "^udhcpd"'
check '[ ! -e "$ONU_ROOT/run/onu-pass" ]'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "a session that names no MAC address sends from the stick's, made locally administered"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
# The locally administered forms are put together here: only the
# documentation range may be written out (tools/provenance).
la=$(printf %02x 2):00:5e:00:53:01
turned=$(printf %02x 2):00:5e:00:53:00
check 'logged "^ip link set wan410 address $la"'
"$MODE_SCRIPT" stop 2>/dev/null
echo "$la" >"$ONU_ROOT/sys/class/net/sfp/address"
: >"$LOG"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
check 'logged "^ip link set wan410 address $turned"'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "in one VLAN to the host one session hands its IPv4 address down"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION2_VLAN=410 SESSION2_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user2 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
check '[ "$(grep -c "^ip link add link sfp name lan410 type vlan id 410" "$LOG")" = 1 ]'
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe1 -D -4 -P -M$"'
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe2 -4 -M$"'
check '[ "$(cat "$ONU_ROOT/run/session/1/lan")" = lan410 ]'
check '[ ! -e "$ONU_ROOT/run/session/2/lan" ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/hands")" = ipv4 ] && [ ! -s "$ONU_ROOT/run/session/2/hands" ]'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "each VLAN to the host a link of its own: the second session routes by its own table"
setup
mkdir -p "$ONU_ROOT/sys/class/net/ppp0"
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION2_VLAN=410 SESSION2_FAMILY=ipv4 \
	SESSION2_HOST_VLAN=37
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user2 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" start 2>/dev/null
check 'logged "^ip link add link sfp name lan410 type vlan id 410"'
check 'logged "^ip link add link sfp name lan37 type vlan id 37"'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/conf/lan37/rp_filter")" = 1 ]'
"$MODE_SCRIPT" tick 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$ONU_ROOT/run/session/2/pass" ] && [ -s "$ONU_ROOT/run/session/1/pass" ] && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe1 -D -4 -P -M$"'
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe2 -4 -P -M$"'
check '[ "$(cat "$ONU_ROOT/run/session/2/lan")" = lan37 ]'
check 'logged "^ip rule add from 192.0.2.2 lookup 102"'
check 'logged "^ip route replace default dev ppp0 table 102"'
check '! logged "^ip rule add from 192.0.2.2 lookup 101"'
check 'grep -q "^interface lan410" "$ONU_ROOT/run/udhcpd-lan410.conf"'
check 'grep -q "^interface lan37" "$ONU_ROOT/run/udhcpd-lan37.conf"'
check 'grep -q "^pidfile $ONU_ROOT/run/udhcpd-lan37.pid" "$ONU_ROOT/run/udhcpd-lan37.conf"'
check '[ "$("$MODE_SCRIPT" status | grep -c "^pass ")" = 2 ]'
# What the status daemon shows as the address handed on is the first's.
check '[ "$(cat "$ONU_ROOT/run/onu-lan")" = lan410 ]'
: >"$LOG"
"$MODE_SCRIPT" stop 2>/dev/null
check 'logged "^ip rule del from 192.0.2.2 lookup 102"'
check 'logged "^ip route flush table 102"'
check 'logged "^ip link del lan410"'
check 'logged "^ip link del lan37"'
teardown

echo "two prefixes handed down in two VLANs: the second by its own table"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv6 SESSION2_VLAN=410 SESSION2_FAMILY=ipv6 \
	SESSION2_HOST_VLAN=37
echo "user/ipv6 secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user2/ipv6 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ "$(grep -c "^dhcp6d" "$LOG")" = 2 ] && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^dhcp6d -w ppp1 -l lan410 -m wan410 -o $ONU_ROOT/run/session/1 -b $ONU_ROOT/run/onu-pd1$"'
check 'logged "^dhcp6d -w ppp1 -l lan37 -m wan410 -o $ONU_ROOT/run/session/2 -b $ONU_ROOT/run/onu-pd2 -t 102$"'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv6/conf/lan37/disable_ipv6")" = 0 ]'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "no VLAN to the host: the host link itself"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION1_HOST_VLAN=untagged
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
check 'logged "^ip address add 169.254.0.1/32 dev sfp"'
check '! logged "^ip link add link sfp"'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/conf/sfp/rp_filter")" = 1 ]'
"$MODE_SCRIPT" stop 2>/dev/null
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/conf/sfp/rp_filter")" = 0 ]'
teardown

echo "the settings of before: an empty LAN_VLAN is the host link itself"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 LAN_VLAN=
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
check 'logged "^ip address add 169.254.0.1/32 dev sfp"'
check '! logged "^ip link add link sfp"'
"$MODE_SCRIPT" stop 2>/dev/null
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
# omcid runs as the instance of procd's service "onu" that onu-apply gave it.
mkdir -p "$ONU_ROOT/run/onu-svc.d"
echo "-- $T/bin/onu-run omcid" >"$ONU_ROOT/run/onu-svc.d/omcid"
cp "$ONU_ROOT/run/onu-svc.d/omcid" "$T/svc-omcid"
: >"$T/omcid.running"
"$MODE_SCRIPT" reidentify 2>/dev/null
check 'logged "^ip link set pon down"'
check 'logged "gpon_serial cmode runtime value XXXXXXXXXXXX"'
check 'logged "gpon_password cmode runtime value 0x00112233445566778899"'
check '[ "$(grep -n "ip link set pon down" "$LOG" | cut -d: -f1)" -lt "$(grep -n "gpon_serial" "$LOG" | cut -d: -f1)" ]'
check '[ "$(grep -n "gpon_serial" "$LOG" | cut -d: -f1)" -lt "$(grep -n "ip link set pon up" "$LOG" | cut -d: -f1)" ]'
# omcid ends as procd's service and starts again, in bridge mode.
check 'logged "^procd stop omcid$"'
check 'logged "^procd start omcid -- .*/onu-run omcid$"'
check 'logged "^omcid $"'
check '[ "$(grep -n "^procd stop omcid" "$LOG" | cut -d: -f1)" -lt "$(grep -n "^omcid" "$LOG" | cut -d: -f1)" ]'
teardown

echo "without a serial number the PON stays down"
setup
: >"$ONU_ROOT/run/onu-identity"
"$MODE_SCRIPT" identity 2>/dev/null
rc=$?
check '[ $rc -ne 0 ]'
check '! logged "ip link set pon up"'
teardown

echo "each session an interface of netifd, of the protocol pppoed, in its role"
setup
mkdir -p "$ONU_ROOT/sys/class/net/ppp0"
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION2_VLAN=410 SESSION2_FAMILY=ipv4 \
	SESSION2_HOST_VLAN=37 SESSION3_VLAN=410 SESSION3_FAMILY=ipv6 SESSION3_PASS=off
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user2 secret" >"$ONU_ROOT/run/onu-pppoe2"
echo "user3 secret" >"$ONU_ROOT/run/onu-pppoe3"
"$MODE_SCRIPT" start 2>/dev/null
for _ in 1 2 3; do "$MODE_SCRIPT" tick 2>/dev/null; done
net() {
	tr -d "\t'" <"$ONU_ROOT/run/network.d/ses$1" | grep "^option" | tr '\n' ' '
}
# The stick's way out hands its address on and is the default route; the
# second hands on by its own table; the third keeps its prefix.
check '[ "$(net 1)" = "option device wan410 option proto pppoed option session 1 option family ipv4 option pass 1 option defaultroute 1 " ]'
check '[ "$(net 2)" = "option device wan410 option proto pppoed option session 2 option family ipv4 option pass 1 option defaultroute 0 " ]'
check '[ "$(net 3)" = "option device wan410 option proto pppoed option session 3 option family ipv6 option pass 0 option defaultroute 0 " ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/role")" = "pass none" ] && [ "$(cat "$ONU_ROOT/run/session/2/role")" = "routed none" ] && [ "$(cat "$ONU_ROOT/run/session/3/role")" = "none none" ]'
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$ONU_ROOT/run/session/1/pass" ] && break
	"$REAL_SLEEP" 0.2
done
# Another event of the same session hands on nothing again; another
# interface's is no business of onu-apply.
: >"$LOG"
ACTION=ifupdate INTERFACE=ses1 DEVICE=ppp0 "$MODE_SCRIPT" iface 2>/dev/null
ACTION=ifup INTERFACE=mgmt DEVICE=mgmt "$MODE_SCRIPT" iface 2>/dev/null
rc=$?
check '[ $rc -eq 0 ] && ! logged "^procd start" && ! logged "^ip "'
"$MODE_SCRIPT" stop 2>/dev/null
check 'logged "^onu-net del ses1$" && logged "^onu-net del ses2$" && logged "^onu-net del ses3$"'
check '[ -z "$(ls "$ONU_ROOT/run/network.d" 2>/dev/null)" ]'
teardown

echo "the bench's router: its PPPoE WAN an interface of netifd too"
setup
mkdir -p "$ONU_ROOT/sys/class/net/ppp0"
conf MODE=router WAN_PROTO=pppoe WAN_VLAN=410
echo "user secret" >"$ONU_ROOT/etc/onu-pppoe"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" tick 2>/dev/null
check '[ "$(net 1)" = "option device wan option proto pppoed option session 1 option family both option pass 0 option defaultroute 1 " ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/role")" = "primary none" ]'
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$ONU_ROOT/run/onu-dns" ] && break
	"$REAL_SLEEP" 0.2
done
check '[ "$(cat "$ONU_ROOT/run/onu-dns")" = 198.51.100.53 ]'
check '! logged "^udhcpd -f $ONU_ROOT/run/udhcpd-lan"'
"$MODE_SCRIPT" stop 2>/dev/null
check 'logged "^onu-net del ses1$" && [ ! -e "$ONU_ROOT/run/session/1" ]'
teardown

echo "test_onu_apply: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
