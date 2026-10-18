#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of scripts/onu-apply, against stand-ins for the
# programs they run, in a scratch root: each stand-in writes its name and its
# arguments to a log.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$HERE/build}
MODE_SCRIPT=$HERE/scripts/onu-apply
REAL_SLEEP=$(command -v sleep)
checks=0
failures=0

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

logged() {
	grep -q -- "$1" "$LOG"
}

# The part $1 of netifd's configuration, a section a line: "TYPE NAME:" or
# "TYPE:", then each option as KEY=VALUE.
part() {
	awk '/^config/ { if (l != "") print l; l = $2 ($3 != "" ? " " $3 : "") ":"; next }
		/option/ { l = l " " $2 "=" $3 } END { if (l != "") print l }' \
		"$ONU_ROOT/run/network.d/$1" 2>/dev/null | tr -d "'"
}

# Whether the part $1 has the section $2, as part() writes it.
has() {
	part "$1" | grep -qxF -- "$2"
}

setup() {
	T=$(mktemp -d)
	LOG=$T/log
	export ONU_ROOT=$T/root LOG APPLY=$MODE_SCRIPT
	mkdir -p "$ONU_ROOT/run" "$ONU_ROOT/etc" "$ONU_ROOT/proc/sys/net/ipv4" \
		"$ONU_ROOT/sys/class/net/sfp" "$ONU_ROOT/sys/class/net/pon" \
		"$ONU_ROOT/sys/class/net/gem4095" "$ONU_ROOT/sys/class/net/gem1024" \
		"$ONU_ROOT/sys/class/net/gem256" "$T/bin"
	echo 00:00:5e:00:53:01 >"$ONU_ROOT/sys/class/net/sfp/address"
	echo 0 >"$ONU_ROOT/proc/sys/net/ipv4/ip_forward"
	mkdir -p "$ONU_ROOT/proc/sys/net/ipv6/conf/all"
	echo 0 >"$ONU_ROOT/proc/sys/net/ipv6/conf/all/forwarding"
	# The links to the host the tests make: the host link itself and VLANs.
	for lan in sfp lan410 lan36 lan37; do
		mkdir -p "$ONU_ROOT/proc/sys/net/ipv6/conf/$lan" \
			"$ONU_ROOT/proc/sys/net/ipv4/conf/$lan" \
			"$ONU_ROOT/proc/sys/net/ipv4/neigh/$lan"
		echo 1 >"$ONU_ROOT/proc/sys/net/ipv6/conf/$lan/disable_ipv6"
		echo 0 >"$ONU_ROOT/proc/sys/net/ipv4/conf/$lan/rp_filter"
		echo 80 >"$ONU_ROOT/proc/sys/net/ipv4/neigh/$lan/proxy_delay"
	done
	: >"$LOG"
	for prog in killall udhcpd logger kill onu-config dhcp6d chown; do
		printf '#!/bin/sh\necho "%s $*" >>"$LOG"\n' "$prog" >"$T/bin/$prog"
	done
	# A running omcid, signalled, programs the data path again and says so.
	printf '#!/bin/sh\necho "killall $*" >>"$LOG"\ncase "$1" in\n-USR1) echo router ;;\n-USR2) echo bridge ;;\n*) exit 0 ;;\nesac >"%s/run/omcid/applied"\n' \
		"$ONU_ROOT" >"$T/bin/killall"
	# The defaults of the settings are the real table's, and no call to log.
	printf '#!/bin/sh\n[ "$1" = defaults ] && exec "%s/onu-config" defaults\necho "onu-config $*" >>"$LOG"\n' \
		"$BUILD" >"$T/bin/onu-config"
	# onu-svc stands in for procd: "sync onu DIR" runs, at once, the program
	# after "--" of each file of DIR that is new or changed, as procd starts
	# it ("procd start NAME LINE" in the log), and forgets the ones whose file
	# went ("procd stop NAME"), omcid's stand-in ending with its own. onu-run
	# is the real one.
	cat >"$T/bin/onu-svc" <<-EOF
	#!/bin/sh
	echo "onu-svc \$*" >>"\$LOG"
	[ "\$1 \$2" = "sync onu" ] || exit 0
	dir=\$3
	for f in "\$dir"/*; do
	 	[ -f "\$f" ] || continue
	 	n=\${f##*/}
	 	cmp -s "\$f" "$T/svc-\$n" && continue
	 	cp "\$f" "$T/svc-\$n"
	 	echo "procd start \$n \$(cat "\$f")" >>"\$LOG"
	 	set -- \$(cat "\$f")
	 	while [ \$# -gt 0 ] && [ "\$1" != -- ]; do shift; done
	 	shift
	 	"\$@"
	done
	for m in "$T"/svc-*; do
	 	[ -e "\$m" ] || continue
	 	n=\${m##*/svc-}
	 	[ -e "\$dir/\$n" ] && continue
	 	rm -f "\$m"
	 	[ "\$n" = omcid ] && rm -f "$T/omcid.running"
	 	echo "procd stop \$n" >>"\$LOG"
	done
	exit 0
	EOF
	ln -s "$HERE/scripts/onu-run" "$T/bin/onu-run"
	# onu-net stands in for netifd and its protocol "pppoed" as well.
	ln -s "$HERE/tests/netifd-standin.sh" "$T/bin/onu-net"
	# onu-drop notes the account and the powers it is told of, and runs the
	# program as it is, in place of the real one, which needs root.
	mkdir -p "$ONU_ROOT/usr/lib/onu" "$ONU_ROOT/run/dhcp6d" "$ONU_ROOT/run/omcid"
	printf '#!/bin/sh\necho "onu-drop $*" >>"$LOG"\n[ "$1" = -c ] && shift 2\nshift\nexec "$@"\n' \
		>"$ONU_ROOT/usr/lib/onu/onu-drop"
	chmod +x "$ONU_ROOT/usr/lib/onu/onu-drop"
	# onu-nl lists, on the ingress of sfp and pon, the priority of the boot's
	# filter of the management VLAN, 1, and those of two of omcid's VLAN rules.
	# It makes the interfaces it is told to add, and takes away those it is
	# told to delete, as the kernel would.
	printf '#!/bin/sh\necho "onu-nl $*" >>"$LOG"\n%s\n%s\n[ "$1 $2" = "filter prefs" ] || exit 0\n%s\n' \
		'[ "$1 $2 $3" = "link add vlan" ] && mkdir -p "$ONU_ROOT/sys/class/net/$5"' \
		'[ "$1 $2" = "link del" ] && rm -rf "$ONU_ROOT/sys/class/net/$3"' \
		'printf "1\n1000\n1001\n"' >"$T/bin/onu-nl"
	# onu-apply has no use for ip: a call of it shows in the log.
	printf '#!/bin/sh\necho "ip $*" >>"$LOG"\n' >"$T/bin/ip"
	# omcid runs from its first start on, as on the board.
	printf '#!/bin/sh\necho "omcid $*" >>"$LOG"\n: >"%s/omcid.running"\n' "$T" >"$T/bin/omcid"
	printf '#!/bin/sh\n[ "$1" = omcid ] && [ -e "%s/omcid.running" ]\n' "$T" >"$T/bin/pidof"
	# nft stays a stand-in, so that a call to it would show in the log: the
	# stick has no nft, and onu-apply puts its table in place with onu-fw,
	# whose stand-in keeps the file it is given.
	printf '#!/bin/sh\necho "nft $*" >>"$LOG"\n' >"$T/bin/nft"
	printf '#!/bin/sh\necho "onu-fw $*" >>"$LOG"\n[ "$1" = apply ] && cp "$2" "%s/fw.conf"\nexit 0\n' \
		"$T" >"$T/bin/onu-fw"
	# pppoed opens IPv4, or IPv6 alone when told -6, and keeps it two seconds.
	printf '#!/bin/sh\necho "pppoed $*" >>"$LOG"\ncase " $* " in\n%s\n%s\nesac\n%s 2\n' \
		'*" -6 "*) echo "up6 ppp1 local fe80::2 peer fe80::1 session 18" ;;' \
		'*) echo "up ppp0 local 192.0.2.2 peer 192.0.2.1 session 17 dns 198.51.100.53 -" ;;' \
		"$REAL_SLEEP" >"$T/bin/pppoed"
	# A wait of a fraction of a second, as for onu-apply's lock, is real.
	printf '#!/bin/sh\ncase $1 in 0.*) exec %s "$1" ;; esac\nexit 0\n' "$REAL_SLEEP" >"$T/bin/sleep"
	printf '#!/bin/sh\necho "onu-apply $*" >>"$LOG"\n' >"$T/bin/onu-apply"
	printf '#!/bin/sh\n[ "$1" = gem ] || exit 1\n%s\n' \
		'printf "gem4095 port 4095 down multicast\ngem256 port 256 both tcont 0\ngem1024 port 1024 both tcont 1\n"' \
		>"$T/bin/pon"
	chmod +x "$T/bin"/*
	PATH=$T/bin:$PATH
}

teardown() {
	"$MODE_SCRIPT" stop 2>/dev/null
	rm -rf "$T"
}

conf() {
	printf '%s\n' "$@" >"$ONU_ROOT/etc/onu.conf"
}

echo "bridge mode"
setup
conf MODE=bridge
"$MODE_SCRIPT" start 2>/dev/null
check 'logged "^omcid -m .*/run/omcid/mib -x .*/usr/lib/onu/onu-call$"'
check '! logged "omcid -r"'
check '! logged "^nft"'
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ]'
# A boot applies what the store gave and saves nothing.
check '! logged "^onu-config"'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/ip_forward")" = 0 ]'
teardown

echo "a local network, routed out by a session that keeps its address"
setup
mkdir -p "$ONU_ROOT/sys/class/net/ppp0"
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION1_PASS=off LAN1_CONN=pppoe:1 \
	LAN1_NAME=Dom LAN1_VLAN=100 LAN1_ADDRESS=192.168.1.1/24 LAN1_DHCP=on \
	LAN1_POOL_FROM=192.168.1.100 LAN1_POOL_TO=192.168.1.199 LAN1_LEASE=3600 \
	FIXED1=1/00:00:5e:00:53:a1/192.168.1.10 FIXED1_NAME=Drukarka \
	FIXED2=2/00:00:5e:00:53:a2/10.0.0.10
echo "user secret" >"$ONU_ROOT/etc/onu-pppoe"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	logged "IPv4 up on ppp0" && break
	"$REAL_SLEEP" 0.2
