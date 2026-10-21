#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# make-certs.sh DIR: the certificates and keys of the tests of an own
# certificate, as an authority makes them, with openssl: a root with an RSA
# key of 4096 bits, an intermediate with one of 2048, and a server's
# certificate with a P-256 key for 127.0.0.1, 192.0.2.254 and a name, valid
# from 2026-01-01 to 2036-01-01, its key as SEC1 (with the curve's block
# that openssl writes before it), as PKCS#8 and encrypted; and what the
# panel refuses: a server's certificate with an RSA key, one with a key of
# P-384, and another P-256 key. An expired one, of 2020, and the chains:
# chain.pem (the server's and the intermediate), chain3.pem (and the root).

set -eu
D=$1
mkdir -p "$D"
cd "$D"

cat >ext.cnf <<'EOF'
[ca]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[leaf]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
subjectAltName = IP:127.0.0.1, IP:192.0.2.254, DNS:stick.example, IP:2001:db8::1
[critical]
basicConstraints = critical, CA:FALSE
subjectAltName = critical, IP:192.0.2.254
EOF

q() {
	"$@" >/dev/null 2>&1
}

q openssl req -x509 -newkey rsa:4096 -nodes -keyout root.key -subj "/O=Example CA/CN=Example Root" \
	-days 3650 -out root.pem -config ext.cnf -extensions ca
q openssl req -newkey rsa:2048 -nodes -keyout int.key -subj "/O=Example CA/CN=Example Intermediate" \
	-out int.csr
q openssl x509 -req -in int.csr -CA root.pem -CAkey root.key -set_serial 2 -days 1825 \
	-extfile ext.cnf -extensions ca -out int.pem

# The server's: a key of P-256, signed by the intermediate.
leaf() {
	q openssl req -new -key "$1" -subj "/O=Example/CN=stick.example" -out "$2.csr"
	q openssl x509 -req -in "$2.csr" -CA int.pem -CAkey int.key -set_serial "$3" \
		-not_before "$4" -not_after "$5" -extfile ext.cnf -extensions leaf -out "$2.pem"
}

q openssl ecparam -name prime256v1 -genkey -out leaf.key
leaf leaf.key leaf 3 20260101000000Z 20360101000000Z
q openssl pkcs8 -topk8 -nocrypt -in leaf.key -out leaf.p8
q openssl pkcs8 -topk8 -in leaf.key -out leaf-enc.p8 -passout pass:secret
leaf leaf.key expired 4 20200101000000Z 20210101000000Z
q openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out rsa.key
leaf rsa.key rsa 5 20260101000000Z 20270101000000Z
q openssl ecparam -name secp384r1 -genkey -noout -out p384.key
leaf p384.key p384 6 20260101000000Z 20270101000000Z
q openssl ecparam -name prime256v1 -genkey -noout -out other.key
q openssl genpkey -algorithm ed25519 -out ed25519.key
q openssl rsa -in rsa.key -traditional -out rsa-traditional.key
# Valid past 2049, a GeneralizedTime; and names longer than the panel shows.
leaf leaf.key far 7 20260101000000Z 20510101000000Z
# Its alternative names said to be critical, as for a subject without a name.
q openssl x509 -req -in leaf.csr -CA int.pem -CAkey int.key -set_serial 9 \
	-not_before 20260101000000Z -not_after 20270101000000Z -extfile ext.cnf \
	-extensions critical -out critical.pem
long=$(printf '%064d' 0 | tr 0 a)
q openssl req -new -key leaf.key -subj "/O=$long/CN=$long/CN=$long" -out long.csr
q openssl x509 -req -in long.csr -CA int.pem -CAkey int.key -set_serial 8 \
	-not_before 20260101000000Z -not_after 20270101000000Z -out long.pem

cat leaf.pem int.pem >chain.pem
cat leaf.pem int.pem root.pem >chain3.pem
for f in leaf int root; do
	openssl x509 -in "$f.pem" -outform DER -out "$f.der"
done
# The server's key as the scalar the store keeps, 64 hexadecimal digits.
openssl ec -in leaf.key -text -noout 2>/dev/null | awk '
	/^priv:/ { on = 1; next }
	/^pub:/ { on = 0 }
	on { gsub(/[ :]/, ""); s = s $0 }
	END {
		if (length(s) == 66)
			s = substr(s, 3)
		while (length(s) < 64)
			s = "0" s
		print s
	}' >leaf.hex
openssl x509 -in leaf.pem -noout -fingerprint -sha256 | cut -d= -f2 >leaf.fp
