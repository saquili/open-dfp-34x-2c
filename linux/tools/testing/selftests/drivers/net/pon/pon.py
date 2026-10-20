#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Tests of the PON class and its generic netlink family, over pon_sim, a
stand-in ONU MAC that has no hardware (drivers/net/pon/pon_sim.c).

The family is driven with YNL; the "gem" links are made with plain rtnetlink,
as iproute2 does not know the kind; the OMCI port is a character device.
"""

import errno
import json
import os
import select
import socket
import struct
import sys
import time
from pathlib import Path

KSFT_DIR = Path(__file__).resolve().parents[3]
sys.path.insert(0, (KSFT_DIR / "net").as_posix())

# pylint: disable=wrong-import-position
from lib.py import ksft_run, ksft_exit, ksft_eq, ksft_ne, ksft_true, ksft_in
from lib.py import ksft_ge, ksft_lt
from lib.py import KsftSkipEx, NlError, YnlFamily, cmd, ip
from lib.py.ynl import SPEC_PATH

DFS = Path("/sys/kernel/debug/pon_sim")

NUM_FLOWS = 16
NUM_TCONTS = 4
NUM_QUEUES = 4
OMCC_PORT_ID = 4095

NLM_F_REQUEST = 1
NLM_F_ACK = 4
NLM_F_EXCL = 0x200
NLM_F_CREATE = 0x400
NLM_F_CAPPED = 0x100
NLM_F_ACK_TLVS = 0x200
NLMSG_ERROR = 2
RTM_NEWLINK = 16
IFLA_IFNAME = 3
IFLA_LINK = 5
IFLA_LINKINFO = 18
IFLA_INFO_KIND = 1
IFLA_INFO_DATA = 2
IFLA_GEM_PORT_ID = 1
IFLA_GEM_DIRECTION = 2
IFLA_GEM_TCONT = 3
IFLA_GEM_MULTICAST = 4
IFLA_GEM_QUEUE = 5
IFLA_GEM_WEIGHT = 6
IFLA_GEM_CIR = 7
IFLA_GEM_PIR = 8
IFLA_GEM_PBITS = 9
IFLA_GEM_DS_RATE = 10
IFLA_GEM_DS_BURST = 11
DIR_UP, DIR_DOWN, DIR_BOTH = 1, 2, 3
NLMSGERR_ATTR_MSG = 1
SOL_NETLINK = 270
NETLINK_EXT_ACK = 11


class PonFamily(YnlFamily):
    def __init__(self, recv_size=0):
        super().__init__((SPEC_PATH / "pon.yaml").as_posix(), schema='',
                         recv_size=recv_size)


class LinkError(Exception):
    """A refusal by the kernel: its errno and the message it gave."""

    def __init__(self, error, msg):
        super().__init__(f"{errno.errorcode.get(error, error)}: {msg}")
        self.error = error
        self.msg = msg


def nla(kind, data=b""):
    """One netlink attribute, padded to four bytes."""
    attr = struct.pack("HH", 4 + len(data), kind) + data
    return attr + b"\0" * (-len(attr) % 4)


def gem_link_attrs(**kw):
    """The attributes in IFLA_INFO_DATA of the gem link kind."""
    packers = {
        "port_id": (IFLA_GEM_PORT_ID, "H"), "direction": (IFLA_GEM_DIRECTION, "B"),
        "tcont": (IFLA_GEM_TCONT, "I"), "queue": (IFLA_GEM_QUEUE, "B"),
        "weight": (IFLA_GEM_WEIGHT, "B"), "cir": (IFLA_GEM_CIR, "Q"),
        "pir": (IFLA_GEM_PIR, "Q"), "pbits": (IFLA_GEM_PBITS, "B"),
        "ds_rate": (IFLA_GEM_DS_RATE, "Q"), "ds_burst": (IFLA_GEM_DS_BURST, "Q"),
    }
    data = b""
    for name, val in kw.items():
        if name == "multicast":
            if val:
                data += nla(IFLA_GEM_MULTICAST)
            continue
        kind, fmt = packers[name]
        data += nla(kind, struct.pack(fmt, val))
    return data


def rtnl_gem(flags, attrs):
    """Send one RTM_NEWLINK for a gem link and return nothing or raise LinkError."""
    sock = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, socket.NETLINK_ROUTE)
    sock.setsockopt(SOL_NETLINK, NETLINK_EXT_ACK, 1)
    try:
        ifi = struct.pack("BxHiII", socket.AF_UNSPEC, 0, attrs["index"], 0, 0)
        body = ifi + attrs["head"] + nla(IFLA_LINKINFO,
                                          nla(IFLA_INFO_KIND, b"gem\0") +
                                          nla(IFLA_INFO_DATA, attrs["data"]))
        msg = struct.pack("IHHII", 16 + len(body), RTM_NEWLINK,
                          NLM_F_REQUEST | NLM_F_ACK | flags, 1, 0) + body
        sock.send(msg)
        reply = sock.recv(4096)
    finally:
        sock.close()
    length, mtype, mflags, _, _ = struct.unpack_from("IHHII", reply, 0)
    if mtype != NLMSG_ERROR:
        raise RuntimeError("no acknowledgment")
    error = struct.unpack_from("i", reply, 16)[0]
    if not error:
        return
    off = 20 + 16
    if not mflags & NLM_F_CAPPED:
        off = 20 + struct.unpack_from("I", reply, 20)[0]
    text = ""
    while mflags & NLM_F_ACK_TLVS and off + 4 <= length:
        alen, akind = struct.unpack_from("HH", reply, off)
        if alen < 4:
            break
        if (akind & 0x3fff) == NLMSGERR_ATTR_MSG:
            text = reply[off + 4:off + alen].split(b"\0")[0].decode()
        off += (alen + 3) & ~3
    raise LinkError(-error, text)


def gem_add(name, lower, **kw):
    attrs = {"index": 0, "data": gem_link_attrs(**kw),
             "head": nla(IFLA_IFNAME, name.encode() + b"\0") +
                     nla(IFLA_LINK, struct.pack("I", socket.if_nametoindex(lower)))}
    rtnl_gem(NLM_F_CREATE | NLM_F_EXCL, attrs)


def gem_set(name, **kw):
    attrs = {"index": socket.if_nametoindex(name), "data": gem_link_attrs(**kw),
             "head": b""}
    rtnl_gem(0, attrs)


def dfs_read(name):
    return (DFS / name).read_text()


def dfs_write(name, val):
    (DFS / name).write_text(f"{val}\n")


def calls():
    """The counters of the stand-in's "calls" file."""
    return {k: int(v) for k, v in (line.split() for line in dfs_read("calls").splitlines())}