done
check 'has nets "device: type=8021q ifname=sfp vid=100 name=net1 rpfilter=strict ipv6=0"'
check 'has nets "interface net1: device=net1 proto=static ipaddr=192.168.1.1/24"'
check 'has nets "rule: src=192.168.1.0/24 lookup=101"'
check 'has nets "route: interface=ses1 target=0.0.0.0/0 table=101"'
check 'has nets "route: interface=net1 target=192.168.1.0/24 table=101"'
# No session hands an address down: the stick's own way out is the network's
# session, the default route of the main table, which the switch routes by.
check 'has ses1 "interface ses1: device=wan410 proto=pppoed session=1 family=ipv4 pass=0 defaultroute=1"'
U=$ONU_ROOT/run/udhcpd-net1.conf
check 'grep -qx "interface net1" "$U" && grep -qx "start 192.168.1.100" "$U" &&
	grep -qx "end 192.168.1.199" "$U" && grep -qx "max_leases 100" "$U"'
check 'grep -qx "opt router 192.168.1.1" "$U" && grep -qx "opt subnet 255.255.255.0" "$U" &&
	grep -qx "opt lease 3600" "$U" && grep -qx "auto_time 10" "$U"'
# A device of the network gets its fixed address; one of another network not.
check 'grep -qx "static_lease 00:00:5e:00:53:a1 192.168.1.10" "$U" && ! grep -q "10.0.0.10" "$U"'
# The session's name servers, once it brought them.
check 'grep -qx "opt dns 198.51.100.53" "$U"'
check 'logged "procd start udhcpd-net1 .*udhcpd -f .*/run/udhcpd-net1.conf"'
check 'logged "^onu-fw apply" && grep -qx "lan net1 192.168.1.0/24" "$T/fw.conf" &&
	grep -qx "wan gem256" "$T/fw.conf" && grep -qxF "out ppp*" "$T/fw.conf"'
# A router of networks is a hop, and forwards.
check 'logged "l3_ttl_decrement true"'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/ip_forward")" = 1 ]'
check '! logged "^nft"'
# A sync with nothing changed puts the table in place no second time.
: >"$LOG"
"$MODE_SCRIPT" sync 2>/dev/null
check '! logged "^onu-fw"'

echo "a network untagged on the host link, its pool all of its addresses"
printf '%s\n' SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION1_PASS=off LAN1_CONN=pppoe:1 \
	LAN1_ADDRESS=10.0.0.1/24 LAN1_DHCP=on LAN1_LEASE=86400 >"$ONU_ROOT/run/onu.conf"
: >"$LOG"
"$MODE_SCRIPT" reload 2>/dev/null
check 'has nets "interface net1: device=sfp proto=static ipaddr=10.0.0.1/24"'
check '! part nets | grep -q "8021q"'
check 'grep -qx "interface sfp" "$U" && grep -qx "start 10.0.0.1" "$U" && grep -qx "end 10.0.0.254" "$U"'
check 'grep -qx "lan sfp 10.0.0.0/24" "$T/fw.conf"'

echo "a network without DHCP has no server"
printf '%s\n' SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION1_PASS=off LAN1_CONN=pppoe:1 \
	LAN1_ADDRESS=10.0.0.1/24 LAN1_DHCP=off LAN1_LEASE=86400 >"$ONU_ROOT/run/onu.conf"
: >"$LOG"
"$MODE_SCRIPT" reload 2>/dev/null
check 'logged "procd stop udhcpd-net1" && [ ! -e "$U" ]'
check 'has nets "interface net1: device=sfp proto=static ipaddr=10.0.0.1/24"'

echo "the network gone"
printf '%s\n' SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION1_PASS=off >"$ONU_ROOT/run/onu.conf"
: >"$LOG"
"$MODE_SCRIPT" reload 2>/dev/null
check '[ ! -e "$ONU_ROOT/run/network.d/nets" ] && logged "^onu-fw clear"'
check '[ ! -e "$ONU_ROOT/run/onu-fw.conf" ]'
check 'logged "l3_ttl_decrement false"'
# The session is the stick's way out no more, and comes up again without it.
check 'logged "session 1 down" && has ses1 "interface ses1: device=wan410 proto=pppoed session=1 family=ipv4 pass=0 defaultroute=0"'
teardown

echo "a PPPoE session without name servers"
setup
printf '#!/bin/sh\necho "pppoed $*" >>"$LOG"\n%s\n%s 2\n' \
	'echo "up ppp0 local 192.0.2.2 peer 192.0.2.1 session 17 dns - -"' \
	"$REAL_SLEEP" >"$T/bin/pppoed"
mkdir -p "$ONU_ROOT/sys/class/net/ppp0"
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/etc/onu-pppoe"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	logged "IPv4 up on ppp0" && break
	"$REAL_SLEEP" 0.2
done
check 'logged "IPv4 up on ppp0"'
check '[ ! -e "$ONU_ROOT/run/onu-dns" ]'
check '"$MODE_SCRIPT" status >/dev/null'
check '! "$MODE_SCRIPT" status | grep -q "^dns"'
teardown

echo "a session whose interface went with the GEM interface under it is made again"
setup
mkdir -p "$ONU_ROOT/sys/class/net/ppp0"
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/etc/onu-pppoe"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
check '[ "$(grep -c "^onu-nl link add vlan gem256 wan410 410" "$LOG")" = 1 ]'
# A sync with the interface there changes nothing.
: >"$LOG"
"$MODE_SCRIPT" sync 2>/dev/null
check '! logged "^onu-nl link" && ! logged "^onu-net"'
# The kernel takes a VLAN interface away with the GEM interface it is on, which
# omcid makes again when the OLT provisions anew (seen after a new identity: the
# session stayed on a name that was gone, and a sync saw the same names).
rm -rf "$ONU_ROOT/sys/class/net/wan410"
: >"$LOG"
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^onu-nl link add vlan gem256 wan410 410"'
check '[ -e "$ONU_ROOT/sys/class/net/wan410" ] && [ "$(cat "$ONU_ROOT/run/session/1/dev")" = wan410 ]'
check 'logged "^onu-net set ses1" || logged "^onu-net add ses1"'
teardown

echo "a session's packets to the host side's networks are dropped on its interface"
setup
printf '#!/bin/sh\necho "pppoed $*" >>"$LOG"\n%s\n%s 2\n' \
	'echo "up ppp0 local 192.0.2.2 peer 192.0.2.1 session 17 dns - -"' \
	"$REAL_SLEEP" >"$T/bin/pppoed"
mkdir -p "$ONU_ROOT/sys/class/net/ppp0"
printf 'VLAN=4094\nADDRESS=198.51.100.2/30\nIFACE=mgmt\nDATA_ADDRESS=203.0.113.2/30\n' \
	>"$ONU_ROOT/run/onu-mgmt"
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/etc/onu-pppoe"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	logged "IPv4 up on ppp0" && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^onu-nl clsact add ppp0$"'
check 'logged "^onu-nl filter del ppp0 pref 1$" && ! logged "^onu-nl filter del ppp0 pref 1000$"'
check 'logged "^onu-nl filter add ppp0 pref 1 dst 198.51.100.2/30 drop$"'
check 'logged "^onu-nl filter add ppp0 pref 2 dst 203.0.113.2/30 drop$"'
check 'logged "^onu-nl filter add ppp0 pref 3 dst 169.254.0.0/16 drop$"'
# A pair that waits is kept off as well, once onu-mgmt asks again.
mkdir -p "$ONU_ROOT/run/onu-trial.d"
printf 'VLAN=100\nADDRESS=192.0.2.66/29\n' >"$ONU_ROOT/run/onu-trial.d/mgmt"
: >"$LOG"
"$MODE_SCRIPT" guard 2>/dev/null
check 'logged "^onu-nl filter add ppp0 pref 3 dst 192.0.2.66/29 drop$"'
check 'logged "^onu-nl filter add ppp0 pref 4 dst 169.254.0.0/16 drop$"'
teardown

echo "an account: the stick holds the session, in a VLAN, and hands it down in another"
setup
mkdir -p "$ONU_ROOT/sys/class/net/ppp0"
echo 1492 >"$ONU_ROOT/sys/class/net/ppp0/mtu"
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION1_HOST_VLAN=36 \
	SESSION1_MAC=00:00:5e:00:53:01
echo "user secret" >"$ONU_ROOT/etc/onu-pppoe"
"$MODE_SCRIPT" start 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = passthrough ]'
# omcid bridges the rest of what the OLT provisions, without the session's VLAN.
check 'logged "^omcid -m .*/run/omcid/mib -x .*/usr/lib/onu/onu-call$"'
check '! logged "omcid -r"'
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans")" = 410 ]'
check 'logged "^onu-nl link del omcibr0"'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/ip_forward")" = 1 ]'
# The link forwards only the handed address's packets (BCP 38).
check 'has links "device: type=8021q ifname=sfp vid=36 name=lan36 rpfilter=strict ipv6=0"'
check 'has links "interface lan36: device=lan36 proto=static ipaddr=169.254.0.1/32"'
# The stick is no hop: the switch routes with the TTL unchanged.
check 'logged "^onu-nl param l3_ttl_decrement false"'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/conf/sfp/rp_filter")" = 0 ]'
check '! logged "^udhcpd"'
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^onu-nl link add vlan gem256 wan410 410"'
check 'logged "^onu-nl link set wan410 address 00:00:5e:00:53:01"'
check '[ "$(grep -n "onu-nl link set wan410 address" "$LOG" | cut -d: -f1)" -lt "$(grep -n "^onu-nl link set wan410 up" "$LOG" | cut -d: -f1)" ]'
check 'logged "^onu-nl link set wan410 mtu 1508"'
# The links are onu-nl's: ip is not run.
check '! logged "^ip "'
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$ONU_ROOT/run/onu-pass" ] && logged "^udhcpd" && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe1 -D -4 -P -M$"'
check '[ "$(cat "$ONU_ROOT/run/session/1/lan")" = lan36 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/hands")" = ipv4 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/dev")" = wan410 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/gem")" = gem256 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/ipv4")" = 192.0.2.2 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/ppp")" = ppp0 ]'
check '[ -s "$ONU_ROOT/run/network.d/ses1" ] && grep -q "option proto .pppoed." "$ONU_ROOT/run/network.d/ses1"'
check '[ "$(cat "$ONU_ROOT/run/onu-pass")" = 192.0.2.2 ]'
check '[ "$(cat "$ONU_ROOT/run/onu-lan")" = lan36 ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/pass")" = 192.0.2.2 ]'
check 'has pass-1 "route: interface=lan36 target=192.0.2.2/32"'
# The stick's own way out: no table of its own.
check '! part pass-1 | grep -q "^rule"'
check 'grep -q "^interface lan36" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'grep -q "^start 192.0.2.2" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'grep -q "^end 192.0.2.2" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'grep -q "^opt subnet 255.255.255.255" "$ONU_ROOT/run/udhcpd-lan36.conf"'
# The gateway is the concentrator's address, as the router would have it with
# a session of its own; the stick answers ARP for it, at once, without
# taking it.
check 'grep -q "^opt router 192.0.2.1" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'grep -q "^opt staticroutes 192.0.2.1/32 0.0.0.0, 0.0.0.0/0 192.0.2.1" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'has pass-1 "neighbor: interface=lan36 ipaddr=192.0.2.1 proxy=1"'
check '! logged "^ip address add 192.0.2.1"'
check '[ "$(cat "$ONU_ROOT/run/session/1/gateway")" = 192.0.2.1 ]'
check 'grep -q "^opt dns 198.51.100.53" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'grep -q "^opt mtu 1492" "$ONU_ROOT/run/udhcpd-lan36.conf"'
check 'logged "^udhcpd -f $ONU_ROOT/run/udhcpd-lan36.conf"'
check '"$MODE_SCRIPT" status | grep -q "^pass 192.0.2.2 on lan36 from session 1"'
check '"$MODE_SCRIPT" status | grep -q "^session 1 on wan410 (ipv4), up on ppp0, 192.0.2.2"'
# The stand-in session ends after two seconds: the address goes with it,
# its route and the proxy ARP entry with netifd's part.
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	logged "^onu-net del pass-1$" && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^onu-net del pass-1$"'
"$MODE_SCRIPT" stop 2>/dev/null
check '[ ! -e "$ONU_ROOT/run/session/1" ]'
check '[ ! -e "$ONU_ROOT/run/onu-pass" ]'
check 'logged "^onu-nl link del wan410"'
teardown

