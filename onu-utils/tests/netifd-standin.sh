#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# The stand-in of test_onu_apply.sh for onu-net, netifd and netifd's protocol
# "pppoed": it keeps a part as onu-net does, in /run/network.d, and for a
# session's part, sesN, does what netifd and pppoed do while the part is
# there: it runs pppoed's stand-in with the options netifd's pppoed gives it,
# writes what the stand-in's "up" lines say to /run/session/N, as pppoed
# does, runs "onu-apply iface" as netifd does when the interface comes up and
# goes down, and starts pppoed again when the session ends. APPLY names
# onu-apply.
#
# The interface runs while its file NAME, sesN.XXXXXX in STANDIN, is there,
# which the part's removal or a new part takes away: its loop then ends, and
# tells onu-apply nothing, which took the session down itself. pppoed's
# stand-in holds its session while that file (TOKEN) is there, and ends it
# once when the test puts the file dropN (DROP) in STANDIN, as a
# concentrator may end a session; while the file held is in STANDIN, no
# session comes up, as before the concentrator answers. The file
# pending.NAME (PENDING) is there from the part, and from the end of a
# session, until the next session's coming up is told. Each loop is a line
# "PID NAME" of STANDIN/loops, and leaves the file ended.NAME when it ends,
# for the test to wait for.

echo "onu-net $*" >>"$LOG"
parts=$ONU_ROOT/run/network.d
case ${1:-} in
set)
	mkdir -p "$parts"
	cat >"$parts/$2"
	;;
del)
	rm -f "$parts/$2"
	;;
*)
	exit 0
	;;
esac
case $2 in
ses[1-7]) n=${2#ses} ;;
*) exit 0 ;;
esac
rm -f "$STANDIN/$2".*
[ "$1" = set ] || exit 0

opt() {
	sed -n "s/^[[:space:]]*option $1 '\(.*\)'\$/\1/p" "$parts/$2"
}
dev=$(opt device "$2")
fam=$(opt family "$2")
flags=
[ "$(opt defaultroute "$2")" = 1 ] && flags="$flags -D"
case $fam in
ipv4) flags="$flags -4" ;;
ipv6) flags="$flags -6" ;;
esac
[ "$(opt pass "$2")" = 1 ] && flags="$flags -P"
d=$ONU_ROOT/run/session/$n
mkdir -p "$STANDIN"
tok=$(mktemp "$STANDIN/$2.XXXXXX")
name=${tok##*/}
: >"$STANDIN/pending.$name"

(
	while [ -e "$tok" ]; do
		while [ -e "$STANDIN/held" ] && [ -e "$tok" ]; do
			sleep 0.05
		done
		# $flags is a list of options, split on purpose.
		TOKEN=$tok DROP=$STANDIN/drop$n PENDING=$STANDIN/pending.$name \
			pppoed up -i "$dev" -f "$ONU_ROOT/run/onu-pppoe$n"$flags -M 2>/dev/null |
			while read -r word ppp _ addr _ peer _ _ _ dns1 dns2; do
				case $word in
				up | up6) ;;
				*) continue ;;
				esac
				if [ -e "$tok" ] && [ -d "$d" ]; then
					echo "$ppp" >"$d/ppp"
					if [ "$word" = up ]; then
						echo "$addr" >"$d/ipv4"
						echo "$peer" >"$d/peer"
						dns=$(echo "${dns1:-} ${dns2:-}" | sed 's/ *- *//g; s/^ *//; s/ *$//')
						[ -z "$dns" ] || echo "$dns" >"$d/dns"
					else
						echo "$addr" >"$d/ipv6"
					fi
					ACTION=ifup INTERFACE=ses$n DEVICE=$ppp "$APPLY" iface 2>/dev/null
				fi
				rm -f "$STANDIN/pending.$name"
			done
		# The session ended while the interface runs: what it brought goes,
		# netifd tells of the end, and pppoed starts again.
		[ -e "$tok" ] || break
		: >"$STANDIN/pending.$name"
		[ -d "$d" ] && rm -f "$d/ppp" "$d/ipv4" "$d/ipv6" "$d/dns" "$d/peer"
		ACTION=ifdown INTERFACE=ses$n "$APPLY" iface 2>/dev/null
	done
	rm -f "$STANDIN/pending.$name"
	: >"$STANDIN/ended.$name"
) >/dev/null &
echo "$! $name" >>"$STANDIN/loops"
exit 0
