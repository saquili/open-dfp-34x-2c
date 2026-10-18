#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of onu-fw against the kernel it runs on, read back with nft, on dummy
# interfaces named as the stick's: sfp, a VLAN on it, a GEM interface and a
# VLAN on that, the interface of an IPoE connection.
# A dummy interface carries no flow in hardware, so the kernel refuses the
# flowtable and the table falls back to the CPU, which is tried here; the
# flowtable itself needs the stick, and is tried there. Making the
# interfaces needs root with CAP_NET_ADMIN, as in a container started with
# --cap-add NET_ADMIN; without that, or without nft, the test says that it
# skipped. Last, packets go through a table in network namespaces, sent and
# taken by python3, without which that part says that it skipped.

set -u
FW=${BUILD:-$(cd "$(dirname "$0")/.." && pwd)/build}/onu-fw
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

if ! command -v nft >/dev/null 2>&1 || ! nft list tables >/dev/null 2>&1 ||
	[ -e /sys/class/net/sfp ] || ! ip link add sfp type dummy 2>/dev/null; then
	echo "test_onu_fw: SKIPPED: it needs root with CAP_NET_ADMIN, nft, and no interface sfp"
	exit 0
fi
T=$(mktemp -d)
cleanup() {
	nft delete table inet onu 2>/dev/null
	nft delete table inet ref 2>/dev/null
	ip link del net1 2>/dev/null
	ip link del wan421 2>/dev/null
	ip link del sfp 2>/dev/null
	ip link del gem257 2>/dev/null
	for ns in onudev onuinet onustick; do
		ip netns del $ns 2>/dev/null
	done
	rm -rf "$T"
}
trap cleanup EXIT
ip link add gem257 type dummy && ip link add link sfp name net1 type vlan id 100 &&
	ip link add link gem257 name wan421 type vlan id 421 || exit 1

run() {
	out=$("$FW" "$@" 2>"$T/err")
	rc=$?
	err=$(cat "$T/err")
}

table() {
	nft list table inet onu 2>/dev/null
}

has() {
	table | grep -qF -- "$1"
}

echo "the NAT and the protection of the local networks"
printf 'lan net1 192.168.1.0/24\nlan sfp 10.0.0.0/8\nwan gem257\nout ppp*\n' >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ] && echo "$err" | grep -q "flowtable was refused"'
check 'has "chain forward" && has "chain postrouting"'
check 'has "ct state established,related accept"'
check "has 'oifname \"ppp*\" tcp flags syn tcp option maxseg size set rt mtu'"
check "has 'iifname \"ppp*\" oifname \"net1\" ct state new drop'"
check "has 'oifname \"ppp*\" ip saddr 192.168.1.0/24 masquerade'"
check "has 'oifname \"ppp*\" ip saddr 10.0.0.0/8 masquerade'"
check '! has "flowtable" && ! has "flow add"'
check '! has "chain prerouting" && ! has "ct status dnat"'

echo "out by a session and by an IPoE connection"
printf 'lan net1 192.168.1.0/24\nwan gem257\nout ppp*\nout wan421\n' >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ]'
check "has 'oifname \"wan421\" tcp flags syn tcp option maxseg size set rt mtu'"
check "has 'iifname \"wan421\" oifname \"net1\" ct state new drop'"
check "has 'oifname \"wan421\" ip saddr 192.168.1.0/24 masquerade'"
check "has 'oifname \"ppp*\" ip saddr 192.168.1.0/24 masquerade'"