echo "the settings of before: two sessions in one VLAN hand IPv4 and IPv6 down in LAN_VLAN"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION2_VLAN=410 SESSION2_FAMILY=ipv6 \
	LAN_VLAN=36 PASS_GATEWAY=192.0.2.254 PASS_PEER=off
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user/ipv6 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" start 2>/dev/null
# Both start at once: pppoed under netifd keeps their authentications apart
# (-w, pppoed's tests).
check '[ -s "$ONU_ROOT/run/network.d/ses1" ] && [ -s "$ONU_ROOT/run/network.d/ses2" ]'
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$ONU_ROOT/run/session/2/ipv6" ] && [ -s "$ONU_ROOT/run/onu-pass" ] && break
	"$REAL_SLEEP" 0.2
done
check '[ "$(grep -c "^onu-nl link add vlan gem256 wan410" "$LOG")" = 1 ]'
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe1 -D -4 -P"'
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe2 -6 -M$"'
check '[ "$(cat "$ONU_ROOT/run/session/2/ipv6")" = fe80::2 ]'
check '[ "$(cat "$ONU_ROOT/run/session/2/ppp")" = ppp1 ]'
# The IPv6 session hands its delegated prefix on, through dhcp6d.
for _ in 1 2 3 4 5 6 7 8 9 10; do
	logged "^dhcp6d" && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^dhcp6d -w ppp1 -l lan36 -m wan410 -o $ONU_ROOT/run/session/2 -b $ONU_ROOT/run/dhcp6d/pd2 -x $ONU_ROOT/usr/lib/onu/onu-call -n ses2$"'
# It runs as its own account with the three powers it uses, and tells onu-apply
# of a change through onu-call, which an account that is not root's can run.
check 'logged "^onu-drop -c net_bind_service,net_admin,net_raw dhcp6d dhcp6d -w ppp1 "'
# The session's directory is the group onu's, which the programs of the session write to.
check 'logged "^chown :onu $ONU_ROOT/run/session/2$"'
check 'ls -ld "$ONU_ROOT/run/session/2" | grep -q "^drwxrws---"'
# pppoed, an account of its own, reads the session's account: the group pppoed's.
check 'logged "^chown :pppoed $ONU_ROOT/run/onu-pppoe2$"'
check 'ls -l "$ONU_ROOT/run/onu-pppoe2" | grep -q "^-rw-r-----"'
check '[ "$(grep -c "^dhcp6d" "$LOG")" = 1 ]'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv6/conf/all/forwarding")" = 1 ]'
check 'has links "device: type=8021q ifname=sfp vid=36 name=lan36 rpfilter=strict ipv6=1"'
check '[ "$(cat "$ONU_ROOT/run/session/2/family")" = ipv6 ]'
check '[ "$(cat "$ONU_ROOT/run/session/2/lan")" = lan36 ]'
check '[ "$(cat "$ONU_ROOT/run/onu-pass")" = 192.0.2.2 ]'
# The gateway the stick told of before is no setting any more: the
# concentrator's address is the gateway, from the stick's own address.
check 'has links "interface lan36: device=lan36 proto=static ipaddr=169.254.0.1/32"'
check 'grep -q "^opt router 192.0.2.1" "$ONU_ROOT/run/udhcpd-lan36.conf"'
# The IPv6 account goes: its session ends, the VLAN stays for the other.
: >"$LOG"
rm "$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" sync 2>/dev/null
check '[ ! -e "$ONU_ROOT/run/session/2" ]'
check '[ -e "$ONU_ROOT/run/session/1" ]'
check '! logged "^onu-nl link del wan410"'
check '! logged "^onu-nl link set pon mtu 1500"'
check 'logged "^onu-net del ses2$" && [ ! -e "$ONU_ROOT/run/network.d/ses2" ]'
check 'logged "^procd stop dhcp6d2$" && [ ! -e "$ONU_ROOT/run/onu-svc.d/dhcp6d2" ]'
# The host dhcp6d delegates to goes into the store once, and its going too.
echo "0003 1 fe80::2 2001:db8:1200::/56" >"$ONU_ROOT/run/dhcp6d/pd2"
: >"$LOG"
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^onu-config pd 2$"'
check '[ "$(grep -c "^onu-config pd" "$LOG")" = 1 ]'
check 'cmp -s "$ONU_ROOT/run/dhcp6d/pd2" "$ONU_ROOT/run/onu-pd2.kept"'
: >"$LOG"
"$MODE_SCRIPT" sync 2>/dev/null
check '! logged "^onu-config pd"'
rm "$ONU_ROOT/run/dhcp6d/pd2"
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^onu-config pd 2$"'
check '[ ! -e "$ONU_ROOT/run/onu-pd2.kept" ]'
"$MODE_SCRIPT" stop 2>/dev/null
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv6/conf/all/forwarding")" = 0 ]'
teardown

echo "two sessions follow their GEM port when the OLT replaces it"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION2_VLAN=410 SESSION2_FAMILY=ipv6
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user/ipv6 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^onu-nl link add vlan gem256 wan410 410"'
check '[ "$(cat "$ONU_ROOT/run/session/2/gem")" = gem256 ]'
# The OLT provisions again: GEM port 256 goes, and the kernel takes wan410
# with it; the sessions move to the GEM port there is now.
rm -r "$ONU_ROOT/sys/class/net/gem256" "$ONU_ROOT/sys/class/net/wan410"
printf '#!/bin/sh\n[ "$1" = gem ] || exit 1\n%s\n' \
	'printf "gem4095 port 4095 down multicast\ngem1024 port 1024 both tcont 1\n"' \
	>"$T/bin/pon"
: >"$LOG"
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^onu-nl link add vlan gem1024 wan410 410"'
check '[ "$(cat "$ONU_ROOT/run/session/1/gem")" = gem1024 ]'
# The second session moves a round later, onto the interface the first made.
"$MODE_SCRIPT" sync 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/session/2/gem")" = gem1024 ]'
check '[ "$(grep -c "^onu-nl link add vlan gem1024 wan410" "$LOG")" = 1 ]'
teardown

echo "a session in no VLAN runs on the GEM interface, on the GEM port named"
setup
conf SESSION1_GEM=1024 SESSION1_FAMILY=both
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^pppoed up -i gem1024 -f $ONU_ROOT/run/onu-pppoe1 -D -P"'
check '[ "$(cat "$ONU_ROOT/run/session/1/hands")" = "ipv4 ipv6" ]'
check '! logged "type vlan id"'
# The session holds the whole GEM port: omcid leaves the OLT's bridge out, and
# every frame of the downstream ports goes to the session's GEM interface.
check 'logged "^omcid -r"'
check '[ ! -e "$ONU_ROOT/run/onu-held-vlans" ]'
check 'logged "^onu-nl filter add gem4095 redirect gem1024"'
"$MODE_SCRIPT" stop 2>/dev/null
check 'logged "^onu-nl link set gem1024 mtu 1500"'
check 'logged "^onu-nl link set pon mtu 1500"'
check '[ ! -e "$ONU_ROOT/run/session/1" ]'
teardown

echo "a session in no VLAN and a GEM port on auto: the lowest that carries both ways and is no multicast"
setup
# Lower than the others: a port that only receives, and a multicast port that
# is not 4095. Neither is a port a session can run on.
mkdir -p "$ONU_ROOT/sys/class/net/gem100" "$ONU_ROOT/sys/class/net/gem200"
printf '#!/bin/sh\n[ "$1" = gem ] || exit 1\n%s\n' \
	'printf "gem100 port 100 down\ngem200 port 200 down multicast\ngem256 port 256 both tcont 0\ngem1024 port 1024 both tcont 1\n"' \
	>"$T/bin/pon"
conf SESSION1_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^pppoed up -i gem256 "'
check '! logged "pppoed up -i gem100" && ! logged "pppoed up -i gem200"'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "a driver that has no GEM port for a moment: the session looks again, and does not go down"
setup
# pon gem lists nothing the first two times it is asked, as while the ports are made again.
printf '#!/bin/sh\necho x >>"%s/pon-asked"\n[ "$1" = gem ] || exit 1\n[ "$(wc -l <"%s/pon-asked")" -gt 2 ] || exit 0\nprintf "gem256 port 256 both tcont 0\\n"\n' \
	"$T" "$T" >"$T/bin/pon"
conf SESSION1_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
check 'logged "^pppoed up -i gem256 "'
check '! grep -q "GEM port is gone" "$LOG"'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "the settings of before: the WAN's VLAN and MAC are the first session's"
setup
conf MODE=passthrough WAN_VLAN=410 WAN_MAC=00:00:5e:00:53:01
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^onu-nl link add vlan gem256 wan410 410"'
check 'logged "^onu-nl link set wan410 address 00:00:5e:00:53:01"'
teardown

