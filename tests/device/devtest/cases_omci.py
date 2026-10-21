# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""What an OLT may provision that the bench's OLT does not: messages of a stand-in OLT
written into the PON's inject file (CONFIG_PON_OMCI_INJECT, a RAM image's), which omcid
takes as the OLT's, while its answers still go to the bench's OLT.

The values are made up for the test: the names of example.org, numbers of the range
555-0100 to 555-0199 that no one is given, groups of 239.255.0.0/16. Each case takes away
what it made: a Delete of each instance it created, or a registration of the stick again,
which makes the OLT start the MIB anew.
"""

from __future__ import annotations

import ipaddress
import json
import time

from . import omci
from .core import Context, case, failed, passed, skipped, verdict

INJECT = "/sys/kernel/debug/pon/*/inject"
NO_INJECT = ("needs a kernel with the PON's inject file: make ram-image "
             "OPEN_DFP_34X_2C_RAM_KCONFIG=\"--enable DEBUG_FS --enable PON_OMCI_INJECT\"")

# The instances the stand-in OLT creates, apart from those an OLT numbers from 1.
OWN = 0x0F01
SIP_AUTH, LOGIN_AUTH = 0x0F01, 0x0F02
POTS = 0x0201
ALLOWED = ("239.255.1.0", "239.255.1.255")
INSIDE, OUTSIDE = "239.255.1.5", "239.255.2.5"
PHONE_VID = 600
SETTLE = 90.0


def ubus(ctx: Context, method: str, args: dict | None = None) -> dict:
    text = ctx.stick.text(f"ubus call omcid {method}" + (f" '{json.dumps(args)}'" if args else ""))
    return json.loads(text) if text else {}


def inject_path(ctx: Context) -> str | None:
    """The inject file, with debugfs mounted for it; None for a kernel without it."""
    reply = ctx.stick.run("grep -q ' /sys/kernel/debug ' /proc/mounts || "
                          "mount -t debugfs debugfs /sys/kernel/debug; ls " + INJECT)
    paths = reply.out.split()
    return paths[0] if reply.ok and paths else None


def rejected(ctx: Context) -> int:
    return int(ubus(ctx, "status")["frames"]["rejected"])


def inject(ctx: Context, path: str, messages: list[bytes]) -> None:
    """Each message into the inject file, a little apart, as an OLT sends them."""
    ctx.stick.run(f'while read -r h; do echo "$h" > {path} || exit 1; sleep 0.2; done',
                  stdin=omci.hexes(messages), check=True, timeout=10 + len(messages))


def instances(ctx: Context, class_id: int) -> dict[int, dict[str, str]]:
    """The instances of @class_id in omcid's MIB, by number: their attributes in hex."""
    mib = ubus(ctx, "mib", {"classes": [class_id], "max": 60000})
    return {m["instance"]: m["attrs"] for m in mib.get("mes", []) if m["class"] == class_id}


def group(address: str) -> int:
    """A group as ubus's command line gives omcid a 32-bit number: its bits as a signed
    one, since a JSON number above 2^31 - 1 becomes a 64-bit one there."""
    n = int(ipaddress.IPv4Address(address))
    return n - (1 << 32) if n >= 1 << 31 else n


@case("omci.olt-login", "OMCI",
      "A PPPoE login the OLT sends (class 148) is the status daemon's to give, its user, "
      "and its password only on request", tools=("ubus",))
def olt_login(ctx: Context):
    path = inject_path(ctx)
    if not path:
        return skipped(NO_INJECT)
    if instances(ctx, 148):
        return skipped("the OLT sent an authentication method of its own")
    olt, before = omci.Olt(), rejected(ctx)
    inject(ctx, path, [olt.create(148, OWN),
                       olt.set(148, OWN, a2="client@isp.example.org"),
                       olt.set(148, OWN, a3="example-login-secret"),
                       olt.set(148, OWN, a4="isp.example.org")])
    got = ubus(ctx, "login")
    inject(ctx, path, [olt.delete(148, OWN)])
    gone = ubus(ctx, "login")
    bad = rejected(ctx) - before
    ok = (got.get("user") == "client@isp.example.org" and got.get("secret") == "example-login-secret"
          and got.get("realm") == "isp.example.org" and not gone and bad == 0)
    return verdict(ok, f"login {got.get('user')!r} realm {got.get('realm')!r}, "
                       f"{'a' if got.get('secret') else 'no'} password; after the Delete "
                       f"{'none' if not gone else gone}; {bad} message(s) refused")


