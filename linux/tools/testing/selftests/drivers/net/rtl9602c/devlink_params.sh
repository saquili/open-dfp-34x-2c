#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# The devlink parameters of the RTL9602C switch driver: the 30 that
# Documentation/networking/devlink/rtl9602c.rst lists, what each gives back
# after it is set, the refusals of a value out of range, and the storm-control
# classes, whose rate must exist before their ports can.
#
# Every parameter that a test changes is put back as it was, whether the test
# passes or not. The three of the ONU (gpon_serial, gpon_password and
# gpon_disabled) and the SFP lane are read and refused, never set to a value
# that is taken: that would change what a unit that is in service says to its
# OLT or to its host, or keep it off its OLT.

ALL_TESTS="
	test_the_parameters_are_there
	test_they_are_all_runtime
	test_a_set_value_is_read_back
	test_a_value_out_of_range_is_refused
	test_a_storm_class_needs_its_rate_first
	test_the_lane_is_read_only_or_a_name
	test_driverinit_is_refused
	test_the_info_names_the_part
"

. "$(dirname "$0")/lib.sh"

PARAMS="
	dos_ports dos_checks dos_actions
	dos_synflood_thresh dos_finflood_thresh dos_icmpflood_thresh
	storm_unknown_unicast_ports storm_unknown_unicast_pps
	storm_unknown_multicast_ports storm_unknown_multicast_pps
	storm_dhcp_ports storm_dhcp_pps
	storm_igmp_mld_ports storm_igmp_mld_pps
	pktdbg_mode pktdbg_ingress_ports pktdbg_egress_ports pktdbg_reason
	loop_detect loop_detect_ports loop_detect_period
	cpu_rx_pps l3_ttl_decrement
	sfp_lane sfp_lane_now
	gpon_serial gpon_password gpon_disabled
	ptp_transparent_clock ptp_path_delay_ns
"

# Every parameter a test sets, in the order they are put back in: the ports
# of a storm class before its rate to give it up, and after it to take it up.
TOUCHED="
	storm_unknown_unicast_ports storm_unknown_unicast_pps
	storm_unknown_multicast_ports storm_unknown_multicast_pps
	storm_dhcp_ports storm_dhcp_pps
	storm_igmp_mld_ports storm_igmp_mld_pps
	dos_checks dos_actions dos_ports
	dos_synflood_thresh dos_finflood_thresh dos_icmpflood_thresh
	pktdbg_mode pktdbg_ingress_ports pktdbg_egress_ports pktdbg_reason
	loop_detect_ports loop_detect_period cpu_rx_pps l3_ttl_decrement
"
SAVED=

save_params()
{
	local name

	for name in $TOUCHED; do
		SAVED="$SAVED $name=$(param_get "$name")"
	done
}

# Twice, for the storm classes that were in use.
restore_params()
{
	local item pass

	for pass in 1 2; do
		for item in $SAVED; do
			param_set "${item%%=*}" "${item#*=}"
		done
	done
}

# Parameters that take a value other than the one they have, and the value.
ROUNDTRIP="
	dos_ports=15
	dos_checks=6
	dos_actions=6
	dos_synflood_thresh=7
	dos_finflood_thresh=8
	dos_icmpflood_thresh=9
	pktdbg_mode=5
	pktdbg_ingress_ports=5
	pktdbg_egress_ports=5
	pktdbg_reason=3
	loop_detect_ports=5
	loop_detect_period=1234
	cpu_rx_pps=4000
"

# Parameters and a value that is too wide for them: the driver answers -ERANGE.
TOO_WIDE="
	dos_ports=16
	dos_checks=65536
	dos_actions=65536
	pktdbg_ingress_ports=16
	pktdbg_egress_ports=16
	loop_detect=2
	loop_detect_ports=16
	loop_detect_period=65536
	cpu_rx_pps=524287
	storm_unknown_unicast_pps=524287
"

test_the_parameters_are_there()
{
	local listed want

	listed=$(devlink dev param show "$DEVLINK_DEV" |
		 sed -n 's/^ *name \([a-z0-9_]*\) type .*/\1/p' | sort | tr '\n' ' ')
	want=$(echo $PARAMS | tr ' ' '\n' | sort | tr '\n' ' ')
	check_eq "the parameters" "$want" "$listed"
}

test_they_are_all_runtime()
{
	local modes

	modes=$(devlink dev param show "$DEVLINK_DEV" |
		sed -n 's/^ *cmode \([a-z]*\) value .*/\1/p' | sort -u | tr '\n' ' ')
	check_eq "the modes" "runtime " "$modes"
}

