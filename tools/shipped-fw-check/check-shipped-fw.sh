#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# check-shipped-fw.sh: a quick, read-only check that a DFP-34X-2C stick which runs its shipped
# firmware is fit for an update: the laser and the optics, the processor, the memory, the flash
# banks and the boot environment. It writes nothing and changes nothing.
#
# Run it in the shipped firmware's shell, over ssh or telnet (both want the shipped firmware's own
# password) or on the serial console:
#
#     sh check-shipped-fw.sh
#     ssh root@192.0.2.1 'sh -s' < check-shipped-fw.sh
#
# Each line is "PASS", "WARN" or "FAIL", a name and the figures. The exit status is 0 when nothing
# needs a look, 1 for warnings, 2 for a failure and 3 when the stick does not run the shipped
# firmware. It needs only what every build of the shipped firmware has: busybox 1.12.4's ash, awk,
# sed, cut, grep, md5sum, sleep and the tools nv and diag.

P=0
W=0
F=0

say() {
	case "$1" in
	PASS) P=$((P + 1)) ;;
	WARN) W=$((W + 1)) ;;
	FAIL) F=$((F + 1)) ;;
	esac
	echo "$1 $2: $3"
}

# within VALUE LOW HIGH: whether LOW <= VALUE <= HIGH, as numbers.
within() {
	awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(v + 0 >= lo && v + 0 <= hi) }'
}

first() {
	awk 'NR == 1 { print; exit }'
}

# judge NAME VALUE UNIT PASS-LOW PASS-HIGH WARN-LOW WARN-HIGH: PASS inside the first range, WARN
# inside the wider one, FAIL outside both or without a value.
judge() {
	if [ -z "$2" ]; then
		say FAIL "$1" "no reading"
	elif within "$2" "$4" "$5"; then
		say PASS "$1" "$2 $3"
	elif within "$2" "$6" "$7"; then
		say WARN "$1" "$2 $3, expected $4 to $5"
	else
		say FAIL "$1" "$2 $3, expected $4 to $5"
	fi
}

type diag >/dev/null 2>&1 || {
	echo "FAIL firmware: no diag tool, this is not the shipped firmware"
	exit 3
}

# What runs.
ver=
[ -r /etc/version ] && ver=$(first < /etc/version)
echo "INFO firmware: ${ver:-unknown}"
echo "INFO uptime: $(cut -d' ' -f1 /proc/uptime) s"

# The bank that runs is the one whose kernel and root filesystem partitions the kernel's command
# line marks read-only. The shipped firmware's page writes the bank "1 - sw_active", so sw_active
# has to be the bank that runs, or an update would write over the running system.
run=$(sed -n 's/.*(k\([01]\))ro.*/\1/p' /proc/cmdline)
envs=$(nv getenv sw_commit 2>&1)
if echo "$envs" | grep -q "Bad CRC"; then
	say FAIL env "the boot environment has a bad CRC"
else
	say PASS env "boot environment reads"
fi
getv() { nv getenv "$1" 2>/dev/null | sed 's/^[^=]*=//'; }
commit=$(getv sw_commit)
active=$(getv sw_active)
tryact=$(getv sw_tryactive)
echo "INFO banks: runs ${run:-?}, committed ${commit:-?}, active ${active:-?}, trial ${tryact:-?}"
if [ -z "$run" ] || [ -z "$active" ]; then
	say FAIL bank "cannot tell the running bank from the boot environment"
elif [ "$run" != "$active" ]; then
	say FAIL bank "bank $run runs but sw_active is $active: an update would write the running bank"
else
	say PASS bank "sw_active is the running bank $run, an update writes bank $((1 - run))"
fi
if [ "$tryact" = 2 ]; then
	say PASS trial "no trial boot is pending"
else
	say WARN trial "sw_tryactive is ${tryact:-unset}, 2 means no trial is pending"
fi

# The flash: the shipped firmware's split of the partitions, both banks against the known builds,
# the boot loader, the partition for the settings.
mtdn() { sed -n "s/^mtd\([0-9]*\):.*\"$1\"/\1/p" /proc/mtd; }
if [ "$(grep -c ' 0014c000 .*"k[01]"' /proc/mtd)" = 2 ] &&
	[ "$(grep -c ' 00274000 .*"r[01]"' /proc/mtd)" = 2 ]; then
	say PASS split "kernel partitions 1328K, root filesystem partitions 2512K"
else
	say FAIL split "the partitions are not those of the shipped firmware"
