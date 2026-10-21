#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""test_tls.py POND KEYDIR CERTS: the daemon over HTTPS, as a browser meets it.

POND is the daemon built without ubus (pond-none), run on 127.0.0.1 with the
environment of the other tests (POND_ROOT, ONU_ROOT, POND_ONU_CONFIG); KEYDIR
is a directory to write its key in; its token is in POND_ROOT, where it
reads every file but its key. Python's ssl module is
OpenSSL, which takes only DER signatures, as BoringSSL does: the handshakes
check Mongoose's signature as Chrome would. Without a key, or with a key
that is not one, the daemon does not start. CERTS holds the own certificates
of make-certs.sh, which an authority's root signed through an intermediate:
a client that trusts the root alone verifies the daemon's chain.
"""

import base64
import hashlib
import http.client
import json
import os
import pwd
import socket
import ssl
import subprocess
import sys
import time

HTTPS, HTTP = 18443, 18080
KEY = "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721"
TOKEN = "0123456789abcdef0123456789abcdef"
checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    print(f"  {'ok  ' if cond else 'FAIL'}  {what}")
    if not cond:
        fails += 1


def context():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


def started(proc, port):
    for _ in range(50):
        if proc.poll() is not None:
            return False
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def request(conn, method, path, body=None, headers=None):
    conn.request(method, path, body=body, headers=headers or {})
    r = conn.getresponse()
    return r.status, dict(r.getheaders()), r.read()


def main():
    pond, keydir = sys.argv[1], sys.argv[2]
    key, token = os.path.join(keydir, "key"), "/tls-token"
    with open(key, "w") as f:
        f.write(KEY + "\n")
    with open(os.environ["POND_ROOT"] + token, "w") as f:
        f.write(TOKEN + "\n")
    urls = ["-l", f"https://127.0.0.1:{HTTPS}", "-l", f"http://127.0.0.1:{HTTP}"]

    print("what the daemon refuses")
    r = subprocess.run([pond, *urls, "-t", token], capture_output=True, text=True, timeout=10)
    check(r.returncode == 1 and "needs a key" in r.stderr, "an https:// address without a key")
    bad = os.path.join(keydir, "bad")
    with open(bad, "w") as f:
        f.write("00" * 32 + "\n")
    r = subprocess.run([pond, *urls, "-k", bad], capture_output=True, text=True, timeout=10)
    check(r.returncode == 1 and "not a key of P-256" in r.stderr, "a key that is no scalar of the curve")

    printed = subprocess.run([pond, "-k", key, "-F", "127.0.0.1"], capture_output=True,
                             text=True, timeout=10)
    words = printed.stdout.split()
    check(printed.returncode == 0 and words[:2] == ["127.0.0.1", "SHA-256"] and len(words[2]) == 95,
          "-F prints the address's fingerprint for the console")

    proc = subprocess.Popen([pond, *urls, "-k", key, "-t", token], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        if not started(proc, HTTPS) or not started(proc, HTTP):
            check(False, "the daemon listens on both")
            return
        print("over HTTPS")
        conn = http.client.HTTPSConnection("127.0.0.1", HTTPS, context=context(), timeout=10)
        status, _, body = request(conn, "GET", "/api/health")
        check(status == 200 and b'"ok":true' in body, "/api/health answers")
        check(conn.sock.version() == "TLSv1.3", "in TLS 1.3")
        der = conn.sock.getpeercert(binary_form=True)
        conn.close()
        fp = ":".join(f"{b:02X}" for b in hashlib.sha256(der).digest())
        check(fp == words[2], "with the certificate whose fingerprint -F printed")

        conn = http.client.HTTPSConnection("127.0.0.1", HTTPS, context=context(), timeout=10)
        status, _, _ = request(conn, "POST", "/api/auth/login",
                               '{"user":"root","secret":"admin"}',
                               {"Content-Type": "text/plain"})
        check(status == 403, "a sign-in without X-Pond, as a form of another site sends it, is refused")
        status, headers, body = request(conn, "POST", "/api/auth/login",
                                        '{"user":"root","secret":"admin"}',
                                        {"Content-Type": "application/json", "X-Pond": "1"})
        cookie = headers.get("Set-Cookie", "")
        check(status == 200 and "pond_sid=" in cookie and "; Secure" in cookie,
              "a sign-in sets a Secure cookie")
        status, _, body = request(conn, "GET", "/api/get/tls",
                                  headers={"Authorization": f"Bearer {TOKEN}"})
        check(status == 200 and words[2].encode() in body and b'"port":18443' in body
              and b'"https":true' in body, "/api/get/tls shows the same fingerprint")
        conn.close()

        ok = 0
        for _ in range(100):
            try:
                c = http.client.HTTPSConnection("127.0.0.1", HTTPS, context=context(), timeout=10)
                ok += request(c, "GET", "/api/health")[0] == 200
                c.close()
            except (OSError, ssl.SSLError):
                pass
        check(ok == 100, f"100 handshakes in a row: {ok} ok")

        print("over HTTP")
        conn = http.client.HTTPConnection("127.0.0.1", HTTP, timeout=10)
        status, headers, _ = request(conn, "GET", "/settings?tab=lan",
                                     headers={"Host": "192.0.2.1:18080"})
        check(status == 308 and headers.get("Location") == f"https://192.0.2.1:{HTTPS}/settings?tab=lan",
              "a page is sent on to HTTPS, same host and path, the HTTPS port")
        conn.close()
        conn = http.client.HTTPConnection("127.0.0.1", HTTP, timeout=10)
        status, headers, _ = request(conn, "POST", "/api/auth/login",
                                     '{"user":"root","secret":"admin"}',
                                     {"Host": "a b", "Content-Type": "application/json"})
        check(status == 308 and headers.get("Location") == f"https://127.0.0.1:{HTTPS}/api/auth/login"
              and "Set-Cookie" not in headers,
              "a sign-in too, before it is read; a Host that is no host gives the address")
        conn.close()
        conn = http.client.HTTPConnection("127.0.0.1", HTTP, timeout=10)
        status, _, body = request(conn, "GET", "/api/health")
        check(status == 200 and b'"ok":true' in body, "but /api/health answers over HTTP")
        conn.close()
    finally:
        proc.terminate()
        proc.wait(5)

    as_account(pond)
    slow_change(pond, os.path.dirname(key))
    token_file(pond)
    downloads(pond, os.path.dirname(key), token)
    own_file(pond, os.path.dirname(key), sys.argv[3])
    servers_keys(pond, os.path.dirname(key), sys.argv[3])


# A second scalar of P-256, for a new key of the panel.
KEY2 = "11" * 32

# The header of root's password, which a change of the servers' keys takes,
# and root's, the default of the tests' store.
CONFIRM = "X-Pond-Password"
ROOT = "admin"

# A stand-in for onu-config, of the keys' verbs, which writes the daemon's
# files as onu-config does, and says what it was asked; root's password the
# real one judges.
STUB = r"""#!/bin/sh
D=@DIR@
echo "$*" >>"$D/calls"
case "$1 ${2:-}" in
"password-check ")
	exec @REAL@ "$@"
	;;
