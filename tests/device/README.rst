.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

============
tests/device
============

The tests that run on the stick itself. Each case checks one function of the
stick, says so in one sentence, and records the measured values that its
verdict rests on. A run writes a page with the table of every case and its
result, the log of every command each case ran, and the same results as JSON.
The pages of the runs that a change was tested with are kept in
``docs/testing/``.

What it needs
=============

* The stick, booted from an image of this tree, with the bench's ssh key in
  ``/root/.ssh/authorized_keys`` (a local overlay of the bench, see
  ``docs/handbook/building.rst``).
* A bench host: a Linux machine that reaches the stick's management address and
  through which ssh reaches the stick (``ssh -J``). For the data path, the bench
  host is also both far ends: one port is the far end of the stick's host link
  (the subscriber side) and one port is behind the OLT (the network side). The
  data-path cases send their bursts from a raw socket, with ``python3`` and
  ``sudo`` without a password for it. The VLAN translation case also needs the
  path through the OLT to carry VLANs 3001 and 3002 both ways; it installs a
  translation between them on the stick, at a tc priority of its own, and
  removes it again. The ACL case likewise installs drop and trap rules on the
  stick's PON port, and a filter that counts the trapped frames on each of its
  GEM interfaces, and removes them again; the mirror case mirrors the PON port
  to the SFP port both ways, the shaper case puts a tbf on the SFP port, the
  storm case police and drop rules on broadcast on the SFP port, the PCP
  rewrite case a DCB rewrite entry on the PON port, and the address-table case
  gives the bridge a static address, each removing what it added. The
  CPU-limit case floods the stick's own address from the subscriber side for
  10 s, then lifts the limit for 3 s and sets it back; the stick answers slowly
  while the flood lasts. The hardware clock's case switches the clock's pulse
  on for a few seconds and off again.
* A bench host that serves more than one stick may give each its own network
  namespace, so that two sticks can keep the same management address: the
  address in the bench's own namespace is then another stick's, and a run
  through it would test, and with ``--destructive`` restart, the wrong one.
  ``--bench-netns NAME`` names the namespace that holds the stick under test:
  ssh reaches the stick through the bench host with ``nc``, started inside the
  namespace, instead of ssh's own jump, which connects from the bench's own
  namespace; and every command of a case that runs on the bench host, such as
  the requests to the status daemon, runs inside it too, as root. This needs
  ``nc`` on the bench host and ``sudo`` without a password for
  ``ip netns exec``. The ports that the data path names must then be in that
  namespace as well.
* The host-link search changes the subscriber-side port itself, which it takes
  to run 2500BaseX without negotiation: it sets the port to 1000BaseX with
  in-band negotiation and back to the speed and negotiation it had, with
  ``ethtool``, and when NetworkManager holds the port, it lets NetworkManager
  go of the port for the change and gives the port back to it only then, since
  NetworkManager puts back the settings it found whenever the link goes.
  NetworkManager's own settings then come back too. Before any change it
  arms a systemd timer, ``devtest-host-link``, that puts the port back after
  30 minutes should the case die halfway, and stops the timer at the end. This
  needs ``sudo`` without a password for ``ethtool``, ``nmcli``,
  ``systemd-run`` and ``systemctl``. The stick's management rides the link, so
  the stick is out of reach for a few seconds each time.
* Python 3.11 or later and an ssh client on the machine that runs the tests;
  ``python3`` and ``curl`` on the bench host.

The cases and the image of today
================================

The suite was last recorded on 2026-10-12 (``docs/testing/``), on the image of
that day, run from the flash: 32 of 48 cases passed and 16 were skipped, the 15
of the data path (``data.*``) and the search of the host link
(``net.host-link-search``), which need the ports of a bench. Those were last
recorded on 2026-10-07, on an image of before procd, netifd, the panel's HTTPS and
the store in the partition ``config``. Their cases have been brought up to
today's image, but none has run on it. What a case expects of the image:

* The cases that install rules of their own, read the switch or stop procd's
  feeding of the watchdog run iproute2's ``tc``, ``bridge``, ``devlink`` and
  ``dcb``, ``ethtool`` and ``ubus``, which the flash's image does not carry
  (ADR 0017); only a RAM image does (``make ram-image``). Each such case names
  the tools it runs, and a run on the flash's image has them put on the stick
  for it: ``--tools build/image/diag`` (the directory that ``make image`` sets
  them aside in) unpacks the programs, the C library whole and a script for
  each into a file system of memory of their own, ``/run/devtest-tools``,
  takes them away at the end of the run and writes nothing to the flash. A
  case whose tool is not there and was not given is skipped, with what to do.
  The image under test is then the flash's: a RAM image without the tools of
  diagnosis, ``make ram-image RAM_DIAG=no``, has its root filesystem, and
  ``--tools`` gives it what the cases need. The data-path cases read the
  switch's own frame counts with ``ethtool`` when the stick has it, and do
  without otherwise.
