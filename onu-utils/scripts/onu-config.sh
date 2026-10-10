#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# onu-config.sh: the files the running system reads, to and from the stick's
# configuration store in the flash partition "config", which onu-config keeps.
# onu-config runs it for these verbs, and is how a person or a program asks
# for them (ADR 0015; it was onu-store before):
#
#   onu-config load      at boot: the identity, the settings and the PPPoE
#                        accounts from the store into /run/onu-identity,
#                        /run/onu.conf and /run/onu-pppoeN
#   onu-config save      the settings of /run/onu.conf and the accounts of
#                        /run/onu-pppoeN into the store, in one write
#   onu-config migrate   the same, with the identity of the shipped firmware's
#                        configuration (ponid) taken into the store
#   onu-config identity  the identity of the KEY=VALUE lines of the standard
#                        input (SERIAL, PLOAM, ...) into the store and into
#                        /run/onu-identity
#   onu-config password  root's new password, the line of the standard input,
#                        hashed into the store, /run/onu-auth and /run/shadow
#   onu-config auth HASH a hash of root's password made elsewhere, the same way
#   onu-config system    the stick's own settings of the KEY=VALUE lines of the
#                        standard input (HOSTNAME, MGMT_VLAN, MGMT_ADDRESS,
#                        SFP_LANE, NTP_SERVER, LOG_SERVER) into the store and
#                        into /run/onu-system; an empty value removes the key
#   onu-config pd N      the host session N delegates its IPv6 prefix to, the
#                        line dhcp6d keeps in /run/onu-pdN, into the store;
#                        without the file, out of it
#
# A key ID_<NAME> of the store is the line "<name>=value" of /run/onu-identity,
# the name in lower case; PPPOEn_USER and PPPOEn_SECRET, for the sessions 1 to
# 7, are the line "user secret" of /run/onu-pppoeN, which an empty PPPOEn_USER
# leaves out (PPPOE_USER and PPPOE_SECRET, the single account of before, are
# the first session's);
# AUTH_HASH is the content of /run/onu-auth: the hash of root's password, the
# one account of the stick, which the console's login, the ssh server and the
# status daemon's sign-in all check (/run/shadow carries it for the first
# two);
# a key SYS_<NAME> is the line "<NAME>=value" of /run/onu-system, the stick's
# own settings, which the boot script reads before it brings up the links;
# PDn_HOST is the content of /run/onu-pdN, the host session n delegated its
# IPv6 prefix to, which dhcp6d takes back after a restart (see onu-apply);
# every other key is a line of /run/onu.conf. The files of secrets are for
# root only.
#
# Until the store holds an identity, the identity is the one ponid reads from
# the shipped firmware's configuration, which shares the partition. The first
# write of the store overwrites the start of that configuration, so a save
# that finds no identity in the store takes ponid's into the same write: the
# unit never loses its identity to its first saved setting.
#
# What became of the settings is the word in /run/onu-store, for the status
# daemon: "loaded" when load found settings in the store, "defaults" when it
# found none, "saved" or "failed" after a save, "none" without a store.
#
# ONU_ROOT names another root for the files it reads and writes, and
# ONU_STORE another store (a file), which the tests use.

set -u

ROOT=${ONU_ROOT:-}
RUN=$ROOT/run
# ponid is one of the helpers out of a person's way (ADR 0015).
PATH=$PATH:$ROOT/usr/lib/onu
CONF=$RUN/onu.conf
CREDS=$RUN/onu-pppoe
PD=$RUN/onu-pd
MAX_SESSIONS=7
IDENTITY=$RUN/onu-identity
AUTH=$RUN/onu-auth
SYSTEM=$RUN/onu-system
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

# The store: ONU_STORE, else the character device of the partition "config".
store() {
	if [ -n "${ONU_STORE:-}" ]; then
		echo "$ONU_STORE"
		return 0
	fi
	dev=$(awk '$4 == "\"config\"" { sub(":", "", $1); print $1 }' \
		"$ROOT/proc/mtd" 2>/dev/null)
	[ -n "$dev" ] || return 1
	echo "/dev/$dev"
}

# The store's KEY=VALUE lines; nothing for an empty store.
contents() {
	onu-config --store "$STORE" list
}

has_identity() {
	contents | grep -q '^ID_'
}

# The identity ponid reads, as ID_ keys.
factory_identity() {
	ponid | awk '{
		i = index($0, "=")
		if (i > 1)
			print "ID_" toupper(substr($0, 1, i - 1)) "=" substr($0, i + 1)
	}'
}

# The settings and the accounts in force, as keys of the store.
settings() {
	if [ -r "$CONF" ]; then
		grep '^[A-Z][A-Z0-9_]*=' "$CONF" |
			grep -v -e '^ID_' -e '^PPPOE[0-9]*_' -e '^AUTH_' -e '^SYS_'
	fi
	stored=$(contents 2>/dev/null)
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
	list=$(contents) || {
		log "the store cannot be read"
		list=
	}
	umask 077
	rm -f "$IDENTITY.new" "$CONF.new" "$CREDS"*.new "$AUTH.new" "$SYSTEM.new" "$PD"*
	printf '%s\n' "$list" | awk -v id="$IDENTITY.new" -v conf="$CONF.new" \
		-v creds="$CREDS" -v auth="$AUTH.new" -v max="$MAX_SESSIONS" \
		-v sys="$SYSTEM.new" -v pd="$PD" '
		{
			i = index($0, "=")
			if (i < 2)
				next
			k = substr($0, 1, i - 1)
			v = substr($0, i + 1)
		}
		k ~ /^ID_/ { print tolower(substr(k, 4)) "=" v > id; next }
		k == "AUTH_HASH" { if (v != "") print v > auth; next }
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
		}'
	if [ -e "$CONF.new" ]; then
		chmod 644 "$CONF.new" && mv "$CONF.new" "$CONF"
		state loaded
	else
		state defaults
	fi
	for new in "$CREDS"*.new; do
		[ -e "$new" ] && mv "$new" "${new%.new}"
	done
	[ -e "$AUTH.new" ] && mv "$AUTH.new" "$AUTH"
	shadow
	if [ -e "$SYSTEM.new" ]; then
		chmod 644 "$SYSTEM.new" && mv "$SYSTEM.new" "$SYSTEM"
	else
		rm -f "$SYSTEM"
	fi
	if [ -s "$IDENTITY.new" ]; then
		mv "$IDENTITY.new" "$IDENTITY"
		(umask 022 && echo store >"$SOURCE")
		return 0
	fi
	rm -f "$IDENTITY.new"
	# No identity in the store yet: the shipped firmware's.
	(umask 022 && echo factory >"$SOURCE")
	ponid >"$IDENTITY"
}