"tls new")
	[ -e "$D/slow" ] && sleep 1
	if [ -e "$D/fail" ]; then
		echo "onu-config: import TLS_KEY: the store has no room for it, of its 64 KiB" >&2
		echo "onu-config: the new key is not kept: the store refused it" >&2
		exit 1
	fi
	cat "$D/next.key" >"$D/tls.key"
	;;
"tls own")
	in=$(cat)
	{
		printf '%s\n' "$in" | sed -n 's/^KEY=//p'
		printf '%s\n' "$in" | sed -n 's/^CHAIN=//p'
	} >"$D/tls.own"
	;;
"tls self-signed")
	rm -f "$D/tls.own"
	;;
"ssh show")
	[ -e "$D/fail" ] && exit 1
	[ -e "$D/nohost" ] || printf 'TYPE=ssh-ed25519\nFINGERPRINT=SHA256:host\n'
	printf 'ONLY=1\nKEY=ssh-ed25519 256 SHA256:one me@laptop\nKEY=garbled\n'
	;;
"ssh new")
	printf 'TYPE=ssh-ed25519\nFINGERPRINT=SHA256:new\n'
	;;
"ssh-keys add")
	in=$(cat)
	case $in in
	ssh-ed25519*) echo "KEY=ssh-ed25519 256 SHA256:two ${in##* }" ;;
	*) echo "onu-config: a key of the type ${in%% *}" >&2; exit 1 ;;
	esac
	;;
