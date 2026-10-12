#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Helpers of the tests of the RTL9602C switch driver. The tests are written for
# a POSIX shell and BusyBox, which is all that the board has: no bash, no jq.
# They need root, the devlink and tc of iproute2, and a board whose device tree
# has the switch (compatible "realtek,rtl9602c-switch"); on any other machine
# they report that they are skipped.
#
# DEVLINK_DEV names the devlink instance, and UNI the interface of the SFP
# port, if they are not the board's.

. "$(dirname "$0")/../../../kselftest/ktap_helpers.sh"

DEVLINK_DEV=${DEVLINK_DEV:-platform/1b000000.switch}
UNI=${UNI:-sfp}

# Tell that nothing was tried and why, and leave.
skip_all() {
	ktap_print_header
	ktap_skip_all "$*"
	exit "$KSFT_SKIP"
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || skip_all "$1 is not installed"
}

require_device() {
	[ "$(id -u)" = 0 ] || skip_all "the tests need root"
	require_command devlink
	devlink dev show 2>/dev/null | grep -qx "$DEVLINK_DEV" ||
		skip_all "no $DEVLINK_DEV: this is not a board with the RTL9602C switch"
}

# The value of the parameter $1 in the mode runtime.
param_get() {
	devlink dev param show "$DEVLINK_DEV" name "$1" 2>/dev/null |
		sed -n 's/.*cmode runtime value //p'
}

# Set the parameter $1 to $2. What devlink said is in $out, its status in $rc.
param_set() {
	out=$(devlink dev param set "$DEVLINK_DEV" name "$1" value "$2" cmode runtime 2>&1)
	rc=$?
}

# Say what was expected and what came, for a check that failed.
mismatch() {
	ktap_print_msg "$1: expected \"$2\", got \"$3\""
}

# Fail, with the mismatch, unless $2 is $3.
check_eq() {
	[ "$2" = "$3" ] && return 0
	mismatch "$1" "$2" "$3"
	return 1
}

# Fail, with the mismatch, unless $3 holds the pattern $2.
check_has() {
	echo "$3" | grep -q "$2" && return 0
	mismatch "$1" "$2" "$3"
	return 1
}

# Fail, with what was said ($out), unless the last set or add worked ($rc is 0).
check_ok() {
	[ "${rc:-1}" -eq 0 ] && return 0
	mismatch "$1" "a success" "$rc ($out)"
	return 1
}

# Fail unless the parameter $1 has the value $2.
check_param() {
	check_eq "$1" "$2" "$(param_get "$1")"
}

# Fail unless the last set was refused (its status is $rc) with $2 in what
# devlink or tc said ($out).
check_refused() {
	[ "${rc:-0}" -ne 0 ] && echo "$out" | grep -q "$2" && return 0
	mismatch "$1" "a refusal with \"$2\"" "$rc ($out)"
	return 1
}

# Run the test functions of ALL_TESTS as KTAP, one line each, and leave with
# the status of the run.
run_tests() {
	ktap_print_header
	ktap_set_plan "$(echo $ALL_TESTS | wc -w)"
	for t in $ALL_TESTS; do
		ktap_test_result "$t" "$t"
	done
	ktap_finished
}
