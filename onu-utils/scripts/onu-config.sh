#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# onu-config.sh: the files the running system reads, to and from the stick's
# configuration store, which onu-config keeps: a file of the JFFS2 file system
# of the flash partition "config", which "onu-config load" mounts on /config,
# beside the files of the firmware the stick shipped with (ADR 0016).
# onu-config runs it for these verbs, and is how a person or a program asks
# for them (ADR 0012):
#
#   onu-config load      at boot: /config mounted, and the identity, the
#                        settings and the PPPoE accounts from the store into
#                        /run/onu-identity, /run/onu.conf and /run/onu-pppoeN;
#                        at the first start, root's password from the shipped
#                        firmware's administrator's
#   onu-config save      the settings of /run/onu.conf and the accounts of
#                        /run/onu-pppoeN into the store, in one write
#   onu-config identity  the identity of the KEY=VALUE lines of the standard
#                        input (SERIAL, PLOAM, ...) into the store, over the
#                        shipped firmware's, and into /run/onu-identity
#   onu-config password  root's new password, the line of the standard input,
#                        hashed into the store, /run/onu-auth and /run/shadow
#   onu-config auth HASH a hash of root's password made elsewhere, the same way
#   onu-config system    the stick's own settings of the KEY=VALUE lines of the
#                        standard input (HOSTNAME, MGMT_VLAN, MGMT_ADDRESS,
#                        SFP_LANE, NTP_SERVER, LOG_SERVER, PRODUCT_NAME) into
#                        the store and into /run/onu-system; an empty value
#                        removes the key
#   onu-config pd N      the host session N delegates its IPv6 prefix to, the
#                        line dhcp6d keeps in /run/dhcp6d/pdN, into the store;
#                        without the file, out of it
#   onu-config tls       the panel's key: the one load found, or a new one,
#                        kept in the store, in /run/onu-tls-key; "tls new"
#                        makes a new one in its place, and the status daemon
#                        makes new certificates from it; "tls own" keeps the
#                        own certificate of the standard input's lines KEY=
#                        (its P-256 key, as the panel's) and CHAIN= (the
#                        certificate and the CA certificates after it, DER
#                        one after another, in base64), which the status
#                        daemon then serves in place of its own, in
#                        /run/onu-tls-own, and "tls self-signed" takes it
#                        away; "tls install" hands the key and the own
#                        certificate to the status daemon (the boot script,
#                        and each change)
#   onu-config ssh       the ssh server's host key: the one load found, or a
#                        new one, kept in the store, in /run/dropbear/host_key;
#                        "ssh new" makes a new one in its place and ends the
#                        server, which procd starts again with it, and prints
#                        it as "ssh show" does: its TYPE= and FINGERPRINT=,
#                        which "ssh show" follows with the public keys root
#                        logs in with (onu-config ssh-keys)
#   onu-config token     the token of scripts, which the status daemon takes in
#                        place of a sign-in: printed, if there is one;
#                        "token new" makes a new one, in place of the old,
#                        kept in the store, and prints it; "token off" takes
#                        it away; "token install" hands the one in force to the
#                        status daemon (the boot script, after it made the
#                        daemon's directory, and each change of the token)
#
# The keys of the store, and the file of /run that a load puts each in, are
# listed in onu-utils/README.rst, "onu-config (the files)"; every key that no
# row of that table claims is a line of /run/onu.conf. The files of secrets are
# for root only.
#
# The identity is the one ponid reads from the shipped firmware's files, which
# share the file system, with the store's ID_ keys over it: the store holds
# only what was changed here, and the shipped firmware's files are never
# written, so that the shipped firmware finds its own after a switch of the
# bank.
#
# What became of the settings is the word in /run/onu-store, for the status
# daemon: "loaded" when load found settings in the store, "defaults" when it
# found none, "saved" or "failed" after a save, "none" without a store, when
# /config is not mounted or the store cannot be read.
#
# ONU_ROOT names another root for the files it reads and writes, and
# ONU_STORE another store (a file), which the tests use.

set -u

ROOT=${ONU_ROOT:-}
RUN=$ROOT/run
# ponid is one of the helpers out of a person's way (ADR 0012).
PATH=$PATH:$ROOT/usr/lib/onu
CONF=$RUN/onu.conf
CREDS=$RUN/onu-pppoe
PD=$RUN/dhcp6d/pd
MAX_SESSIONS=7
IDENTITY=$RUN/onu-identity
AUTH=$RUN/onu-auth
SYSTEM=$RUN/onu-system
TLS=$RUN/onu-tls-key
TLS_OWN=$RUN/onu-tls-own
SSHKEY=$RUN/onu-ssh-key
HOSTKEY=$RUN/dropbear/host_key
TOKEN=$RUN/onu-pond-token
POND_DIR=$RUN/pond
SOURCE=$RUN/onu-identity-source
STATE=$RUN/onu-store
SHADOW=$RUN/shadow
IMAGE_SHADOW=$ROOT/etc/shadow.image

