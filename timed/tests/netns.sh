#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# timed end to end, in two network namespaces joined by a veth pair: a host
# that serves the time (tools/lab/ntpserve and tools/lab/ptpmaster, each with
# a clock a little ahead of the host's) and the stick (timed). timed runs dry
# (-D): it measures and chooses, and sets no clock, as the namespaces share
# the one clock of the machine. The state file says what it chose:
#
#   the OLT's time alone        olt, set, 4 to 5 s behind (it gives whole seconds)
#   NTP and the OLT             ntp, 30 ms behind
#   PTP over layer 2 joins      ptp, 50 ms behind, the path delay measured
#   the PTP master goes         ntp again
#   PTP over UDP                ptp again
#
# It needs root and network namespaces, a privileged container for one, which
# "make check" is not:
#
#   docker run --rm --privileged -v "$PWD":/r debian:trixie \
#     sh -c 'apt-get -qq update && apt-get -qq install -y make gcc iproute2 \
#            python3 >/dev/null && cd /r/timed && make && tests/netns.sh'

set -u
TIMED=${TIMED:-build/timed}
LAB=${LAB:-../tools/lab}
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
	ip netns del ho 2>/dev/null
	ip netns del st 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT

# A key of the state file.
state() {
	sed -n "s/^$1=//p" "$T/state" 2>/dev/null
}

# A number within a range: $1 the key, $2 and $3 the bounds.
within() {
	v=$(state "$1")
	[ -n "$v" ] && [ "$v" -ge "$2" ] && [ "$v" -le "$3" ]
}

# Wait up to $2 seconds for the state's source to be $1.
wait_source() {
	n=0
	while [ "$(state source)" != "$1" ] && [ $n -lt $(($2 * 5)) ]; do
		sleep 0.2
		n=$((n + 1))
	done
}

ip netns add ho
ip netns add st
ip link add ho0 netns ho type veth peer name st0 netns st
ip -n ho address add 192.0.2.1/30 dev ho0
ip -n st address add 192.0.2.2/30 dev st0
for pair in ho:ho0 st:st0; do
	ip -n "${pair%%:*}" link set lo up
	ip -n "${pair%%:*}" link set "${pair#*:}" up
done
sleep 1

# The OLT's time, 5 s ahead of the clock, as omcid writes it.
python3 -c 'import time; print(f"utc={time.time_ns() // 10**9 + 5}\nmonotonic_ns={time.monotonic_ns()}")' \
	>"$T/clock"

echo "the OLT's time alone"
ip netns exec st "$TIMED" -D -f -o "$T/clock" -s "$T/state" >"$T/olt.log" 2>&1 &
echo $! >"$T/olt.pid"
wait_source olt 3
check '[ "$(state source)" = olt ] && [ "$(state state)" = set ]'
check 'within olt_offset_ns -6000000000 -4000000000'
check '[ "$(state steps)" = 1 ] && grep -q "clock stepped by +[45]\.[0-9]* s from olt" "$T/olt.log"'
kill "$(cat "$T/olt.pid")"
rm "$T/olt.pid"
sleep 1

echo "NTP and the OLT"
ip netns exec ho python3 "$LAB/ntpserve" --address 192.0.2.1 --skew-ms 30 --seconds 120 \
	>"$T/ntp.log" 2>&1 &
echo $! >"$T/ntp.pid"
sleep 1
ip netns exec st "$TIMED" -D -f -v -n 192.0.2.1 -p st0 -o "$T/clock" -s "$T/state" \
	>"$T/timed.log" 2>&1 &
echo $! >"$T/timed.pid"
wait_source ntp 5
check '[ "$(state source)" = ntp ] && [ "$(state state)" = syncing ]'
check 'within ntp_offset_ns -40000000 -20000000'
check '[ "$(state ntp_stratum)" = 2 ] && [ "$(state ntp_good)" = 1 ] && [ "$(state olt_good)" = 1 ]'
check '[ "$(state ptp_interface)" = st0 ] && [ "$(state ptp_good)" = 0 ]'

echo "PTP over layer 2 joins"
ip netns exec ho python3 "$LAB/ptpmaster" --transport l2 --skew-ms 50 --seconds 10 ho0 \
	>"$T/ptp.log" 2>&1 &
echo $! >"$T/ptp.pid"
wait_source ptp 6
sleep 3
check '[ "$(state source)" = ptp ] && [ "$(state ptp_via)" = l2 ]'
check 'within ptp_offset_ns -60000000 -40000000'
check 'within ptp_delay_ns 1 5000000'
check '[ -n "$(state ptp_master)" ] && grep -q "time source: ptp (was ntp)" "$T/timed.log"'

echo "the PTP master goes"
wait "$(cat "$T/ptp.pid")" 2>/dev/null
rm "$T/ptp.pid"
wait_source ntp 15
n=0
while ! grep -q "PTP master lost" "$T/timed.log" && [ $n -lt 50 ]; do
	sleep 0.2
	n=$((n + 1))
done
check '[ "$(state source)" = ntp ] && grep -q "PTP master lost" "$T/timed.log"'

echo "PTP over UDP"
ip netns exec ho python3 "$LAB/ptpmaster" --transport udp --skew-ms 50 --seconds 10 ho0 \
	>"$T/ptp.log" 2>&1 &
echo $! >"$T/ptp.pid"
wait_source ptp 8
check '[ "$(state source)" = ptp ] && [ "$(state ptp_via)" = udp ]'
check 'within ptp_offset_ns -60000000 -40000000'

if [ $failures -ne 0 ]; then
	echo "--- timed"
	cat "$T/timed.log"
	echo "--- state"
	cat "$T/state"
fi
echo "$checks checks, $failures failures"
[ $failures -eq 0 ]
