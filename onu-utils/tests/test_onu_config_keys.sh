#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Tests of the public keys root logs in to the ssh server with: onu-config
# ssh-keys, of the build, on a store in a file, with stand-ins for BusyBox's
# hexdump, dropbearkey, chown and logger. The keys were made with OpenSSH's
# ssh-keygen, and their fingerprints are the ones it printed.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
STORE_SCRIPT=$HERE/scripts/onu-config.sh
BUILD=${BUILD:-$HERE/build}
checks=0
failures=0

ED25519='ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJUZp+xf8C/t3NT+0VYL5nV7MNOQXr21ORnzDVmED9xE test-ed25519@example'
ED25519_FP=SHA256:lxckqci4MwTwEjJFI7q4oX1lApk+VkklJ0/B5U0TTc0
P256='ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBKi6OUiZeRdSZadzWqI/9qS1pPepAzshYaS90KLBFnpy8aHIQZ8IWWdP4jHEn8D0ylXqGBBWU3mMd/CsU/Ueedg= p256'
P256_FP=SHA256:QVzpbdO2XiR+U/GoiobfqifyWwLvSY5QBcJvz5JlvTc
RSA1024='ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQC4WKVwESjp9sdRo62oW55S64G3eVqIYUlT+q+xVqp03ARK0Oj2yoFl8Df/VkE02pdFdlRASR0aD+o5EoHaKu21mxYU5mwShl85lftmV7po42nmwn7QZu8aoWAtXdDFtK7yvQrn0XBGn3ealTZSGOEHpR52e7BcjqN/o8CIOiQZhw== rsa1024'
RSA4096='ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAACAQClwHPntBSVYjrernMrqfVsUuLGWU5acxU552ov7cBaGc62UcGOgMW2yVsSaJ8utN89tOY1Jk3PE57aX65a0WoDuVXJlG74gjb25LnSLlXgr4ua5TXIUK5FYj58iMcv5fEd6gzQMYrj764eMtWwC+ATZ5YB0sSCE/CtuMtF1m+fV1ZjJWRfUyIK8e8w1vmAGyiQkhOPAii9QZq9s/B22ZreQSaG+ae30yRYLqz1fOk4c6Tvdi8OZadcFmTUm2h50Hp8H62g/6FdsCa5zmN9agJOROCvhU7kD7rwm3MaMlqjk07m6i0IhK9aV4D8GqZ++il57B1cpdV4z44+exkVQvWes04MrE+9IwGhFnmilCxvEad4duLGFLjmsT/EMrqDmzdMOV648QsQwq6OTeTtjNTkVGIM5wvB3TjREfQ0hNBQJPCSLAs//Y4TGVLlr+zsdZiEp0Palgbn1MONpj8Zy8MSsfsgCQ6MhQ3RACqHE4JKelyjKaMk7pYw633HzaupxHDE7MijmgmHHva7n2dXUmwU454UwqhdNH2xx7uE6N3aVjF7oku070E30wpzSllcMkNeugMmwSPTQ60FK2TMw7/D5wRr/CNSKWuxaEKhp/QwQ3lqPGvCBaQ89evCovcSINMHfmNi9SrV25uzdFSIZfVlgrH8ecbe7j900Vc0tNbnFw== rsa4096'

check() {
	checks=$((checks + 1))
	if ! eval "$1"; then
		failures=$((failures + 1))
		echo "  FAIL: $1"
	fi
}

