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
# skipped. Then packets go through a table in network namespaces, sent and
# taken by python3, without which that part says that it skipped. Last, the
# guard of the stick's own programs, in network namespaces of its own, read
# back with nft and tried packet by packet.

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
	for ns in onudev onuinet onustick onug onugmgmt onugppp onugwan onugnet onuglink; do
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
# Without a default that blocks, nothing keeps the networks in or apart.
check '! has "allow_" && ! has "oifname \"ppp*\" ct state new" && ! has "iifname \"net1\" oifname \"sfp\""'

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

echo "the firewall's defaults, and the rules that let through what they drop"
{
	printf 'lan net1 192.168.1.0/24\nlan sfp 10.0.0.0/8\nwan gem257\nout ppp*\nout wan421\n'
	printf 'default out block\ndefault between block\n'
	printf 'allow between iif sfp ether 00:00:5e:00:53:a2 oif net1 daddr 192.168.1.10 tcp 9100\n'
	printf 'allow out iif net1 daddr 203.0.113.0/24 udp 53\n'
	printf 'allow in saddr 198.51.100.7 oif net1 daddr 192.168.1.10 tcp 8000-8100\n'
	printf 'allow out icmp\n'
	printf 'allow between\n'
} >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ] && echo "$err" | grep -q "flowtable was refused"'
check 'has "chain allow_in {" && has "chain allow_out {" && has "chain allow_between {"'
# Each way's new connections jump to the chain of its rules.
check "has 'iifname \"ppp*\" oifname \"net1\" ct state new jump allow_in'"
check "has 'iifname \"wan421\" oifname \"sfp\" ct state new jump allow_in'"
check "has 'iifname \"net1\" oifname \"ppp*\" ct state new jump allow_out'"
check "has 'iifname \"sfp\" oifname \"wan421\" ct state new jump allow_out'"
check "has 'iifname \"net1\" oifname \"sfp\" ct state new jump allow_between'"
check "has 'iifname \"sfp\" oifname \"net1\" ct state new jump allow_between'"
check '! has "oifname \"net1\" oifname" && ! has "iifname \"net1\" oifname \"net1\""'
check '[ "$(at "ct state established,related accept")" -lt "$(at "jump allow_in")" ]'
# The rules, of IPv4, each in its way's chain, which drops what none takes.
check "has 'iifname \"sfp\" ether saddr 00:00:5e:00:53:a2 oifname \"net1\" ip daddr 192.168.1.10 tcp dport 9100 accept'"
check "has 'iifname \"net1\" ip daddr 203.0.113.0/24 udp dport 53 accept'"
check "has 'oifname \"net1\" ip saddr 198.51.100.7 ip daddr 192.168.1.10 tcp dport 8000-8100 accept'"
check "has 'meta l4proto icmp accept'"
chain_of() {
	nft list chain inet onu "$1" 2>/dev/null | grep -v '^table\|chain\|^[[:space:]]*}' | sed 's/^[[:space:]]*//'
}
check '[ "$(chain_of allow_out | tail -n 1)" = drop ] && [ "$(chain_of allow_out | wc -l)" -eq 3 ]'
check '[ "$(chain_of allow_between | tail -n 1)" = drop ] && [ "$(chain_of allow_in | tail -n 1)" = drop ]'
check 'chain_of allow_between | head -n 1 | grep -q "^iifname \"sfp\" ether saddr"'
# Of IPv4 alone, though nft does not say so where an address or ICMP does.
check '[ "$(nft --debug=netlink list chain inet onu allow_out | grep -c "meta load nfproto")" -eq 2 ]'

echo "the rules as nft itself makes them"
nft add table inet ref &&
	nft add chain inet ref allow_between &&
	nft add rule inet ref allow_between meta nfproto ipv4 iifname "sfp" ether saddr 00:00:5e:00:53:a2 \
		oifname "net1" ip daddr 192.168.1.10 tcp dport 9100 accept &&
	nft add rule inet ref allow_between meta nfproto ipv4 accept &&
	nft add rule inet ref allow_between drop &&
	nft add chain inet ref allow_in &&
	nft add rule inet ref allow_in meta nfproto ipv4 oifname "net1" ip saddr 198.51.100.7 \
		ip daddr 192.168.1.10 tcp dport 8000-8100 accept &&
	nft add rule inet ref allow_in drop &&
	nft add chain inet ref forward '{ type filter hook forward priority 0; }' &&
	nft add rule inet ref forward iifname "net1" oifname "sfp" ct state new jump allow_between