* The stick's own servers answer on its management VLAN alone
  (``docs/handbook/system.rst``): they listen on the management addresses,
  which ``net.listeners`` checks, and the stick's guard drops what comes to
  them by any other interface, which ``net.guard`` checks from the bench's
  subscriber-side port. It sends a TCP SYN to ssh and to the panel at the
  management address, from a raw socket, in the management VLAN, where it
  must be answered, then untagged and in each other VLAN the stick has on
  its host link, where nothing may answer, not even a reset. By a local
  network's VLAN the SYN comes from an address of that network, the last
  but one, as a device of it would send it, and the bench answers the
  stick's ARP for that address for the length of the probe; elsewhere it
  comes from the bench's own address on the management network, and the
  record names the interface's reverse-path check. With ``--bench-netns``
  the probe runs inside the namespace, as the other cases' commands do.
* procd feeds the watchdog (ADR 0010): ``system.watchdog`` looks for procd
  holding the device, and ``system.watchdog-reset`` has procd let go of it
  without closing it properly, so that nobody feeds an armed watchdog.
* The ssh server takes root's key and root's password (ADR 0009), which
  ``net.ssh-login`` checks.
* ``omci.gem-ports`` knows both shapes of the stick: bridged by omcid, or
  holding PPPoE sessions that each run on a GEM port. ``data.igmp`` needs the
  bridge ``omcibr0``, which omcid builds only while the stick holds no PPPoE
  account.

* ``net.sessions`` and ``system.restart`` look at the PPPoE sessions the stick
  holds, and are skipped (or fail to find them) on a stick with no PPPoE account.
  ``system.restart`` kills with SIGKILL every program procd keeps and each
  session's ``pppoed``, which cuts the sessions off without a goodbye, so that the
  concentrator may keep the old ones for minutes before it takes new ones; the
  case waits up to five minutes for the sessions. It is run with
  ``--destructive``.
* ``update.trial-and-back`` writes the update file given with ``--update`` into
  the bank the stick does not run, has the loader start it once, waits until it
  has committed itself, and goes back to the bank it came from by the same way.
  It needs a stick that runs a bank of the flash, not a RAM boot, and refuses to
  write over a bank that holds the shipped firmware unless ``--overwrite-shipped``
  says so. It is run with ``--destructive``; the stick is restarted three times.
* ``mgmt.pond-no-store`` puts the mark that ``onu-config`` leaves when the store
  cannot be read, ``!`` in ``/run/onu-auth``, for the length of one request and
  puts the file back.

The way in through the shipped firmware's web page has no case yet; the host tests
of ``mkweb.py`` cover the package. The warning of ``omcid`` about a VLAN of the OLT
that is the stick's management VLAN has none either, since the lab's OLT carries
no such VLAN: ``omcid/tests/kernel`` tests it.

Running
=======

From this directory::

    python3 -m devtest --bench BENCH --stick ADDRESS [--bench-netns NAME] \
        --olt-port PORT --subscriber-port PORT \
        --token-file FILE [--tools ../../build/image/diag] \
        [--update ../../build/image/images/update.bin] --out ../../docs/testing/NAME

``--list`` lists the cases; naming cases or areas (``gpon``, ``data.upstream``)
runs only those. The kernel log is read after the other cases, so that it
holds what they made the kernel say. The cases that reset the stick or take
its host link down run only with ``--destructive``, and after all the others.
The router cases of the bench's router of before, which the stick no longer
has, are gone with it. Without ``--olt-port`` and
``--subscriber-port`` the data-path cases are skipped; without
``--token-file`` so is the case that reads values from the status daemon. The
token is the one root makes on the stick, once, with ``onu-config token new``;
the store keeps it across restarts and updates of any image, a RAM boot
included, and ``ssh root@STICK onu-config token >FILE`` brings it to the file
(keep the file private). A
case that resets the stick waits for the boot loader to count down, which can
take minutes; ``--reload COMMAND`` names a command, run on the machine that
runs the tests as soon as the stick is gone, that brings it back at once, such
as the bench tool's RAM boot of the image under test, which breaks into the
countdown (``tools/bench``). The settings can also come from the environment:
``DEVTEST_BENCH``, ``DEVTEST_STICK``, ``DEVTEST_BENCH_NETNS``, ``DEVTEST_OLT_PORT``,
``DEVTEST_SUBSCRIBER_PORT``, ``DEVTEST_TOKEN_FILE``, ``DEVTEST_TOOLS``,
``DEVTEST_UPDATE`` and ``DEVTEST_RELOAD``.

What a run records leaves out what names one unit or one bench: MAC addresses,
GPON serial numbers, the token, and the names of the bench host and the stick
are replaced by ``<mac>``, ``<serial>``, ``<token>``, ``<bench>`` and
``<stick>``.

Layout
======

``devtest/access.py``
    The two remote ends, ssh to them, and the redaction of what is recorded.
``devtest/core.py``
    Cases, outcomes and the loop that runs them.
``devtest/cases_*.py``
    The cases, by area: the system, the host side and management, the PON side
    and the optics, the data path.
``devtest/report.py``
    The page, the log and the JSON of a run.
``devtest/tests/``
    Unit tests, which need no stick: from this directory,
    ``python3 -m unittest discover -s devtest/tests -t .``.

Finer tests live with their component: the OMCI daemon's in ``omcid/tests/``,
the other programs' in their ``tests/`` directories, and the kernel's
selftests under ``linux/tools/testing/selftests/``.