echo "without an account there is no session: the OLT's bridge"
setup
conf MODE=passthrough SESSION1_VLAN=410
"$MODE_SCRIPT" start 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	logged "^omcid" && break
	"$REAL_SLEEP" 0.2
done
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ]'
check 'logged "^omcid -m .*/run/omcid/mib -x .*/usr/lib/onu/onu-call$"'
teardown

echo "an untagged session: the host link itself is its link, until its account goes"
setup
"$MODE_SCRIPT" start 2>/dev/null
: >"$LOG"
: >"$ONU_ROOT/run/onu.conf"
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" reload 2>/dev/null
check 'has links "interface sfp: device=sfp proto=static ipaddr=169.254.0.1/32"'
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = passthrough ]'
# The host link itself is the LAN link: it checks the sources, until the
# stick leaves passthrough with the account.
check 'has links "device: name=sfp rpfilter=strict ipv6=1"'
rm "$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" reload 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ]'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/conf/sfp/rp_filter")" = 0 ]'
teardown

echo "an account in a VLAN beside a running omcid: its bridge stays, without that VLAN"
setup
conf MODE=bridge
"$MODE_SCRIPT" start 2>/dev/null
: >"$LOG"
printf 'SESSION1_VLAN=1410\n' >"$ONU_ROOT/run/onu.conf"
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" reload 2>/dev/null
check 'logged "^killall -USR2 omcid"'
check '! logged "^killall -USR1"'
check '[ "$(grep -n "onu-nl link del omcibr0" "$LOG" | cut -d: -f1)" -lt "$(grep -n "killall -USR2 omcid" "$LOG" | cut -d: -f1)" ]'
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans")" = 1410 ]'
"$MODE_SCRIPT" sync 2>/dev/null
# The other ports hand on the frames of the session's VLAN alone, and the
# session's own port none.
check 'logged "^onu-nl filter add gem4095 vlan 1410 redirect gem256"'
check 'logged "^onu-nl filter add gem1024 vlan 1410 redirect gem256"'
check '! logged "^onu-nl clsact add gem256$"'
check '! logged "matchall"'
# The same ports again: their filters stay as they are.
: >"$LOG"
"$MODE_SCRIPT" sync 2>/dev/null
check '! logged "^onu-nl filter add"'
# The OLT provisions its GEM ports again: the interfaces come back under the
# same names, with new indexes and without filters, and get them again.
for dev in gem4095 gem256 gem1024; do echo 9 >"$ONU_ROOT/sys/class/net/$dev/ifindex"; done
: >"$LOG"
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^onu-nl filter add gem4095 vlan 1410 redirect gem256"'
check 'logged "^onu-nl filter add gem1024 vlan 1410 redirect gem256"'
# A second session in another VLAN: omcid builds its bridge again without both.
: >"$LOG"
printf 'SESSION1_VLAN=1410\nSESSION2_VLAN=410\n' >"$ONU_ROOT/run/onu.conf"
echo "user/ipv6 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" reload 2>/dev/null
check 'logged "^killall -USR2 omcid"'
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans" | tr "\n" " ")" = "410 1410 " ]'
# The same sessions again: omcid is left as it is.
: >"$LOG"
"$MODE_SCRIPT" reload 2>/dev/null
check '! logged "omcid"'
teardown

echo "a session in the router's VLAN runs in the VLAN the OLT translates it into"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
# Before the OLT's translations the session runs in the VLAN it was given.
check 'logged "^onu-nl link add vlan gem256 wan410 410"'
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans")" = 410 ]'
: >"$LOG"
printf '410 1410\n420 1430\n' >"$ONU_ROOT/run/omcid/xlate"
"$MODE_SCRIPT" sync 2>/dev/null
# With them, omcid leaves 1410 out, and the session moves into it.
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans")" = 1410 ]'
check 'logged "^killall -USR2 omcid"'
check 'logged "^onu-nl link del wan410"'
check 'logged "^onu-nl link add vlan gem256 wan1410 1410"'
check 'logged "^pppoed up -i wan1410 "'
check 'logged "^onu-nl filter add gem4095 vlan 1410 redirect gem256"'
# The next round changes nothing.
: >"$LOG"
"$MODE_SCRIPT" sync 2>/dev/null
check '! logged "omcid"'
check '! logged "^onu-nl link"'
teardown

echo "a session follows its service to the GEM port and the VLAN the OLT moves it to"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
printf '410 1410\n' >"$ONU_ROOT/run/omcid/xlate"
# The OLT lets 1410 through Port-ID 1024, not the lowest Port-ID, 256.
printf '1410 1024\n1430 256\n' >"$ONU_ROOT/run/omcid/gem-vlans"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^onu-nl link add vlan gem1024 wan1410 1410"'
check '[ "$(cat "$ONU_ROOT/run/session/1/gem")" = gem1024 ]'
: >"$LOG"
# The operator moves the service to 1600 on Port-ID 256, and keeps 410.
printf '410 1600\n' >"$ONU_ROOT/run/omcid/xlate"
printf '1600 256\n' >"$ONU_ROOT/run/omcid/gem-vlans"
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^onu-nl link del wan1410"'
check 'logged "^onu-nl link add vlan gem256 wan1600 1600"'
check 'logged "^pppoed up -i wan1600 "'
check '[ "$(cat "$ONU_ROOT/run/session/1/gem")" = gem256 ]'
# A GEM port named by its Port-ID stays, whatever the OLT lets through it.
: >"$LOG"
printf '%s\n' SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION1_GEM=1024 >"$ONU_ROOT/run/onu.conf"
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^onu-nl link add vlan gem1024 wan1600 1600"'
teardown

echo "the account removed: from passthrough back to bridge"
setup
conf LAN_VLAN=36
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
: >"$LOG"
rm "$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" reload 2>/dev/null
check 'logged "^onu-net del links$" && [ ! -e "$ONU_ROOT/run/network.d/links" ]'
check 'logged "^onu-nl param l3_ttl_decrement true"'
check 'logged "^killall -USR2 omcid"'
check '[ ! -e "$ONU_ROOT/run/onu-held-vlans" ]'
check '[ ! -e "$ONU_ROOT/run/onu-lan" ]'
check '[ "$(cat "$ONU_ROOT/proc/sys/net/ipv4/ip_forward")" = 0 ]'
teardown

echo "a session told to keep its address: no link to the host, the address on the stick"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION1_PASS=off
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$ONU_ROOT/run/session/1/ipv4" ] && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe1 -4 -M$"'
check '[ ! -e "$ONU_ROOT/run/network.d/links" ]'
check '[ ! -e "$ONU_ROOT/run/session/1/lan" ]'
check '[ -e "$ONU_ROOT/run/session/1/hands" ] && [ ! -s "$ONU_ROOT/run/session/1/hands" ]'
check '! logged "^udhcpd"'
check '[ ! -e "$ONU_ROOT/run/onu-pass" ]'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "a session that names no MAC address sends from the stick's, made locally administered"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
# The locally administered forms are put together here: only the
# documentation range may be written out (tools/provenance).
la=$(printf %02x 2):00:5e:00:53:01
turned=$(printf %02x 2):00:5e:00:53:00
check 'logged "^onu-nl link set wan410 address $la"'
"$MODE_SCRIPT" stop 2>/dev/null
echo "$la" >"$ONU_ROOT/sys/class/net/sfp/address"
: >"$LOG"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
check 'logged "^onu-nl link set wan410 address $turned"'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "in one VLAN to the host one session hands its IPv4 address down"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION2_VLAN=410 SESSION2_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user2 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
check '[ "$(part links | grep -c "name=lan410")" = 1 ]'
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe1 -D -4 -P -M$"'
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe2 -4 -M$"'
check '[ "$(cat "$ONU_ROOT/run/session/1/lan")" = lan410 ]'
check '[ ! -e "$ONU_ROOT/run/session/2/lan" ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/hands")" = ipv4 ] && [ ! -s "$ONU_ROOT/run/session/2/hands" ]'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "each VLAN to the host a link of its own: the second session routes by its own table"
setup
mkdir -p "$ONU_ROOT/sys/class/net/ppp0"
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION2_VLAN=410 SESSION2_FAMILY=ipv4 \
	SESSION2_HOST_VLAN=37
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user2 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" start 2>/dev/null
check 'has links "device: type=8021q ifname=sfp vid=410 name=lan410 rpfilter=strict ipv6=0"'
check 'has links "device: type=8021q ifname=sfp vid=37 name=lan37 rpfilter=strict ipv6=0"'
"$MODE_SCRIPT" sync 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -s "$ONU_ROOT/run/session/2/pass" ] && [ -s "$ONU_ROOT/run/session/1/pass" ] && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe1 -D -4 -P -M$"'
check 'logged "^pppoed up -i wan410 -f $ONU_ROOT/run/onu-pppoe2 -4 -P -M$"'
check '[ "$(cat "$ONU_ROOT/run/session/2/lan")" = lan37 ]'
check 'has pass-2 "rule: src=192.0.2.2/32 lookup=102"'
check 'has pass-2 "route: interface=ses2 target=0.0.0.0/0 table=102"'
check '! part pass-1 | grep -q "^rule"'
check 'grep -q "^interface lan410" "$ONU_ROOT/run/udhcpd-lan410.conf"'
check 'grep -q "^interface lan37" "$ONU_ROOT/run/udhcpd-lan37.conf"'
check 'grep -q "^pidfile $ONU_ROOT/run/udhcpd-lan37.pid" "$ONU_ROOT/run/udhcpd-lan37.conf"'
check '[ "$("$MODE_SCRIPT" status | grep -c "^pass ")" = 2 ]'
# What the status daemon shows as the address handed on is the first's.
check '[ "$(cat "$ONU_ROOT/run/onu-lan")" = lan410 ]'
: >"$LOG"
"$MODE_SCRIPT" stop 2>/dev/null
check 'logged "^onu-net del pass-2$" && logged "^onu-net del links$"'
check '[ -z "$(ls "$ONU_ROOT/run/network.d")" ]'
teardown