# Root's password is kept as a SHA-256 crypt hash ("$5$"), which crypt() of
# the C library checks for login and the ssh server, with ROUNDS rounds: a
# check takes about 0.5 s on the stick.
ROUNDS=20000
# Until a password is set, root's password is "admin", the same on every
# unit; so is its hash, made once with a fixed salt.
DEFAULT_HASH='$5$rounds=20000$rootdefaultadmin$v44Onkx4q.cie3KOpV7pUy4Ri7vI57rVX7WcFAofPT9'

state() {
	(umask 022 && echo "$1" >"$STATE")
}

log() {
	echo "onu-config: $*" >&2
	logger -t onu-config "$*" 2>/dev/null
	return 0
}

# The store: ONU_STORE, else the file of /config, when the partition is
# mounted there.
STORE_FILE=/config/open-dfp-34x-2c.conf
store() {
	if [ -n "${ONU_STORE:-}" ]; then
		echo "$ONU_STORE"
		return 0
	fi
	mountpoint -q "$ROOT/config" || return 1
	echo "$ROOT$STORE_FILE"
}

# The partition "config" mounted on /config, read-only: onu-config mounts it
# read-write for each change of the store alone. One that still holds the
# two-slot store of older builds, which took the whole partition, has it taken
# into the file first: its lines are kept in memory, the partition is erased
# and mounted empty, and the lines go to the file, since JFFS2 would erase the
# blocks it does not know. Fails when the partition cannot be mounted.
mount_config() {
	mountpoint -q "$ROOT/config" && return 0
	dev=$(awk '$4 == "\"config\"" { sub(":", "", $1); print $1 }' \
		"$ROOT/proc/mtd" 2>/dev/null)
	[ -n "$dev" ] || return 1
	before=$RUN/onu-store.before
	(umask 077 && onu-config slots "$ROOT/dev/$dev" >"$before")
	case $? in
	0)
		log "the store of before goes into $STORE_FILE"
		onu-config erase-slots "$ROOT/dev/$dev" &&
			mount -t jffs2 -o ro,nosuid,nodev,noexec mtd:config "$ROOT/config" &&
			onu-config --store "$ROOT$STORE_FILE" import <"$before" || {
			log "the store of before is in $before only"
			return 1
		}
		rm -f "$before"
		;;
	3)
		rm -f "$before"
		mount -t jffs2 -o ro,nosuid,nodev,noexec mtd:config "$ROOT/config"
		;;
	*)
		rm -f "$before"
		return 1
		;;
	esac
}

# The store's KEY=VALUE lines; nothing for an empty store or none.
contents() {
	[ -n "$STORE" ] || return 0
	onu-config --store "$STORE" list
}

# /run/onu-identity: the identity ponid reads from the shipped firmware's
# files, with the ID_ keys of the store's lines $1 over it, and where it came
# from in /run/onu-identity-source: "store" when the store holds one of them,
# else "factory". Fails when there is none at all.
put_identity() {
	umask 077
	{
		ponid 2>/dev/null
		printf '%s\n' "$1" | awk '/^ID_/ {
			i = index($0, "=")
			print tolower(substr($0, 4, i - 4)) "=" substr($0, i + 1)
		}'
	} | awk '{
		i = index($0, "=")
		if (i < 2)
			next
		k = substr($0, 1, i - 1)
		if (!(k in v))
			order[n++] = k
		v[k] = substr($0, i + 1)
	}
	END {
		for (j = 0; j < n; j++)
			print order[j] "=" v[order[j]]
	}' >"$IDENTITY.new" && mv "$IDENTITY.new" "$IDENTITY" || return 1
	if printf '%s\n' "$1" | grep -q '^ID_'; then
		(umask 022 && echo store >"$SOURCE")
	else
		(umask 022 && echo factory >"$SOURCE")
	fi
	[ -s "$IDENTITY" ]
}