echo "the ports forwarded and a DMZ"
{
	printf 'lan net1 192.168.1.0/24\nwan gem257\nout ppp*\nout wan421\n'
	printf 'fwd ppp0 203.0.113.24 tcp 8080 192.168.1.20 80\n'
	printf 'fwd ppp0 203.0.113.24 udp 6000-6010 192.168.1.20 6000\n'
	printf 'fwd ppp0 203.0.113.24 tcp 8000-8002 192.168.1.20 9000\n'
	printf 'fwd wan421 192.0.2.10 tcp 443 192.168.1.21 443\n'
	printf 'dmz wan421 192.0.2.10 192.168.1.30\n'
} >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ] && echo "$err" | grep -q "flowtable was refused"'
check 'has "chain prerouting" && has "type nat hook prerouting priority dstnat; policy accept;"'
check "has 'iifname \"ppp0\" ip daddr 203.0.113.24 tcp dport 8080 dnat ip to 192.168.1.20:80'"
check "has 'iifname \"ppp0\" ip daddr 203.0.113.24 udp dport 6000-6010 dnat ip to 192.168.1.20'"
# A range moved to other ports, each to its own by a map.
check "has 'iifname \"ppp0\" ip daddr 203.0.113.24 tcp dport 8000-8002 dnat ip to 192.168.1.20:tcp dport map { 8000 : 9000, 8001 : 9001, 8002 : 9002 }'"
check "has 'iifname \"wan421\" ip daddr 192.0.2.10 tcp dport 443 dnat ip to 192.168.1.21:443'"
check "has 'iifname \"wan421\" ip daddr 192.0.2.10 udp dport 68 accept'"
check "has 'iifname \"wan421\" ip daddr 192.0.2.10 dnat ip to 192.168.1.30'"
check '! has "icmp"'
# What the forwards took is let through after the answers, before the drops.
at() {
	table | grep -nF -- "$1" | head -n 1 | cut -d: -f1
}
check '[ "$(at "ct state established,related accept")" -lt "$(at "ct status dnat accept")" ] &&
	[ "$(at "ct status dnat accept")" -lt "$(at "ct state new drop")" ]'
# The DMZ comes after the forwards of its connection.
check '[ "$(at "tcp dport 443 dnat")" -lt "$(at "udp dport 68 accept")" ] &&
	[ "$(at "udp dport 68 accept")" -lt "$(at "dnat ip to 192.168.1.30")" ]'

echo "the forwards as nft itself makes them"
nft add table inet ref &&
	nft add chain inet ref prerouting '{ type nat hook prerouting priority dstnat; }' &&
	nft add chain inet ref forward '{ type filter hook forward priority 0; }' &&
	nft add rule inet ref prerouting iifname "ppp0" ip daddr 203.0.113.24 tcp dport 8080 \
		dnat ip to 192.168.1.20:80 &&
	nft add rule inet ref prerouting iifname "ppp0" ip daddr 203.0.113.24 udp dport 6000-6010 \
		dnat ip to 192.168.1.20 &&
	nft add rule inet ref prerouting iifname "ppp0" ip daddr 203.0.113.24 tcp dport 8000-8002 \
		dnat ip to 192.168.1.20 : tcp dport map { 8000 : 9000, 8001 : 9001, 8002 : 9002 } &&
	nft add rule inet ref prerouting iifname "wan421" ip daddr 192.0.2.10 tcp dport 443 \
		dnat ip to 192.168.1.21:443 &&
	nft add rule inet ref prerouting iifname "wan421" ip daddr 192.0.2.10 udp dport 68 accept &&
	nft add rule inet ref prerouting iifname "wan421" ip daddr 192.0.2.10 \
		dnat ip to 192.168.1.30 &&
	nft add rule inet ref forward ct status dnat accept
# The expressions of the rules of a chain, as the kernel gives them back.
bytecode() {
	nft --debug=netlink list chain inet "$1" "$2" 2>/dev/null | grep '^  \['
}
check '[ -n "$(bytecode ref prerouting)" ] &&
	[ "$(bytecode onu prerouting)" = "$(bytecode ref prerouting)" ]'
check '[ "$(bytecode onu forward | grep -A3 "ct load status")" = "$(bytecode ref forward)" ]'
# The map's elements, as the kernel gives them back, the same as nft's; a
# hash table gives them in an order of its own.
elements() {
	nft --debug=netlink list table inet "$1" 2>/dev/null | grep '^	element' | sort
}
check '[ "$(elements onu | wc -l)" -eq 3 ] && [ "$(elements onu)" = "$(elements ref)" ]'
nft delete table inet ref