echo "a change of one session leaves the other session up, on trial and back"
setup
echo "1234.56 99.0" >"$ONU_ROOT/proc/uptime"
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION2_VLAN=36 SESSION2_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user2 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" start 2>/dev/null
check '[ -d "$ONU_ROOT/run/session/1" ] && [ -d "$ONU_ROOT/run/session/2" ]'
check '[ "$(stat -c %a "$ONU_ROOT/run/session/1/makings" 2>/dev/null || stat -f %Lp "$ONU_ROOT/run/session/1/makings")" = 600 ]'
check '! grep -q secret "$ONU_ROOT/run/session/1/makings"'
sed -i.bak 's/^SESSION2_FAMILY=ipv4$/SESSION2_FAMILY=both/' "$ONU_ROOT/run/onu.conf"
: >"$LOG"
TRIAL_LIMIT=30 "$MODE_SCRIPT" try 2>/dev/null
check '! logged "^onu-net del ses1$" && logged "^onu-net del ses2$"'
check '[ "$(cat "$ONU_ROOT/run/session/2/family")" = both ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/family")" = ipv4 ] && ! logged "^onu-net del links$"'
: >"$LOG"
"$MODE_SCRIPT" revert 2>/dev/null
check '! logged "^onu-net del ses1$" && logged "^onu-net del ses2$"'
check '[ "$(cat "$ONU_ROOT/run/session/2/family")" = ipv4 ]'
# A new account for one session is a change of that one alone.
echo "user2 another" >"$ONU_ROOT/run/onu-pppoe2"
: >"$LOG"
"$MODE_SCRIPT" reload 2>/dev/null
check '! logged "^onu-net del ses1$" && logged "^onu-net del ses2$"'
# Nothing changed: nothing comes down.
: >"$LOG"
"$MODE_SCRIPT" reload 2>/dev/null
check '! logged "^onu-net del ses"'
# One session's account gone: that one goes, the other stays.
rm -f "$ONU_ROOT/run/onu-pppoe2"
: >"$LOG"
"$MODE_SCRIPT" reload 2>/dev/null
check '! logged "^onu-net del ses1$" && logged "^onu-net del ses2$" && [ ! -d "$ONU_ROOT/run/session/2" ]'
# The last account gone: back to the bridge, the session down.
rm -f "$ONU_ROOT/run/onu-pppoe1"
: >"$LOG"
"$MODE_SCRIPT" reload 2>/dev/null
check 'logged "^onu-net del ses1$" && [ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ]'
teardown

echo "two prefixes handed down in two VLANs: the second by its own table"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv6 SESSION2_VLAN=410 SESSION2_FAMILY=ipv6 \
	SESSION2_HOST_VLAN=37
echo "user/ipv6 secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user2/ipv6 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" start 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
"$MODE_SCRIPT" sync 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ "$(grep -c "^dhcp6d" "$LOG")" = 2 ] && break
	"$REAL_SLEEP" 0.2
done
check 'logged "^dhcp6d -w ppp1 -l lan410 -m wan410 -o $ONU_ROOT/run/session/1 -b $ONU_ROOT/run/dhcp6d/pd1 -x $ONU_ROOT/usr/lib/onu/onu-call -n ses1$"'
check 'logged "^dhcp6d -w ppp1 -l lan37 -m wan410 -o $ONU_ROOT/run/session/2 -b $ONU_ROOT/run/dhcp6d/pd2 -x $ONU_ROOT/usr/lib/onu/onu-call -n ses2 -t 102$"'
check 'has links "device: type=8021q ifname=sfp vid=37 name=lan37 rpfilter=strict ipv6=1"'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "no VLAN to the host: the host link itself"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION1_HOST_VLAN=untagged
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
check 'has links "device: name=sfp rpfilter=strict ipv6=1"'
check 'has links "interface sfp: device=sfp proto=static ipaddr=169.254.0.1/32"'
check '[ "$(part links | wc -l)" -eq 2 ]'
"$MODE_SCRIPT" stop 2>/dev/null
check '[ ! -e "$ONU_ROOT/run/network.d/links" ]'
teardown

echo "the settings of before: an empty LAN_VLAN is the host link itself"
setup
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 LAN_VLAN=
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
check 'has links "interface sfp: device=sfp proto=static ipaddr=169.254.0.1/32"'
check '! part links | grep -q "type=8021q"'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "settings that are not values"
setup
printf 'MODE=router;reboot\nWAN_VLAN=$(reboot)\nOTHER=x\n' >"$ONU_ROOT/etc/onu.conf"
"$MODE_SCRIPT" start 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ]'
check '! logged "reboot"'
teardown

echo "a new identity: the sessions end, the PON goes down, takes it, comes up, and omcid starts over"
setup
printf 'serial=XXXXXXXXXXXX\nploam_password=00112233445566778899\n' >"$ONU_ROOT/run/onu-identity"
echo passthrough >"$ONU_ROOT/run/onu-mode"
mkdir -p "$ONU_ROOT/run/session/1"
echo gem256 >"$ONU_ROOT/run/session/1/gem"
echo wan410 >"$ONU_ROOT/run/session/1/dev"
# omcid runs as the instance of procd's service "onu" that onu-apply gave it.
mkdir -p "$ONU_ROOT/run/onu-svc.d"
echo "-- $T/bin/onu-run omcid" >"$ONU_ROOT/run/onu-svc.d/omcid"
cp "$ONU_ROOT/run/onu-svc.d/omcid" "$T/svc-omcid"
: >"$T/omcid.running"
"$MODE_SCRIPT" reidentify 2>/dev/null
check 'logged "^onu-nl link set pon down"'
check 'logged "^onu-net del ses1$" && [ ! -d "$ONU_ROOT/run/session/1" ]'
check '[ "$(grep -n "^onu-net del ses1" "$LOG" | cut -d: -f1)" -lt "$(grep -n "onu-nl link set pon down" "$LOG" | cut -d: -f1)" ]'
check 'logged "^onu-nl param gpon_serial XXXXXXXXXXXX$"'
check 'logged "^onu-nl param gpon_password 0x00112233445566778899$"'
check '[ "$(grep -n "onu-nl link set pon down" "$LOG" | cut -d: -f1)" -lt "$(grep -n "gpon_serial" "$LOG" | cut -d: -f1)" ]'
check '[ "$(grep -n "gpon_serial" "$LOG" | cut -d: -f1)" -lt "$(grep -n "onu-nl link set pon up" "$LOG" | cut -d: -f1)" ]'
# omcid ends as procd's service and starts again, in bridge mode.
check 'logged "^procd stop omcid$"'
check 'logged "^procd start omcid -- .*/onu-run omcid$"'
check 'logged "^omcid -m .*/run/omcid/mib -x .*/usr/lib/onu/onu-call$"'
check '[ "$(grep -n "^procd stop omcid" "$LOG" | cut -d: -f1)" -lt "$(grep -n "^omcid" "$LOG" | cut -d: -f1)" ]'
# omcid, an account of its own, reads the identity without the PLOAM password,
# in its own directory, as the group's.
check 'grep -qx serial=XXXXXXXXXXXX "$ONU_ROOT/run/omcid/identity" && ! grep -q ploam "$ONU_ROOT/run/omcid/identity"'
check 'logged "^chown :omcid $ONU_ROOT/run/omcid/identity.new$"'
check 'ls -l "$ONU_ROOT/run/omcid/identity" | grep -q "^-rw-r-----"'
check 'grep -q ploam_password "$ONU_ROOT/run/onu-identity"'
teardown

echo "a new identity: omcid starts over although procd ends the old one late"
setup
printf 'serial=XXXXXXXXXXXX\nploam_password=00112233445566778899\n' >"$ONU_ROOT/run/onu-identity"
echo passthrough >"$ONU_ROOT/run/onu-mode"
mkdir -p "$ONU_ROOT/run/onu-svc.d"
echo "-- $T/bin/onu-run omcid" >"$ONU_ROOT/run/onu-svc.d/omcid"
cp "$ONU_ROOT/run/onu-svc.d/omcid" "$T/svc-omcid"
: >"$T/omcid.running"
# The old daemon lingers for a moment after procd was told to end it.
printf '#!/bin/sh\n[ "$1" = omcid ] && { [ -e "%s/omcid.running" ] || [ -e "%s/dying" ]; }\n' "$T" "$T" >"$T/bin/pidof"
: >"$T/dying"
("$REAL_SLEEP" 0.4; rm -f "$T/dying") &
"$MODE_SCRIPT" reidentify 2>/dev/null
check 'logged "^procd start omcid -- .*/onu-run omcid$" && ! logged "^killall"'
teardown

echo "without a serial number the PON stays down"
setup
: >"$ONU_ROOT/run/onu-identity"
"$MODE_SCRIPT" identity 2>/dev/null
rc=$?
check '[ $rc -ne 0 ]'
check '! logged "onu-nl link set pon up"'
teardown

echo "each session an interface of netifd, of the protocol pppoed, in its role"
setup
mkdir -p "$ONU_ROOT/sys/class/net/ppp0"
# The sessions last a while here, so that none ends and starts again while
# the events below are told.
sed "s/ 2\$/ 6/" "$T/bin/pppoed" >"$T/pppoed" && cat "$T/pppoed" >"$T/bin/pppoed"
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION2_VLAN=410 SESSION2_FAMILY=ipv4 \
	SESSION2_HOST_VLAN=37 SESSION3_VLAN=410 SESSION3_FAMILY=ipv6 SESSION3_PASS=off
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user2 secret" >"$ONU_ROOT/run/onu-pppoe2"
echo "user3 secret" >"$ONU_ROOT/run/onu-pppoe3"
"$MODE_SCRIPT" start 2>/dev/null
for _ in 1 2 3; do "$MODE_SCRIPT" sync 2>/dev/null; done
net() {
	tr -d "\t'" <"$ONU_ROOT/run/network.d/ses$1" | grep "^option" | tr '\n' ' '
}
# The stick's way out hands its address on and is the default route; the
# second hands on by its own table; the third keeps its prefix.
check '[ "$(net 1)" = "option device wan410 option proto pppoed option session 1 option family ipv4 option pass 1 option defaultroute 1 " ]'
check '[ "$(net 2)" = "option device wan410 option proto pppoed option session 2 option family ipv4 option pass 1 option defaultroute 0 " ]'
check '[ "$(net 3)" = "option device wan410 option proto pppoed option session 3 option family ipv6 option pass 0 option defaultroute 0 " ]'
check '[ "$(cat "$ONU_ROOT/run/session/1/role")" = "pass none" ] && [ "$(cat "$ONU_ROOT/run/session/2/role")" = "routed none" ] && [ "$(cat "$ONU_ROOT/run/session/3/role")" = "none none" ]'
# Both sessions that hand their addresses on have, and every event of
# their coming up is told.
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	[ -s "$ONU_ROOT/run/session/1/pass" ] && [ -s "$ONU_ROOT/run/session/2/pass" ] && break
	"$REAL_SLEEP" 0.2