def flows():
    """The flows of the stand-in: {flow: (port_id, tcont, direction, ds_rate, ds_burst)}."""
    out = {}
    for line in dfs_read("flows").splitlines():
        f, port, tcont, direction, ds_rate, ds_burst = (int(x) for x in line.split())
        out[f] = (port, tcont, direction, ds_rate, ds_burst)
    return out


class Sim:
    """The stand-in MAC, as the family and the system show it."""

    def __init__(self):
        if not DFS.exists():
            cmd("modprobe pon_sim", fail=False)
        if not DFS.exists():
            raise KsftSkipEx("pon_sim is not there: CONFIG_PON_SIM, as a module or built in")
        self.nf = PonFamily()
        self.ifname = next(p.name for p in Path("/sys/class/net").iterdir()
                           if p.name.startswith("ponsim"))
        self.ifindex = socket.if_nametoindex(self.ifname)
        ip(f"link set dev {self.ifname} up")
        self.dev = next(d for d in self.nf.dev_get({}, dump=True)
                        if d.get("ifindex") == self.ifindex)
        self.id = self.dev["id"]
        self.omci = Path(f"/dev/pon{self.id}omci0")

    def get(self):
        return self.nf.dev_get({"id": self.id})


SIM = None


def dev_dump(_cfg) -> None:
    dev = SIM.dev
    ksft_eq(dev["num-gem-ports"], NUM_FLOWS)
    ksft_eq(dev["num-tconts"], NUM_TCONTS)
    ksft_eq(dev["num-tcont-queues"], NUM_QUEUES)
    ksft_eq(dev["ifindex"], SIM.ifindex)
    ksft_eq(dev["type"], "gpon")


def dev_get_errors(_cfg) -> None:
    for attrs, want in (({}, errno.EINVAL), ({"id": 4000}, errno.ENODEV)):
        try:
            SIM.nf.dev_get(attrs)
        except NlError as e:
            ksft_eq(e.error, want, comment=f"dev-get {attrs}")
        else:
            raise AssertionError(f"dev-get {attrs} did not fail")