check '[ -n "$(bytecode ref allow_between)" ] &&
	[ "$(bytecode onu allow_between)" = "$(bytecode ref allow_between)" ]'
check '[ -n "$(bytecode ref allow_in)" ] && [ "$(bytecode onu allow_in)" = "$(bytecode ref allow_in)" ]'
check '[ "$(bytecode onu forward | grep -B7 "jump -> allow_between" | head -n 8)" = "$(bytecode ref forward)" ]'
nft delete table inet ref

echo "the defaults alone: each way blocked by a drop, no chain without rules"
printf 'lan net1 192.168.1.0/24\nlan sfp 10.0.0.0/8\nout ppp*\ndefault out block\ndefault between block\noffload off\n' >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ] && [ -z "$err" ] && ! has "allow_"'
check "has 'iifname \"net1\" oifname \"ppp*\" ct state new drop'"
check "has 'iifname \"sfp\" oifname \"net1\" ct state new drop'"
check "has 'iifname \"ppp*\" oifname \"sfp\" ct state new drop'"
# A rule of a way its default lets through is no rule of the table.
printf 'lan net1 192.168.1.0/24\nout ppp*\ndefault out allow\nallow out tcp 443\noffload off\n' >"$T/fw"
run apply "$T/fw"
check '[ $rc -eq 0 ] && ! has "allow_" && ! has "dport 443"'
printf 'default in allow\n' >"$T/bad"
run apply "$T/bad"
check '[ $rc -eq 2 ] && echo "$err" | grep -q "no default of the table"'