done
"$REAL_SLEEP" 0.5
# Another event of the same session hands on nothing again; another
# interface's is no business of onu-apply.
: >"$LOG"
ACTION=ifupdate INTERFACE=ses1 DEVICE=ppp0 "$MODE_SCRIPT" iface 2>/dev/null
ACTION=ifup INTERFACE=mgmt DEVICE=mgmt "$MODE_SCRIPT" iface 2>/dev/null
rc=$?
check '[ $rc -eq 0 ] && ! logged "^procd start" && ! logged "^ip "'
"$MODE_SCRIPT" stop 2>/dev/null
check 'logged "^onu-net del ses1$" && logged "^onu-net del ses2$" && logged "^onu-net del ses3$"'
check '[ -z "$(ls "$ONU_ROOT/run/network.d" 2>/dev/null)" ]'
teardown

echo "one onu-apply at a time, and changes told while one waits are that one's"
setup
# A session on a GEM port the OLT has not provisioned yet.
conf SESSION1_GEM=999 SESSION1_FAMILY=ipv4
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
"$MODE_SCRIPT" start 2>/dev/null
lock=$ONU_ROOT/run/onu-apply.lock
# A live process holds the lock: a sync waits for it, and one more sync
# told meanwhile leaves the change to the one that waits.
"$REAL_SLEEP" 30 &
holder=$!
ln -s "$holder" "$lock"
# The OLT provisions the session's GEM port, which a sync follows.
mkdir -p "$ONU_ROOT/sys/class/net/gem999"
"$MODE_SCRIPT" sync 2>/dev/null &
waiting=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -L "$ONU_ROOT/run/onu-apply.again" ] && break
	"$REAL_SLEEP" 0.1
done
check '[ "$(readlink "$ONU_ROOT/run/onu-apply.again")" = "$waiting" ]'
"$MODE_SCRIPT" omcid 2>/dev/null
rc=$?
check '[ $rc -eq 0 ] && kill -0 "$waiting" 2>/dev/null'
check '[ ! -e "$ONU_ROOT/run/network.d/ses1" ]'
kill "$holder"
wait "$holder" 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	kill -0 "$waiting" 2>/dev/null || break
	"$REAL_SLEEP" 0.2
done
check '! kill -0 "$waiting" 2>/dev/null'
check '[ -e "$ONU_ROOT/run/network.d/ses1" ]'
check '[ ! -L "$ONU_ROOT/run/onu-apply.again" ] && [ ! -L "$lock" ]'
# A lock whose holder is gone is taken over; dhcp6d's change is a sync too.
"$REAL_SLEEP" 0 &
dead=$!
wait "$dead"
ln -s "$dead" "$lock"
"$MODE_SCRIPT" dhcp6d 2>/dev/null
check '[ $? -eq 0 ] && [ ! -L "$lock" ]'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "in bridge mode a sync leaves omcid alone with the OLT"
setup
"$MODE_SCRIPT" start 2>/dev/null
: >"$LOG"
"$MODE_SCRIPT" sync 2>/dev/null
check '[ $? -eq 0 ] && ! logged "^ip " && ! logged "^procd start" && ! logged "^onu-net"'
teardown

# A change on trial, as the status daemon makes it: the new settings and
# accounts are written, then "onu-apply try". A trial has no watcher of its
# own, and the stand-in sleep here returns at once: one that waited on it
# would end the trial at once. The keeper of the deadlines (onu-change watch)
# takes it back, with its token.
trial_setup() {
	setup
	echo "1234.56 99.0" >"$ONU_ROOT/proc/uptime"
	conf MODE=bridge
	"$MODE_SCRIPT" start 2>/dev/null
	printf 'SESSION1_VLAN=410\nSESSION1_FAMILY=ipv4\n' >"$ONU_ROOT/run/onu.conf"
	echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
	: >"$LOG"
}

echo "a change on trial, confirmed: the store takes it then, and not before"
trial_setup
TRIAL_LIMIT=30 "$MODE_SCRIPT" try 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = passthrough ] && ! logged "^onu-config save"'
check 'grep -qx UNTIL=1264 "$ONU_ROOT/run/onu-trial.d/settings"'
check '"$MODE_SCRIPT" status | grep -qx "trial until 1264"'
# What it goes back to: the settings of before, without the account.
check 'grep -qx MODE=bridge "$ONU_ROOT/run/onu-kept/onu.conf" && [ ! -e "$ONU_ROOT/run/onu-kept/onu-pppoe1" ]'
check '[ "$(stat -c %a "$ONU_ROOT/run/onu-kept" 2>/dev/null || stat -f %Lp "$ONU_ROOT/run/onu-kept")" = 700 ]'
"$MODE_SCRIPT" confirm 2>/dev/null
check 'logged "^onu-config save$"'
check '[ ! -e "$ONU_ROOT/run/onu-trial.d/settings" ] && [ ! -e "$ONU_ROOT/run/onu-kept" ]'
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = passthrough ] && [ -e "$ONU_ROOT/run/onu-pppoe1" ]'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "a change waits aside until try: a sync between applies the settings in force"
trial_setup
"$MODE_SCRIPT" try 2>/dev/null
"$MODE_SCRIPT" confirm 2>/dev/null
# What onu-config change writes: the settings and a new account, aside.
mkdir -p "$ONU_ROOT/run/onu-pending"
printf 'SESSION1_VLAN=36\nSESSION1_FAMILY=ipv4\n' >"$ONU_ROOT/run/onu-pending/onu.conf"
echo "user another" >"$ONU_ROOT/run/onu-pending/onu-pppoe1"
: >"$LOG"
"$MODE_SCRIPT" sync 2>/dev/null
check '! logged "^onu-net del ses1$" && grep -qx SESSION1_VLAN=410 "$ONU_ROOT/run/onu.conf"'
TRIAL_LIMIT=30 "$MODE_SCRIPT" try 2>/dev/null
check 'grep -qx SESSION1_VLAN=36 "$ONU_ROOT/run/onu.conf" && [ ! -e "$ONU_ROOT/run/onu-pending" ]'
check '[ "$(cat "$ONU_ROOT/run/onu-pppoe1")" = "user another" ] && logged "^onu-net del ses1$"'
# And back: the settings and the account of before.
"$MODE_SCRIPT" revert 2>/dev/null
check 'grep -qx SESSION1_VLAN=410 "$ONU_ROOT/run/onu.conf" && [ "$(cat "$ONU_ROOT/run/onu-pppoe1")" = "user secret" ]'
# An account removed waits as an empty file.
mkdir -p "$ONU_ROOT/run/onu-pending"
printf 'SESSION1_VLAN=410\n' >"$ONU_ROOT/run/onu-pending/onu.conf"
: >"$ONU_ROOT/run/onu-pending/onu-pppoe1"
TRIAL_LIMIT=30 "$MODE_SCRIPT" try 2>/dev/null
check '[ ! -e "$ONU_ROOT/run/onu-pppoe1" ] && [ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ]'
"$MODE_SCRIPT" revert 2>/dev/null
check '[ -e "$ONU_ROOT/run/onu-pppoe1" ] && [ "$(cat "$ONU_ROOT/run/onu-mode")" = passthrough ]'
teardown

echo "a change on trial unconfirmed: back to the settings and the accounts of before"
trial_setup
TRIAL_LIMIT=1 "$MODE_SCRIPT" try 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = passthrough ]'
# The keeper's call when the time has come: the program and the token.
token=$(sed -n 's/^TOKEN=//p' "$ONU_ROOT/run/onu-trial.d/settings")
"$MODE_SCRIPT" expire "$token" 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ]'
check 'grep -qx MODE=bridge "$ONU_ROOT/run/onu.conf" && [ ! -e "$ONU_ROOT/run/onu-pppoe1" ]'
check '[ ! -e "$ONU_ROOT/run/onu-trial.d/settings" ] && [ ! -e "$ONU_ROOT/run/onu-kept" ]'
check '! logged "^onu-config save" && logged "not confirmed"'
teardown

echo "a trial that goes back puts the accounts in place anew, and pppoed reads them again"
setup
echo "1234.56 99.0" >"$ONU_ROOT/proc/uptime"
conf SESSION1_VLAN=410 SESSION1_FAMILY=ipv4 SESSION2_VLAN=410 SESSION2_FAMILY=ipv6
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
echo "user/ipv6 secret" >"$ONU_ROOT/run/onu-pppoe2"
"$MODE_SCRIPT" start 2>/dev/null
# A change of the second session's account alone, as the status daemon makes it.
mkdir -p "$ONU_ROOT/run/onu-pending"
cp "$ONU_ROOT/run/onu.conf" "$ONU_ROOT/run/onu-pending/onu.conf"
echo "user/ipv6 other" >"$ONU_ROOT/run/onu-pending/onu-pppoe2"
TRIAL_LIMIT=30 "$MODE_SCRIPT" try 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-pppoe2")" = "user/ipv6 other" ]'
check 'ls -l "$ONU_ROOT/run/onu-pppoe2" | grep -q "^-rw-r-----"'
: >"$LOG"
"$MODE_SCRIPT" revert 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-pppoe2")" = "user/ipv6 secret" ]'
# The first session did not change and did not start again; its account came
# back from the copy that only root reads, and is pppoed's again.
check '! logged "^onu-net del ses1$"'
check 'logged "^chown :pppoed $ONU_ROOT/run/onu-pppoe1$"'
check 'ls -l "$ONU_ROOT/run/onu-pppoe1" | grep -q "^-rw-r-----"'
check 'ls -l "$ONU_ROOT/run/onu-pppoe2" | grep -q "^-rw-r-----"'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "a trial ended at once, and one that a second change starts again"
trial_setup
TRIAL_LIMIT=30 "$MODE_SCRIPT" try 2>/dev/null
# A second change on trial: back is still to the settings before the first.
printf 'SESSION1_VLAN=36\nSESSION1_FAMILY=ipv4\n' >"$ONU_ROOT/run/onu.conf"
TRIAL_LIMIT=30 "$MODE_SCRIPT" try 2>/dev/null
check 'grep -qx MODE=bridge "$ONU_ROOT/run/onu-kept/onu.conf"'
"$MODE_SCRIPT" revert 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ] && [ ! -e "$ONU_ROOT/run/onu-pppoe1" ]'
check '[ ! -e "$ONU_ROOT/run/onu-trial.d/settings" ] && ! logged "^onu-config save"'
# A reload saves what it applies, which no trial goes back on.
printf 'SESSION1_VLAN=410\nSESSION1_FAMILY=ipv4\n' >"$ONU_ROOT/run/onu.conf"
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
TRIAL_LIMIT=30 "$MODE_SCRIPT" try 2>/dev/null
"$MODE_SCRIPT" reload 2>/dev/null
check 'logged "^onu-config save$" && [ ! -e "$ONU_ROOT/run/onu-trial.d/settings" ] && [ ! -e "$ONU_ROOT/run/onu-kept" ]'
"$MODE_SCRIPT" revert 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = passthrough ]'
"$MODE_SCRIPT" stop 2>/dev/null
teardown