def dev_status(_cfg) -> None:
    dfs_write("state", 5)
    dev = SIM.get()
    ksft_eq(dev["onu-state"], "o5")
    ksft_eq(dev["onu-id"], 7)
    ksft_eq(dev["omcc-port-id"], OMCC_PORT_ID)
    ksft_eq(dev["serial-number"], b"SIMU\0\0\0\1")
    ksft_eq(dev["power-level"], "high")
    ksft_eq(dev["stats"]["gem-frames"], 42)
    ksft_eq(dev["stats"]["password-requests"], 3)
    ksft_eq(dev["stats"]["password-replies"], 2)
    ksft_eq(dev["eqd"], 12345)
    ksft_true(not dev.get("alarms"), "no alarm in O5")

    # The time in the state runs on, and a new state starts it again.
    first = dev["onu-state-time"]
    time.sleep(0.3)
    ksft_ge(SIM.get()["onu-state-time"] - first, 250)

    # Below O4 the ONU has no ONU-ID, and in O1 the downstream is lost.
    dfs_write("state", 1)
    dev = SIM.get()
    ksft_eq(dev["onu-state"], "o1")
    ksft_true("onu-id" not in dev, "no ONU-ID in O1")
    ksft_true("eqd" not in dev, "no equalization delay in O1")
    ksft_lt(dev["onu-state-time"], 250)
    ksft_in("los", dev["alarms"])
    dfs_write("state", 5)


def dev_notify(_cfg) -> None:
    nf = PonFamily()
    nf.ntf_subscribe("monitor")
    dfs_write("state", 3)
    seen = None
    for msg in nf.poll_ntf(duration=2):
        if msg["name"] == "dev-change-ntf" and msg["msg"].get("id") == SIM.id:
            seen = msg["msg"]
            break
    ksft_ne(seen, None, "a dev-change-ntf message")
    ksft_eq(seen["onu-state"], "o3")
    ksft_true("onu-id" not in seen, "no ONU-ID in O3")
    dfs_write("state", 5)


def dev_encryption(_cfg) -> None:
    """encrypted-port-id: the Port-IDs the OLT encrypts, the OMCC's among them."""
    dfs_write("state", 5)
    ksft_true("encrypted-port-id" not in SIM.get(), "nothing encrypted")

    # A mark is announced, with the attributes of a get.
    nf = PonFamily()
    nf.ntf_subscribe("monitor")
    dfs_write("encrypt", "300 1")
    seen = None
    for msg in nf.poll_ntf(duration=2):
        if msg["name"] == "dev-change-ntf" and msg["msg"].get("id") == SIM.id:
            seen = msg["msg"]
            break
    ksft_ne(seen, None, "a dev-change-ntf message")
    ksft_eq(seen.get("encrypted-port-id"), [300])

    # In ascending order, Port-ID 0 and the OMCC's included; a second mark
    # of a Port-ID changes nothing, and an unmark takes it out.
    dfs_write("encrypt", f"{OMCC_PORT_ID} 1")
    dfs_write("encrypt", "0 1")
    dfs_write("encrypt", "300 1")
    ksft_eq(SIM.get()["encrypted-port-id"], [0, 300, OMCC_PORT_ID])
    dfs_write("encrypt", "300 0")
    ksft_eq(SIM.get()["encrypted-port-id"], [0, OMCC_PORT_ID])

    for val, want in (("4096 1", errno.ERANGE), ("300 2", errno.EINVAL)):
        try:
            dfs_write("encrypt", val)
        except OSError as e:
            ksft_eq(e.errno, want, comment=val)
        else:
            raise AssertionError(f"encrypt {val} was taken")

    # The marks go with the ONU-ID, and without one nothing is marked.
    dfs_write("state", 3)
    ksft_true("encrypted-port-id" not in SIM.get(), "nothing encrypted in O3")
    try:
        dfs_write("encrypt", "300 1")
    except OSError as e:
        ksft_eq(e.errno, errno.ENXIO)
    else:
        raise AssertionError("a Port-ID was marked without an ONU-ID")
    dfs_write("state", 5)
    ksft_true("encrypted-port-id" not in SIM.get(), "the marks are gone in O5")