save() {
	need_identity=$1
	ids=
	if ! has_identity; then
		ids=$(factory_identity)
		if [ -z "$ids" ] && [ "$need_identity" = yes ]; then
			log "no identity in the shipped firmware's configuration"
			return 1
		fi
	fi
	{
		[ -n "$ids" ] && printf '%s\n' "$ids"
		settings
	} | onu-config --store "$STORE" import || return 1
	# What the store gives back is what was meant to go in.
	if [ -n "$ids" ]; then
		printf '%s\n' "$ids" | while IFS= read -r line; do
			contents | grep -q -x -F -e "$line" || {
				log "the store does not hold ${line%%=*}"
				return 1
			}
		done || return 1
	fi
	return 0
}

# The identity of KEY=VALUE lines on the standard input, as ID_ keys.
identity() {
	ids=$(awk '{
		i = index($0, "=")
		if (i > 1)
			print "ID_" toupper(substr($0, 1, i - 1)) "=" substr($0, i + 1)
	}')
	[ -n "$ids" ] || return 1
	printf '%s\n' "$ids" | onu-config --store "$STORE" import || return 1
	umask 077
	contents | awk '/^ID_/ {
		i = index($0, "=")
		print tolower(substr($0, 4, i - 4)) "=" substr($0, i + 1)
	}' >"$IDENTITY.new" && mv "$IDENTITY.new" "$IDENTITY" &&
		(umask 022 && echo store >"$SOURCE")
}

# The stick's own settings of KEY=VALUE lines on the standard input, as SYS_
# keys; only the keys the boot script knows, with values of their shape.
system() {
	lines=$(awk '{
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
		if (k == "SFP_LANE" && v !~ /^(auto|2500basex|1000basex-an|1000basex)?$/)
			exit 1
		# A time server: an address or a name, never an option of ntpd.
		if (k == "NTP_SERVER" && (length(v) > 253 || v !~ /^([A-Za-z0-9:][A-Za-z0-9.:-]*)?$/))
			exit 1
		# A server of the system log: an address or a name, and a port after
		# a colon, an IPv6 address in brackets then.
		if (k == "LOG_SERVER" && (length(v) > 253 ||
		    v !~ /^([A-Za-z0-9:][A-Za-z0-9.:-]*|\[[0-9A-Fa-f:.]+\](:[0-9]+)?)?$/))
			exit 1
		if (k !~ /^(HOSTNAME|MGMT_VLAN|MGMT_ADDRESS|SFP_LANE|NTP_SERVER|LOG_SERVER)$/)
			exit 1
		print "SYS_" k "=" v
	}') || return 1
	[ -n "$lines" ] || return 1
	printf '%s\n' "$lines" | onu-config --store "$STORE" import || return 1
	contents | awk '/^SYS_/ {
		i = index($0, "=")
		if (i < length($0))
			print substr($0, 5)
	}' >"$SYSTEM.new" && chmod 644 "$SYSTEM.new" && mv "$SYSTEM.new" "$SYSTEM"
}

# The host session $1 delegates its IPv6 prefix to, from /run/onu-pdN: the
# DUID in hex, the IAID, the host's address and the prefix; none without the
# file.
pd() {
	case $1 in
	[1-9]) ;;
	*) return 1 ;;
	esac
	[ "$1" -le $MAX_SESSIONS ] || return 1
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
shadow() {
	hash=
	[ -r "$AUTH" ] && read -r hash <"$AUTH"
	case $hash in
	'') hash=$DEFAULT_HASH ;;
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
	echo "AUTH_HASH=$1" | onu-config --store "$STORE" import || return 1
	(umask 077 && echo "$1" >"$AUTH.new" && mv "$AUTH.new" "$AUTH")
	shadow
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
	salt=$(tr -dc 'A-Za-z0-9./' </dev/urandom | head -c 16)
	[ ${#salt} -eq 16 ] || return 1
	hash=$(printf '%s\n' "$secret" | cryptpw -m sha256 -S "rounds=$ROUNDS\$$salt") ||
		return 1
	case $hash in
	"\$5\$rounds=$ROUNDS\$$salt\$"?*) ;;
	*)
		log "the password could not be hashed"
		return 1
		;;
	esac
	auth "$hash"
}

STORE=$(store) || {
	log "no partition \"config\""
	state none
	exit 1
}

case ${1:-} in
load)
	load
	;;
save)
	if save no; then
		state saved
	else
		state failed
		exit 1
	fi
	;;
migrate)
	if has_identity; then
		log "the store already holds an identity"
		exit 0
	fi
	if save yes; then
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
*)
	echo "usage: onu-config load|save|migrate|identity|password|auth HASH|system|pd N" >&2
	exit 2
	;;
esac
