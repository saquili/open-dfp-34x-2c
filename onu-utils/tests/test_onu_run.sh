#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-run, with stand-ins for dropbear, dropbearkey, pond,
# getty, login, omcid, onu-drop and sleep, which log what they are asked to do.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
RUNNER=$HERE/scripts/onu-run
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
	export ONU_ROOT=$T/root LOG=$T/log
	mkdir -p "$ONU_ROOT/run" "$T/bin"
	: >"$LOG"
	printf 'VLAN=4094\nADDRESS=198.51.100.2/30\nIFACE=mgmt\nNAME=mgmt\nPREF=1\nDATA_ADDRESS=192.0.2.2/30\n' \
		>"$ONU_ROOT/run/onu-mgmt"
	# The OMCI port of the PON driver, and chown, which the group of it needs.
	mkdir -p "$ONU_ROOT/dev"
	: >"$ONU_ROOT/dev/pon0omci0"
	for c in dropbear pond getty login omcid sleep timed reboot chown; do
		printf '#!/bin/sh\necho "%s $*" >>"$LOG"\n' "$c" >"$T/bin/$c"
	done
	printf '#!/bin/sh\necho "dropbearkey $*" >>"$LOG"\nwhile [ $# -gt 0 ]; do [ "$1" = -f ] && echo key >"$2"; shift; done\n' \
		>"$T/bin/dropbearkey"
	# onu-drop logs the account and the powers it is told of, then runs the
	# program as it is, in place of the real one, which needs root.
	mkdir -p "$ONU_ROOT/usr/lib/onu"
	printf '#!/bin/sh\necho "onu-drop $*" >>"$LOG"\n[ "$1" = -c ] && shift 2\nshift\nexec "$@"\n' \
		>"$ONU_ROOT/usr/lib/onu/onu-drop"
	chmod +x "$T/bin"/* "$ONU_ROOT/usr/lib/onu/onu-drop"
	PATH=$T/bin:$PATH
}

teardown() {
	rm -rf "$T"
}

run() {
	"$RUNNER" "$@" 2>/dev/null
	rc=$?
}

logged() {
	grep -q -x -F -- "$1" "$LOG"
}

echo "the ssh server, in the foreground, on both addresses, with a host key made first"
setup
run ssh
check '[ $rc -eq 0 ]'
check '[ "$(head -n 1 "$LOG")" = "dropbearkey -t ed25519 -f $ONU_ROOT/run/dropbear/host_key" ]'
check 'logged "dropbear -F -r $ONU_ROOT/run/dropbear/host_key -P $ONU_ROOT/run/dropbear.pid -p 198.51.100.2:22 -p 192.0.2.2:22"'
# Root's password as well as keys (ADR 0012).
check '! grep -q "^dropbear .*-[sB]\\b" "$LOG"'
: >"$LOG"
run ssh
check '! grep -q "^dropbearkey" "$LOG"'
# Root's password not known, without a store: keys only.
echo '!' >"$ONU_ROOT/run/onu-auth"
: >"$LOG"
run ssh
check 'logged "dropbear -F -s -r $ONU_ROOT/run/dropbear/host_key -P $ONU_ROOT/run/dropbear.pid -p 198.51.100.2:22 -p 192.0.2.2:22"'
teardown

echo "the status daemon on the same addresses, without a key over HTTP"
setup
run pond
check '[ $rc -eq 0 ]'
check 'logged "pond -u pond -l http://198.51.100.2:80 -l http://192.0.2.2:80 -t $ONU_ROOT/run/pond/token"'
teardown

echo "with the key, over HTTPS on 443, and on 80 to send browsers there"
setup
mkdir -p "$ONU_ROOT/run/pond"
echo 0123 >"$ONU_ROOT/run/pond/tls.key"
run pond
check 'logged "pond -u pond -l https://198.51.100.2:443 -l http://198.51.100.2:80 -l https://192.0.2.2:443 -l http://192.0.2.2:80 -k $ONU_ROOT/run/pond/tls.key -t $ONU_ROOT/run/pond/token"'
check '! grep -q "no key for HTTPS" "$ONU_ROOT/run/pond/log"'
teardown

echo "the status daemon's token and log, in its directory of /run: the token's file is named before there is one"
setup
mkdir -p "$ONU_ROOT/run/pond"
run pond
check '[ ! -e "$ONU_ROOT/run/pond/token" ]'
check 'logged "pond -u pond -l http://198.51.100.2:80 -l http://192.0.2.2:80 -t $ONU_ROOT/run/pond/token"'
check '[ -e "$ONU_ROOT/run/pond/log" ] && grep -q "no key for HTTPS" "$ONU_ROOT/run/pond/log"'
teardown

echo "without an address on the host link, the management address alone"
setup
sed -i.bak 's/^DATA_ADDRESS=.*/DATA_ADDRESS=/' "$ONU_ROOT/run/onu-mgmt"
run ssh
check 'logged "dropbear -F -r $ONU_ROOT/run/dropbear/host_key -P $ONU_ROOT/run/dropbear.pid -p 198.51.100.2:22"'
run pond
check 'logged "pond -u pond -l http://198.51.100.2:80 -t $ONU_ROOT/run/pond/token"'
teardown