def tcont_map(_cfg) -> None:
    nf = SIM.nf
    key = {"id": SIM.id}

    nf.tcont_set(key | {"index": 1, "alloc-id": 300})
    ksft_eq(nf.tcont_get(key | {"index": 1})["alloc-id"], 300)
    ksft_in("300", [str(a) for a in SIM.get().get("alloc-id", [])])

    # An Alloc-ID serves one T-CONT.
    try:
        nf.tcont_set(key | {"index": 2, "alloc-id": 300})
    except NlError as e:
        ksft_eq(e.error, errno.EBUSY)
    else:
        raise AssertionError("a second T-CONT took the Alloc-ID")

    # Alloc-ID 0 is valid, and the dump lists every T-CONT of the ONU.
    nf.tcont_set(key | {"index": 2, "alloc-id": 0})
    ksft_eq(nf.tcont_get(key | {"index": 2})["alloc-id"], 0)
    dump = [t for t in nf.tcont_get({}, dump=True) if t["id"] == SIM.id]
    ksft_eq(len(dump), NUM_TCONTS)
    ksft_eq({t["index"] for t in dump}, set(range(NUM_TCONTS)))

    # No Alloc-ID unmaps; an idle T-CONT reports none.
    nf.tcont_set(key | {"index": 1})
    ksft_true("alloc-id" not in nf.tcont_get(key | {"index": 1}), "T-CONT 1 is idle")
    nf.tcont_set(key | {"index": 2})

    for op, attrs, want in (
            ("tcont_get", key | {"index": NUM_TCONTS}, errno.ENOENT),
            ("tcont_set", key | {"index": NUM_TCONTS, "alloc-id": 1}, errno.ENOENT),
            ("tcont_get", {"index": 0}, errno.EINVAL),
            ("tcont_get", key | {"index": 0} | {"id": 4000}, errno.ENODEV),
            ("tcont_set", key | {"index": 0, "alloc-id": 4096}, errno.ERANGE)):
        try:
            getattr(nf, op)(attrs)
        except NlError as e:
            ksft_eq(e.error, want, comment=f"{op} {attrs}")
        else:
            raise AssertionError(f"{op} {attrs} did not fail")


def tod_refused(_cfg) -> None:
    """tod-set: the policy, the ONU, and a MAC without a clock to set."""
    nf = SIM.nf
    tod = {"id": SIM.id, "superframe": 1000, "sec": 1000000000, "nsec": 0}

    for attrs, want in (
            ({k: v for k, v in tod.items() if k != "superframe"}, errno.EINVAL),
            ({k: v for k, v in tod.items() if k != "nsec"}, errno.EINVAL),
            (tod | {"superframe": 1 << 30}, errno.ERANGE),
            (tod | {"sec": 1 << 48}, errno.ERANGE),
            (tod | {"nsec": 1000000000}, errno.ERANGE),
            (tod | {"id": 4000}, errno.ENODEV),
            (tod, errno.EOPNOTSUPP)):
        try:
            nf.tod_set(attrs)
        except NlError as e:
            ksft_eq(e.error, want, comment=f"tod_set {attrs}")
        else:
            raise AssertionError(f"tod_set {attrs} did not fail")


def gem_lifecycle(_cfg) -> None:
    gem_add("gemt0", SIM.ifname, port_id=100, direction=DIR_BOTH, tcont=0)
    try:
        ksft_in("gemt0", os.listdir("/sys/class/net"))
        ksft_eq([v[:2] for v in flows().values()], [(100, 0)])

        # A Port-ID has one GEM port, and the OMCC's is not for one.
        for port in (100, OMCC_PORT_ID):
            try:
                gem_add("gemt1", SIM.ifname, port_id=port, direction=DIR_BOTH, tcont=0)
            except LinkError as e:
                ksft_eq(e.error, errno.EEXIST, comment=f"Port-ID {port}")
            else:
                ip("link del gemt1")
                raise AssertionError(f"a second GEM port took Port-ID {port}")
    finally:
        ip("link del gemt0")
    ksft_eq(flows(), {})