echo "a signal to omcid is waited for until omcid says that it changed, not for a second"
setup
conf MODE=bridge
"$MODE_SCRIPT" start 2>/dev/null
printf 'SESSION1_VLAN=1410\n' >"$ONU_ROOT/run/onu.conf"
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
: >"$LOG"
"$MODE_SCRIPT" reload 2>/dev/null
check 'logged "^killall -USR2 omcid"'
check '[ "$(cat "$ONU_ROOT/run/omcid/applied")" = bridge ]'
# The sleep of a second after the signal is gone: the stand-in sleep, which
# returns at once for a whole second, would show it.
check '! logged "^sleep 1"'
check '! logged "omcid did not say"'
# An omcid that says nothing: the wait ends, says so, and the change goes on.
printf '#!/bin/sh\necho "killall $*" >>"%s"\n' "$LOG" >"$T/bin/killall"
rm -f "$ONU_ROOT/run/omcid/applied"
printf 'SESSION1_VLAN=1410\nSESSION2_VLAN=410\n' >"$ONU_ROOT/run/onu.conf"
echo "user/ipv6 secret" >"$ONU_ROOT/run/onu-pppoe2"
: >"$LOG"
APPLIED_TENTHS=3 "$MODE_SCRIPT" reload 2>/dev/null
check 'logged "^killall -USR2 omcid"'
check 'logged "omcid did not say that it is in bridge mode"'
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans" | tr "\n" " ")" = "410 1410 " ]'
teardown

echo "the state for the status daemon is lines name=value, and leaves out what is not there"
setup
mkdir -p "$ONU_ROOT/usr/lib/onu" "$ONU_ROOT/run/session/1" "$ONU_ROOT/run/session/3" \
	"$ONU_ROOT/run/onu-trial.d"
ln -s "$HERE/scripts/onu-change" "$ONU_ROOT/usr/lib/onu/onu-change"
check '[ -z "$("$MODE_SCRIPT" state)" ]'
echo passthrough >"$ONU_ROOT/run/onu-mode"
echo "198.51.100.53 198.51.100.54" >"$ONU_ROOT/run/onu-dns"
echo saved >"$ONU_ROOT/run/onu-store"
echo wan410 >"$ONU_ROOT/run/session/1/dev"
echo gem1 >"$ONU_ROOT/run/session/1/gem"
printf 'ipv4 ipv6' >"$ONU_ROOT/run/session/1/hands"
: >"$ONU_ROOT/run/session/1/lan"
echo lan410 >"$ONU_ROOT/run/session/3/lan"
echo 192.0.2.1 >"$ONU_ROOT/run/session/3/gateway"
got=$("$MODE_SCRIPT" state)
check 'echo "$got" | grep -qx "mode=passthrough"'
check 'echo "$got" | grep -qx "dns=198.51.100.53 198.51.100.54"'
check 'echo "$got" | grep -qx "store=saved"'
check 'echo "$got" | grep -qx "session.1.dev=wan410"'
check 'echo "$got" | grep -qx "session.1.gem=gem1"'
# A file without a final newline is read, and an empty one is left out.
check 'echo "$got" | grep -qx "session.1.hands=ipv4 ipv6"'
check '! echo "$got" | grep -q "^session.1.lan"'
check 'echo "$got" | grep -qx "session.3.lan=lan410"'
check 'echo "$got" | grep -qx "session.3.gateway=192.0.2.1"'
check '! echo "$got" | grep -q "^session.2\|^pass\|^trial"'
# The address handed on is told once it is there, with where.
echo 192.0.2.2 >"$ONU_ROOT/run/onu-pass"
echo lan >"$ONU_ROOT/run/onu-lan"
echo 192.0.2.1 >"$ONU_ROOT/run/onu-pass-proxy"
got=$("$MODE_SCRIPT" state)
check 'echo "$got" | grep -qx "pass.address=192.0.2.2"'
check 'echo "$got" | grep -qx "pass.lan=lan"'
check 'echo "$got" | grep -qx "pass.peer_gateway=192.0.2.1"'
# The changes on trial are onu-change's: when each goes back, and the pair's address.
printf 'UNTIL=1264\nTOKEN=99\n' >"$ONU_ROOT/run/onu-trial.d/settings"
printf 'UNTIL=1300\nADDRESS=198.51.100.2/30\n' >"$ONU_ROOT/run/onu-trial.d/mgmt"
printf 'UNTIL=1\n' >"$ONU_ROOT/run/onu-trial.d/settings.new"
got=$("$MODE_SCRIPT" state)
check 'echo "$got" | grep -qx "trial.settings=1264"'
check 'echo "$got" | grep -qx "trial.mgmt=1300"'
check 'echo "$got" | grep -qx "trial.mgmt.address=198.51.100.2/30"'
check '! echo "$got" | grep -q "trial.lane\|settings.new"'
# It reads, and takes no lock: a change that holds it does not hold this up.
ln -s 1 "$ONU_ROOT/run/onu-apply.lock"
check '"$MODE_SCRIPT" state >/dev/null'
rm -f "$ONU_ROOT/run/onu-apply.lock"
teardown

echo "a connection with its address typed in, and a network out by it"
setup
conf IPOE1_KIND=static IPOE1_GEM=auto IPOE1_VLAN=420 IPOE1_MAC= IPOE1_FAMILY=ipv4 \
	IPOE1_ADDRESS=192.0.2.10/24 IPOE1_GATEWAY=192.0.2.1 \
	"IPOE1_DNS=198.51.100.53 2001:db8::53" IPOE1_ADDRESS6= IPOE1_GATEWAY6= IPOE1_PREFIX6= \
	LAN1_CONN=ipoe:1 LAN1_VLAN=100 LAN1_ADDRESS=192.168.1.1/24 LAN1_DHCP=on LAN1_LEASE=3600
"$MODE_SCRIPT" start 2>/dev/null
# No account, and yet the stick holds the connection's VLAN itself.
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = passthrough ]'
check '[ "$(cat "$ONU_ROOT/run/onu-held-vlans")" = 420 ]'
check 'logged "onu-nl link add vlan gem256 wan420 420" && logged "onu-nl link set wan420 mtu 1500"'
check 'logged "onu-nl link set wan420 address 02:00:5e:00:53:01"'
check 'has ipoe-1 "interface ipoe1: device=wan420 proto=static ipaddr=192.0.2.10/24"'
check 'has ipoe-1 "route: interface=ipoe1 target=192.0.2.0/24 table=111"'
check 'has ipoe-1 "route: interface=ipoe1 target=0.0.0.0/0 gateway=192.0.2.1 table=111"'
check 'has ipoe-1 "rule: src=192.0.2.10/32 lookup=111"'
# No session hands an address down: the network's connection is the way out.
check 'has ipoe-1 "route: interface=ipoe1 target=0.0.0.0/0 gateway=192.0.2.1"'
check 'has nets "rule: src=192.168.1.0/24 lookup=111"'
check 'has nets "route: interface=net1 target=192.168.1.0/24 table=111"'
check '! part nets | grep -q "interface=ses"'
U=$ONU_ROOT/run/udhcpd-net1.conf
check 'grep -qx "opt dns 198.51.100.53" "$U"'
check 'grep -qx "lan net1 192.168.1.0/24" "$T/fw.conf" && grep -qx "wan gem256" "$T/fw.conf" &&
	grep -qx "out wan420" "$T/fw.conf" && ! grep -qF "out ppp*" "$T/fw.conf"'
# The operator's side is kept from the host side's networks.
check 'logged "onu-nl clsact add wan420" && logged "onu-nl filter add wan420 pref 1 dst 169.254.0.0/16 drop"'
"$MODE_SCRIPT" state >"$T/state" 2>/dev/null
check 'grep -qx "ipoe.1.dev=wan420" "$T/state" && grep -qx "ipoe.1.gem=gem256" "$T/state" &&
	grep -qx "ipoe.1.ipv4=192.0.2.10/24" "$T/state" && grep -qx "ipoe.1.router=192.0.2.1" "$T/state" &&
	grep -qx "ipoe.1.dns=198.51.100.53 2001:db8::53" "$T/state" && ! grep -q "ipoe.1.lease" "$T/state"'
# A sync with nothing changed gives netifd nothing again.
: >"$LOG"
"$MODE_SCRIPT" sync 2>/dev/null
check '! logged "onu-net set ipoe-1" && ! logged "onu-fw"'

echo "the connection's address from DHCP"
# The client gets its lease a moment after it starts, as the real one does.
cat >"$T/bin/udhcpc" <<-EOF
#!/bin/sh
echo "udhcpc \$*" >>"\$LOG"
while [ \$# -gt 0 ]; do
	case \$1 in
	-i) dev=\$2; shift ;;
	-s) script=\$2; shift ;;
	esac
	shift
done
(
	interface=\$dev "\$script" deconfig
	"$REAL_SLEEP" 0.3
	interface=\$dev ip=198.51.100.77 mask=24 router="198.51.100.1 198.51.100.2" \\
		dns="198.51.100.53  198.51.100.54" lease=86400 "\$script" bound
) >/dev/null 2>&1 &
EOF
printf '#!/bin/sh\necho "onu-call $*" >>"$LOG"\n' >"$ONU_ROOT/usr/lib/onu/onu-call"
mkdir -p "$ONU_ROOT/usr/lib/onu"
ln -s "$HERE/scripts/onu-dhcpc" "$ONU_ROOT/usr/lib/onu/onu-dhcpc"
chmod +x "$T/bin/udhcpc" "$ONU_ROOT/usr/lib/onu/onu-call"
echo "1000.25 99.0" >"$ONU_ROOT/proc/uptime"
printf '%s\n' IPOE1_KIND=dhcp IPOE1_GEM=auto IPOE1_VLAN=420 IPOE1_FAMILY=ipv4 \
	LAN1_CONN=ipoe:1 LAN1_VLAN=100 LAN1_ADDRESS=192.168.1.1/24 LAN1_DHCP=on \
	LAN1_LEASE=3600 >"$ONU_ROOT/run/onu.conf"