# The settings and the accounts in force, as keys of the store, and a -KEY
# line for each setting the store holds and the settings in force no longer
# have, such as a key that a later version replaced: it would come back at
# boot.
# Without the file of the settings in force, none goes.
settings() {
	stored=$(contents 2>/dev/null)
	if [ -r "$CONF" ]; then
		grep '^[A-Z][A-Z0-9_]*=' "$CONF" |
			grep -v -e '^ID_' -e '^PPPOE[0-9]*_' -e '^AUTH_' -e '^SYS_' -e '^TLS_' \
				-e '^SSH_' -e '^POND_' -e '^PON_DISABLED='
		[ -s "$CONF" ] && printf '%s\n' "$stored" | awk -v conf="$CONF" '
			BEGIN {
				while ((getline line < conf) > 0) {
					i = index(line, "=")
					if (i > 1)
						kept[substr(line, 1, i - 1)] = 1
				}
			}
			{
				i = index($0, "=")
				if (i < 2)
					next
				k = substr($0, 1, i - 1)
			}
			k ~ /^(ID_|PPPOE|AUTH_|SYS_|TLS_|SSH_|POND_)/ || k ~ /^PD[1-9]_HOST$/ { next }
			k == "PON_DISABLED" { next }
			!(k in kept) { print "-" k }'
	fi
	n=1
	while [ $n -le $MAX_SESSIONS ]; do
		user=
		secret=
		[ -r "$CREDS$n" ] && read -r user secret <"$CREDS$n"
		# No account in force removes the stored one: it would come back
		# at boot.
		if [ -n "$user" ] || printf '%s\n' "$stored" | grep -q "^PPPOE${n}_USER=."; then
			echo "PPPOE${n}_USER=${user:-}"
			echo "PPPOE${n}_SECRET=${secret:-}"
		fi
		n=$((n + 1))
	done
	# The single account of before is the first session's now.
	if printf '%s\n' "$stored" | grep -q '^PPPOE_USER=.'; then
		echo "PPPOE_USER="
		echo "PPPOE_SECRET="
	fi
}

load() {
	known=$STORE
	list=$(contents) || {
		log "the store cannot be read"
		list=
		known=
	}
	umask 077
	rm -f "$CONF.new" "$CREDS"*.new "$AUTH.new" "$SYSTEM.new" "$PD"* "$TLS.new" \
		"$TLS_OWN.new" "$SSHKEY.new" "$TOKEN.new"
	printf '%s\n' "$list" | awk -v conf="$CONF.new" \
		-v creds="$CREDS" -v auth="$AUTH.new" -v max="$MAX_SESSIONS" \
		-v sys="$SYSTEM.new" -v pd="$PD" -v tls="$TLS.new" -v ssh="$SSHKEY.new" \
		-v tok="$TOKEN.new" -v own="$TLS_OWN.new" '
		{
			i = index($0, "=")
			if (i < 2)
				next
			k = substr($0, 1, i - 1)
			v = substr($0, i + 1)
		}
		k ~ /^ID_/ { next }
		# The OLT disabled the ONU: pon keep keeps it, and onu-apply
		# identity reads it from the store, for the PON driver.
		k == "PON_DISABLED" { next }
		k == "AUTH_HASH" { if (v != "") print v > auth; next }
		k == "TLS_KEY" { if (v != "") print v > tls; next }
		# The own certificate: its key, and its chain in pieces of
		# one value each, which go together again in their order.
		k == "TLS_OWN_KEY" { own_key = v; next }
		k ~ /^TLS_OWN_CHAIN[0-9]+$/ { chain[substr(k, 14) + 0] = v; next }
		k ~ /^TLS_/ { next }
		k == "SSH_HOST_KEY" { if (v != "") print v > ssh; next }
		# The keys root logs in with: onu-config ssh-keys install.
		k ~ /^SSH_/ { next }
		k == "POND_TOKEN" { if (v != "") print v > tok; next }
		k ~ /^SYS_/ { if (v != "") print substr(k, 5) "=" v > sys; next }
		k ~ /^PD[1-9]_HOST$/ {
			if (v != "")
				print v > (pd substr(k, 3, 1))
			next
		}
		k == "PPPOE_USER" { old_user = v; next }
		k == "PPPOE_SECRET" { old_secret = v; next }
		k ~ /^PPPOE[1-9]_USER$/ { user[substr(k, 6, 1)] = v; next }
		k ~ /^PPPOE[1-9]_SECRET$/ { secret[substr(k, 6, 1)] = v; next }
		{ print k "=" v > conf }
		END {
			if (user[1] == "" && old_user != "") {
				user[1] = old_user
				secret[1] = old_secret
			}
			for (n = 1; n <= max; n++)
				if (user[n] != "")
					print user[n] " " secret[n] > (creds n ".new")
			if (own_key != "" && chain[1] != "") {
				print own_key > own
				line = ""
				for (n = 1; n in chain; n++)
					line = line chain[n]
				print line > own
			}
		}'
	# dhcp6d runs as an account of its own, in a directory of its own: the
	# bindings it takes back after a restart are its to read and to replace.
	for f in "$PD"[1-9]; do
		[ -e "$f" ] || continue
		chown dhcp6d "$f" 2>/dev/null && chmod 644 "$f" ||
			log "dhcp6d cannot take its binding $f back"
	done
	if [ -e "$CONF.new" ]; then
		chmod 644 "$CONF.new" && mv "$CONF.new" "$CONF"
		state loaded
	else
		state defaults
	fi
	for new in "$CREDS"*.new; do
		[ -e "$new" ] && mv "$new" "${new%.new}"
	done
	if [ -z "$known" ]; then
		echo '!' >"$AUTH"
		rm -f "$AUTH.new"
	elif [ -e "$AUTH.new" ]; then
		mv "$AUTH.new" "$AUTH"
	else
		rm -f "$AUTH"
	fi
	if [ -e "$TLS.new" ]; then
		mv "$TLS.new" "$TLS"
	else
		rm -f "$TLS"
	fi
	if [ -e "$TLS_OWN.new" ]; then
		mv "$TLS_OWN.new" "$TLS_OWN"
	else
		rm -f "$TLS_OWN"
	fi
	if [ -e "$SSHKEY.new" ]; then
		mv "$SSHKEY.new" "$SSHKEY"
	else
		rm -f "$SSHKEY"
	fi
	if [ -e "$TOKEN.new" ]; then
		mv "$TOKEN.new" "$TOKEN"
	else
		rm -f "$TOKEN"
	fi
	[ -n "$known" ] && [ -z "$list" ] && first_password
	shadow
	if [ -e "$SYSTEM.new" ]; then
		chmod 644 "$SYSTEM.new" && mv "$SYSTEM.new" "$SYSTEM"
	else
		rm -f "$SYSTEM"
	fi
	rm -f "$IDENTITY.new"
	# The keys root logs in to the ssh server with, and whether it takes
	# them alone: the server's files, with the image's keys first.
	onu-config ${known:+--store "$known"} ssh-keys install ||
		log "the ssh server takes the image's keys alone"
	[ -n "$known" ] || state none
	put_identity "$list"
}

