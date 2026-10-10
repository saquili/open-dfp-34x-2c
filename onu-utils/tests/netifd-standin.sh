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
# goes down, and starts pppoed again when it ends. APPLY names onu-apply.

echo "onu-net $*" >>"$LOG"
parts=$ONU_ROOT/run/network.d
case ${1:-} in
set)
	mkdir -p "$parts"
	cat >"$parts/$2"
	;;
del)
	rm -f "$parts/$2"
	exit 0
	;;
*)
	exit 0
	;;
esac
case $2 in
ses[1-7]) n=${2#ses} ;;
*) exit 0 ;;
esac

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

(
	while [ -e "$parts/ses$n" ]; do
		# $flags is a list of options, split on purpose.
		pppoed up -i "$dev" -f "$ONU_ROOT/run/onu-pppoe$n"$flags -M 2>/dev/null |
			while read -r word ppp _ addr _ peer _ _ _ dns1 dns2; do
				[ -d "$d" ] || continue
				case $word in
				up)
					echo "$ppp" >"$d/ppp"
					echo "$addr" >"$d/ipv4"
					echo "$peer" >"$d/peer"
					dns=$(echo "${dns1:-} ${dns2:-}" | sed 's/ *- *//g; s/^ *//; s/ *$//')
					[ -z "$dns" ] || echo "$dns" >"$d/dns"
					;;
				up6)
					echo "$ppp" >"$d/ppp"
					echo "$addr" >"$d/ipv6"
					;;
				*)
					continue
					;;
				esac
				ACTION=ifup INTERFACE=ses$n DEVICE=$ppp "$APPLY" iface 2>/dev/null
			done
		[ -d "$d" ] && rm -f "$d/ppp" "$d/ipv4" "$d/ipv6" "$d/dns" "$d/peer"
		[ -e "$parts/ses$n" ] && ACTION=ifdown INTERFACE=ses$n "$APPLY" iface 2>/dev/null
	done
) >/dev/null &
exit 0
