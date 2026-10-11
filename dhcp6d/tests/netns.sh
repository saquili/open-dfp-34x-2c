#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# dhcp6d end to end, in three network namespaces: an operator that delegates
# 2001:db8:1200::/56 (dhcp6d -s), the stick (dhcp6d -w -l) and a host behind
# it (dhcp6d -c, and the kernel taking the stick's advertisements). The host
# puts an address of the delegated prefix on itself and reaches the
# operator's network through the stick. It needs root and network
# namespaces, a privileged container for one, which "make check" is not:
#
#   docker run --rm --privileged -v "$PWD":/d6 debian:trixie \
#     sh -c 'apt-get -qq update && apt-get -qq install -y make gcc iproute2 \
#            iputils-ping procps >/dev/null && cd /d6 &&
#            make build/dhcp6d-none && tests/netns.sh'

set -u
D6=${D6:-build/dhcp6d-none}
T=$(mktemp -d)
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

cleanup() {
	for p in $(cat "$T"/*.pid 2>/dev/null); do kill "$p" 2>/dev/null; done
	sleep 1
	ip netns del op 2>/dev/null
	ip netns del st 2>/dev/null
	ip netns del ho 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT

for ns in op st ho; do ip netns add $ns; done
ip link add op0 netns op type veth peer name st0 netns st
ip link add st1 netns st type veth peer name h0 netns ho
for pair in op:op0 st:st0 st:st1 ho:h0; do
	ns=${pair%%:*} dev=${pair#*:}
	ip -n "$ns" link set lo up
	ip netns exec "$ns" sysctl -q -w net.ipv6.conf."$dev".accept_dad=0
	ip -n "$ns" link set "$dev" up
done
ip netns exec st sysctl -q -w net.ipv6.conf.all.forwarding=1
ip netns exec ho sysctl -q -w net.ipv6.conf.h0.accept_ra=2
# The operator's network, which the host should reach.
ip -n op address add 2001:db8:ffff::1/128 dev lo
sleep 2

ip netns exec op "$D6" -s 2001:db8:1200::/56 -l op0 -d 2001:db8::53 \
	>"$T/op.log" 2>&1 &
echo $! >"$T/op.pid"
mkdir "$T/st"
ip netns exec st "$D6" -w st0 -l st1 -m st0 -o "$T/st" -v >"$T/st.log" 2>&1 &
echo $! >"$T/st.pid"
ip netns exec ho "$D6" -c h0 >"$T/ho.out" 2>"$T/ho.log" &
echo $! >"$T/ho.pid"

for _ in $(seq 1 30); do
	grep -q '^prefix' "$T/ho.out" && [ -s "$T/st/host" ] && break
	sleep 1
done

echo "the stick takes the operator's prefix and delegates it to the host"
check '[ "$(cat "$T/st/prefix")" = 2001:db8:1200::/56 ]'
check 'grep -q "^prefix 2001:db8:1200::/56$" "$T/ho.out"'
check '[ -s "$T/st/host" ]'
check 'ip -n st -6 route show proto dhcp | grep -q "^default via fe80::.* dev st0"'
check 'ip -n st -6 route show proto dhcp | grep -q "^unreachable 2001:db8:1200::/56"'
check 'ip -n st -6 route show proto dhcp | grep -q "^2001:db8:1200::/56 via fe80::.* dev st1"'
check 'ip -n op -6 route show proto dhcp | grep -q "^2001:db8:1200::/56 via fe80::.* dev op0"'

echo "the host takes the stick as its router, and reaches the operator's network"
for _ in $(seq 1 10); do
	ip -n ho -6 route show default | grep -q "via fe80::" && break
	sleep 1
done
check 'ip -n ho -6 route show default | grep -q "via fe80::.* dev h0"'
ip -n ho address add 2001:db8:1200:1::1/128 dev lo
check 'ip netns exec ho ping -6 -c 2 -W 2 -I 2001:db8:1200:1::1 2001:db8:ffff::1 >/dev/null'

echo "a stopped stick takes its routes away and tells the host"
kill "$(cat "$T/st.pid")"
sleep 2
check '[ -z "$(ip -n st -6 route show proto dhcp)" ]'
check '[ ! -e "$T/st/prefix" ] && [ ! -e "$T/st/host" ]'
check 'grep -q "stopping" "$T/st.log"'
for _ in $(seq 1 5); do
	ip -n ho -6 route show default | grep -q "via fe80::" || break
	sleep 1
done
check '! ip -n ho -6 route show default | grep -q "via fe80::"'

echo "with a table of its own: the default route in it, and the prefix routed by it"
# A new host, which asks the new stick from the start.
kill "$(cat "$T/ho.pid")"
ip netns exec st "$D6" -w st0 -l st1 -m st0 -o "$T/st" -t 102 -v >"$T/st.log" 2>&1 &
echo $! >"$T/st.pid"
ip netns exec ho "$D6" -c h0 >"$T/ho.out" 2>"$T/ho.log" &
echo $! >"$T/ho.pid"
for _ in $(seq 1 30); do
	[ -s "$T/st/host" ] && ip -n st -6 rule show | grep -q "lookup 102" && break
	sleep 1
done
check 'ip -n st -6 rule show | grep -q "from 2001:db8:1200::/56 lookup 102"'
check 'ip -n st -6 route show table 102 proto dhcp | grep -q "^default via fe80::.* dev st0"'
check '! ip -n st -6 route show proto dhcp | grep -q "^default"'
check 'ip -n st -6 route show proto dhcp | grep -q "^2001:db8:1200::/56 via fe80::.* dev st1"'
for _ in $(seq 1 10); do
	ip -n ho -6 route show default | grep -q "via fe80::" && break
	sleep 1
done
check 'ip netns exec ho ping -6 -c 2 -W 2 -I 2001:db8:1200:1::1 2001:db8:ffff::1 >/dev/null'
kill "$(cat "$T/st.pid")"
sleep 2
check '! ip -n st -6 rule show | grep -q "lookup 102"'
check '[ -z "$(ip -n st -6 route show table 102 proto dhcp 2>/dev/null)" ]'

echo "a stick that starts again routes the prefix to the host of before at once"
# A new host, bound by a stick that keeps the binding in a file and tells a
# program of it.
printf '#!/bin/sh\necho "$1" >>"%s"\n' "$T/told" >"$T/tell"
chmod +x "$T/tell"
kill "$(cat "$T/ho.pid")"
ip netns exec st "$D6" -w st0 -l st1 -m st0 -o "$T/st" -b "$T/binding" -x "$T/tell" -v \
	>"$T/st.log" 2>&1 &
echo $! >"$T/st.pid"
ip netns exec ho "$D6" -c h0 >"$T/ho.out" 2>"$T/ho.log" &
echo $! >"$T/ho.pid"
for _ in $(seq 1 30); do
	[ -s "$T/binding" ] && [ -s "$T/told" ] && break
	sleep 1
done
check 'grep -q "^[0-9a-f]* 1 fe80::[0-9a-f:]* 2001:db8:1200::/56$" "$T/binding"'
check '[ "$(cat "$T/told")" = dhcp6d ]'
kill "$(cat "$T/st.pid")"
sleep 2
check '[ -s "$T/binding" ]'
check '[ -z "$(ip -n st -6 route show proto dhcp)" ]'
# The host asks for nothing until its T1, ten minutes on: the route comes
# from the binding of before alone.
ip netns exec st "$D6" -w st0 -l st1 -m st0 -o "$T/st" -b "$T/binding" -v >"$T/st.log" 2>&1 &
echo $! >"$T/st.pid"
for _ in $(seq 1 15); do
	ip -n st -6 route show proto dhcp | grep -q "^2001:db8:1200::/56 via" && break
	sleep 1
done
check 'grep -q "the host of before" "$T/st.log"'
check 'ip -n st -6 route show proto dhcp | grep -q "^2001:db8:1200::/56 via fe80::.* dev st1"'
check '[ -s "$T/st/host" ]'
for _ in $(seq 1 10); do
	ip -n ho -6 route show default | grep -q "via fe80::" && break
	sleep 1
done
check 'ip netns exec ho ping -6 -c 2 -W 2 -I 2001:db8:1200:1::1 2001:db8:ffff::1 >/dev/null'
# The host that releases its binding is forgotten.
kill "$(cat "$T/ho.pid")"
for _ in $(seq 1 5); do
	[ -e "$T/binding" ] || break
	sleep 1
done
check '[ ! -e "$T/binding" ]'
check '! ip -n st -6 route show proto dhcp | grep -q "^2001:db8:1200::/56 via"'
kill "$(cat "$T/st.pid")"
sleep 2

echo "a stick killed leaves its routes, which the next one takes away"
ip netns exec st "$D6" -w st0 -l st1 -m st0 -o "$T/st" -t 103 -v >"$T/st.log" 2>&1 &
echo $! >"$T/st.pid"
ip netns exec ho "$D6" -c h0 >"$T/ho.out" 2>"$T/ho.log" &
echo $! >"$T/ho.pid"
for _ in $(seq 1 30); do
	[ -s "$T/st/host" ] && ip -n st -6 rule show | grep -q "lookup 103" && break
	sleep 1
done
kill -9 "$(cat "$T/st.pid")"
sleep 1
check 'ip -n st -6 route show proto dhcp | grep -q "^unreachable 2001:db8:1200::/56"'
check 'ip -n st -6 rule show | grep -q "from 2001:db8:1200::/56 lookup 103"'
check '[ -s "$T/st/prefix" ]'
# The operator delegates another prefix now.
kill "$(cat "$T/op.pid")"
sleep 1
ip netns exec op "$D6" -s 2001:db8:3400::/56 -l op0 -d 2001:db8::53 >"$T/op.log" 2>&1 &
echo $! >"$T/op.pid"
ip netns exec st "$D6" -w st0 -l st1 -m st0 -o "$T/st" -t 103 -v >"$T/st.log" 2>&1 &
echo $! >"$T/st.pid"
for _ in $(seq 1 30); do
	[ "$(cat "$T/st/prefix" 2>/dev/null)" = 2001:db8:3400::/56 ] && break
	sleep 1
done
check '[ "$(cat "$T/st/prefix")" = 2001:db8:3400::/56 ]'
check '! ip -n st -6 route show proto dhcp | grep -q "2001:db8:1200::/56"'
check '! ip -n st -6 rule show | grep -q "from 2001:db8:1200::/56"'
check 'ip -n st -6 route show proto dhcp | grep -q "^unreachable 2001:db8:3400::/56"'
check 'ip -n st -6 rule show | grep -q "from 2001:db8:3400::/56 lookup 103"'
kill "$(cat "$T/st.pid")"
sleep 2

echo "netns: $checks checks, $failures failed"
[ "$failures" -eq 0 ] || { echo "--- stick"; cat "$T/st.log"; echo "--- operator"; cat "$T/op.log"; }
[ "$failures" -eq 0 ]