save() {
	[ -n "$STORE" ] || return 1
	settings | onu-config --store "$STORE" import
}

# The identity of KEY=VALUE lines on the standard input, as ID_ keys.
identity() {
	ids=$(awk '{
		i = index($0, "=")
		if (i > 1)
			print "ID_" toupper(substr($0, 1, i - 1)) "=" substr($0, i + 1)
	}')
	[ -n "$ids" ] && [ -n "$STORE" ] || return 1
	printf '%s\n' "$ids" | onu-config --store "$STORE" import || return 1
	put_identity "$(contents)"
}

# The stick's own settings of KEY=VALUE lines on the standard input, as SYS_
# keys; only the keys their readers know, with values of their shape. The
# stick runs offline and has no resolver, so the servers are addresses, never
# names.
system() {
	lines=$(awk '
	function ip4(s,   p, n, i) {
		n = split(s, p, ".")
		if (n != 4)
			return 0
		for (i = 1; i <= 4; i++)
			if (p[i] !~ /^[0-9]+$/ || length(p[i]) > 3 || p[i] + 0 > 255)
				return 0
		return 1
	}
	function ip6(s) {
		return s ~ /^[0-9A-Fa-f:.]+$/ && s ~ /:.*:/ && length(s) <= 45
	}
	function port(s) {
		return s ~ /^[0-9]+$/ && s + 0 >= 1 && s + 0 <= 65535
	}
	# An address of either family, or nothing.
	function address(s) {
		return s == "" || ip4(s) || ip6(s)
	}
	# An address, and after it a port: IPv4 behind a colon, IPv6 in brackets.
	function logserver(s,   i) {
		if (address(s))
			return 1
		if (substr(s, 1, 1) == "[") {
			i = index(s, "]")
			if (i == 0 || !ip6(substr(s, 2, i - 2)))
				return 0
			return i == length(s) || (substr(s, i + 1, 1) == ":" && port(substr(s, i + 2)))
		}
		i = index(s, ":")
		return i > 0 && ip4(substr(s, 1, i - 1)) && port(substr(s, i + 1))
	}
	{
		i = index($0, "=")
		if (i < 2)
			next
		k = substr($0, 1, i - 1)
		v = substr($0, i + 1)
		if (k == "HOSTNAME" && v !~ /^([A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?)?$/)
			exit 1
		if (k == "MGMT_VLAN" && v !~ /^([1-9][0-9]*)?$/)
			exit 1
		if (k == "MGMT_ADDRESS" && v !~ /^([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+\/[0-9]+)?$/)
			exit 1
		if (k == "SFP_LANE" && v !~ /^(auto|2500basex|1000basex-an|1000basex|2500basex-an)?$/)
			exit 1
		# A time server: an address, which cannot read as an option of timed.
		if (k == "NTP_SERVER" && !address(v))
			exit 1
		# A server of the system log: an address, with a port after it.
		if (k == "LOG_SERVER" && !logserver(v))
			exit 1
		# The name the panel shows for the system, which the files the
		# status daemon hands out are named after: 1 to 20 characters
		# of ASCII, letters, digits, blanks, hyphens and dots, a letter
		# or a digit first and last; pond checks it alike.
		if (k == "PRODUCT_NAME" &&
		    (length(v) > 20 || v !~ /^([A-Za-z0-9]([A-Za-z0-9 .-]*[A-Za-z0-9])?)?$/))
			exit 1
		if (k !~ /^(HOSTNAME|MGMT_VLAN|MGMT_ADDRESS|SFP_LANE|NTP_SERVER|LOG_SERVER|PRODUCT_NAME)$/)
			exit 1
		print "SYS_" k "=" v
	}') || return 1
	[ -n "$lines" ] && [ -n "$STORE" ] || return 1
	printf '%s\n' "$lines" | onu-config --store "$STORE" import || return 1
	contents | awk '/^SYS_/ {
		i = index($0, "=")
		if (i < length($0))
			print substr($0, 5)
	}' >"$SYSTEM.new" && chmod 644 "$SYSTEM.new" && mv "$SYSTEM.new" "$SYSTEM"
}

# The host session $1 delegates its IPv6 prefix to, from /run/dhcp6d/pdN: the
# DUID in hex, the IAID, the host's address and the prefix; none without the
# file.
pd() {
	case $1 in
	[1-9]) ;;
	*) return 1 ;;
	esac
	[ "$1" -le $MAX_SESSIONS ] && [ -n "$STORE" ] || return 1
	host=
	[ -r "$PD$1" ] && read -r host <"$PD$1"
	if [ -z "$host" ]; then
		onu-config --store "$STORE" unset "PD$1_HOST"
		return
	fi
	case $host in
	*[!0-9a-fA-F:./\ ]*) return 1 ;;
	esac
	echo "PD$1_HOST=$host" | onu-config --store "$STORE" import
}

