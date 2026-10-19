#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of onu-fw's rules in the ACL against the kernel it runs on, read back
# with tc, on dummy interfaces named as the stick's, sfp and pon. A dummy
# interface offloads nothing, so the filters are kept in software
# (ONU_FW_ACL_SW) and their skip_sw is the stick's to try. The kernel must
# give back for each of onu-fw's requests the filter it gives back for the
# tc command that onu-fw acl-show prints. Making the interfaces needs root
# with CAP_NET_ADMIN, as in a container started with --cap-add NET_ADMIN;
# without that, or without tc, the test says that it skipped.

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

if ! command -v tc >/dev/null 2>&1 || [ -e /sys/class/net/sfp ] ||
	! ip link add sfp type dummy 2>/dev/null; then
	echo "test_onu_fw_acl: SKIPPED: it needs root with CAP_NET_ADMIN, tc, and no interface sfp"
	exit 0
fi
T=$(mktemp -d)
cleanup() {
	ip link del sfp 2>/dev/null
	ip link del pon 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT
ip link add pon type dummy || exit 1
export ONU_FW_ACL_SW=1

# The filters of DEV's ingress as tc lists them, but the numbers of the
# actions, which the kernel counts.
filters() {
	tc filter show dev "$1" ingress | grep -v "index [0-9]* ref"
}

# The priorities of the firewall's range on DEV's ingress, one a line.
prefs() {
	tc filter show dev "$1" ingress | awk '$4 == "pref" && $5 >= 400 && $5 < 500 { print $5 }' |
		sort -un
}

cat >"$T/acl.conf" <<-EOF
	lan 1 192.168.1.0/24 pppoe:1
	lan 2 10.0.0.0/24 ipoe:1
	wan pppoe:1 ppp0 198.51.100.4
	wan ipoe:1 wan420 203.0.113.2
	dev 1 00:00:5e:00:53:a1 192.168.1.10
	dev 2 00:00:5e:00:53:a2 10.0.0.20
	fwd ppp0 198.51.100.4 tcp 8080 192.168.1.10 80
	fwd wan420 203.0.113.2 udp 51820 10.0.0.30 51820
	dmz wan420 203.0.113.2 10.0.0.20
	rule 1_allow_lan:2_dev:1:00:00:5e:00:53:a1_tcp_9100_any_on
	rule 2_block_lans_internet_tcp_6000-6010_any_on
	rule 3_allow_lan:1_internet:203.0.113.0/24_both_53_any_on
	rule 4_allow_internet:198.51.100.0/24_lan:1_tcp_80_any_on
	rule 5_block_internet_lans_udp__any_on
	rule 6_block_internet_dev:2:00:00:5e:00:53:a2_icmp__any_on
	rule 7_block_dev:1:00:00:5e:00:53:a1_internet_any__any_on
	rule 8_allow_lans_internet_tcp_443_ipv6_on
EOF

echo "the rules into the ACL, beside a filter of another writer"
tc qdisc add dev sfp clsact
tc filter add dev sfp ingress pref 1 protocol 802.1Q flower vlan_id 4094 action drop
"$FW" acl "$T/acl.conf" "$T/status" 2>"$T/err"
rc=$?
check '[ $rc -eq 0 ] && [ ! -s "$T/err" ]'
check '[ "$(cat "$T/status")" = "$(printf "%s\n" "1 in 2" "2 in 6" "3 in 4" "4 in 2" "5 part 1 dmz" "6 in 1" "7 in 1" "8 out 0 ipv6")" ]'
# On sfp: 1 and its answers, the passes of the two networks, broadcast and
# the groups, 2, 3, the answers of 4 and 7; on pon the answers of 3, 4, 5, 6.
check '[ "$(prefs sfp | wc -l)" -eq 16 ] && [ "$(prefs pon | wc -l)" -eq 5 ]'
check '[ "$(prefs sfp | head -n 1)" = 400 ] && [ "$(prefs sfp | tail -n 1)" = 415 ]'
check 'filters pon | grep -q "protocol ppp_ses pref 402 flower" && filters pon | grep -q "ppp_proto ip"'
check 'filters sfp | grep -q "dst_port 6000/0xfff8"'
check 'filters sfp | grep -q "protocol 802.1Q pref 1 flower"'
filters sfp >"$T/ours.sfp"
filters pon >"$T/ours.pon"

echo "the same filters as tc's for the commands acl-show prints"
"$FW" acl-clear
check '[ -z "$(prefs sfp)" ] && [ -z "$(prefs pon)" ]'
check 'filters sfp | grep -q "protocol 802.1Q pref 1 flower"'
tc qdisc add dev pon clsact 2>/dev/null
"$FW" acl-show "$T/acl.conf" | grep "^tc " | sed "s/ skip_sw//" >"$T/tc.sh"
check '[ "$(wc -l <"$T/tc.sh")" -eq 21 ]'
sh -e "$T/tc.sh"; rc=$?
check '[ $rc -eq 0 ]'
check 'filters sfp | cmp -s - "$T/ours.sfp"'
check 'filters pon | cmp -s - "$T/ours.pon"'
"$FW" acl-clear

echo "a new set in place of the one there is, and none without a rule in force"
"$FW" acl "$T/acl.conf" "$T/status"
grep -v "^rule [2-8]_" "$T/acl.conf" >"$T/one.conf"
"$FW" acl "$T/one.conf" "$T/status"
check '[ "$(prefs sfp | tr "\n" " ")" = "400 401 " ] && [ -z "$(prefs pon)" ]'
check '[ "$(cat "$T/status")" = "1 in 2" ]'
sed "s/_on$/_off/" "$T/one.conf" >"$T/off.conf"
"$FW" acl "$T/off.conf" "$T/status"
check '[ -z "$(prefs sfp)" ] && [ "$(cat "$T/status")" = "1 out 0 off" ]'

echo "a file that is none, and an interface that is not there"
echo "rule 1_allow_lans" >"$T/bad.conf"
"$FW" acl "$T/bad.conf" "$T/status" 2>/dev/null; rc=$?
check '[ $rc -eq 2 ]'
ip link del pon
"$FW" acl "$T/acl.conf" "$T/status" 2>"$T/err"; rc=$?
check '[ $rc -eq 1 ] && grep -q "no interface" "$T/err"'
check '"$FW" acl-clear'

echo "test_onu_fw_acl: $((checks - failures)) of $checks"
[ "$failures" -eq 0 ]