echo "the longest table: every rule of the longest beside every forward moved"
{
	awk '!/^offload/' "$T/fw.moved"
	echo "default out block"
	echo "default between block"
	i=0
	while [ $i -lt 192 ]; do
		case $((i % 3)) in
		0) w=in ;;
		1) w=out ;;
		*) w=between ;;
		esac
		echo "allow $w iif averylongname1 ether 00:00:5e:00:53:ff oif averylongname2 saddr 10.0.0.0/9 daddr 192.168.255.254 $([ $((i % 2)) -eq 0 ] && echo tcp || echo udp) 60000-60100"
		i=$((i + 1))
	done
	echo "offload off"
} >"$T/fw.rules"
run apply "$T/fw.rules"
check '[ $rc -eq 0 ] && [ -z "$err" ]'
check '[ "$(table | grep -c "dport 60000-60100 accept")" -eq 192 ] &&
	[ "$(table | grep -c "dport map {")" -eq 192 ]'

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

	echo "what the defaults keep, and what a rule lets through, packet by packet"
	$I ip route add 192.168.2.0/24 via 192.0.2.10
	# Out blocked: the device's datagram to the internet goes nowhere.
	printf 'lan net2 192.168.2.0/24\nout wan840\noffload off\ndefault out block\n' >"$T/stick"
	$S "$FW" apply "$T/stick"
	# The listener prints the first datagram that reaches it.
	$I python3 "$T/pkt.py" listen 7000 >"$T/i7000" &
	sleep 0.5
	$D python3 "$T/pkt.py" send 192.0.2.1 7000 blocked
	sleep 1
	echo "allow out iif net2 daddr 192.0.2.1 udp 7000" >>"$T/stick"
	$S "$FW" apply "$T/stick"
	$D python3 "$T/pkt.py" send 192.0.2.1 7000 allowed
	wait
	check '[ "$(cat "$T/i7000")" = allowed ]'
	# In: a datagram to the device's own address, which no forward takes, is
	# dropped, and a rule from the internet lets it through.
	$D python3 "$T/pkt.py" listen 7001 >"$T/d7001" &
	sleep 0.5
	$I python3 "$T/pkt.py" send 192.168.2.30 7001 blocked
	sleep 1
	echo "allow in saddr 192.0.2.0/24 oif net2 daddr 192.168.2.30 udp 7001" >>"$T/stick"
	$S "$FW" apply "$T/stick"
	$I python3 "$T/pkt.py" send 192.168.2.30 7001 allowed
	wait
	check '[ "$(cat "$T/d7001")" = allowed ]'
fi

# The guard, in a namespace of its own: in the test's own it would drop what
# the container itself takes.
echo "the guard of the stick's own programs, as nft lists it"
G="ip netns exec onug"
if ! ip netns add onug 2>/dev/null; then
	echo "  skipped: it needs network namespaces"
else
	# The guard of the interfaces below and of the lines $1, put in place.
	guard() {
		{
			echo "mgmt mgmt*"
			echo "data sfp 192.0.2.2"
			echo "session ppp*"
			echo "ipoe wan420 dhcp"
			echo "ipoe wan421"
			echo "link lan36 dhcp dhcp6"
			echo "lan net1 dhcp"
			echo "lan net2"
			printf '%b' "$1"
		} >"$T/guard"
		run_g "$T/guard"
	}
	run_g() {
		$G "$FW" guard "$1" >/dev/null 2>"$T/err"
		rc=$?
		err=$(cat "$T/err")
	}
	gtable() {
		$G nft list table inet onu_guard 2>/dev/null
	}
	ghas() {
		gtable | sed 's/^[[:space:]]*//' | grep -qxF -- "$1"
	}
	# The rules of a chain of the guard, a line each.
	gchain() {
		$G nft list chain inet onu_guard "$1" 2>/dev/null |
			grep -v '^table\|chain\|^[[:space:]]*}\|type filter' | sed 's/^[[:space:]]*//'
	}
	guard ""
	check '[ $rc -eq 0 ] && [ -z "$err" ]'
	check 'ghas "type filter hook input priority filter; policy drop;"'
	check 'ghas "type filter hook forward priority filter - 10; policy accept;"'
	# The loopback, the management VLAN and the bench's address on the host
	# link first, whole; then the answers, the errors and the drop of what is
	# wrong, ahead of what each interface takes.
	cat >"$T/want" <<'EOF'
iifname "lo" accept
iifname "mgmt*" accept
iifname "sfp" ip daddr 192.0.2.2 accept
ct state established,related accept
icmp type destination-unreachable accept
icmp type time-exceeded accept
icmp type parameter-problem accept
icmpv6 type destination-unreachable accept
icmpv6 type packet-too-big accept
icmpv6 type time-exceeded accept
icmpv6 type parameter-problem accept
ct state invalid drop
iifname "ppp*" udp dport 546 accept
iifname "ppp*" icmpv6 type nd-router-advert accept
iifname "ppp*" icmpv6 type nd-neighbor-solicit accept
iifname "ppp*" icmpv6 type nd-neighbor-advert accept
iifname "wan420" udp dport 68 accept
iifname "wan420" icmpv6 type nd-neighbor-solicit accept
iifname "wan420" icmpv6 type nd-neighbor-advert accept
iifname "wan421" icmpv6 type nd-neighbor-solicit accept
iifname "wan421" icmpv6 type nd-neighbor-advert accept
iifname "lan36" udp dport 67 accept
iifname "lan36" udp dport 547 accept
iifname "lan36" icmpv6 type nd-router-solicit accept
iifname "lan36" icmpv6 type nd-neighbor-solicit accept
iifname "lan36" icmpv6 type nd-neighbor-advert accept
iifname "net1" udp dport 67 accept
EOF
	check '[ "$(gchain input)" = "$(cat "$T/want")" ]'
	# Nothing to or from the management VLAN is forwarded.
	check '[ "$(gchain forward | tr "\n" ";")" = "iifname \"mgmt*\" drop;oifname \"mgmt*\" drop;" ]'

	echo "a ping answered from either side, as the settings say"
	guard "ping in\n"
	check '[ $rc -eq 0 ] && ghas "iifname \"ppp*\" icmp type echo-request accept" &&
		ghas "iifname \"wan421\" icmpv6 type echo-request accept" &&
		[ "$(gtable | grep -c "echo-request")" -eq 6 ]'
	guard "ping lans\n"
	check '[ $rc -eq 0 ] && ghas "iifname \"lan36\" icmp type echo-request accept" &&
		ghas "iifname \"net2\" icmpv6 type echo-request accept" &&
		[ "$(gtable | grep -c "echo-request")" -eq 6 ]'

	echo "the guard as nft itself makes it"
	guard "ping in\nping lans\n"
	{
		echo "table inet ref {"
		echo "chain input {"
		echo "type filter hook input priority filter; policy drop;"
		gchain input
		echo "}"
		echo "chain forward {"
		echo "type filter hook forward priority filter - 10; policy accept;"
		gchain forward
		echo "}"
		echo "}"
	} >"$T/ref.nft"
	$G nft -f "$T/ref.nft"
	# The expressions of the rules of a chain, as the kernel gives them back.
	gbytecode() {
		$G nft --debug=netlink list chain inet "$1" "$2" 2>/dev/null | grep '^  \['
	}
	check '[ "$(gchain input | wc -l)" -eq 39 ] && [ -n "$(gbytecode ref input)" ] &&
		[ "$(gbytecode onu_guard input)" = "$(gbytecode ref input)" ]'
	check '[ -n "$(gbytecode ref forward)" ] &&
		[ "$(gbytecode onu_guard forward)" = "$(gbytecode ref forward)" ]'
	$G nft delete table inet ref

	echo "what is no guard changes nothing"
	printf 'session ppp*\n' >"$T/bad"
	run_g "$T/bad"
	check '[ $rc -eq 2 ] && echo "$err" | grep -q "no management interface"'
	printf 'mgmt mgmt*\nlink lan36 dhcp7\n' >"$T/bad"
	run_g "$T/bad"
	check '[ $rc -eq 2 ] && echo "$err" | grep -q "no interface of the guard"'
	run_g "$T/none"
	check '[ $rc -eq 2 ]'
	check 'ghas "iifname \"lan36\" icmp type echo-request accept"'
	# A guard replaces the one there is, whole, beside the table of the networks.
	printf 'mgmt mgmt\n' >"$T/guard"
	run_g "$T/guard"
	check '[ $rc -eq 0 ] && ghas "iifname \"mgmt\" accept" && ! gtable | grep -q "lan36\|mgmt\*"'
	printf 'lan net1 192.168.1.0/24\nout ppp*\noffload off\n' >"$T/fw"
	$G "$FW" apply "$T/fw"
	check '[ "$($G nft list tables | sort | tr "\n" " ")" = "table inet onu table inet onu_guard " ]'
	check 'ghas "iifname \"mgmt\" accept"'
	ip netns del onug
fi

# Packets, by a namespace for the stick, with the guard, and one for each of
# its sides: the management VLAN's host, the far end of a session and of an
# IPoE connection, a device of a local network, and the router on a link a
# session hands its address down by. The stick has a server on port 22 of
# every address; the guard decides who reaches it.
echo "the guard, packet by packet"
if ! command -v python3 >/dev/null 2>&1 || ! ip netns add onug 2>/dev/null; then
	echo "  skipped: it needs python3 and network namespaces"
else
	cat >"$T/g.py" <<'EOF'
# tcp DST PORT: open, refused or timeout. hold PORT: a server of TCP on PORT of
# every address, for a minute. echo DST: whether a ping of DST, IPv4 or IPv6,
# has an answer. listen PORT: what one datagram to PORT brings, of either
# family, or none. send DST PORT TEXT.
import os, socket, struct, sys, time
m, a = sys.argv[1], sys.argv[2:]
fam = socket.AF_INET6 if a and ":" in a[0] else socket.AF_INET
if m == "tcp":
    s = socket.socket(fam)
    s.settimeout(2)
    try:
        s.connect((a[0], int(a[1])))
        print("open")
    except ConnectionRefusedError:
        print("refused")
    except OSError:
        print("timeout")
elif m == "hold":
    s = socket.socket(socket.AF_INET6)
    s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("::", int(a[0])))
    s.listen(16)
    time.sleep(60)