# /run/shadow, which /etc/shadow names: the image's accounts, root's with the
# hash in force. A hash of the older form the status daemon made (PBKDF2),
# which crypt() cannot check, gives root no password for the console and
# ssh, only keys, until a sign-in to the panel stores the password again.
# A password not known ("!") is the default one here, for the console only.
# The ssh server takes no password then, nor while the default is in force,
# as that is printed in this repository: keys only (onu-run, onu-mgmt).
shadow() {
	hash=
	[ -r "$AUTH" ] && read -r hash <"$AUTH"
	case $hash in
	'' | '!') hash=$DEFAULT_HASH ;;
	'$'*) ;;
	*) hash='*' ;;
	esac
	(
		umask 077
		if [ -r "$IMAGE_SHADOW" ]; then
			awk -F: -v OFS=: -v hash="$hash" \
				'$1 == "root" { $2 = hash } { print }' "$IMAGE_SHADOW"
		else
			echo "root:$hash:::::::"
		fi >"$SHADOW.new" && mv "$SHADOW.new" "$SHADOW"
	)
}

auth() {
	case $1 in
	*[!A-Za-z0-9\$./=_-]* | "") return 1 ;;
	esac
	[ -n "$STORE" ] || return 1
	was=
	[ -r "$AUTH" ] && read -r was <"$AUTH"
	echo "AUTH_HASH=$1" | onu-config --store "$STORE" import || return 1
	(umask 077 && echo "$1" >"$AUTH.new" && mv "$AUTH.new" "$AUTH")
	shadow
	# The ssh server took no password while the default was in force
	# (onu-run): procd starts it again once it ends, now with passwords.
	if [ -z "$was" ]; then
		pid=$(cat "$RUN/dropbear.pid" 2>/dev/null)
		[ -n "$pid" ] && kill "$pid" 2>/dev/null
	fi
	return 0
}

# The SHA-256 crypt hash of the password $1, with a salt of 16 random
# characters.
hash_secret() {
	salt=$(tr -dc 'A-Za-z0-9./' </dev/urandom | head -c 16)
	[ ${#salt} -eq 16 ] || return 1
	hash=$(printf '%s\n' "$1" | cryptpw -m sha256 -S "rounds=$ROUNDS\$$salt") ||
		return 1
	case $hash in
	"\$5\$rounds=$ROUNDS\$$salt\$"?*) echo "$hash" ;;
	*)
		log "the password could not be hashed"
		return 1
		;;
	esac
}

