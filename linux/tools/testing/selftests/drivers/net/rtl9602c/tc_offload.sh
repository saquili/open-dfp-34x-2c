#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Filters of tc that the RTL9602C switch carries out (skip_sw): a filter on
# the ingress of the SFP port that drops or traps the frames of a VLAN or the
# IPv4 packets to an address is in the switch ("in_hw"), and one it cannot
# carry out is refused with the driver's own words and left out.
#
# The filters have priorities from 60001, outside those that a running ONU
# stack keeps its own rules at, and match a VLAN and an address that no
# traffic of the board uses, so that the test cannot take frames from it. The
# clsact qdisc is made if the port has none, and taken away again if so.

ALL_TESTS="
	test_a_vlan_is_dropped_in_the_switch
	test_a_vlan_is_trapped_in_the_switch
	test_an_address_is_dropped_in_the_switch
	test_a_key_the_switch_cannot_match_is_refused
	test_an_action_the_switch_cannot_do_is_refused
	test_a_filter_goes_when_it_is_deleted
"

. "$(dirname "$0")/lib.sh"

PREF=60001
LAST=60005
MADE=no

cleanup()
{
	local pref=$PREF

	while [ $pref -le $LAST ]; do
		tc filter del dev "$UNI" ingress pref $pref >/dev/null 2>&1
		pref=$((pref + 1))
	done
	[ $MADE = no ] || tc qdisc del dev "$UNI" clsact >/dev/null 2>&1
}

# Whether the filter of priority $1 is in the switch.
in_hw()
{
	tc filter show dev "$UNI" ingress pref "$1" 2>/dev/null | grep -q "in_hw"
}

# Whether a filter of priority $1 is there.
installed()
{
	tc filter show dev "$UNI" ingress pref "$1" 2>/dev/null | grep -q flower
}

# Add the filter of priority $1 with the words after it; what tc said is in $out.
add()
{
	local pref=$1

	shift
	out=$(tc filter add dev "$UNI" ingress pref "$pref" "$@" 2>&1)
	rc=$?
}

test_a_vlan_is_dropped_in_the_switch()
{
	add $PREF protocol 802.1Q flower skip_sw vlan_id 4001 action drop
	check_ok "a drop by VLAN" || return 1
	in_hw $PREF || { mismatch "the drop by VLAN" "in_hw" "not"; return 1; }
}

test_a_vlan_is_trapped_in_the_switch()
{
	add $((PREF + 1)) protocol 802.1Q flower skip_sw vlan_id 4002 action trap
	check_ok "a trap by VLAN" || return 1
	in_hw $((PREF + 1)) || { mismatch "the trap by VLAN" "in_hw" "not"; return 1; }
}

test_an_address_is_dropped_in_the_switch()
{
	add $((PREF + 2)) protocol ip flower skip_sw dst_ip 192.0.2.77/32 action drop
	check_ok "a drop by address" || return 1
	in_hw $((PREF + 2)) || { mismatch "the drop by address" "in_hw" "not"; return 1; }
}

# A key the ACL has no field for, here the TTL, is refused, with the words of the driver.
test_a_key_the_switch_cannot_match_is_refused()
{
	add $((PREF + 3)) protocol ip flower skip_sw ip_ttl 5 action drop
	check_refused "a match of the TTL" "rtl9602c:" || return 1
	! installed $((PREF + 3)) || { mismatch "the refused filter" "left out" "there"; return 1; }
}

# The ACL does VLAN actions, drop and trap; a mirror to another port it cannot.
test_an_action_the_switch_cannot_do_is_refused()
{
	add $((PREF + 4)) protocol 802.1Q flower skip_sw vlan_id 4005 \
		action mirred egress mirror dev pon
	check_refused "a mirror" "rtl9602c:" || return 1
	! installed $((PREF + 4)) || { mismatch "the refused filter" "left out" "there"; return 1; }
}

test_a_filter_goes_when_it_is_deleted()
{
	local pref ret=0

	for pref in $PREF $((PREF + 1)) $((PREF + 2)); do
		tc filter del dev "$UNI" ingress pref "$pref" 2>/dev/null ||
			{ mismatch "a delete of $pref" 0 "failed"; ret=1; continue; }
		! installed "$pref" || { mismatch "filter $pref" "gone" "there"; ret=1; }
	done
	return $ret
}

require_device
require_command tc
tc qdisc show dev "$UNI" 2>/dev/null | grep -q clsact || {
	tc qdisc add dev "$UNI" clsact 2>/dev/null || skip_all "no clsact on $UNI"
	MADE=yes
}
trap cleanup EXIT
run_tests
