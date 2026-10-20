.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

======
netifd
======

OpenWrt's network interface daemon, carried as a fork. On the stick it holds
the interfaces of the host side: the address on the host link, the management
VLAN and its address, the PPPoE sessions, and the links that hand a session's
address on to the host, with their routes and rules (ADR 0011,
``docs/handbook/system.rst``). The daemon's own description is ``DESIGN``.

Where it comes from
===================

Imported with ``git subtree add --squash`` from

    https://git.openwrt.org/project/netifd.git

at commit ``06d06c86d757196e56bb13a1a8d6d69df7cad04d`` (2026-09-11), unchanged.
Its files carry the GPL-2.0 notices of their authors, and ``REUSE.toml`` states
the licence for the directory. A file this project adds, such as this one,
carries its own SPDX lines.

What the fork changes
=====================

Each change is a commit of this repository, whose message gives the reason
and how it was measured:

* ucode, udebug and the shell helpers of the protocols are left out: the stick
  runs no ucode, has no udebug daemon to read udebug's ring buffers, and its
  protocols are programs in C.
* It takes the network as it finds it at start: a device keeps its state, and a
  VLAN device or a bridge that exists already is adopted, so that a restart of
  the daemon does not cut the management VLAN off.
* It sets down only the devices it raised itself, so that the switch's conduit,
  which carries the PON too, stays up when the host side lets ``sfp`` go.
* SIGHUP reads the configuration again, as ``ubus call network reload`` does;
  the image has no ubus command-line tool.
* At its end it gives each of its processes SIGTERM and two seconds before
  SIGKILL, so that a PPPoE session ends with a PADT.

How the stick runs it
=====================

procd runs it as a service, started by the boot script::

    netifd -c /run/config -r /run/resolv.conf.auto -h /usr/sbin/onu-apply

* ``/run/config/network`` is its configuration, which ``onu-net``
  (``onu-utils``) writes from parts and tells it of with SIGHUP; the image
  carries libuci for the daemon and no ``uci`` command.
* Its protocol ``pppoed`` is ``pppoed`` of this tree, which the image links as
  ``/lib/netifd/proto/pppoed.sh``; it reports a session's address, peer, routes
  and name servers to netifd over ubus (``pppoed/README.rst``).
* For each interface that comes up or goes down it runs ``onu-apply iface``.

``br2-external/package/netifd`` builds it, with json-c, libubox, libuci and
ubus, and libnl-tiny linked in.

Taking a newer upstream
=======================

A newer commit of upstream comes in as the import did, as a squash of that
commit merged into ``netifd/``::

    git fetch https://git.openwrt.org/project/netifd.git <commit>
    git subtree merge --squash -P netifd \
        -m 'netifd: Update to the upstream commit <commit>' FETCH_HEAD

The fork's commits stay on top; a conflict with one of them is resolved in the
merge. The provenance gates look at what differs from the latest pristine
import, which is the fork's changes and nothing of upstream.

Tests
=====

The fork has no tests in this tree. Each change was measured with veth or
dummy devices in a network namespace, as its commit says, and the daemon was
tried on the bench's stick (ADR 0011); no run of the device tests
(``tests/device``) has been recorded with it yet.