# At the first start, while the store holds nothing yet, root's password
# becomes the one of the shipped firmware's administrator, which its owner
# knows, so that the panel, ssh and the console take it. The shipped
# firmware's own default, "admin", is this system's default as well, printed
# in this repository: then nothing is taken, and the panel asks for a
# password. A password with a control character, or longer than this
# system's limit, is not taken either. Later starts, and a password cleared
# to have the default back (ADR 0009), take nothing.
first_password() {
	secret=$(ponid admin_password 2>/dev/null) || return 0
	[ -n "$secret" ] && [ "$secret" != admin ] && [ ${#secret} -le 128 ] ||
		return 0
	case $secret in
	*[[:cntrl:]]*) return 0 ;;
	esac
	hash=$(hash_secret "$secret")
	unset secret
	[ -n "$hash" ] && auth "$hash" || return 0
	log "root's password is the shipped firmware's administrator's"
}

# Root's new password, the line of the standard input: 8 to 128 characters
# and no control character, hashed with a salt of 16 random characters.
password() {
	IFS= read -r secret || [ -n "$secret" ] || return 1
	if [ ${#secret} -lt 8 ] || [ ${#secret} -gt 128 ]; then
		log "a password has 8 to 128 characters"
		return 1
	fi
	case $secret in
	*[[:cntrl:]]*)
		log "a password has no control characters"
		return 1
		;;
	esac
	hash=$(hash_secret "$secret") || return 1
	auth "$hash"
}

# The order of P-256 and its scalars that are keys: 1 to the order less one,
# here in 64 lowercase hexadecimal digits.
P256_N=ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551

# Whether $1 is such a scalar; awk compares the two as text, of one length.
is_key() {
	scalar=$1
	case $scalar in
	*[!0-9a-f]* | "") return 1 ;;
	esac
	[ ${#scalar} -eq 64 ] && [ "$scalar" != "$(printf '%064d' 0)" ] &&
		awk -v k="$scalar" -v n="$P256_N" 'BEGIN { exit !((k "") < (n "")) }'
}

# Wait until the kernel's random number generator is seeded: a read of
# /dev/random blocks until then, and returns at once afterwards. The boot
# script runs before the kernel says "crng init done" (about 3 s after its
# start on the stick), and a key is made once, at the first start.
seeded() {
	head -c 1 /dev/random >/dev/null
}

# The panel's key: the store's, which load put in /run/onu-tls-key, or a new
# one of /dev/urandom, whose bytes that are hexadecimal digits each carry
# four bits, kept in the store. Without a store, the new one lasts until the
# next start, and the daemon's certificates change with it.
tls() {
	key=
	[ -r "$TLS" ] && read -r key <"$TLS"
	is_key "$key" && return 0
	new_scalar || {
		log "no key for the panel's HTTPS"
		return 1
	}
	(umask 077 && echo "$key" >"$TLS.new" && mv "$TLS.new" "$TLS") || return 1
	if [ -z "$STORE" ]; then
		log "the panel's key is not kept: no store"
	elif ! echo "TLS_KEY=$key" | onu-config --store "$STORE" import; then
		log "the panel's key is not kept: the store refused it"
	fi
	return 0
}

# A new scalar of P-256 in key, of /dev/urandom, whose bytes that are
# hexadecimal digits each carry four bits; fails without one.
new_scalar() {
	seeded
	for _ in 1 2 3; do
		key=$(tr -dc '0-9a-f' </dev/urandom | head -c 64)
		is_key "$key" && return 0
	done
	key=
	return 1
}

# The status daemon's copy of the file $1, as POND_DIR/$2, for its account
# alone, put in place whole; none without the file. The boot script makes
# the daemon's directory; without it there is nothing to hand over.
give_pond() {
	to=$POND_DIR/$2
	if [ ! -s "$1" ]; then
		rm -f "$to" "$to.new"
		return 0
	fi
	(umask 077 && cat "$1" >"$to.new") && chown pond:pond "$to.new" &&
		chmod 400 "$to.new" && mv "$to.new" "$to" || {
		rm -f "$to.new"
		log "$2 is not the status daemon's"
		return 1
	}
}

# The panel's key and the own certificate, to the status daemon, which
# reads them again once the change that asked for it has ended.
install_tls() {
	[ -d "$POND_DIR" ] || return 0
	give_pond "$TLS" tls.key && give_pond "$TLS_OWN" tls.own
}

# A new key for the panel in place of the one in force, kept in the store
# before it is: the status daemon makes new certificates from it.
tls_new() {
	[ -n "$STORE" ] || {
		log "no store: a new key would last until the next start"
		return 1
	}
	new_scalar || {
		log "no key for the panel's HTTPS"
		return 1
	}
	echo "TLS_KEY=$key" | onu-config --store "$STORE" import || {
		log "the new key is not kept: the store refused it"
		return 1
	}
	(umask 077 && echo "$key" >"$TLS.new" && mv "$TLS.new" "$TLS") || return 1
	install_tls
}

# The longest chain of an own certificate in base64, 8 KiB of DER, and the
# piece of it that one value of the store holds (CFG_VALUE_MAX).
CHAIN_MAX=10924
PIECE=1024
PIECES=$((CHAIN_MAX / PIECE + 1))

# An own certificate, of the lines KEY= and CHAIN= on the standard input,
# which the status daemon checked: kept in the store, TLS_OWN_KEY and
# TLS_OWN_CHAIN1 on, in one write, then in force.
tls_own() {
	key=
	chain=
	while IFS= read -r line || [ -n "$line" ]; do
		case $line in
		KEY=*) key=${line#KEY=} ;;
		CHAIN=*) chain=${line#CHAIN=} ;;
		esac
	done
	is_key "$key" || {
		log "the certificate's key is not a key of P-256"
		return 1
	}
	case $chain in
	"" | *[!A-Za-z0-9+/=]*)
		log "the certificate is not base64"
		return 1
		;;
	esac
	[ ${#chain} -le $CHAIN_MAX ] || {
		log "the certificate and its chain are longer than 8 KiB"
		return 1
	}
	[ -n "$STORE" ] || {
		log "no store: the certificate would last until the next start"
		return 1
	}
	printf '%s\n' "$chain" | awk -v key="$key" -v piece=$PIECE -v max=$PIECES '{
		print "TLS_OWN_KEY=" key
		n = 0
		for (i = 1; i <= length($0); i += piece)
			print "TLS_OWN_CHAIN" ++n "=" substr($0, i, piece)
		while (++n <= max)
			print "-TLS_OWN_CHAIN" n
	}' | onu-config --store "$STORE" import || {
		log "the certificate is not kept: the store refused it"
		return 1
	}
	(umask 077 && printf '%s\n%s\n' "$key" "$chain" >"$TLS_OWN.new" &&
		mv "$TLS_OWN.new" "$TLS_OWN") || return 1
	install_tls
}

# Back to the certificates the status daemon makes from the panel's key.
tls_self() {
	if [ -n "$STORE" ]; then
		n=1
		{
			echo "-TLS_OWN_KEY"
			while [ $n -le $PIECES ]; do
				echo "-TLS_OWN_CHAIN$n"
				n=$((n + 1))
			done
		} | onu-config --store "$STORE" import || return 1
	fi
	rm -f "$TLS_OWN"
	install_tls
}

# The bytes of the hexadecimal digits on the standard input, as printf's
# octal escapes.
unhex() {
	awk '{
		for (i = 1; i < length($0); i += 2) {
			hi = index("0123456789abcdef", substr($0, i, 1)) - 1
			lo = index("0123456789abcdef", substr($0, i + 1, 1)) - 1
			printf "\\%03o", hi * 16 + lo
		}
	}'
}

