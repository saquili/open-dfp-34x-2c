#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of onu-nl against the kernel it runs on, read back with tc of
# iproute2, on two dummy interfaces, which take filters in software only.
# Making them needs root with CAP_NET_ADMIN, as in a container started with
# --cap-add NET_ADMIN; without that, or without tc, the test says that it
# skipped. Filters in the switch and the parameters of devlink need the
# stick, and are tried there.

set -u
NL=${BUILD:-$(cd "$(dirname "$0")/.." && pwd)/build}/onu-nl
D0=onunl0
D1=onunl1
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

if ! command -v tc >/dev/null 2>&1 || ! ip link add $D0 type dummy 2>/dev/null; then
	echo "test_onu_nl: SKIPPED: it needs root with CAP_NET_ADMIN, and tc"
	exit 0
fi
T=$(mktemp -d)
cleanup() {
	ip link del $D0 2>/dev/null
	ip link del $D1 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT
ip link add $D1 type dummy && ip link set $D0 up && ip link set $D1 up || exit 1

run() {
	out=$("$NL" "$@" 2>"$T/err")
	rc=$?
	err=$(cat "$T/err")
}

# The filters of $D0's ingress as tc shows them, one line each.
shown() {
	tc filter show dev $D0 ingress | tr '\n\t' '  ' | sed 's/filter protocol/\nfilter protocol/g'
}

# The line of the filter of priority $1.
at() {
	shown | grep -E "pref $1 "
}

prefs() {
	tc filter show dev $D0 ingress |
		awk '{ for (i = 1; i < NF; i++) if ($i == "pref") print $(i + 1) }' | sort -n -u
}

# What tc says when "tc $@" fails: the kernel's own message, or the error.
tc_says() {
	tc "$@" 2>&1 >/dev/null |
		sed -n -e '1s/^Error: \(.*\)\.$/\1/p' -e '1s/^RTNETLINK answers: //p'
}

# Whether this kernel has the matchall classifier, which some leave out.
matchall=false
tc qdisc add dev $D1 clsact && tc filter add dev $D1 ingress matchall action drop 2>/dev/null &&
	matchall=true
tc qdisc del dev $D1 clsact

echo "the clsact qdisc, and again"
run clsact add $D0
check '[ $rc -eq 0 ] && [ -z "$err" ]'
check 'tc qdisc show dev $D0 | grep -q "^qdisc clsact ffff: parent ffff:fff1"'
run clsact add $D0
check '[ $rc -eq 0 ] && [ -z "$err" ]'

echo "the frames of a VLAN trapped, at a priority"
run filter add $D0 pref 1 vlan 410 trap
check '[ $rc -eq 0 ] && [ -z "$out$err" ]'
check 'at 1 | grep -q "filter protocol 802.1Q pref 1 flower chain 0 handle 0x1 *vlan_id 410 "'
check 'at 1 | grep -q "action order 1: gact action trap"'

echo "the frames of a VLAN dropped"
run filter add $D0 pref 2 vlan 36 drop
check '[ $rc -eq 0 ]'
check 'at 2 | grep -q "pref 2 flower .*vlan_id 36 .*gact action drop"'

echo "the IPv4 packets to an address dropped, and to a prefix"
run filter add $D0 pref 3 dst 192.0.2.10 drop
check '[ $rc -eq 0 ] && [ -z "$out$err" ]'
check 'at 3 | grep -q "filter protocol ip pref 3 flower .*dst_ip 192.0.2.10 .*gact action drop"'
run filter add $D0 pref 4 dst 198.51.100.77/24 drop
check '[ $rc -eq 0 ]'
check 'at 4 | grep -q "dst_ip 198.51.100.0/24 .*gact action drop"'
run filter add $D0 pref 6 dst 192.0.2.300 drop
check '[ $rc -eq 2 ] && [ -z "$(at 6)" ]'
run filter add $D0 pref 6 vlan 5 dst 192.0.2.1 drop
check '[ $rc -eq 2 ] && [ -z "$(at 6)" ]'
run filter del $D0 pref 3
run filter del $D0 pref 4

echo "every frame handed to the ingress of another interface, at the priority the kernel picks"
run filter add $D0 redirect $D1
if $matchall; then
	check '[ $rc -eq 0 ]'
	check '[ "$(shown | grep -c "filter protocol all pref [0-9]* matchall")" -eq 1 ]'
	check 'shown | grep "matchall" | grep -q "mirred (Ingress Redirect to device $D1) stolen"'
else
	echo "  (this kernel has no matchall classifier: onu-nl is to fail as tc does)"
	why=$(tc_says filter add dev $D0 ingress matchall action mirred ingress redirect dev $D1)
	check '[ $rc -eq 1 ] && [ -n "$why" ] && [ "$err" = "onu-nl: filter not added: No such file or directory: $why" ]'
fi

echo "the frames of a VLAN handed on"
run filter add $D0 pref 1000 vlan 7 redirect $D1
check '[ $rc -eq 0 ]'
check 'at 1000 | grep -q "vlan_id 7 .*mirred (Ingress Redirect to device $D1) stolen"'

echo "the priorities, as tc lists them"
run filter prefs $D0
check '[ $rc -eq 0 ] && [ -z "$err" ]'
check '[ "$out" = "$(prefs)" ]'
check '[ "$(echo "$out" | wc -l)" -eq $($matchall && echo 4 || echo 3) ]'
check '[ "$(echo "$out" | head -n 3 | tr "\n" " ")" = "1 2 1000 " ]'

echo "a priority removed, and again"
run filter del $D0 pref 2
check '[ $rc -eq 0 ] && [ -z "$err" ]'
check '[ -z "$(at 2)" ] && [ -n "$(at 1)" ]'
run filter del $D0 pref 2
why=$(tc_says filter del dev $D0 ingress pref 2)
check '[ $rc -eq 1 ] && [ -n "$why" ]'
check '[ "$err" = "onu-nl: filter not removed: No such file or directory: $why" ]'

echo "a filter in the switch alone, which a dummy interface has not"
run filter add $D0 pref 5 vlan 9 hw trap
why=$(tc_says filter add dev $D0 ingress pref 5 protocol 802.1Q flower skip_sw vlan_id 9 action trap)
check '[ $rc -eq 1 ] && [ -z "$(at 5)" ] && [ -n "$why" ]'
check 'case $err in "onu-nl: filter not added: "*"$why") true ;; *) false ;; esac'

echo "the clsact qdisc removed, its filters with it"
run clsact del $D0
check '[ $rc -eq 0 ]'
check '! tc qdisc show dev $D0 | grep -q clsact'
run filter prefs $D0
check '[ $rc -eq 0 ] && [ -z "$out" ]'
run clsact del $D0
check '[ $rc -eq 1 ] && echo "$err" | grep -q "^onu-nl: clsact not removed: "'

echo "interfaces that are not there"
run clsact add $D0
run filter add nosuch0 pref 1 vlan 1 trap
check '[ $rc -eq 1 ] && [ "$err" = "onu-nl: no interface nosuch0" ]'
run filter add $D0 pref 1 redirect nosuch0
check '[ $rc -eq 1 ] && [ "$err" = "onu-nl: no interface nosuch0" ] && [ -z "$(prefs)" ]'

echo "command lines it refuses"
for line in "" "clsact" "clsact add" "clsact show $D0" "clsact add $D0 $D1" \
	"filter add $D0" "filter add $D0 pref 0 trap" "filter add $D0 pref 65536 trap" \
	"filter add $D0 pref 1x trap" "filter add $D0 pref -1 trap" "filter add $D0 vlan 4096 drop" \
	"filter add $D0 pref 1 pref 2 trap" "filter add $D0 hw hw trap" "filter add $D0 trap drop" \
	"filter add $D0 drop $D1" "filter add $D0 redirect" "filter add $D0 redirect $D1 $D1" \
	"filter add $D0 pref" "filter del $D0" "filter del $D0 pref" "filter del $D0 pref 0" \
	"filter prefs" "filter prefs $D0 $D1" "filter show $D0" "param" "param a b c" "nosuch a b"; do
	# $line is split into words on purpose.
	run $line
	check '[ $rc -eq 2 ] && echo "$err" | grep -q "^usage: onu-nl "'
done
check '[ -z "$(prefs)" ]'

echo "a parameter that the kernel has not: no devlink instance, or not this parameter"
run param sfp_lane
check '[ $rc -eq 1 ] && [ -z "$out" ] && echo "$err" | grep -q "^onu-nl: "'

echo "test_onu_nl: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