setup() {
	T=$(mktemp -d)
	export ONU_ROOT=$T/root ONU_STORE=$T/store
	mkdir -p "$ONU_ROOT/run" "$ONU_ROOT/root/.ssh" "$T/bin"
	: >"$ONU_STORE"
	printf '#!/bin/sh\nexit 0\n' >"$T/bin/logger"
	printf '#!/bin/sh\necho "chown $*" >>"%s/chown.log"\n' "$T" >"$T/bin/chown"
	# dropbearkey: -t ed25519 -f FILE writes a key, a new one each time,
	# unless told to fail; -y -f FILE prints it as the real one does.
	cat >"$T/bin/dropbearkey" <<-EOF
	#!/bin/sh
	case "\$1" in
	-t)
		[ -e "$T/dropbearkey.fail" ] && exit 1
		[ -e "\$4" ] && exit 1
		printf '\\000\\000\\000\\013ssh-ed25519%s' "\$(date +%N)" >"\$4"
		;;
	-y)
		[ -s "\$3" ] || exit 1
		echo "Public key portion is:"
		echo "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5 root@stick"
		echo "Fingerprint: SHA256:\$(od -An -v -tx1 "\$3" | tr -d ' \\n' | cut -c1-43)"
		;;
	*) exit 2 ;;
	esac
	EOF
	cat >"$T/bin/hexdump" <<-'EOF'
	#!/bin/sh
	[ "$1 $2 $3" = '-v -e /1 "%02x"' ] || exit 2
	od -An -v -tx1 "$4" | tr -d ' \n'
	EOF
	chmod +x "$T/bin"/*
	PATH=$T/bin:$BUILD:$PATH
	: >"$T/in"
}

teardown() {
	rm -rf "$T"
}

stored() {
	onu-config --store "$ONU_STORE" list | grep -q -x -F -e "$1"
}

# The line $1 on the standard input of the next run.
feed() {
	printf '%s\n' "$1" >"$T/in"
}

# onu-config ssh-keys, and onu-config.sh, with the input fed: the exit
# status in rc, which check() would lose, the output in out and err.
keys() {
	onu-config --store "$ONU_STORE" ssh-keys "$@" <"$T/in" >"$T/out" 2>"$T/err"
	rc=$?
}

script() {
	"$STORE_SCRIPT" "$@" <"$T/in" >"$T/out" 2>"$T/err"
	rc=$?
}

said() {
	grep -q -F -e "$1" "$T/err"
}

# A process that stands in for the ssh server, its id where the server's is.
server() {
	sleep 30 &
	ssh=$!
	echo "$ssh" >"$ONU_ROOT/run/dropbear.pid"
}

# Whether the stand-in was ended.
ended() {
	sleep 0.2
	! kill -0 "$ssh" 2>/dev/null
}

# A key of RSA of 4096 bits, other than the one above by a digit of its
# modulus, with a comment of 100 bytes.
rsa4096() {
	b64=${RSA4096#ssh-rsa }
	b64=${b64% rsa4096}
	printf 'ssh-rsa %s%s%s %0100d\n' "$(echo "$b64" | cut -c1-299)" "$1" \
		"$(echo "$b64" | cut -c301-)" 0
}

echo "the keys root logs in with: none at first"
setup
keys
check '[ $rc -eq 0 ] && [ "$(cat "$T/out")" = ONLY=0 ]'
teardown

echo "a key added: kept, said, and in the server's file after the image's own"
setup
printf 'ssh-ed25519 AAAAimage bench' >"$ONU_ROOT/root/.ssh/authorized_keys"
feed "  $ED25519  "
keys add
check '[ $rc -eq 0 ] && [ "$(cat "$T/out")" = "KEY=ssh-ed25519 256 $ED25519_FP test-ed25519@example" ]'
check 'stored "SSH_KEY1=$ED25519"'
check '[ "$(cat "$ONU_ROOT/run/dropbear/authorized_keys")" = "$(printf "ssh-ed25519 AAAAimage bench\n%s" "$ED25519")" ]'
check '[ "$(ls -l "$ONU_ROOT/run/dropbear/authorized_keys" | cut -c1-10)" = -rw------- ]'
check '[ "$(ls -ld "$ONU_ROOT/run/dropbear" | cut -c1-10)" = drwx------ ]'
check '[ ! -e "$ONU_ROOT/run/dropbear/keys-only" ]'
feed "$P256"
keys add
check '[ $rc -eq 0 ] && stored "SSH_KEY2=$P256"'
keys
check '[ $rc -eq 0 ] && [ "$(cat "$T/out")" = "$(printf "ONLY=0\nKEY=ssh-ed25519 256 %s test-ed25519@example\nKEY=ecdsa-sha2-nistp256 256 %s p256" "$ED25519_FP" "$P256_FP")" ]'
teardown

echo "what a key added may not be"
setup
feed "$ED25519"
keys add
cp "$ONU_STORE" "$T/before"
keys add
check '[ $rc -eq 1 ] && said "the key $ED25519_FP is kept already" && cmp -s "$ONU_STORE" "$T/before"'
feed "$RSA1024"
keys add
check '[ $rc -eq 1 ] && said "an RSA key of 1024 bits: the stick takes 2048 to 4096 bits"'
head -c 5000 /dev/zero | tr '\0' a >"$T/in"
keys add
check '[ $rc -eq 1 ] && said "not a key: a line TYPE BASE64 [COMMENT]"'
printf 'ssh-ed25519\000AAAA\n' >"$T/in"
keys add
check '[ $rc -eq 1 ] && said "not a key"'
printf '%s\n%s\n' "$P256" "$ED25519" >"$T/in"
keys add
check '[ $rc -eq 1 ] && said "one key at a time" && cmp -s "$ONU_STORE" "$T/before"'
keys add more
check '[ $rc -eq 2 ] && said usage'
keys sideways
check '[ $rc -eq 2 ]'
keys only maybe
check '[ $rc -eq 2 ]'
keys install now
check '[ $rc -eq 2 ]'
teardown

echo "eight keys at most, each in the first free place"
setup
for d in A B C D E F G H; do
	rsa4096 "$d" >"$T/in"
	keys add
	check '[ $rc -eq 0 ]'
done
rsa4096 I >"$T/in"
keys add
check '[ $rc -eq 1 ] && said "8 keys at most: remove one first"'
keys
fp=$(sed -n 's/^KEY=ssh-rsa 4096 \([^ ]*\) .*/\1/p' "$T/out" | sed -n 3p)
keys remove "$fp"
check '[ $rc -eq 0 ] && ! stored "SSH_KEY3=$(rsa4096 C)"'
feed "$ED25519"
keys add
check '[ $rc -eq 0 ] && stored "SSH_KEY3=$ED25519"'
check '[ "$(grep -c . "$ONU_ROOT/run/dropbear/authorized_keys")" -eq 8 ]'
teardown