"ssh-keys remove")
	[ -e "$D/fail" ] && exit 1
	cat >>"$D/removed"
	;;
"ssh-keys only")
	echo "$3" >"$D/only"
	;;
*)
	exit 2
	;;
esac
"""


def read(path):
    with open(path) as f:
        return f.read()


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def served(ctx=None):
    """The fingerprint of the certificate a new connection gets, and how many
    certificates the daemon sent, where Python can say (3.13 on)."""
    c = http.client.HTTPSConnection("127.0.0.1", HTTPS, context=ctx or context(), timeout=10)
    c.connect()
    der = c.sock.getpeercert(binary_form=True)
    sent = len(c.sock.get_unverified_chain()) if hasattr(c.sock, "get_unverified_chain") else 0
    c.close()
    return ":".join(f"{b:02X}" for b in hashlib.sha256(der).digest()), sent


def own_file(pond, workdir, certs):
    """An own certificate of the file of -c, as onu-config writes it: served
    on every address with its chain, which a client that trusts the root
    alone verifies by the address; printed for the console; and, when its key
    is another, or the file is not there, the panel's own served instead."""
    print("an own certificate of the file of -c")
    d = os.path.join(workdir, "own-file")
    os.makedirs(d, exist_ok=True)
    keyf, ownf = os.path.join(d, "tls.key"), os.path.join(d, "tls.own")
    write(keyf, KEY + "\n")
    chain = b"".join(open(os.path.join(certs, n), "rb").read() for n in ("leaf.der", "int.der"))
    leaf_fp = read(os.path.join(certs, "leaf.fp")).strip()
    mine = subprocess.run([pond, "-k", keyf, "-F", "127.0.0.1"], capture_output=True, text=True,
                          timeout=10).stdout.split()[2]
    trust = ssl.create_default_context(cafile=os.path.join(certs, "root.pem"))
    urls = ["-l", f"https://127.0.0.1:{HTTPS}", "-l", f"http://127.0.0.1:{HTTP}"]
    for key, what in ((read(os.path.join(certs, "leaf.hex")).strip(), "its own key"),
                      (KEY, "another key"), (None, "no file")):
        if key:
            write(ownf, key + "\n" + base64.b64encode(chain).decode() + "\n")
        elif os.path.exists(ownf):
            os.unlink(ownf)
        log = open(os.path.join(d, "log"), "w")
        proc = subprocess.Popen([pond, *urls, "-k", keyf, "-c", ownf, "-t", "/tls-token"],
                                stdout=subprocess.DEVNULL, stderr=log)
        try:
            if not started(proc, HTTPS):
                check(False, "the daemon listens")
                continue
            if what == "its own key":
                got, sent = served(trust)
                check(got == leaf_fp and sent in (0, 2),
                      f"with {what}: served with its chain, {sent} certificates, which a "
                      "client that trusts the root verifies by the name 127.0.0.1")
                conn = http.client.HTTPSConnection("127.0.0.1", HTTPS, context=context(), timeout=10)
                status, _, body = request(conn, "GET", "/api/get/tls",
                                          headers={"Authorization": f"Bearer {TOKEN}"})
                conn.close()
                tls = json.loads(body)["value"]
                check(status == 200 and tls["own"]["chain"] == 2 and tls["own"]["sha256"] == leaf_fp
                      and tls["certificates"][0]["sha256"] == leaf_fp
                      and tls["certificates"][0]["named"] is True,
                      "/api/get/tls says it, and that it names the address")
            else:
                check(served()[0] == mine, f"with {what}: the panel's own certificate is served")
        finally:
            proc.terminate()
            proc.wait(5)
            log.close()
        said = read(os.path.join(d, "log"))
        if what == "another key":
            check("the own certificate of" in said and "the key is not the certificate's" in said,
                  "and the daemon says why")
        if what == "its own key":
            printed = subprocess.run([pond, "-k", keyf, "-c", ownf, "-F", "127.0.0.1"],
                                     capture_output=True, text=True, timeout=10).stdout.split()
            check(printed[:3] == ["127.0.0.1", "SHA-256", leaf_fp]
                  and printed[4].startswith("2036-01-01"),
                  "-F prints the own certificate's fingerprint and end, for the console")
        if what == "no file":
            check("own certificate" not in said, "a file that is not there is nothing to say")