@case("omci.igmp-policy", "OMCI",
      "The OLT's multicast profile (classes 309 and 310) is the television's policy, and a "
      "join of a group outside its list is refused", tools=("ubus",))
def igmp_policy(ctx: Context):
    path = inject_path(ctx)
    if not path:
        return skipped(NO_INJECT)
    ports = [n for n, a in instances(ctx, 47).items() if a.get("3") == "01"]
    if not ports:
        return skipped("the OLT set up no bridge port of an Ethernet UNI")
    port = ports[0]
    if port in instances(ctx, 310) or OWN in instances(ctx, 309):
        return skipped("the OLT set up a multicast profile of its own")
    low, high = (ipaddress.IPv4Address(a).packed for a in ALLOWED)
    row = bytes.fromhex("4001") + (0x0FFF).to_bytes(2, "big") + bytes(6) + low + high + bytes(6)
    olt, before = omci.Olt(), rejected(ctx)
    inject(ctx, path, [olt.create(309, OWN, a1=3, a2=0, a3=1, a10=2, a12=125, a13=100),
                       olt.set(309, OWN, a7=row),
                       olt.create(310, port, a1=0, a2=OWN)])
    try:
        policy = ubus(ctx, "iptv").get("policy") or {}
        inside = ubus(ctx, "tv_test", {"group": group(INSIDE), "vlan": 0, "seconds": 1})
        ubus(ctx, "tv_test", {"stop": 1})
        outside = ubus(ctx, "tv_test", {"group": group(OUTSIDE), "vlan": 0, "seconds": 1})
    finally:
        inject(ctx, path, [olt.delete(310, port), olt.delete(309, OWN)])
    after = ubus(ctx, "iptv").get("policy")
    bad = rejected(ctx) - before
    allowed = [(a.get("from"), a.get("to")) for a in policy.get("allowed", [])]
    ok = (policy.get("igmp_version") == 3 and policy.get("mode") == "snoop"
          and policy.get("fast_leave") is True and allowed == [ALLOWED]
          and inside.get("ok") is True and outside.get("error") == "refused"
          and after is None and bad == 0)
    return verdict(ok, f"IGMPv{policy.get('igmp_version')} {policy.get('mode')}, allowed "
                       f"{allowed}; {INSIDE} {'taken' if inside.get('ok') else inside}, "
                       f"{OUTSIDE} {outside.get('error') or 'taken'} ({outside.get('reason')}); "
                       f"policy after the Deletes {after}; {bad} message(s) refused")


def registered(ctx: Context, pots: int) -> bool:
    """Registered again, in O5, with the phone socket in the MIB when @pots."""
    end = time.monotonic() + SETTLE
    while time.monotonic() < end:
        time.sleep(5)
        if " O5 " not in ctx.stick.run("pon").out:
            continue
        if (POTS in instances(ctx, 53)) == (pots > 0):
            return True
    return False


def reidentify(ctx: Context, pots: int) -> bool:
    ctx.stick.run(f"printf 'pots_ports={pots}\\n' | onu-config identity && onu-apply reidentify",
                  check=True, timeout=60)
    return registered(ctx, pots)