echo "the longest table: every network by every way out, every forward of each protocol"
{
	for i in 1 2 3 4 5 6 7 8; do
		echo "lan net1 192.168.$i.0/24"
	done
	echo "out ppp*"
	for i in 1 2 3 4 5 6; do
		echo "out wan$((3000 + i))"
	done
	i=0
	while [ $i -lt 96 ]; do
		echo "fwd averylongname1 203.0.113.24 tcp $((10000 + 10 * i))-$((10009 + 10 * i)) 192.168.1.20 $((10000 + 10 * i))"
		echo "fwd averylongname1 203.0.113.24 udp $((10000 + 10 * i))-$((10009 + 10 * i)) 192.168.1.20 $((10000 + 10 * i))"
		i=$((i + 1))
	done
	for i in 1 2 3 4 5 6 7; do
		echo "dmz averylongname$i 203.0.113.$i 192.168.1.30"
	done
	echo "offload off"
} >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ] && [ -z "$err" ]'
check '[ "$(table | grep -c "dnat ip to")" -eq $((192 + 7)) ]'

echo "the longest table of ranges moved: every forward, and every port a table moves"
# 1024 ports of 192 forwards: the first 64 of six ports, the others of five,
# and the last of $1 more.
moved() {
	awk '!/^fwd/' "$T/fw"
	i=0
	while [ $i -lt 192 ]; do
		n=$((5 + (i < 64) + (i == 191) * $1))
		p=$((10000 + 10 * i))
		echo "fwd averylongname1 203.0.113.24 $([ $((i % 2)) -eq 0 ] && echo tcp || echo udp) $p-$((p + n - 1)) 192.168.1.20 $((p + 20000))"
		i=$((i + 1))
	done
}
moved 0 >"$T/fw.moved"
run apply "$T/fw.moved"
check '[ $rc -eq 0 ] && [ -z "$err" ]'
check '[ "$(table | grep -c "dport map {")" -eq 192 ] &&
	[ "$(nft --debug=netlink list table inet onu | grep -c "^	element")" -eq 1024 ]'
check "has '10000 : 30000, 10001 : 30001, 10002 : 30002, 10003 : 30003, 10004 : 30004, 10005 : 30005 }'"
# A port more than a table moves is no table: the one there is stays.
moved 1 >"$T/fw.over"
run apply "$T/fw.over"
check '[ $rc -eq 2 ] && echo "$err" | grep -q "more than 1024 ports moved"'
check '[ "$(table | grep -c "dport map {")" -eq 192 ]'

echo "a table replaces the one there is, whole"
printf 'lan net1 172.16.0.0/12\nout ppp*\noffload off\n' >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ] && [ -z "$err" ]'
check "has 'ip saddr 172.16.0.0/12 masquerade' && ! has '192.168.1.0' && ! has '10.0.0.0'"
check '! has "chain prerouting" && ! has "dnat"'
check '[ "$(nft list tables | grep -c "inet onu")" -eq 1 ]'

echo "what is no table changes nothing"
printf 'lan net1 172.16.0.1/12\n' >"$T/bad"
run apply "$T/bad"
check '[ $rc -eq 2 ] && echo "$err" | grep -q "no network of the table"'
check "has 'ip saddr 172.16.0.0/12 masquerade'"
run apply "$T/none"
check '[ $rc -eq 2 ]'
run frobnicate
check '[ $rc -eq 2 ]'

echo "the table taken away, and again"
run clear
check '[ $rc -eq 0 ] && ! nft list tables | grep -q "inet onu"'
run clear
check '[ $rc -eq 0 ]'

# Packets, by three network namespaces: the stick, with the table; the
# internet, by a connection's interface; and the DMZ, a device of a network.
# The answer to a ping tells who gave it: the stick's own leaves with a TTL
# of 64, the device's, which the stick routes, with 63.
echo "what a DMZ takes, and what stays with the stick, packet by packet"
if ! command -v python3 >/dev/null 2>&1 || ! ip netns add onustick 2>/dev/null; then
	echo "  skipped: it needs python3 and network namespaces"
else
	cat >"$T/pkt.py" <<'EOF'