fi
KNOWN="
27e1ad0c8bcbf48515edaa5a6942d4ac 85a8fda09b98603b596f2fce25e18c18 M110_sfp_ODI_210702
b3d4c9148509200d6e976e1893d3c695 08ed9663ca236934633be2b7c0604b93 M110_sfp_ODI_220304
c089cb438c9df61dbb8979e98aa46d6e b26e71f2302b09cb5273a2e3162f5251 M110_sfp_ODI_220817
6fd79714100cfb5b77863577f5041be3 0e2593d0b4364c0952119fa61ad7a76b M110_sfp_ODI_220923
de1c7591901d91f623d0bc9a73ae3283 ab9642e34990b027443b2668b5073fa0 M114_sfp_ODI_Vlan_220414
00b40a4a39eab182881caf58512f6465 53072c87a74010f0c818b9ac14a24493 M114_sfp_ODI_hybrid_220527
c3f5e10a678f0689d23f4fea6e2a8635 b92abf383f331cd60bfdf51a5feb0b9b M114_sfp_ODI_hybrid_220916
de49ea14b0c5f678e21a9cfc46c4bd90 206a4796273ff655ec71e654caafcf66 M114_sfp_ODI_hybrid_221209
abe90fdf6e70ea298c434c1d1ad20e64 fb65cedbb5cc178832d85055790959c8 open-dfp-34x-2c_2026.10.22-1523
"
for b in 0 1; do
	k=$(mtdn k$b)
	r=$(mtdn r$b)
	if [ -z "$k" ] || [ -z "$r" ]; then
		say FAIL "bank$b" "no partitions k$b and r$b"
		continue
	fi
	mk=$(md5sum /dev/mtd$k | cut -d' ' -f1)
	mr=$(md5sum /dev/mtd$r | cut -d' ' -f1)
	name=$(echo "$KNOWN" | awk -v k="$mk" -v r="$mr" '$1 == k && $2 == r { print $3; exit }')
	if [ -n "$name" ]; then
		say PASS "bank$b" "$name"
	else
		say WARN "bank$b" "not a known build: kernel $mk, root filesystem $mr"
	fi
done
loader=$(md5sum /dev/mtd$(mtdn boot) | cut -d' ' -f1)
echo "INFO loader: md5 $loader"
cfgfree=$(df /var/config 2>/dev/null | awk 'NR == 2 { print $4 }')
if mount | grep -q 'mtdblock3.*jffs2'; then
	if [ "${cfgfree:-0}" -ge 40 ]; then
		say PASS config "the settings partition is mounted, ${cfgfree}K free"
	else
		say WARN config "the settings partition has ${cfgfree:-0}K free"
	fi
else
	say FAIL config "the settings partition is not mounted"
fi

# The processor and the memory. An update goes through the memory first: the page keeps the file
# of about 3.4 MB in RAM while it reads it.
load=$(cut -d' ' -f1 /proc/loadavg)
a=$(awk '/^cpu / { print $2 + $3 + $4 + $7 + $8, $5 + $6 }' /proc/stat)
sleep 2
b=$(awk '/^cpu / { print $2 + $3 + $4 + $7 + $8, $5 + $6 }' /proc/stat)
idle=$(echo "$a $b" | awk '{ t = ($3 - $1) + ($4 - $2); if (t > 0) printf "%d", 100 * ($4 - $2) / t }')
judge cpu-idle "${idle:-}" % 50 100 10 100
judge cpu-load "$load" "" 0 2.5 0 6
free=$(awk '/^MemFree/ { f = $2 } /^Cached/ { c = $2 } END { print f + c }' /proc/meminfo)
judge memory "$free" "kB free or cached" 8000 1000000 4000 1000000

# The optics. At O5 the laser works in bursts and the readings mean something; before it only
# the module's temperature and supply do. Ranges: the module's temperature 0 to 70 C is what the
# model is sold for and 85 C is the part's limit; the supply 3.3 V +-5 % is the SFP's; the bias
# current of a working laser is some mA to some tens; the transmit power of class B+ is 0.5 to
# 5 dBm; the receive power between the sensitivity, -28 dBm, and the overload, -8 dBm.
dg() { diag pon get transceiver "$1" 2>&1; }
raw=$(diag gpon get onu-state 2>&1)
state=$(echo "$raw" | sed -n 's/.*(O\([0-9]\)).*/\1/p' | first)
if [ "$state" = 5 ]; then
	say PASS onu-state "O5, in operation"
elif [ -n "$state" ]; then
	say WARN onu-state "O$state: not in operation, so the burst readings below do not count"
else
	why=$(echo "$raw" | sed -n 's/^Error[^:]*: *//p' | first)
	say WARN onu-state "no GPON state (${why:-no answer from diag}); the burst readings below do not count"
fi
temp=$(dg temperature | sed -n 's/^Temperature: *\([-0-9.]*\) C.*/\1/p' | first)
volt=$(dg voltage | sed -n 's/^Voltage: *\([-0-9.]*\) V.*/\1/p' | first)
bias=$(dg bias-current | sed -n 's/^Bias Current: *\([-0-9.]*\) mA.*/\1/p' | first)
txp=$(dg tx-power | sed -n 's/^Tx Power: *\([-0-9.]*\) *dBm.*/\1/p' | first)
rxp=$(dg rx-power | sed -n 's/^Rx Power: *\([-0-9.]*\) *dBm.*/\1/p' | first)
judge temperature "$temp" C -10 75 -40 90
judge supply "$volt" V 3.135 3.465 3.0 3.6
if [ "$state" = 5 ]; then
	judge bias-current "$bias" mA 2 60 0.5 80
	judge tx-power "$txp" dBm 0 5.5 -1 7
	judge rx-power "$rxp" dBm -28 -8 -31 -3
else
	echo "INFO optics: bias ${bias:-?} mA, tx ${txp:-none} dBm, rx ${rxp:-none} dBm (not judged below O5)"
fi
part=$(dg part-number | sed -n 's/^Part Number: *//p' | first)
echo "INFO module: ${part:-unknown}"

echo "SUMMARY: $P pass, $W warn, $F fail"
if [ "$F" -gt 0 ]; then
	echo "VERDICT: NOT FIT for an update"
	exit 2
elif [ "$W" -gt 0 ]; then
	echo "VERDICT: FIT, but read the warnings"
	exit 1
fi
echo "VERDICT: FIT for an update"
exit 0