def servers_keys(pond, workdir, certs):
    """The keys of the ssh server and of the panel, changed through onu-config
    with root's password, what is asked and answered, and what a browser
    then meets, the new certificate after the answer, the chain of an own
    certificate whole, and the self-signed ones again."""
    print("the servers' keys and an own certificate")
    d = os.path.join(workdir, "keys")
    os.makedirs(d, exist_ok=True)
    for name in os.listdir(d):
        os.unlink(os.path.join(d, name))
    keyf, ownf, stub = os.path.join(d, "tls.key"), os.path.join(d, "tls.own"), os.path.join(d, "stub")
    write(keyf, KEY + "\n")
    write(os.path.join(d, "next.key"), KEY2 + "\n")
    write(stub, STUB.replace("@DIR@", d).replace("@REAL@", os.environ["POND_ONU_CONFIG"]))
    os.chmod(stub, 0o755)
    env = dict(os.environ, POND_ONU_CONFIG=stub)
    fp = {}
    for name, k in (("one", KEY), ("two", KEY2)):
        write(os.path.join(d, name), k + "\n")
        out = subprocess.run([pond, "-k", os.path.join(d, name), "-F", "127.0.0.1"],
                             capture_output=True, text=True, timeout=10).stdout.split()
        fp[name] = out[2]
    log = open(os.path.join(d, "log"), "w")
    urls = ["-l", f"https://127.0.0.1:{HTTPS}", "-l", f"http://127.0.0.1:{HTTP}"]
    proc = subprocess.Popen([pond, *urls, "-k", keyf, "-c", ownf, "-t", "/tls-token"], env=env,
                            stdout=subprocess.DEVNULL, stderr=log)
    auth = {"Authorization": f"Bearer {TOKEN}"}
    pw = dict(auth, **{CONFIRM: ROOT})

    def ask(method, path, body=None, headers=None, conn=None):
        c = conn or http.client.HTTPSConnection("127.0.0.1", HTTPS, context=context(), timeout=10)
        status, _, reply = request(c, method, path, body, headers or pw)
        if not conn:
            c.close()
        try:
            return status, json.loads(reply)
        except ValueError:
            return status, {}

    try:
        if not started(proc, HTTPS):
            check(False, "the daemon listens")
            return
        status, tls = ask("GET", "/api/get/tls", headers=auth)
        check(status == 200 and tls["value"]["own"] is None
              and tls["value"]["certificates"][0]["named"] is True
              and tls["value"]["certificates"][0]["sha256"] == fp["one"],
              "no own certificate: the one of the panel's key, which names the address")

        print("  the ssh server")
        status, ssh = ask("GET", "/api/get/ssh", headers=auth)
        v = ssh.get("value", {})
        check(status == 200 and v.get("host_key") == {"type": "ssh-ed25519", "fingerprint": "SHA256:host"}
              and v.get("keys_only") is True and v.get("keys_max") == 8
              and v.get("keys", [{}])[0] == {"type": "ssh-ed25519", "bits": 256,
                                              "fingerprint": "SHA256:one", "comment": "me@laptop"}
              and v["keys"][1]["type"] == "garbled" and v["keys"][1]["comment"] == "",
              "the host key, the keys and keys alone, as onu-config says them")
        write(os.path.join(d, "nohost"), "")
        status, ssh = ask("GET", "/api/get/ssh", headers=auth)
        check(status == 200 and ssh["value"]["host_key"] is None, "no host key: null")
        write(os.path.join(d, "fail"), "")
        status, _ = ask("GET", "/api/get/ssh", headers=auth)
        check(status == 500, "onu-config failing: no value")
        os.unlink(os.path.join(d, "fail"))

        status, body = ask("POST", "/api/ssh/keys", "ssh-ed25519 AAAA me@desk\n", auth)
        check(status == 403 and body.get("error") == "wrong password",
              "a change of a key without root's password is refused")
        status, body = ask("POST", "/api/ssh/keys", "ssh-ed25519 AAAA me@desk\n",
                           dict(auth, **{CONFIRM: "not%20it"}))
        check(status == 403, "and with a wrong one")
        status, body = ask("POST", "/api/ssh/keys", "ssh-ed25519 AAAA me@desk\n")
        check(status == 200 and body.get("key") == {"type": "ssh-ed25519", "bits": 256,
                                                    "fingerprint": "SHA256:two",
                                                    "comment": "me@desk"},
              "a key added, said back")
        check("ssh-keys add" in read(os.path.join(d, "calls")) and "admin" not in read(os.path.join(d, "calls")),
              "onu-config ssh-keys add, the password never among its arguments")
        status, body = ask("POST", "/api/ssh/keys", "ecdsa-sha2-nistp999 AAAA x")
        check(status == 400 and body.get("detail") == "a key of the type ecdsa-sha2-nistp999",
              "a key onu-config refuses: its reason")
        status, body = ask("POST", "/api/ssh/keys", " \n")
        check(status == 400 and body.get("error") == "not a key", "no key")
        status, body = ask("POST", "/api/ssh/keys", "a" * 5000)
        check(status == 400 and body.get("error") == "not a key", "a line too long")
        status, body = ask("POST", "/api/ssh/keys/remove", "SHA256:two=\n")
        check(status == 200 and read(os.path.join(d, "removed")) == "SHA256:two=\n",
              "a key removed by its fingerprint, on onu-config's input")
        status, body = ask("POST", "/api/ssh/keys/remove", "SHA256:no such key")
        check(status == 400 and body.get("error") == "not a fingerprint", "no fingerprint")
        write(os.path.join(d, "fail"), "")
        status, body = ask("POST", "/api/ssh/keys/remove", "SHA256:two")
        check(status == 400 and body.get("detail") == "onu-config refused it",
              "refused without a word: said so")
        os.unlink(os.path.join(d, "fail"))
        for state in ("on", "off"):
            status, _ = ask("POST", "/api/ssh/keys-only", state + "\n")
            check(status == 200 and read(os.path.join(d, "only")) == state + "\n", f"keys alone {state}")
        status, body = ask("POST", "/api/ssh/keys-only", "maybe")
        check(status == 400 and body.get("error") == "not on or off", "keys alone, neither on nor off")
        status, body = ask("POST", "/api/ssh/host-key", "")
        check(status == 200 and body.get("host_key") == {"type": "ssh-ed25519", "fingerprint": "SHA256:new"},
              "a new host key, and its fingerprint")
        status, _ = ask("GET", "/api/ssh/host-key", headers=pw)
        check(status == 405, "a change is a POST")
        status, _ = ask("POST", "/api/ssh/keys", "a" * (33 * 1024))
        check(status == 413, "a body of more than 32 KiB")

        print("  the panel's key")
        conn = http.client.HTTPSConnection("127.0.0.1", HTTPS, context=context(), timeout=10)
        status, body = ask("POST", "/api/tls/key", "", conn=conn)
        check(status == 200 and body["tls"]["certificates"][0]["sha256"] == fp["two"]
              and read(keyf) == KEY2 + "\n", "a new key: the answer gives the new certificate")
        old = ":".join(f"{b:02X}" for b in hashlib.sha256(conn.sock.getpeercert(binary_form=True)).digest())
        status, _ = ask("GET", "/api/health", headers=auth, conn=conn)
        check(status == 200 and old == fp["one"],
              "the connection that asked goes on, with the certificate it had")
        conn.close()
        check(served()[0] == fp["two"], "a new connection gets the new one")
        write(os.path.join(d, "fail"), "")
        status, body = ask("POST", "/api/tls/key", "")
        check(status == 400 and body.get("detail") ==
              "import TLS_KEY: the store has no room for it, of its 64 KiB",
              "a key the store has no room for: the cause")
        os.unlink(os.path.join(d, "fail"))
        # Two at once: the second waits for none, and is told so.
        write(os.path.join(d, "slow"), "")
        write(os.path.join(d, "next.key"), KEY + "\n")
        first = http.client.HTTPSConnection("127.0.0.1", HTTPS, context=context(), timeout=10)
        first.request("POST", "/api/tls/key", body="", headers=pw)
        time.sleep(0.8)
        status, body = ask("POST", "/api/tls/key", "")
        check(status == 409 and body.get("error") == "busy", "a second change while one runs: 409")
        r = first.getresponse()
        r.read()
        first.close()
        check(r.status == 200 and served()[0] == fp["one"], "the first is answered when it ends")
        # The one who asked went away once onu-config ran: the change is made all the same.
        write(os.path.join(d, "next.key"), KEY2 + "\n")
        asked = read(os.path.join(d, "calls")).count("tls new")
        gone = http.client.HTTPSConnection("127.0.0.1", HTTPS, context=context(), timeout=10)
        gone.request("POST", "/api/tls/key", body="", headers=pw)
        for _ in range(50):
            if read(os.path.join(d, "calls")).count("tls new") > asked:
                break
            time.sleep(0.1)
        gone.close()
        time.sleep(1.5)
        os.unlink(os.path.join(d, "slow"))
        check(served()[0] == fp["two"], "a change whose asker went is made")
        # A key that is none: the one before stays, until a key comes.
        write(os.path.join(d, "next.key"), "zz\n")
        status, _ = ask("POST", "/api/tls/key", "")
        check(status == 200 and served()[0] == fp["two"], "a key file that is no key: the one before")
        write(os.path.join(d, "next.key"), KEY2 + "\n")
        status, _ = ask("POST", "/api/tls/key", "")
        check(status == 200 and read(keyf) == KEY2 + "\n", "and a key again")

        print("  an own certificate")
        leaf, chain, key = (read(os.path.join(certs, n)) for n in ("leaf.pem", "chain.pem", "leaf.key"))
        trust = ssl.create_default_context(cafile=os.path.join(certs, "root.pem"))
        status, body = ask("POST", "/api/tls/certificate", json.dumps({"certificate": leaf, "key": key}))
        check(status == 200 and body["tls"]["own"]["chain"] == 1, "the server's certificate alone is taken")
        try:
            served(trust)
            alone = "verified"
        except ssl.SSLCertVerificationError as e:
            alone = e.verify_message
        check(alone == "unable to get local issuer certificate",
              f"without the CA certificate between, a client that trusts the root cannot verify it: {alone}")
        status, body = ask("POST", "/api/tls/certificate", json.dumps({"certificate": chain, "key": key}))
        own = body.get("tls", {}).get("own") or {}
        leaf_fp = read(os.path.join(certs, "leaf.fp")).strip()
        check(status == 200 and own.get("chain") == 2 and own.get("sha256") == leaf_fp
              and own.get("subject") == "O=Example, CN=stick.example"
              and own.get("issuer") == "O=Example CA, CN=Example Intermediate"
              and own.get("not_after") == "2036-01-01T00:00:00Z"
              and "IP:192.0.2.254" in own.get("names", [])
              and body["tls"]["certificates"][0]["named"] is True
              and body["tls"]["certificates"][0]["sha256"] == leaf_fp,
              "with its chain: taken, and said back")
        got, sent = served(trust)
        check(got == leaf_fp and sent in (0, 2),
              f"a client that trusts the root verifies it by the name 127.0.0.1; {sent} certificates sent")
        status, body = ask("POST", "/api/tls/certificate",
                           json.dumps({"certificate": read(os.path.join(certs, "rsa.pem")),
                                       "key": read(os.path.join(certs, "rsa.key"))}))
        check(status == 400 and "RSA" in body.get("detail", ""), "a certificate of an RSA key is refused")
        status, body = ask("POST", "/api/tls/certificate", json.dumps({"certificate": chain}))
        check(status == 400 and body.get("error") == "not a certificate", "one without its key")
        check(served(trust)[0] == leaf_fp, "the refused leave the own one in force")
        status, body = ask("POST", "/api/tls/self-signed", "")
        check(status == 200 and body["tls"]["own"] is None and not os.path.exists(ownf)
              and served()[0] == fp["two"], "back to the certificates of the panel's key")
    finally:
        proc.terminate()
        proc.wait(5)
        log.close()