: >"$LOG"
"$MODE_SCRIPT" reload 2>/dev/null
check 'logged "connection 1 down" && logged "procd start udhcpc-ipoe1 .*onu-drop -c net_raw,net_bind_service dhcpc udhcpc -f -R -i wan420 -s .*/usr/lib/onu/onu-dhcpc"'
check 'logged "^udhcpc -f -R -i wan420"'
# Until the lease comes, the interface is up without an address.
check 'has ipoe-1 "interface ipoe1: device=wan420 proto=none"'
check '! part ipoe-1 | grep -q route'
for _ in 1 2 3 4 5 6 7 8 9 10; do
	logged "onu-call dhcpc" && break
	"$REAL_SLEEP" 0.1
done
check 'logged "onu-call dhcpc"'
check '[ "$(cat "$ONU_ROOT/run/ipoe/1/ipv4")" = 198.51.100.77/24 ] &&
	[ "$(cat "$ONU_ROOT/run/ipoe/1/router")" = 198.51.100.1 ] &&
	[ "$(cat "$ONU_ROOT/run/ipoe/1/dns")" = "198.51.100.53 198.51.100.54" ] &&
	[ "$(cat "$ONU_ROOT/run/ipoe/1/until")" = 87400 ]'
# onu-act runs onu-apply for the call: the lease is put in place.
"$MODE_SCRIPT" dhcpc 2>/dev/null
check 'has ipoe-1 "interface ipoe1: device=wan420 proto=static ipaddr=198.51.100.77/24"'
check 'has ipoe-1 "route: interface=ipoe1 target=0.0.0.0/0 gateway=198.51.100.1 table=111"'
check 'has ipoe-1 "route: interface=ipoe1 target=0.0.0.0/0 gateway=198.51.100.1"'
check 'grep -qx "opt dns 198.51.100.53 198.51.100.54" "$U"'
echo "1100.75 99.0" >"$ONU_ROOT/proc/uptime"
"$MODE_SCRIPT" state >"$T/state" 2>/dev/null
check 'grep -qx "ipoe.1.ipv4=198.51.100.77/24" "$T/state" && grep -qx "ipoe.1.lease=86300" "$T/state"'
# A lease renewed as it was moves its end, and tells no one.
: >"$LOG"
interface=wan420 ip=198.51.100.77 mask=24 router=198.51.100.1 dns="198.51.100.53 198.51.100.54" \
	lease=600 "$HERE/scripts/onu-dhcpc" renew
check '! logged "onu-call" && [ "$(cat "$ONU_ROOT/run/ipoe/1/until")" = 1700 ]'
# The lease lost: the address goes.
interface=wan420 "$HERE/scripts/onu-dhcpc" deconfig
check 'logged "onu-call dhcpc" && [ ! -e "$ONU_ROOT/run/ipoe/1/ipv4" ]'
"$MODE_SCRIPT" dhcpc 2>/dev/null
check 'has ipoe-1 "interface ipoe1: device=wan420 proto=none"'
# An interface that is no connection's is left alone.
: >"$LOG"
interface=wan410 ip=198.51.100.9 mask=24 "$HERE/scripts/onu-dhcpc" bound
check '! logged "onu-call"'

echo "a session beside the connection in its VLAN"
echo "user secret" >"$ONU_ROOT/run/onu-pppoe1"
printf '%s\n' IPOE1_KIND=dhcp IPOE1_GEM=auto IPOE1_VLAN=420 IPOE1_FAMILY=ipv4 \
	SESSION1_VLAN=420 SESSION1_FAMILY=ipv4 SESSION1_PASS=off >"$ONU_ROOT/run/onu.conf"
: >"$LOG"
"$MODE_SCRIPT" reload 2>/dev/null
# They share the interface, which stays at Ethernet's MTU: the session keeps to 1492.
check 'has ses1 "interface ses1: device=wan420 proto=pppoed session=1 family=ipv4 pass=0 defaultroute=0"'
check '! logged "link set wan420 mtu 1508"'
check 'grep -qx "out wan420" "$T/fw.conf" || [ ! -e "$ONU_ROOT/run/onu-fw.conf" ]'
"$MODE_SCRIPT" state >"$T/state" 2>/dev/null
check 'grep -qx "session.1.dev=wan420" "$T/state" && grep -qx "ipoe.1.dev=wan420" "$T/state"'

echo "the connection gone"
printf '%s\n' SESSION1_VLAN=420 SESSION1_FAMILY=ipv4 SESSION1_PASS=off >"$ONU_ROOT/run/onu.conf"
: >"$LOG"
"$MODE_SCRIPT" reload 2>/dev/null
check 'logged "procd stop udhcpc-ipoe1" && logged "onu-net del ipoe-1" && [ ! -d "$ONU_ROOT/run/ipoe/1" ]'
# The session comes up again, with its room for PPPoE.
check 'logged "session 1 down" && logged "link set wan420 mtu 1508"'
rm -f "$ONU_ROOT/run/onu-pppoe1"
: >"$ONU_ROOT/run/onu.conf"
"$MODE_SCRIPT" reload 2>/dev/null
check '[ "$(cat "$ONU_ROOT/run/onu-mode")" = bridge ] && [ ! -e "$ONU_ROOT/run/network.d/ipoe-1" ]'
teardown

echo "a connection with an address alone, its gateway on the link"
setup
conf IPOE1_KIND=static IPOE1_GEM=auto IPOE1_VLAN=420 IPOE1_MAC= IPOE1_FAMILY=ipv4 \
	IPOE1_ADDRESS=203.0.113.7/32 IPOE1_GATEWAY=198.51.100.1 IPOE1_DNS=198.51.100.53 \
	IPOE1_ADDRESS6= IPOE1_GATEWAY6= IPOE1_PREFIX6= \
	LAN1_CONN=ipoe:1 LAN1_VLAN=100 LAN1_ADDRESS=192.168.1.1/24 LAN1_DHCP=on LAN1_LEASE=3600
"$MODE_SCRIPT" start 2>/dev/null
check 'has ipoe-1 "interface ipoe1: device=wan420 proto=static ipaddr=203.0.113.7/32"'
# No network to route to: the gateway alone, on the link, ahead of the default route.
check '! part ipoe-1 | grep -q "target=203.0.113"'
check 'has ipoe-1 "route: interface=ipoe1 target=198.51.100.1/32 table=111"'
check 'has ipoe-1 "route: interface=ipoe1 target=0.0.0.0/0 gateway=198.51.100.1 onlink=1 table=111"'
check 'has ipoe-1 "rule: src=203.0.113.7/32 lookup=111"'
# The stick's own default route too, by the network's connection.
check 'has ipoe-1 "route: interface=ipoe1 target=198.51.100.1/32"'
check 'has ipoe-1 "route: interface=ipoe1 target=0.0.0.0/0 gateway=198.51.100.1 onlink=1"'
check '[ "$(part ipoe-1 | grep -n "target=198.51.100.1/32 table" | cut -d: -f1)" -lt \
	"$(part ipoe-1 | grep -n "gateway=198.51.100.1 onlink=1 table" | cut -d: -f1)" ] &&
	[ "$(part ipoe-1 | grep -nx "route: interface=ipoe1 target=198.51.100.1/32" | cut -d: -f1)" -lt \
	"$(part ipoe-1 | grep -nx "route: interface=ipoe1 target=0.0.0.0/0 gateway=198.51.100.1 onlink=1" |
		cut -d: -f1)" ]'
check 'has nets "rule: src=192.168.1.0/24 lookup=111"'

echo "a connection of a /31"
# The settings of a /31 connection with the gateway $1, and a network out by it.
conf31() {
	printf '%s\n' IPOE1_KIND=static IPOE1_GEM=auto IPOE1_VLAN=420 IPOE1_FAMILY=ipv4 \
		IPOE1_ADDRESS=192.0.2.10/31 "IPOE1_GATEWAY=$1" \
		LAN1_CONN=ipoe:1 LAN1_VLAN=100 LAN1_ADDRESS=192.168.1.1/24 LAN1_DHCP=on \
		LAN1_LEASE=3600 >"$ONU_ROOT/run/onu.conf"
}
# The gateway the other address: in its network, as a /24's.
conf31 192.0.2.11
"$MODE_SCRIPT" reload 2>/dev/null
check 'has ipoe-1 "interface ipoe1: device=wan420 proto=static ipaddr=192.0.2.10/31"'
check 'has ipoe-1 "route: interface=ipoe1 target=192.0.2.10/31 table=111"'
check 'has ipoe-1 "route: interface=ipoe1 target=0.0.0.0/0 gateway=192.0.2.11 table=111"'
check '! part ipoe-1 | grep -q "onlink\|target=192.0.2.11/32"'
# A gateway outside it: on the link.
conf31 198.51.100.1
"$MODE_SCRIPT" reload 2>/dev/null
check 'has ipoe-1 "route: interface=ipoe1 target=192.0.2.10/31 table=111"'
check 'has ipoe-1 "route: interface=ipoe1 target=198.51.100.1/32 table=111"'
check 'has ipoe-1 "route: interface=ipoe1 target=0.0.0.0/0 gateway=198.51.100.1 onlink=1 table=111"'

echo "a lease of an address alone"
# A DHCP server that names no network gives the address alone (onu-dhcpc).
printf '%s\n' IPOE1_KIND=dhcp IPOE1_GEM=auto IPOE1_VLAN=420 IPOE1_FAMILY=ipv4 \
	LAN1_CONN=ipoe:1 LAN1_VLAN=100 LAN1_ADDRESS=192.168.1.1/24 LAN1_DHCP=on \
	LAN1_LEASE=3600 >"$ONU_ROOT/run/onu.conf"
printf '#!/bin/sh\necho "udhcpc $*" >>"$LOG"\n' >"$T/bin/udhcpc"
mkdir -p "$ONU_ROOT/usr/lib/onu"
printf '#!/bin/sh\necho "onu-call $*" >>"$LOG"\n' >"$ONU_ROOT/usr/lib/onu/onu-call"
chmod +x "$T/bin/udhcpc" "$ONU_ROOT/usr/lib/onu/onu-call"
echo "1000.25 99.0" >"$ONU_ROOT/proc/uptime"
"$MODE_SCRIPT" reload 2>/dev/null
interface=wan420 ip=203.0.113.9 router=198.51.100.1 lease=600 "$HERE/scripts/onu-dhcpc" bound
"$MODE_SCRIPT" dhcpc 2>/dev/null
check 'has ipoe-1 "interface ipoe1: device=wan420 proto=static ipaddr=203.0.113.9/32"'
check 'has ipoe-1 "route: interface=ipoe1 target=198.51.100.1/32 table=111"'
check 'has ipoe-1 "route: interface=ipoe1 target=0.0.0.0/0 gateway=198.51.100.1 onlink=1 table=111"'
check '! part ipoe-1 | grep -q "target=203.0.113"'
teardown

echo "test_onu_apply: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