def gem_refused(_cfg) -> None:
    cases = (
        ("no Port-ID", dict(direction=DIR_BOTH, tcont=0), errno.EINVAL),
        ("multicast upstream", dict(port_id=101, direction=DIR_BOTH, tcont=0,
                                    multicast=True), errno.EINVAL),
        ("upstream without a T-CONT", dict(port_id=102, direction=DIR_UP), errno.EINVAL),
        ("a T-CONT the MAC has not", dict(port_id=103, direction=DIR_BOTH,
                                          tcont=NUM_TCONTS), errno.ERANGE),
        ("a queue it has not", dict(port_id=104, direction=DIR_BOTH, tcont=0,
                                    queue=NUM_QUEUES), errno.ERANGE),
        ("a commitment above the peak", dict(port_id=105, direction=DIR_BOTH, tcont=0,
                                              cir=2000, pir=1000), errno.EINVAL),
        ("an upstream attribute on a downstream port",
         dict(port_id=106, direction=DIR_DOWN, queue=1), errno.EINVAL),
        ("a downstream attribute on an upstream port",
         dict(port_id=108, direction=DIR_UP, tcont=0, ds_rate=1000), errno.EINVAL),
        ("a downstream burst without a rate",
         dict(port_id=109, direction=DIR_DOWN, ds_burst=4096), errno.EINVAL),
    )
    for what, kw, want in cases:
        try:
            gem_add("gemt2", SIM.ifname, **kw)
        except LinkError as e:
            ksft_eq(e.error, want, comment=what)
            ksft_true(e.msg, f"{what}: a message in the extended acknowledgment")
        else:
            ip("link del gemt2")
            raise AssertionError(f"{what}: the GEM port was made")
    try:
        gem_add("gemt2", "lo", port_id=107, direction=DIR_BOTH, tcont=0)
    except LinkError as e:
        ksft_eq(e.error, errno.EOPNOTSUPP, comment="a link no MAC owns")
    else:
        ip("link del gemt2")
        raise AssertionError("a GEM port was made over lo")
    ksft_eq(flows(), {})


def gem_change(_cfg) -> None:
    gem_add("gemt3", SIM.ifname, port_id=110, direction=DIR_BOTH, tcont=1)
    try:
        before = calls()["change"]
        gem_set("gemt3", pbits=0x0f, queue=1, weight=3, cir=1000, pir=5000)
        ksft_eq(calls()["change"], before + 1)
        try:
            gem_set("gemt3", queue=NUM_QUEUES)
        except LinkError as e:
            ksft_eq(e.error, errno.ERANGE)
        else:
            raise AssertionError("a queue the MAC has not was taken")
        ksft_eq(calls()["change"], before + 1)
    finally:
        ip("link del gemt3")


def gem_downstream(_cfg) -> None:
    gem_add("gemt5", SIM.ifname, port_id=130, direction=DIR_BOTH, tcont=0)
    try:
        flow = next(f for f, v in flows().items() if v[0] == 130)
        ksft_eq(flows()[flow][3:], (0, 0))
        before = calls()["change"]

        # A rate and its bucket reach the MAC, and a change of either is one call.
        gem_set("gemt5", ds_rate=12500000, ds_burst=16384)
        ksft_eq(flows()[flow][3:], (12500000, 16384))
        ksft_eq(calls()["change"], before + 1)
        gem_set("gemt5", ds_rate=12500000, ds_burst=16384)
        ksft_eq(calls()["change"], before + 1, comment="the same limit again")
        gem_set("gemt5", ds_rate=2500000)
        ksft_eq(flows()[flow][3:], (2500000, 16384), comment="the burst stays")
        ksft_eq(calls()["change"], before + 2)

        # A burst needs a rate; a rate of 0 takes the limit and its burst away.
        for kw in (dict(ds_rate=0, ds_burst=4096),):
            try:
                gem_set("gemt5", **kw)
            except LinkError as e:
                ksft_eq(e.error, errno.EINVAL, comment=f"{kw}")
            else:
                raise AssertionError(f"{kw} was taken")
        ksft_eq(calls()["change"], before + 2)
        gem_set("gemt5", ds_rate=0)
        ksft_eq(flows()[flow][3:], (0, 0), comment="the limit is gone")
        try:
            gem_set("gemt5", ds_burst=4096)
        except LinkError as e:
            ksft_eq(e.error, errno.EINVAL, comment="a burst alone")
        else:
            raise AssertionError("a burst without a rate was taken")
        ksft_eq(calls()["change"], before + 3)

        # The limit is the port's own, and an upstream-only port has none.
        gem_add("gemt6", SIM.ifname, port_id=131, direction=DIR_DOWN)
        try:
            gem_set("gemt6", ds_rate=1000)
        finally:
            ip("link del gemt6")
    finally:
        ip("link del gemt5")