echo "a key removed, by its fingerprint as an argument or a line, and one not kept"
setup
feed "$ED25519"
keys add
feed "$P256"
keys add
feed "$P256_FP"
keys remove
check '[ $rc -eq 0 ] && ! stored "SSH_KEY2=$P256" && stored "SSH_KEY1=$ED25519"'
check '! grep -q nistp256 "$ONU_ROOT/run/dropbear/authorized_keys"'
keys remove "$P256_FP"
check '[ $rc -eq 1 ] && said "no key $P256_FP is kept"'
: >"$T/in"
keys remove
check '[ $rc -eq 1 ] && said "no key  is kept"'
keys remove "$ED25519_FP"
check '[ $rc -eq 0 ] && [ ! -s "$ONU_ROOT/run/dropbear/authorized_keys" ]'
teardown

echo "keys alone: not without a key, the last key stays, and the server starts again"
setup
keys only on
check '[ $rc -eq 1 ] && said "no key is kept: add one first" && ! stored SSH_KEYS_ONLY=1'
feed "$ED25519"
keys add
server
keys only on
check '[ $rc -eq 0 ] && stored SSH_KEYS_ONLY=1 && [ -e "$ONU_ROOT/run/dropbear/keys-only" ]'
check '[ "$(ls -l "$ONU_ROOT/run/dropbear/keys-only" | cut -c1-10)" = -rw------- ]'
check ended
keys
check '[ "$(head -n 1 "$T/out")" = ONLY=1 ]'
server
keys only on
check '[ $rc -eq 0 ] && ! ended'
keys remove "$ED25519_FP"
check '[ $rc -eq 1 ] && said "the last key stays while ssh takes keys alone" && stored "SSH_KEY1=$ED25519"'
feed "$P256"
keys add
keys remove "$ED25519_FP"
check '[ $rc -eq 0 ] && [ -e "$ONU_ROOT/run/dropbear/keys-only" ] && ! ended'
keys only off
check '[ $rc -eq 0 ] && ! stored SSH_KEYS_ONLY=1 && [ ! -e "$ONU_ROOT/run/dropbear/keys-only" ]'
check ended
kill "$ssh" 2>/dev/null
teardown

echo "a value of the store that is no key is said, left out, and its place taken"
setup
feed "$ED25519"
keys add
onu-config --store "$ONU_STORE" set SSH_KEY2 "ssh-rsa AAAA broken"
keys
check '[ $rc -eq 0 ] && [ "$(grep -c ^KEY= "$T/out")" -eq 1 ] && said "SSH_KEY2 is no key, left out"'
feed "$P256"
keys add
check '[ $rc -eq 0 ] && stored "SSH_KEY2=$P256"'
# Keys alone that the store asks for with no key: the server keeps its passwords.
printf '%s\n' -SSH_KEY1 -SSH_KEY2 SSH_KEYS_ONLY=1 | onu-config --store "$ONU_STORE" import
keys install
check '[ $rc -eq 0 ] && [ ! -e "$ONU_ROOT/run/dropbear/keys-only" ] && [ ! -s "$T/out" ]'
teardown

