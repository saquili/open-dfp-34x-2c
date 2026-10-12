#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""test_tls.py POND KEYDIR: the daemon over HTTPS, as a browser meets it.

POND is the daemon built without ubus (pond-none), run on 127.0.0.1 with the
environment of the other tests (POND_ROOT, ONU_ROOT, POND_ONU_CONFIG); KEYDIR
is a directory to write its key in; its token is in POND_ROOT, where it
reads every file but its key. Python's ssl module is
OpenSSL, which takes only DER signatures, as BoringSSL does: the handshakes
check Mongoose's signature as Chrome would. Without a key, or with a key
that is not one, the daemon does not start.
"""

import hashlib
import http.client
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