def gem_bridge(_cfg) -> None:
    c0 = calls()
    gem_add("gemt4", SIM.ifname, port_id=120, direction=DIR_BOTH, tcont=0)
    try:
        ip("link add brt0 type bridge vlan_filtering 1")
        try:
            ip("link set gemt4 up")
            ip("link set gemt4 master brt0")
            ksft_eq(calls()["bridge_join"], c0["bridge_join"] + 1)
            cmd("bridge vlan add dev gemt4 vid 100 pvid untagged")
            ksft_true(calls()["vlan_add"] > c0["vlan_add"], "vlan_add reached the MAC")
            cmd("bridge vlan del dev gemt4 vid 100")
            ksft_true(calls()["vlan_del"] > c0["vlan_del"], "vlan_del reached the MAC")
            ip("link set gemt4 nomaster")
            ksft_eq(calls()["bridge_leave"], c0["bridge_leave"] + 1)
        finally:
            ip("link del brt0")
    finally:
        ip("link del gemt4")


def counters(name):
    stats = json.loads(ip(f"-s -j link show dev {name}").stdout)[0]["stats64"]
    return stats["rx"]["packets"], stats["tx"]["packets"]


def gem_traffic(_cfg) -> None:
    gem_add("gemt5", SIM.ifname, port_id=130, direction=DIR_BOTH, tcont=0)
    try:
        ip("link set gemt5 up")
        flow = next(f for f, v in flows().items() if v[0] == 130)
        rx0, tx0 = counters("gemt5")

        dfs_write("rx", flow)
        dfs_write("rx", flow)
        # A flow with no GEM interface does not take the frame.
        try:
            dfs_write("rx", NUM_FLOWS - 1 if flow != NUM_FLOWS - 1 else 0)
        except OSError as e:
            ksft_eq(e.errno, errno.ENOENT)
        else:
            raise AssertionError("a flow without a GEM interface took a frame")
        ksft_eq(counters("gemt5")[0], rx0 + 2)

        sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
        sock.bind(("gemt5", 0))
        frame = b"\xff" * 6 + b"\x00\x00\x5e\x00\x53\x02" + b"\x88\xb5" + b"\0" * 50
        for _ in range(3):
            sock.send(frame)
        sock.close()
        # The stack sends frames of its own when the interface comes up
        # (IPv6 neighbour discovery), so the count is at least the three.
        ksft_true(counters("gemt5")[1] >= tx0 + 3, "three frames or more sent")
    finally:
        ip("link del gemt5")


def omci_loopback(_cfg) -> None:
    ksft_true(SIM.omci.exists(), f"{SIM.omci} exists")
    start0, stop0 = calls()["omci_start"], calls()["omci_stop"]
    fd = os.open(SIM.omci, os.O_RDWR | os.O_NONBLOCK)
    try:
        ksft_eq(calls()["omci_start"], start0 + 1)

        # A message written comes back as it was.
        msg = bytes(range(48))
        ksft_eq(os.write(fd, msg), len(msg))
        ready, _, _ = select.select([fd], [], [], 2)
        ksft_true(ready, "the message comes back")
        ksft_eq(os.read(fd, 2048), msg)

        # An extended message, up to the largest.
        big = bytes(1980)
        ksft_eq(os.write(fd, big), len(big))
        select.select([fd], [], [], 2)
        ksft_eq(len(os.read(fd, 2048)), len(big))

        # The channel being down fails a write, and up again lets it through.
        dfs_write("omci_link", 0)
        try:
            os.write(fd, msg)
        except OSError as e:
            ksft_eq(e.errno, errno.ENOLINK)
        else:
            raise AssertionError("a write passed with the OMCI channel down")
        dfs_write("omci_link", 1)
        ksft_eq(os.write(fd, msg), len(msg))
    finally:
        os.close(fd)
    ksft_eq(calls()["omci_stop"], stop0 + 1)


def main() -> None:
    global SIM
    SIM = Sim()
    ksft_run([dev_dump, dev_get_errors, dev_status, dev_notify, dev_encryption, tcont_map,
              tod_refused, gem_lifecycle, gem_refused, gem_change, gem_downstream,
              gem_bridge, gem_traffic, omci_loopback], args=(None,))
    ksft_exit()


if __name__ == "__main__":
    main()