# The ssh server's host key: the store's, which load put in /run/onu-ssh-key,
# written back in the server's format, or a new one, kept in the store.
# Without a store, the new one lasts until the next start.
ssh_key() {
	hex=
	[ -r "$SSHKEY" ] && read -r hex <"$SSHKEY"
	umask 077
	mkdir -p "${HOSTKEY%/*}"
	rm -f "$HOSTKEY.new"
	case $hex in
	"" | *[!0-9a-f]*) ;;
	*)
		printf "$(echo "$hex" | unhex)" >"$HOSTKEY.new" &&
			dropbearkey -y -f "$HOSTKEY.new" >/dev/null 2>&1 &&
			mv "$HOSTKEY.new" "$HOSTKEY" && return 0
		log "the store's ssh host key cannot be used: a new one"
		rm -f "$HOSTKEY.new"
		;;
	esac
	seeded
	dropbearkey -t ed25519 -f "$HOSTKEY.new" >/dev/null 2>&1 &&
		mv "$HOSTKEY.new" "$HOSTKEY" || {
		log "no ssh host key"
		rm -f "$HOSTKEY.new"
		return 1
	}
	hex=$(hexdump -v -e '/1 "%02x"' "$HOSTKEY")
	echo "$hex" >"$SSHKEY.new" && mv "$SSHKEY.new" "$SSHKEY"
	if [ -z "$STORE" ]; then
		log "the ssh host key is not kept: no store"
	elif ! echo "SSH_HOST_KEY=$hex" | onu-config --store "$STORE" import; then
		log "the ssh host key is not kept: the store refused it"
	fi
	return 0
}

# The host key in force as the server's key tool prints it: its TYPE= and
# its FINGERPRINT=, which an ssh client shows at the first login.
ssh_host() {
	dropbearkey -y -f "$HOSTKEY" 2>/dev/null | awk '
		/^(ssh-|ecdsa-)/ && type == "" { type = $1 }
		/^Fingerprint: / { fp = $2 }
		END {
			if (type == "" || fp == "")
				exit 1
			print "TYPE=" type
			print "FINGERPRINT=" fp
		}'
}

# A new host key in place of the one in force, kept in the store before it
# is; the server reads its key when it starts, and procd starts it again
# once it ends, with the new one. Its sessions go on.
ssh_new() {
	[ -n "$STORE" ] || {
		log "no store: a new host key would last until the next start"
		return 1
	}
	umask 077
	mkdir -p "${HOSTKEY%/*}"
	rm -f "$HOSTKEY.new"
	seeded
	dropbearkey -t ed25519 -f "$HOSTKEY.new" >/dev/null 2>&1 || {
		log "no ssh host key"
		rm -f "$HOSTKEY.new"
		return 1
	}
	hex=$(hexdump -v -e '/1 "%02x"' "$HOSTKEY.new")
	echo "SSH_HOST_KEY=$hex" | onu-config --store "$STORE" import || {
		log "the new host key is not kept: the store refused it"
		rm -f "$HOSTKEY.new"
		return 1
	}
	echo "$hex" >"$SSHKEY.new" && mv "$SSHKEY.new" "$SSHKEY" &&
		mv "$HOSTKEY.new" "$HOSTKEY" || return 1
	pid=$(cat "$RUN/dropbear.pid" 2>/dev/null)
	[ -n "$pid" ] && kill "$pid" 2>/dev/null
	ssh_host
}

