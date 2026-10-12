#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# The devlink region frame_capture of the RTL9602C switch driver: a snapshot of
# the header of one frame that the switch latched, which the parameter
# pktdbg_mode arms. With mode 1, which never latches, a snapshot is refused;
# with mode 0, any frame, the first frame the switch sees is enough.
#
# The mode is put back as it was, and the snapshot taken is deleted.

ALL_TESTS="
	test_the_region_is_there
	test_nothing_latched_is_refused
	test_a_latched_frame_is_a_snapshot
	test_a_snapshot_that_is_not_there_is_refused
"

. "$(dirname "$0")/lib.sh"

REGION=$DEVLINK_DEV/frame_capture
SNAPSHOT=7
MODE=

# Remember the capture mode, once, and put it back when the test ends.
arm() {
	[ -n "$MODE" ] || MODE=$(param_get pktdbg_mode)
	param_set pktdbg_mode "$1"
}

cleanup()
{
	devlink region del "$REGION" snapshot $SNAPSHOT >/dev/null 2>&1
	[ -z "$MODE" ] || param_set pktdbg_mode "$MODE"
}

test_the_region_is_there()
{
	local shown

	shown=$(devlink region show 2>&1 | grep "^$REGION:")
	check_has "the region" "size [1-9][0-9]*" "$shown"
}

test_nothing_latched_is_refused()
{
	arm 1
	out=$(devlink region new "$REGION" snapshot $SNAPSHOT 2>&1)
	rc=$?
	check_refused "a snapshot with the capture armed to never" "nothing is latched"
}

# Any frame will do; the board has frames of its own, the keepalives of its links.
test_a_latched_frame_is_a_snapshot()
{
	local tries=0 dump read shown

	arm 0
	until devlink region new "$REGION" snapshot $SNAPSHOT >/dev/null 2>&1; do
		tries=$((tries + 1))
		[ $tries -lt 20 ] || { mismatch "a snapshot" "one within 10 s" "none"; return 1; }
		sleep 0.5
	done
	shown=$(devlink region show | grep "^$REGION:")
	check_has "the region" "snapshot \\[$SNAPSHOT\\]" "$shown" || return 1
	dump=$(devlink region dump "$REGION" snapshot $SNAPSHOT 2>&1)
	check_has "the dump" "^[0-9a-f]\{16\} [0-9a-f][0-9a-f] " "$dump" || return 1
	read=$(devlink region read "$REGION" snapshot $SNAPSHOT address 0 length 8 2>&1)
	check_eq "eight bytes and their address" 9 "$(echo "$read" | head -n 1 | wc -w)" || return 1
	devlink region del "$REGION" snapshot $SNAPSHOT || return 1
	shown=$(devlink region show | grep "^$REGION:")
	check_has "the region after the delete" "snapshot \\[\\]" "$shown"
}

test_a_snapshot_that_is_not_there_is_refused()
{
	local ret=0

	out=$(devlink region read "$REGION" snapshot 9 address 0 length 4 2>&1)
	rc=$?
	check_refused "a read of a snapshot that is none" "does not exist" || ret=1
	out=$(devlink region del "$REGION" snapshot 9 2>&1)
	rc=$?
	check_refused "a delete of a snapshot that is none" "Invalid argument" || ret=1
	return $ret
}

require_device
trap cleanup EXIT
run_tests