# echo DST: the TTL of the answer to a ping of DST. listen PORT: what one UDP
# datagram to PORT brings. send DST PORT TEXT. ask DST PORT: the answer to a
# datagram. answer PORT TEXT: answer one datagram with TEXT.
import os, socket, struct, sys, time
m, a = sys.argv[1], sys.argv[2:]
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.settimeout(3)
if m == "echo":
    s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)
    s.settimeout(2)
    ident = os.getpid() & 0xffff
    body = struct.pack("!BBHHH", 8, 0, 0, ident, 1) + b"onu!"
    c = sum(struct.unpack("!6H", body))
    c = (c >> 16) + (c & 0xffff)
    c = ~(c + (c >> 16)) & 0xffff
    s.sendto(struct.pack("!BBHHH", 8, 0, c, ident, 1) + b"onu!", (a[0], 0))
    end = time.time() + 2
    while time.time() < end:
        try:
            d, src = s.recvfrom(1500)
        except socket.timeout:
            break
        h = (d[0] & 15) * 4
        if d[h] == 0 and struct.unpack("!H", d[h + 4:h + 6])[0] == ident and src[0] == a[0]:
            print(d[8])
            sys.exit(0)
    sys.exit(1)
if m in ("listen", "answer"):
    u.bind(("0.0.0.0", int(a[0])))
    d, src = u.recvfrom(100)
    if m == "answer":
        u.sendto(a[1].encode(), src)
    else:
        print(d.decode())
if m in ("send", "ask"):
    u.sendto((a[2] if m == "send" else "?").encode(), (a[0], int(a[1])))
    if m == "ask":
        print(u.recvfrom(100)[0].decode())
EOF
	S="ip netns exec onustick" I="ip netns exec onuinet" D="ip netns exec onudev"
	ip netns add onuinet && ip netns add onudev &&
		ip link add wan840 type veth peer name inet0 &&
		ip link set wan840 netns onustick && ip link set inet0 netns onuinet &&
		ip link add net2 type veth peer name dev0 &&
		ip link set net2 netns onustick && ip link set dev0 netns onudev &&
		$S ip addr add 192.0.2.10/24 dev wan840 && $S ip addr add 192.168.2.1/24 dev net2 &&
		$I ip addr add 192.0.2.1/24 dev inet0 && $D ip addr add 192.168.2.30/24 dev dev0 &&
		$S ip link set wan840 up && $S ip link set net2 up && $S ip link set lo up &&
		$I ip link set inet0 up && $I ip link set lo up &&
		$D ip link set dev0 up && $D ip link set lo up &&
		$D ip route add default via 192.168.2.1 &&
		$S sh -c 'echo 1 >/proc/sys/net/ipv4/ip_forward' || exit 1
	printf 'lan net2 192.168.2.0/24\nout wan840\noffload off\n' >"$T/stick"
	$S "$FW" apply "$T/stick"
	check '[ "$($I python3 "$T/pkt.py" echo 192.0.2.10)" = 64 ]'
	echo "dmz wan840 192.0.2.10 192.168.2.30" >>"$T/stick"
	$S "$FW" apply "$T/stick"
	# A ping of the connection's address goes to the DMZ, which answers.
	check '[ "$($I python3 "$T/pkt.py" echo 192.0.2.10)" = 63 ]'
	# The answers to the stick's own ping and UDP come back to it.
	check '[ "$($S python3 "$T/pkt.py" echo 192.0.2.1)" = 64 ]'
	$I python3 "$T/pkt.py" answer 9999 back &
	sleep 0.5
	check '[ "$($S python3 "$T/pkt.py" ask 192.0.2.1 9999)" = back ]'
	wait
	# DHCP's answer to the client's port stays with the stick; another port goes on.
	$S python3 "$T/pkt.py" listen 68 >"$T/s68" &
	$D python3 "$T/pkt.py" listen 5000 >"$T/d5000" &
	sleep 0.5
	$I python3 "$T/pkt.py" send 192.0.2.10 68 offer
	$I python3 "$T/pkt.py" send 192.0.2.10 5000 hello
	wait
	check '[ "$(cat "$T/s68")" = offer ] && [ "$(cat "$T/d5000")" = hello ]'
fi

echo "test_onu_fw: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