def downloads(pond, workdir, token):
    """A file the daemon hands out is named after the product name of the
    stick's own settings, BuchOS without one or with one that is no name,
    and the running bank's version."""
    print("the names of the files handed out")
    update = os.path.join(workdir, "onu-update")
    with open(update, "w") as f:
        f.write("#!/bin/sh\ncase $1 in\n"
                "status) printf 'RUNNING=0\\nBANK0_VERSION=2026.10.20-0130\\n' ;;\n"
                "read) printf ONUF ;;\nesac\n")
    os.chmod(update, 0o755)
    system = os.environ["POND_ROOT"] + "/run/onu-system"
    os.makedirs(os.path.dirname(system), exist_ok=True)
    env = dict(os.environ, POND_ONU_UPDATE=update)
    proc = subprocess.Popen([pond, "-l", f"http://127.0.0.1:{HTTP}", "-t", token],
                            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not started(proc, HTTP):
            check(False, "the daemon listens")
            return
        for line, name in ((None, "buchos-gpon-2026.10.20-0130.bin"),
                           ("PRODUCT_NAME=Fibre Box 2.0",
                            "fibre-box-2-0-gpon-2026.10.20-0130.bin"),
                           ("PRODUCT_NAME=<b>x</b>", "buchos-gpon-2026.10.20-0130.bin")):
            if line is None:
                if os.path.exists(system):
                    os.unlink(system)
            else:
                with open(system, "w") as f:
                    f.write(f"HOSTNAME=stick\n{line}\n")
            conn = http.client.HTTPConnection("127.0.0.1", HTTP, timeout=10)
            status, headers, body = request(conn, "GET", "/api/firmware/download",
                                            headers={"Authorization": f"Bearer {TOKEN}"})
            conn.close()
            check(status == 200 and body == b"ONUF"
                  and headers.get("Content-Disposition") == f'attachment; filename="{name}"',
                  f"{line or 'no product name'}: {name}")
    finally:
        proc.terminate()
        proc.wait(5)
        if os.path.exists(system):
            os.unlink(system)


def slow_change(pond, workdir):
    """A change that takes onu-act's program seconds is answered when it ends,
    and the daemon answers the other requests meanwhile."""
    print("a slow change")
    lane = os.path.join(workdir, "onu-lane")
    with open(lane, "w") as f:
        f.write("#!/bin/sh\n[ \"$1\" = try ] && sleep 2\necho \"onu-lane $1\"\n")
    os.chmod(lane, 0o755)
    env = dict(os.environ, POND_ONU_LANE=lane)
    proc = subprocess.Popen([pond, "-l", f"http://127.0.0.1:{HTTP}", "-t", "/tls-token"],
                            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not started(proc, HTTP):
            check(False, "the daemon listens")
            return
        auth = {"Authorization": f"Bearer {TOKEN}"}
        slow = http.client.HTTPConnection("127.0.0.1", HTTP, timeout=10)
        begun = time.monotonic()
        slow.request("POST", "/api/set/sfp_lane", body="MODE=sgmii\n", headers=auth)
        time.sleep(0.3)
        quick = http.client.HTTPConnection("127.0.0.1", HTTP, timeout=10)
        asked = time.monotonic()
        status, _, _ = request(quick, "GET", "/api/health")
        took = time.monotonic() - asked
        quick.close()
        check(status == 200 and took < 1.0,
              f"/api/health answers while the lane's program runs: in {took:.2f} s")
        r = slow.getresponse()
        body, waited = r.read(), time.monotonic() - begun
        slow.close()
        check(r.status == 200 and b'"ok":true' in body and waited >= 2.0,
              f"the change is answered when the program has ended: {r.status} after {waited:.1f} s")
    finally:
        proc.terminate()
        proc.wait(5)


def token_file(pond):
    """The token of scripts is read from its file at each request: one made,
    replaced or taken away while the daemon runs holds or stops holding at
    once, and a daemon told of no file takes none."""
    print("the token of scripts")
    path = "/reload-token"
    full = os.environ["POND_ROOT"] + path
    first, second = "a" * 64, "b" * 64

    def status(port, token=None):
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        headers = {"Authorization": f"Bearer {token}"} if token else {}
        code = request(conn, "GET", "/api/schema", headers=headers)[0]
        conn.close()
        return code

    def put(token):
        with open(full + ".new", "w") as f:
            f.write(token + "\n")
        os.replace(full + ".new", full)

    if os.path.exists(full):
        os.unlink(full)
    proc = subprocess.Popen([pond, "-l", f"http://127.0.0.1:{HTTP}", "-t", path],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not started(proc, HTTP):
            check(False, "the daemon listens")
            return
        check(status(HTTP, first) == 401, "without the file, a token is none")
        put(first)
        check(status(HTTP, first) == 200, "a token made while the daemon runs holds at once")
        check(status(HTTP, second) == 401 and status(HTTP, first[:-1]) == 401
              and status(HTTP, first + "a") == 401 and status(HTTP) == 401,
              "another, a shorter, a longer one and none do not")
        put(second)
        check(status(HTTP, second) == 200 and status(HTTP, first) == 401,
              "a token replaced: the new holds, the old does not")
        os.unlink(full)
        check(status(HTTP, second) == 401, "a token taken away stops holding")
    finally:
        proc.terminate()
        proc.wait(5)
        if os.path.exists(full):
            os.unlink(full)

    put(first)
    proc = subprocess.Popen([pond, "-l", f"http://127.0.0.1:{HTTP}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not started(proc, HTTP):
            check(False, "the daemon listens")
            return
        check(status(HTTP, first) == 401, "told of no file (no -t), a daemon takes no token")
    finally:
        proc.terminate()
        proc.wait(5)
        os.unlink(full)


def proc_status(pid):
    with open(f"/proc/{pid}/status") as f:
        return dict(line.rstrip("\n").split(":\t", 1) for line in f if ":\t" in line)


def as_account(pond):
    """-u: started as root, the daemon runs as the account, with the one power
    to listen on a port below 1024."""
    if os.geteuid() != 0:
        print("as another account: skipped, not root")
        return
    print("as another account")
    nobody = pwd.getpwnam("nobody")
    proc = subprocess.Popen([pond, "-u", "nobody", "-l", "http://127.0.0.1:80"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not started(proc, 80):
            check(False, "the daemon listens on port 80 as nobody")
            return
        st = proc_status(proc.pid)
        check(st["Uid"].split()[0] == str(nobody.pw_uid)
              and st["Gid"].split()[0] == str(nobody.pw_gid), "it runs as nobody")
        check(int(st["CapEff"], 16) == 1 << 10 and int(st["CapPrm"], 16) == 1 << 10,
              "with CAP_NET_BIND_SERVICE alone")
        check(st.get("NoNewPrivs") == "1", "and no new privileges")
        conn = http.client.HTTPConnection("127.0.0.1", 80, timeout=10)
        status, _, body = request(conn, "GET", "/api/health")
        check(status == 200 and b'"ok":true' in body, "/api/health answers on port 80")
        conn.close()
    finally:
        proc.terminate()
        proc.wait(5)
    r = subprocess.run([pond, "-u", "no-such-account", "-l", "http://127.0.0.1:18081"],
                       capture_output=True, text=True, timeout=10)
    check(r.returncode == 1 and "no account no-such-account" in r.stderr,
          "an account that does not exist is refused")


main()
print(f"test_tls: {checks} checks, {fails} failed")
sys.exit(1 if fails else 0)