elif m == "echo":
    v6 = fam == socket.AF_INET6
    s = socket.socket(fam, socket.SOCK_RAW, socket.IPPROTO_ICMPV6 if v6 else socket.IPPROTO_ICMP)
    s.settimeout(2)
    ident = os.getpid() & 0xffff
    body = struct.pack("!BBHHH", 128 if v6 else 8, 0, 0, ident, 1) + b"onu!"
    if not v6:
        c = sum(struct.unpack("!6H", body))
        c = (c >> 16) + (c & 0xffff)
        c = ~(c + (c >> 16)) & 0xffff
        body = struct.pack("!BBHHH", 8, 0, c, ident, 1) + b"onu!"
    s.sendto(body, (a[0], 0))
    end = time.time() + 2
    while time.time() < end:
        try:
            d, src = s.recvfrom(1500)
        except socket.timeout:
            break
        h = 0 if v6 else (d[0] & 15) * 4
        if d[h] == (129 if v6 else 0) and struct.unpack("!H", d[h + 4:h + 6])[0] == ident:
            print("answer")
            sys.exit(0)
    print("none")
elif m == "listen":
    u = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    u.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    u.bind(("::", int(a[0])))
    u.settimeout(3)
    try:
        print(u.recvfrom(100)[0].decode())
    except socket.timeout:
        print("none")