echo "at boot: the keys of the store to the server, and none of them a setting"
setup
feed "$ED25519"
keys add
keys only on
rm -rf "$ONU_ROOT/run/dropbear"
printf 'MODE=passthrough\n' >"$ONU_ROOT/run/onu.conf"
script save
script load
check 'grep -q -x -F "$ED25519" "$ONU_ROOT/run/dropbear/authorized_keys"'
check '[ -e "$ONU_ROOT/run/dropbear/keys-only" ]'
check '! grep -q -e "^SSH_" -e "^TLS_" "$ONU_ROOT/run/onu.conf"'
check 'stored "SSH_KEY1=$ED25519" && stored SSH_KEYS_ONLY=1'
# A store that cannot be read: the image's keys alone.
echo "ssh-ed25519 AAAAimage bench" >"$ONU_ROOT/root/.ssh/authorized_keys"
echo "not a line of a store" >"$ONU_STORE"
keys install
check '[ $rc -eq 1 ] && said "the store cannot be read"'
check '[ "$(cat "$ONU_ROOT/run/dropbear/authorized_keys")" = "ssh-ed25519 AAAAimage bench" ]'
check '[ ! -e "$ONU_ROOT/run/dropbear/keys-only" ]'
keys
check '[ $rc -eq 1 ] && [ ! -s "$T/out" ]'
teardown

echo "what the server's files refuse, and what the store refuses"
setup
feed "$ED25519"
keys add
# The directory cannot be made.
mv "$ONU_ROOT/run" "$ONU_ROOT/run.away"
keys install
check '[ $rc -eq 1 ] && said "files in /run/dropbear: No such file or directory"'
mv "$ONU_ROOT/run.away" "$ONU_ROOT/run"
# The file cannot be written.
mkdir -p "$ONU_ROOT/run/dropbear/authorized_keys.new/x"
keys install
check '[ $rc -eq 1 ] && said "Is a directory"'
rmdir "$ONU_ROOT/run/dropbear/authorized_keys.new/x" "$ONU_ROOT/run/dropbear/authorized_keys.new"
mkdir -p "$ONU_ROOT/run/dropbear/keys-only.new"
keys only on
check '[ $rc -eq 1 ] && [ ! -e "$ONU_ROOT/run/dropbear/keys-only" ]'
rmdir "$ONU_ROOT/run/dropbear/keys-only.new"
keys only on
check '[ $rc -eq 0 ] && [ -e "$ONU_ROOT/run/dropbear/keys-only" ]'
rm "$ONU_ROOT/run/dropbear/keys-only"
mkdir -p "$ONU_ROOT/run/dropbear/keys-only/x"
keys only off
check '[ $rc -eq 1 ] && [ -d "$ONU_ROOT/run/dropbear/keys-only" ]'
rm -rf "$ONU_ROOT/run/dropbear/keys-only"
# The store cannot be written.
mkdir "$ONU_STORE.new"
feed "$P256"
keys add
check '[ $rc -eq 1 ] && said "Is a directory" && ! stored "SSH_KEY2=$P256"'
rmdir "$ONU_STORE.new"
teardown

echo "a store with no room refuses a key, and keys alone, and says so"
setup
feed "$ED25519"
keys add
fill=$((65522 - $(wc -c <"$ONU_STORE")))
printf 'FILLER=%s\n' "$(head -c "$fill" /dev/zero | tr '\0' x)" >>"$ONU_STORE"
cp "$ONU_STORE" "$T/before"
feed "$P256"
keys add
check '[ $rc -eq 1 ] && said "the configuration store has no room for the key" && cmp -s "$ONU_STORE" "$T/before"'
keys only on
check '[ $rc -eq 1 ] && said "the configuration store has no room for it" && cmp -s "$ONU_STORE" "$T/before"'
echo "SYS_MORE=a value of more than ten bytes" | onu-config --store "$ONU_STORE" import 2>"$T/err"
rc=$?
check '[ $rc -eq 1 ] && said "import SYS_MORE: the store has no room for it, of its 64 KiB"'
teardown

echo "a change waits for the store's lock, and gives up after 10 s"
setup
python3 -c '
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind("\0onu-config:" + sys.argv[1])
open(sys.argv[2], "w").close()
time.sleep(12)
' "$T" "$T/held" &
holder=$!
while [ ! -e "$T/held" ]; do sleep 0.05; done
feed "$ED25519"
keys add
check '[ $rc -eq 1 ] && said "no lock" && ! stored "SSH_KEY1=$ED25519"'
kill "$holder" 2>/dev/null
wait "$holder" 2>/dev/null
teardown

echo "test_onu_config_keys: $checks checks, $failures failed"
[ "$failures" -eq 0 ]