# The host key, then the keys root logs in with and whether the server
# takes them alone (onu-config ssh-keys).
ssh_show() {
	ssh_host
	onu-config ${STORE:+--store "$STORE"} ssh-keys
}

# Whether $1 is a token of scripts: 64 hexadecimal digits, 256 bits.
is_token() {
	candidate=$1
	case $candidate in
	*[!0-9a-f]* | "") return 1 ;;
	esac
	[ ${#candidate} -eq 64 ]
}

# The token in force: what load put in /run/onu-pond-token, else nothing.
current_token() {
	hex=
	[ -r "$TOKEN" ] && read -r hex <"$TOKEN"
	is_token "$hex" || hex=
	[ -n "$hex" ]
}

# The status daemon's copy of the token, /run/pond/token, which only its
# account reads, put in place whole: the daemon reads the file at every
# request that comes without a session, and takes none once it is gone. The
# boot script makes the daemon's directory; without it there is nothing to
# hand over.
install_token() {
	[ -d "$POND_DIR" ] || return 0
	if ! current_token; then
		rm -f "$POND_DIR/token" "$POND_DIR/token.new"
		return 0
	fi
	(umask 077 && echo "$hex" >"$POND_DIR/token.new") &&
		chown pond:pond "$POND_DIR/token.new" && chmod 400 "$POND_DIR/token.new" &&
		mv "$POND_DIR/token.new" "$POND_DIR/token" || {
		rm -f "$POND_DIR/token.new"
		log "the token of scripts is not the status daemon's"
		return 1
	}
}

# The token of scripts, which only a person makes, on the stick (ssh, the
# console), and prints once to put in a script: a script has none until then,
# and signs in as a person does. A new token takes the place of the old one at
# once, and "off" takes it away; both stay across restarts, in the store.
# Without a store, a new token lasts until the next start.
token() {
	case ${1:-} in
	"")
		current_token || {
			log "no token of scripts: onu-config token new makes one"
			return 1
		}
		echo "$hex"
		;;
	new)
		seeded
		hex=$(tr -dc '0-9a-f' </dev/urandom | head -c 64)
		is_token "$hex" || {
			log "no token of scripts: no random numbers"
			return 1
		}
		if [ -z "$STORE" ]; then
			log "the token of scripts is not kept: no store"
		elif ! echo "POND_TOKEN=$hex" | onu-config --store "$STORE" import; then
			log "the token of scripts is not kept: the store refused it"
			return 1
		fi
		(umask 077 && echo "$hex" >"$TOKEN.new" && mv "$TOKEN.new" "$TOKEN") || return 1
		install_token
		echo "$hex"
		;;
	off)
		if [ -n "$STORE" ]; then
			onu-config --store "$STORE" unset POND_TOKEN || return 1
		fi
		rm -f "$TOKEN"
		install_token
		;;
	install)
		install_token
		;;
	*)
		return 2
		;;
	esac
}

if [ -z "${ONU_STORE:-}" ] && [ "${1:-}" = load ]; then
	mount_config || log "the partition \"config\" is not mounted"
fi
STORE=$(store) || {
	log "/config is not mounted: no store"
	STORE=
}

case ${1:-} in
load)
	load
	;;
save)
	if save; then
		state saved
	else
		state failed
		exit 1
	fi
	;;
identity)
	identity
	;;
auth)
	auth "${2:-}"
	;;
password)
	password
	;;
system)
	system
	;;
pd)
	pd "${2:-}"
	;;
tls)
	case ${2:-} in
	"") tls ;;
	new) tls_new ;;
	own) tls_own ;;
	self-signed) tls_self ;;
	install) install_tls ;;
	*)
		echo "usage: onu-config tls [new|own|self-signed|install]" >&2
		exit 2
		;;
	esac
	;;
ssh)
	case ${2:-} in
	"") ssh_key ;;
	new) ssh_new ;;
	show) ssh_show ;;
	*)
		echo "usage: onu-config ssh [new|show]" >&2
		exit 2
		;;
	esac
	;;
token)
	token "${2:-}"
	;;
*)
	echo "usage: onu-config load|save|identity|password|auth HASH|system|pd N|tls|ssh" >&2
	echo "       onu-config tls [new|own|self-signed|install] | ssh [new|show]" >&2
	echo "       onu-config token [new|off|install]" >&2
	exit 2
	;;
esac
