#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-lane, with stand-ins for devlink, which keeps each
# parameter in a file, onu-config and logger.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
LANE=$HERE/scripts/onu-lane
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

setup() {
	T=$(mktemp -d)
	export ONU_ROOT=$T/root TRIAL_LIMIT=60 LOG=$T/log
	mkdir -p "$ONU_ROOT/run" "$ONU_ROOT/proc" "$T/bin" "$T/param"
	: >"$LOG"
	echo "1000.50 900.00" >"$ONU_ROOT/proc/uptime"
	echo auto >"$T/param/sfp_lane"
	echo 2500basex >"$T/param/sfp_lane_now"
	# devlink: "dev param show DEV name N" and "dev param set DEV name N value V cmode runtime".
	cat >"$T/bin/devlink" <<EOF
#!/bin/sh
case \$3 in
show) printf 'x:\n  name %s type driver-specific\n    values:\n      cmode runtime value %s\n' "\$6" "\$(cat "$T/param/\$6")" ;;
set) [ -e "$T/devlink.fail" ] && exit 1; echo "\$8" >"$T/param/\$6"; echo "set \$6 \$8" >>"$LOG" ;;
esac
EOF
	printf '#!/bin/sh\n[ "$1" = system ] && cat >>"%s/stored"\n' "$T" >"$T/bin/onu-config"
	printf '#!/bin/sh\nexit 0\n' >"$T/bin/logger"
	chmod +x "$T/bin"/*
	PATH=$T/bin:$PATH
}

teardown() {
	rm -rf "$T"
}

run() {
	out=$("$LANE" "$@" 2>/dev/null)
	rc=$?
}

lane() {
	cat "$T/param/sfp_lane"
}

echo "the setting the lane has, and the one it runs in"
setup
run status
check '[ $rc -eq 0 ]'
check 'echo "$out" | grep -qx CHOICE=auto'
check 'echo "$out" | grep -qx NOW=2500basex'
check '! echo "$out" | grep -q TRIAL_'
teardown

echo "a setting on trial, then kept"
setup
run try 1000basex-an
check '[ $rc -eq 0 ]'
check '[ "$(lane)" = 1000basex-an ]'
run status
check 'echo "$out" | grep -qx TRIAL_MODE=1000basex-an'
check 'echo "$out" | grep -qx TRIAL_PREVIOUS=auto'
check 'echo "$out" | grep -qx TRIAL_UNTIL=1060'
run try 1000basex
check '[ $rc -ne 0 ] && [ "$(lane)" = 1000basex-an ]'
run keep
check '[ $rc -eq 0 ] && [ ! -e "$ONU_ROOT/run/onu-trial.d/lane" ]'
check 'grep -qx SFP_LANE=1000basex-an "$T/stored"'
check '[ "$(lane)" = 1000basex-an ]'
teardown

echo "auto kept takes the setting out of the store"
setup
echo 1000basex >"$T/param/sfp_lane"
run try auto
run keep
check 'grep -qx SFP_LANE= "$T/stored"'
teardown

echo "a trial that is not kept ends by itself, back to the setting of before"
setup
export TRIAL_LIMIT=1
run try 2500basex
check '[ "$(lane)" = 2500basex ]'
sleep 3
check '[ "$(lane)" = auto ]'
check '[ ! -e "$ONU_ROOT/run/onu-trial.d/lane" ]'
check '[ ! -e "$T/stored" ]'
teardown

echo "a trial ended at once, and a watcher of an earlier trial ends no later one"
setup
run try 1000basex
run revert
check '[ "$(lane)" = auto ] && [ ! -e "$ONU_ROOT/run/onu-trial.d/lane" ]'
run try 1000basex-an
run expire 1
check '[ "$(lane)" = 1000basex-an ] && [ -e "$ONU_ROOT/run/onu-trial.d/lane" ]'
teardown

echo "a setting that is none, and a driver that refuses, change nothing"
setup
run try 10gbase-r
check '[ $rc -eq 2 ] && [ "$(lane)" = auto ] && [ ! -e "$ONU_ROOT/run/onu-trial.d/lane" ]'
: >"$T/devlink.fail"
run try 1000basex
check '[ $rc -ne 0 ] && [ ! -e "$ONU_ROOT/run/onu-trial.d/lane" ]'
teardown

echo "at boot, the store's setting"
setup
run apply
check '[ $rc -eq 0 ] && [ "$(lane)" = auto ] && [ ! -s "$LOG" ]'
echo SFP_LANE=1000basex >"$ONU_ROOT/run/onu-system"
run apply
check '[ $rc -eq 0 ] && [ "$(lane)" = 1000basex ]'
echo SFP_LANE=wrong >"$ONU_ROOT/run/onu-system"
run apply
check '[ $rc -ne 0 ] && [ "$(lane)" = 1000basex ]'
teardown

echo "test_onu_lane: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