# Set the parameter $1 to $2 and read it back.
roundtrip()
{
	param_set "$1" "$2"
	check_ok "$1 set to $2" || return 1
	check_param "$1" "$2"
}

test_a_set_value_is_read_back()
{
	local item old new ret=0

	for item in $ROUNDTRIP; do
		roundtrip "${item%%=*}" "${item#*=}" || ret=1
	done

	# A boolean: the other value.
	old=$(param_get l3_ttl_decrement)
	[ "$old" = true ] && new=false || new=true
	roundtrip l3_ttl_decrement "$new" || ret=1
	return $ret
}

test_a_value_out_of_range_is_refused()
{
	local item name old ret=0

	for item in $TOO_WIDE; do
		name=${item%%=*}
		old=$(param_get "$name")
		param_set "$name" "${item#*=}"
		check_refused "$name set to ${item#*=}" "out of range" || ret=1
		check_param "$name" "$old" || ret=1
	done

	# A u8 is cut off by devlink itself, before the driver sees it.
	old=$(param_get dos_synflood_thresh)
	param_set dos_synflood_thresh 256
	check_refused "dos_synflood_thresh set to 256" "range" || ret=1
	check_param dos_synflood_thresh "$old" || ret=1
	return $ret
}

# The ports of a storm class cannot be set while its rate is 0, and the rate
# takes a meter of the switch that 0 gives back.
test_a_storm_class_needs_its_rate_first()
{
	local class ports pps ret=0

	for class in unknown_unicast unknown_multicast dhcp igmp_mld; do
		ports=storm_${class}_ports
		pps=storm_${class}_pps
		if [ "$(param_get $ports)$(param_get $pps)" != 00 ]; then
			ktap_print_msg "$class is in use: left alone"
			continue
		fi
		param_set $ports 1
		check_refused "$ports set to 1 before a rate" "Invalid argument" || ret=1
		param_set $pps 200
		check_ok "$pps set to 200" || ret=1
		param_set $ports 1
		check_ok "$ports set to 1" || ret=1
		check_eq "$class rate/ports" 200/1 "$(param_get $pps)/$(param_get $ports)" || ret=1
		param_set $ports 0
		param_set $pps 0
		check_eq "$class given back" 0/0 "$(param_get $pps)/$(param_get $ports)" || ret=1
	done
	return $ret
}

test_the_lane_is_read_only_or_a_name()
{
	local lane ret=0

	lane=$(param_get sfp_lane)
	case $lane in
	auto | 2500basex | 1000basex-an | 1000basex) ;;
	*) mismatch "sfp_lane" "the name of a lane" "$lane"; ret=1 ;;
	esac
	case $(param_get sfp_lane_now) in
	searching | 2500basex | 1000basex-an | 1000basex) ;;
	*) mismatch "sfp_lane_now" "a lane or searching" "$(param_get sfp_lane_now)"; ret=1 ;;
	esac

	# What the lane runs in cannot be set, and a name that is none is no lane.
	param_set sfp_lane_now searching
	check_refused "sfp_lane_now set" "not supported" || ret=1
	param_set sfp_lane bogus
	check_refused "sfp_lane set to a name that is none" "Invalid argument" || ret=1
	check_param sfp_lane "$lane" || ret=1

	# The ONU's password is never given, only whether one is set.
	case $(param_get gpon_password) in
	set | unset | "") ;;
	*) mismatch "gpon_password" "set or not" "$(param_get gpon_password)"; ret=1 ;;
	esac

	# Whether the OLT disabled the ONU is read, never set.
	case $(param_get gpon_disabled) in
	true | false) ;;
	*) mismatch "gpon_disabled" "true or false" "$(param_get gpon_disabled)"; ret=1 ;;
	esac
	return $ret
}

test_driverinit_is_refused()
{
	local old

	old=$(param_get dos_ports)
	out=$(devlink dev param set "$DEVLINK_DEV" name dos_ports value 1 cmode driverinit 2>&1)
	rc=$?
	check_refused "cmode driverinit" "not supported" || return 1
	check_param dos_ports "$old"
}

test_the_info_names_the_part()
{
	local info ret=0

	info=$(devlink dev info "$DEVLINK_DEV" 2>&1)
	check_has "info" "driver rtl9602c" "$info" || ret=1
	check_has "info" "asic.id RTL9602C" "$info" || ret=1
	check_has "info" "gpon.mac 0x" "$info" || ret=1
	return $ret
}

require_device
save_params
trap restore_params EXIT
run_tests