echo "the pair in use after a change"
setup
printf 'VLAN=100\nADDRESS=192.0.2.6/30\nIFACE=mgmt100\nNAME=mgmt7\nPREF=2\nDATA_ADDRESS=\n' \
	>"$ONU_ROOT/run/onu-mgmt"
run ssh
check 'grep -q "^dropbear .* -p 192.0.2.6:22$" "$LOG"'
teardown

echo "without a management address, a wait and a failure, and nothing started"
setup
rm "$ONU_ROOT/run/onu-mgmt"
run ssh
check '[ $rc -eq 1 ] && logged "sleep 5"'
check '! grep -q -e "^dropbear" -e "^pond" "$LOG"'
teardown

echo "the console asks for root's password, without an address too"
setup
rm "$ONU_ROOT/run/onu-mgmt"
run console
check '[ $rc -eq 0 ] && logged "getty -L console 0 vt100"'
check '! grep -q -e "^login" -e "^sleep" "$LOG"'
teardown

echo "a bench may have root's shell on the console without the password"
setup
mkdir -p "$ONU_ROOT/etc"
printf 'DATA_ADDRESS=192.0.2.2/30\nCONSOLE_AUTOLOGIN=root\n' >"$ONU_ROOT/etc/board.conf"
run console
check 'logged "login -f root" && ! grep -q "^getty" "$LOG"'
: >"$LOG"
for other in "CONSOLE_AUTOLOGIN=admin" "CONSOLE_AUTOLOGIN=" "#CONSOLE_AUTOLOGIN=root" \
	"CONSOLE_AUTOLOGIN=root; reboot"; do
	printf '%s\n' "$other" >"$ONU_ROOT/etc/board.conf"
	run console
done
check '! grep -q "^login" "$LOG" && [ "$(grep -c "^getty" "$LOG")" -eq 4 ]'
teardown

echo "omcid in the mode onu-apply gave it, without an address too"
setup
rm "$ONU_ROOT/run/onu-mgmt"
run omcid
check '[ $rc -eq 0 ] && logged "omcid -m $ONU_ROOT/run/omcid/mib -x $ONU_ROOT/usr/lib/onu/onu-call"'
# It runs as the account omcid with the two powers it uses, in a directory of
# its own, and the OMCI port is the group's.
check 'logged "onu-drop -c net_admin,net_raw omcid omcid -m $ONU_ROOT/run/omcid/mib -x $ONU_ROOT/usr/lib/onu/onu-call"'
check 'logged "chown :omcid $ONU_ROOT/dev/pon0omci0"'
echo router >"$ONU_ROOT/run/omcid.mode"
run omcid
check 'logged "omcid -r -m $ONU_ROOT/run/omcid/mib -x $ONU_ROOT/usr/lib/onu/onu-call"'
echo bridge >"$ONU_ROOT/run/omcid.mode"
run omcid
check '[ "$(grep -c -x -F "omcid -m $ONU_ROOT/run/omcid/mib -x $ONU_ROOT/usr/lib/onu/onu-call" "$LOG")" -eq 2 ] && ! grep -q "^sleep" "$LOG"'
teardown