def phone_messages(olt: omci.Olt) -> list[bytes]:
    """A SIP line on the phone socket, as an OLT sets one up (G.988 9.9), a PPPoE login
    after its SIP account, and the IP host's voice tagged with VID 600."""
    return [
        olt.create(157, 1), olt.set(157, 1, a1=1, a2="sip.example.org"),
        olt.create(157, 2), olt.set(157, 2, a1=1, a2="+12025550142"),
        olt.create(157, 3), olt.set(157, 3, a1=1, a2="example.org"),
        olt.create(137, 1, a1=0xFFFF, a2=1),
        olt.create(136, 1, a1=5060, a2=17),
        olt.create(148, SIP_AUTH), olt.set(148, SIP_AUTH, a2="12025550142"),
        olt.set(148, SIP_AUTH, a3="example-sip-secret"),
        olt.create(148, LOGIN_AUTH), olt.set(148, LOGIN_AUTH, a2="client@isp.example.org"),
        olt.set(148, LOGIN_AUTH, a3="example-login-secret"),
        olt.create(150, 1, a1=1, a2=0xFFFF, a8=3, a10=1, a15=0xFFFF), olt.set(150, 1, a5=1),
        olt.create(143, 1, a1=50000, a2=50100, a3=46, a6=1),
        olt.create(58, 1, a1=3, a4=1, a14=0xFFFF),
        olt.create(142, 1, a2=1, a3=8, a4=20, a6=0, a7=20, a9=18, a10=20, a13=20, a15=1, a16=1),
        olt.create(153, 1, a1=1, a2=2, a4=SIP_AUTH, a5=0xFFFF, a7=0xFFFF, a8=0xFFFF,
                   a9=0xFFFF, a10=POTS),
        olt.set(153, 1, a3="Bench phone"),
        olt.create(139, 1, a1=1, a2=POTS, a3=1, a4=1),
        olt.set(138, 0, a2=1, a4=1),
        # The IP host's rule: untagged frames tagged with VID 600 at priority 5.
        olt.create(171, 0x0901, a1=3, a7=0),
        olt.set(171, 0x0901, a6=bytes.fromhex("f8000000f8000000000f0000")
                + ((5 << 16) | (PHONE_VID << 3)).to_bytes(4, "big")),
    ]


@case("omci.phone", "OMCI",
      "The phone the OLT sets up on a phone socket the stick tells of (G.988 9.9) is the "
      "status daemon's to give, its SIP password apart, the PPPoE login is not the SIP "
      "account, and the IP host's voice gets a host VLAN of its own", destructive=True,
      tools=("ubus",))
def phone(ctx: Context):
    path = inject_path(ctx)
    if not path:
        return skipped(NO_INJECT)
    if instances(ctx, 148) or instances(ctx, 153):
        return skipped("the OLT set up a phone or a login of its own")
    was = ctx.stick.text("onu-config identity-current")
    pots = int(dict(line.split("=", 1) for line in was.splitlines() if "=" in line)
               .get("pots_ports", "0"))
    if not reidentify(ctx, 1):
        reidentify(ctx, pots)
        return failed("the stick did not come back in O5 with its phone socket "
                      f"within {SETTLE:.0f} s")
    try:
        before = rejected(ctx)
        inject(ctx, path, phone_messages(omci.Olt()))
        line = ubus(ctx, "phone")
        secrets = ubus(ctx, "phone_secrets")
        login = ubus(ctx, "login")
        rules = ubus(ctx, "rules").get("rules", [])
        bad = rejected(ctx) - before
    finally:
        back = reidentify(ctx, pots)
    first = (line.get("lines") or [{}])[0]
    server = line.get("server") or {}
    made = [r for r in rules if r.get("fibre") == PHONE_VID and r.get("auto")]
    ok = (line.get("method") == "omci" and first.get("number") == "+12025550142"
          and first.get("display") == "Bench phone" and first.get("user") == "12025550142"
          and first.get("password_set") is True and "example-sip-secret" not in json.dumps(line)
          and server.get("proxy") == "sip.example.org" and server.get("port") == 5060
          and server.get("transport") == "udp" and len(line.get("codecs", [])) == 3
          and (secrets.get("lines") or [{}])[0].get("secret") == "example-sip-secret"
          and login.get("user") == "client@isp.example.org"
          and line.get("vlan") == {"fibre": PHONE_VID, "host": PHONE_VID}
          and len(made) == 1 and bad == 0 and back)
    return verdict(ok, f"line {first.get('number')} {first.get('display')!r} user "
                       f"{first.get('user')}, server {server.get('proxy')}:{server.get('port')}/"
                       f"{server.get('transport')}, codecs {line.get('codecs')}, SIP password "
                       f"{'given apart' if secrets.get('lines') else 'not given'}, login "
                       f"{login.get('user')}, voice VLAN {line.get('vlan')}, {len(made)} move "
                       f"made; {bad} message(s) refused; registered again with "
                       f"pots_ports={pots}: {'yes' if back else 'NO'}")