elif m == "send":
    u = socket.socket(fam, socket.SOCK_DGRAM)
    u.sendto(a[2].encode(), (a[0], int(a[1])))
EOF
	S="ip netns exec onug"
	# The stick's interface $1, and its other end, $3, in the namespace $2.
	pair() {
		ip netns add "$2" && ip link add "$1" netns onug type veth peer name "$3" netns "$2" &&
			$S ip link set "$1" up && ip netns exec "$2" ip link set "$3" up &&
			ip netns exec "$2" ip link set lo up
	}
	# The program of g.py in the namespace $1, with the rest of the words.
	in_ns() {
		ns=$1
		shift
		ip netns exec "$ns" python3 "$T/g.py" "$@"
	}
	# What the stick's own program of g.py takes on port $1 while the
	# namespace $2 sends it $4 to $3, or none.
	stick_takes() {
		$S python3 "$T/g.py" listen "$1" >"$T/took" &
		sleep 0.5
		in_ns "$2" send "$3" "$1" "$4"
		wait $!
		cat "$T/took"
	}
	# What the namespace $1 takes on port 5353 while $2 sends it to $3.
	takes() {
		in_ns "$1" listen 5353 >"$T/took" &
		sleep 0.5
		in_ns "$2" send "$3" 5353 sent
		wait $!
		cat "$T/took"
	}
	pair mgmt onugmgmt host0 && pair ppp0 onugppp op0 && pair wan420 onugwan op1 &&
		pair net1 onugnet dev0 && pair lan36 onuglink rtr0 &&
		$S ip link set lo up &&
		$S ip addr add 198.51.100.2/30 dev mgmt &&
		$S ip addr add 192.0.2.10/24 dev ppp0 &&
		$S ip addr add 2001:db8:10::10/64 dev ppp0 nodad &&
		$S ip addr add 203.0.113.10/24 dev wan420 &&
		$S ip addr add 192.168.2.1/24 dev net1 &&
		$S ip addr add 169.254.0.1/16 dev lan36 &&
		$S ip addr add 2001:db8:36::1/64 dev lan36 nodad &&
		ip netns exec onugmgmt ip addr add 198.51.100.1/30 dev host0 &&
		ip netns exec onugmgmt ip route add default via 198.51.100.2 &&
		ip netns exec onugppp ip addr add 192.0.2.1/24 dev op0 &&
		ip netns exec onugppp ip addr add 2001:db8:10::1/64 dev op0 nodad &&
		ip netns exec onugppp ip route add default via 192.0.2.10 &&
		ip netns exec onugwan ip addr add 203.0.113.1/24 dev op1 &&
		ip netns exec onugwan ip route add default via 203.0.113.10 &&
		ip netns exec onugnet ip addr add 192.168.2.30/24 dev dev0 &&
		ip netns exec onugnet ip route add default via 192.168.2.1 &&
		ip netns exec onuglink ip addr add 169.254.7.7/16 dev rtr0 &&
		ip netns exec onuglink ip addr add 2001:db8:36::2/64 dev rtr0 nodad &&
		$S sh -c 'echo 1 >/proc/sys/net/ipv4/ip_forward' || exit 1
	$S python3 "$T/g.py" hold 22 &
	hold=$!
	sleep 0.5
	# Without the guard, a session's far end reaches the server by the
	# management address, and through the stick the host's own address.
	check '[ "$(in_ns onugppp tcp 198.51.100.2 22)" = open ]'
	check '[ "$(takes onugmgmt onugppp 198.51.100.1)" = sent ]'
	printf 'mgmt mgmt*\nsession ppp*\nipoe wan420 dhcp\nlink lan36 dhcp dhcp6\nlan net1 dhcp\n' \
		>"$T/stick"
	$S "$FW" guard "$T/stick"
	# The server answers on the management VLAN, and nobody else, by
	# whichever address they ask for.
	check '[ "$(in_ns onugmgmt tcp 198.51.100.2 22)" = open ]'
	check '[ "$(in_ns onugppp tcp 198.51.100.2 22)" = timeout ] &&
		[ "$(in_ns onugppp tcp 192.0.2.10 22)" = timeout ] &&
		[ "$(in_ns onugppp tcp 2001:db8:10::10 22)" = timeout ]'
	check '[ "$(in_ns onugwan tcp 198.51.100.2 22)" = timeout ] &&
		[ "$(in_ns onugwan tcp 203.0.113.10 22)" = timeout ]'
	check '[ "$(in_ns onugnet tcp 198.51.100.2 22)" = timeout ] &&
		[ "$(in_ns onugnet tcp 192.168.2.1 22)" = timeout ]'
	check '[ "$(in_ns onuglink tcp 169.254.0.1 22)" = timeout ] &&
		[ "$(in_ns onuglink tcp 2001:db8:36::1 22)" = timeout ]'
	# Nothing is forwarded to the management VLAN, nor from it; between the
	# other sides as before.
	check '[ "$(takes onugmgmt onugppp 198.51.100.1)" = none ] &&
		[ "$(takes onugmgmt onugnet 198.51.100.1)" = none ]'
	check '[ "$(takes onugppp onugmgmt 192.0.2.1)" = none ]'
	check '[ "$(takes onugppp onugnet 192.0.2.1)" = sent ]'
	# What each program of the stick takes on its interface: the DHCP
	# client's answers, the DHCP and DHCPv6 servers' requests, the DHCPv6
	# client's answers; on another interface, or another port, nothing.
	check '[ "$(stick_takes 68 onugwan 203.0.113.10 offer)" = offer ]'
	check '[ "$(stick_takes 68 onugppp 192.0.2.10 offer)" = none ]'
	check '[ "$(stick_takes 67 onugnet 192.168.2.1 discover)" = discover ]'
	check '[ "$(stick_takes 547 onuglink 2001:db8:36::1 solicit)" = solicit ]'
	check '[ "$(stick_takes 546 onugppp 2001:db8:10::10 reply)" = reply ]'
	check '[ "$(stick_takes 5000 onuglink 169.254.0.1 other)" = none ]'
	# The answers to the stick's own traffic come back by any side, its
	# neighbours' of IPv6 among them.
	check '[ "$($S python3 "$T/g.py" echo 192.0.2.1)" = answer ] &&
		[ "$($S python3 "$T/g.py" echo 2001:db8:36::2)" = answer ] &&
		[ "$($S python3 "$T/g.py" echo 2001:db8:10::1)" = answer ]'
	# A ping is answered on the management VLAN, elsewhere as the settings say.
	check '[ "$(in_ns onugmgmt echo 198.51.100.2)" = answer ]'
	check '[ "$(in_ns onugppp echo 192.0.2.10)" = none ] && [ "$(in_ns onugnet echo 192.168.2.1)" = none ] &&
		[ "$(in_ns onuglink echo 2001:db8:36::1)" = none ]'
	echo "ping in" >>"$T/stick"
	$S "$FW" guard "$T/stick"
	check '[ "$(in_ns onugppp echo 192.0.2.10)" = answer ] &&
		[ "$(in_ns onugppp echo 2001:db8:10::10)" = answer ] &&
		[ "$(in_ns onugwan echo 203.0.113.10)" = answer ]'
	check '[ "$(in_ns onugnet echo 192.168.2.1)" = none ]'
	echo "ping lans" >>"$T/stick"
	$S "$FW" guard "$T/stick"
	check '[ "$(in_ns onugnet echo 192.168.2.1)" = answer ] &&
		[ "$(in_ns onuglink echo 2001:db8:36::1)" = answer ]'
	# A ping answered is no way to the servers.
	check '[ "$(in_ns onugnet tcp 192.168.2.1 22)" = timeout ]'
	kill "$hold" 2>/dev/null
	wait
fi

echo "test_onu_fw: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