echo "the time by NTP from the router at the other end of the management link, by PTP on its interface"
setup
# Where timed reads the OLT's date and tells its state.
FILES="-o $ONU_ROOT/run/omcid/clock -s $ONU_ROOT/run/timed/state"
run time
check '[ $rc -eq 0 ] && logged "timed -n 198.51.100.1 -p mgmt $FILES"'
check '[ -s "$ONU_ROOT/run/timed.pid" ]'
# It runs as the account ntp with the powers to set the clock, to listen for
# PTP over layer 2 and on its UDP ports below 1024, and no other.
check 'logged "onu-drop -c sys_time,net_raw,net_bind_service ntp timed -n 198.51.100.1 -p mgmt $FILES"'
: >"$LOG"
for pair in 198.51.100.1/30:198.51.100.2 198.51.100.6/30:198.51.100.5 \
	198.51.100.4/31:198.51.100.5 198.51.100.5/31:198.51.100.4 198.051.100.002/30:198.51.100.1; do
	sed -i.bak "s#^ADDRESS=.*#ADDRESS=${pair%%:*}#" "$ONU_ROOT/run/onu-mgmt"
	run time
	check 'logged "timed -n ${pair#*:} -p mgmt $FILES"'
done
teardown

echo "no router that is known: PTP alone, on the management link's interface"
setup
FILES="-o $ONU_ROOT/run/omcid/clock -s $ONU_ROOT/run/timed/state"
for address in 198.51.100.2/24 198.51.100.0/30 198.51.100.3/30 198.51.100.300/30 198.51.100/30; do
	sed -i.bak "s#^ADDRESS=.*#ADDRESS=$address#" "$ONU_ROOT/run/onu-mgmt"
	run time
	check '[ $rc -eq 0 ]'
done
check '[ "$(grep -c -x -F "timed -p mgmt $FILES" "$LOG")" -eq 5 ] && ! grep -q "^sleep" "$LOG"'
teardown

echo "no router and no management link: a wait of an hour, then a failure"
setup
rm "$ONU_ROOT/run/onu-mgmt"
run time
check '[ $rc -eq 1 ] && logged "sleep 3600" && ! grep -q "^timed" "$LOG"'
check '[ ! -e "$ONU_ROOT/run/timed.pid" ]'
teardown

echo "the time server the settings name, read as data"
setup
FILES="-o $ONU_ROOT/run/omcid/clock -s $ONU_ROOT/run/timed/state"
sed -i.bak "s#^ADDRESS=.*#ADDRESS=198.51.100.2/24#" "$ONU_ROOT/run/onu-mgmt"
printf 'HOSTNAME=stick\nNTP_SERVER=192.0.2.123\n' >"$ONU_ROOT/run/onu-system"
run time
check 'logged "timed -n 192.0.2.123 -p mgmt $FILES"'
rm "$ONU_ROOT/run/onu-mgmt"
printf 'NTP_SERVER=$(reboot)\n' >"$ONU_ROOT/run/onu-system"
run time
check 'logged "timed -n \$(reboot) $FILES" && ! grep -q "^reboot" "$LOG"'
teardown

echo "what is not a server"
setup
run sshd
check '[ $rc -eq 2 ]'
run
check '[ $rc -eq 2 ]'
teardown

echo "test_onu_run: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
