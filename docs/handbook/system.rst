.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=====================
How the system works
=====================

The system as it runs on the stick: its parts, how it starts, which program
owns what, how a change of the settings reaches the hardware, and what is
kept where. This page is the map; each section names the page or the README
that holds the detail, and the decision records that say why.

The parts
=========

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Part
     - What it does
   * - The kernel (``linux/``)
     - Linux 6.18.48 with this project's series: the SoC and its Lexra core,
       the switch as a DSA driver with its CPU port, the GPON and PON MACs, the
       optics, the watchdog, and the PON class, through which programs see the
       ONU: its state, its GEM ports and T-CONTs, and its OMCI channel
       (``linux/Documentation/networking/pon.rst``).
   * - procd and ubusd
     - OpenWrt's first process and message bus. procd feeds the watchdog, runs
       the boot script, and starts each of the stick's programs again when it
       ends (ADR 0013).
   * - netifd
     - OpenWrt's network interface daemon, forked in ``netifd/``: the host
       side's addresses, the management VLAN, the PPPoE sessions and the links
       that hand their addresses on to the host, with their routes (ADR 0014).
   * - omcid
     - The OMCI daemon (ITU-T G.988): it answers the OLT, and builds the bridge,
       the VLAN rules and the GEM ports of what the OLT provisions
       (``omcid/README.rst``).
   * - onu-utils
     - The stick's own programs: the settings and their store
       (``onu-config``), the generator that turns them into what procd and
       netifd keep (``onu-apply``), the banks and updates (``onu-update``,
       ``onu-trial``), and the helpers the others run
       (``onu-utils/README.rst``).
   * - pond
     - The status daemon and its panel, over HTTPS. It changes nothing on the
       stick itself: it asks ``onu-act``, which runs a fixed list of programs
       as root (``pond/README.rst``).
   * - pppoed, dhcp6d
     - The PPPoE client, which netifd runs for each session, and the server
       that hands a session's delegated IPv6 prefix on to the host
       (``pppoed/README.rst``, ``dhcp6d/README.rst``).
   * - timed
     - The clock: it sets the time from the host, by PTP when the host has a
       master and by NTP otherwise, and from the date the OLT gives when the
       host gives none (``timed/README.rst``).
   * - BusyBox, dropbear
     - The shell and the tools, the log, the DHCP server; the ssh server.

Every program loads one C library, cut to what the programs use (ADR 0011).
The root filesystem is a read-only SquashFS image (``building.rst``).

The start
=========

#. The boot loader starts the committed bank, or a bank it was told to try
   once (``boot-contract.rst``).
#. The kernel mounts the bank's root filesystem read-only and runs
   ``/sbin/init``, which mounts ``/proc``, ``/sys`` and the file systems in
   memory, ``/tmp``, ``/run`` and ``/var``, and execs procd.
#. procd opens the watchdog and feeds it from then on, starts ubusd, and runs
   the boot script, ``/etc/init.d/rcS``.
#. The boot script reads the settings from the store in the partition
   ``config``, brings the host side up through netifd, starts the servers,
   brings the PON up with the unit's identity, and has ``onu-apply`` start what
   the settings ask for. ``building.rst`` gives its steps in order.
#. After a start on trial, ``onu-trial`` keeps the bank once the system is up,
   or restarts into the committed one (``updating.rst``).
#. procd starts the console's login prompt.

What the stick does with the traffic
====================================

There is no mode to choose; the settings decide (``onu-utils/README.rst``):

Without a PPPoE account
    ``omcid`` connects the host link to the PON as the OLT provisions it: a
    bridge, with the VLAN rules and the GEM ports of the OLT's provisioning.
    The switch carries the frames; the CPU sees only what is sent to it.
With PPPoE accounts
    The stick holds up to seven PPPoE sessions itself, each on a GEM port or a
    VLAN of one, and hands a session's IPv4 address down to the host behind
    it, and its delegated IPv6 prefix through ``dhcp6d``, or keeps them on the
    stick. ``omcid`` still bridges the rest of what the OLT provisions, such as
    television and telephone.

What the data path carries
==========================

The switch carries the frames in hardware, so the speed of the stick is that
of the PON and of its ports, not of its CPU, which saw 20 to 24 frames in each
run below and was busy 6 % to 7 % of the time. The figures are of one lab (M:
the bench's stick on a RAM image, ``33449f2628fb``, at O5 on the lab OLT with
two PPPoE sessions to the lab concentrator; a host behind the stick's host
link; runs of 12 s of ``iperf3`` against servers on the concentrator's router;
2026-10-12):

.. list-table::
   :header-rows: 1
   :widths: 14 38 48

   * - Way
     - Traffic
     - Result
   * - Up
     - TCP, the IPv4 session, three runs
     - 902, 908 and 909 Mb/s of TCP payload
   * - Up
     - TCP, the IPv6 session
     - 881 Mb/s
   * - Up
     - TCP, both sessions at once, two runs
     - 911 and 911 Mb/s together (427 and 484, 462 and 449)
   * - Up
     - UDP offered at 1500 Mb/s, two runs
     - The PON carried 982 and 983 Mb/s of frames. The receiver counted
       897 and 850 Mb/s of payload, 40 % and 43 % of the datagrams lost.
   * - Down
     - TCP, the IPv4 session, three runs
     - 877, 878 and 882 Mb/s
   * - Down
     - TCP, both sessions at once, two runs
     - 920 and 922 Mb/s together (457 and 463, 461 and 461)
   * - Down
     - UDP offered at 1200 Mb/s, two runs
     - 932 and 937 Mb/s of payload received, 22 % lost

Reading them:

* **Upstream, the OLT's grant is the ceiling.** The bandwidth map gives the
  stick's T-CONT 15,417 bytes in every frame of 125 us, which is 986.7 Mb/s,
  the same with the line idle and under load (M: the ``bwmap`` view in
  ``/sys/kernel/debug/rtl9602c/``). The PON's counter of bytes read 954 to
  962 Mb/s in the TCP runs and 982 to 983 Mb/s in the UDP ones. A grant of
  another OLT is another ceiling.
* **TCP payload is the frame less its headers.** Of 961 Mb/s that the PON
  counted, 909 Mb/s is payload, about 95 %. That is what the PPPoE session's
  MTU of 1492 leaves: 1440 bytes of TCP payload, with its timestamps, in an
  IP packet of 1492 bytes and the 22 bytes of Ethernet and PPPoE around it.
* **Two sessions share the grant.** At once they add up to the same 911 Mb/s
  that one gives, and "some 900 Mb/s each way" for two sessions is that sum.
* **Not all that the PON carried arrives.** In the two UDP runs the stick sent
  983,915 and 983,692 frames upstream and the server counted 935,341 and
  886,436 datagrams, 5 % and 10 % fewer. Where they were lost after the PON
  was not measured.
* **The bench's burst is above the rate.** ``data.upstream`` (``tests/device``)
  sends 400,000 frames of 1514 bytes in about 2 s from a port of 2.5 Gb/s.
  What it records, "159683 arrived (39.91%) at 963 Mb/s", is the share of the
  burst that fit in the grant and the rate the PON carried; it is not 40 % of
  963 Mb/s. The records of ``data.upstream`` in ``docs/testing`` that give a
  rate carry 962 or 963 Mb/s in twenty runs and 983 or 984 in two later ones,
  each with some 40 % of the frames; in the one other, 433 Mb/s, the bench's
  sender was the limit and every frame arrived (``docs/testing/README.rst``).
  No run measured an upstream of 390 Mb/s. The case's text says what the two
  figures are since 2026-10-12; the older records read the same.
* **The RAM image is the kernel of the flash image**, with tools for diagnosis
  in its root file system. These runs did not use the flash image.

Who owns what
=============

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - What
     - Its owner
   * - The programs, and starting one again when it ends
     - procd; ``onu-svc`` gives it the services, and ``onu-apply`` the
       instances of the service ``onu`` (omcid, dhcp6d, udhcpd, udhcpc) as one
       list.
   * - The host side's addresses, the management VLAN, the sessions, the links
       to the host, their routes and rules
     - netifd, from the configuration ``onu-net`` writes in ``/run/config``.
   * - The OLT's bridge, its VLAN rules, the GEM ports and T-CONTs
     - ``omcid``.
   * - The VLAN interfaces of the sessions on the GEM ports
     - ``onu-apply``; netifd runs the sessions on them.
   * - The routes of a delegated IPv6 prefix
     - ``dhcp6d``.
   * - The switch's rules for the management VLAN, the drivers' parameters,
       the unit's identity in the PON driver
     - The boot script, ``onu-mgmt``, ``onu-lane`` and ``onu-apply``, through
       ``onu-nl``.
   * - The settings, the accounts and root's password
     - ``onu-config``, in the store.

Who runs as whom
================

Root is the account to log in with (ADR 0012). The programs that face a
network run as accounts of their own, without a password or a shell, with
only the powers they use: ``onu-drop`` starts them so, and they keep the
powers for what they run. ubusd's lists (``/usr/share/acl.d``) say what each
may do on ubus. Each writes into a directory it owns, or into one of the group
``onu``, which a session's directory is.

.. list-table::
   :header-rows: 1
   :widths: 14 8 22 56

   * - Program
     - Account
     - Powers
     - What it writes, and what ubusd lets it do
   * - pond
     - 200
     - ``net_bind_service``
     - ``/run/pond``; the calls of the panel (``pond/acl.json``).
   * - timed
     - ntp, 201
     - ``sys_time``, ``net_raw``, ``net_bind_service``
     - ``/run/timed`` (its state, which pond reads); the PTP hardware clock,
       ``/dev/ptp0``, which it steers through the device, whose group
       ``onu-run`` makes ``ntp`` (the hardware stamps are turned on before,
       as root, with ``timed -H``); no use of ubus.
   * - dhcp6d
     - dhcp6d, 202
     - ``net_bind_service``, ``net_admin``, ``net_raw``
     - ``/run/dhcp6d`` (the host's binding) and its session's directory;
       it publishes ``dhcp6d.*`` and calls the method ``dhcp6d`` of onu-act.
   * - udhcpc, an IPoE connection's DHCP client, and its script ``onu-dhcpc``
     - dhcpc, 205
     - ``net_raw``, ``net_bind_service``
     - Its connection's directory, ``/run/ipoe/K``; it calls the method
       ``dhcpc`` of onu-act, which puts the lease in place.
   * - pppoed
     - pppoed, 203
     - ``net_admin``, ``net_raw``
     - Its session's directory; it calls ``notify_proto`` of netifd's
       interfaces. The file of its account and ``/dev/ppp`` are its group's.
   * - omcid
     - omcid, 204
     - ``net_admin``, ``net_raw``
     - ``/run/omcid`` (its socket, the MIB, three files for onu-apply);
       it publishes ``omcid`` and calls the method ``omcid`` of onu-act. The
       OMCI port is its group's, and it reads a copy of the identity without
       the PLOAM password.

A program that is not root's cannot run ``onu-apply``, which changes the data
path: omcid, dhcp6d and the DHCP client's script tell it of a change through
``onu-call``, which asks onu-act for the method of their own name, and
onu-act, root's, runs it.

netifd, onu-act, procd, ubusd and the scripts are root's. netifd starts the
sessions as the account pppoed, writes the kernel's sysctls of the interfaces,
and runs ``onu-apply`` for each event of an interface, which only root may do;
to change that, netifd would need the power to start a program as another
account, and ``onu-apply iface`` a way to be asked for from one that is not
root's.

A change of the settings
========================

#. The panel sends the change to ``onu-act`` over ubus; ``onu-config change``
   tests it against the one table of the settings and the rules that bind
   them together, and refuses it with the reason, or writes it aside, in
   ``/run/onu-pending``.
#. ``onu-apply try`` applies it on trial: it writes netifd's configuration
   and procd's list of instances, and procd and netifd change what differs.
#. The panel confirms the change as soon as it reaches the stick again. A
   change that is not confirmed in 90 seconds is undone, and the stick goes
   back to the settings it had. Only a confirmed change is written to the
   store.

ADR 0014 gives the design. A session goes down and up again only when what
it is made of changes (its GEM port, interface, family, MAC address, account,
or what it hands down by which link, ``/run/session/N/makings``); the other
sessions stay up.

How the parts talk
==================

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Channel
     - Between
   * - ubus
     - procd's services (``onu-svc``); the panel's requests to ``onu-act``,
       which ubusd's access list limits; the state of ``omcid``, of netifd's
       interfaces and of ``dhcp6d``, which the panel reads.
   * - Generic netlink ``pon``, rtnetlink, devlink
     - The kernel and ``omcid``, ``pon``, ``onu-nl``, ``dhcp6d`` and the panel.
   * - The OMCI character device
     - The kernel and ``omcid``.
   * - Programs run on an event
     - ``omcid``, ``dhcp6d`` and netifd run ``onu-apply`` when what they
       publish changes; one ``onu-apply`` at a time changes the stick.
   * - Files in ``/run``
     - The settings in force (``/run/onu.conf``), the accounts, the identity,
       the state of each session (``/run/session/N``) and of the trial, and
       ``omcid``'s MIB. The programs of the stick read them; the panel does
       not, but asks ``onu-apply state`` through ``onu-act``. They are gone at
       the next start. ``run-files.rst`` lists them all.
   * - HTTPS
     - A browser or a script and the panel's API.

Reaching the stick
==================

* The console, on the UART (``board.rst``).
* ssh, and the panel over HTTPS on port 443, on the two addresses of the host
  side and nowhere else: one on the host link, one on a VLAN of it that the
  switch keeps off the PON both ways. Port 80 sends a browser to port 443.
* The OLT, over OMCI, which provisions the data path and nothing else.

There is one account to log in with, root, with one password for the console,
ssh and the panel; until one is set it is ``admin`` (ADR 0012). The panel runs
as an account of its own, which no one logs in with (ADR 0014), and answers
with a certificate it signs itself (ADR 0016). The stick has no packet filter:
its servers keep off the PON side by the addresses they listen on.

What is kept where
==================

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Where
     - What
   * - The root filesystem
     - Read-only, the bank's SquashFS image.
   * - ``/run``, ``/tmp``, ``/var``
     - In memory, gone at the next start: the settings in force, the state of
       the programs, the log.
   * - The partition ``config``
     - The shipped firmware's JFFS2 file system with its files, which hold the
       unit's identity, and this system's own file beside them, at most 64
       KiB, of what must outlive a restart: the settings, the accounts, the
       password, the keys (``boot-contract.rst``; the keys are listed in
       ``onu-utils/README.rst``).
   * - ``env``, ``env2``
     - The boot loader's environment: which bank starts, and a trial.
   * - The two banks
     - A kernel and a root filesystem each; an update writes the one that does
       not run (``updating.rst``).

Where to read next
==================

* ``building.rst``: the image, its budget, and the boot script step by step.
* ``updating.rst``: the banks, updates, trials and the way in through the
  shipped firmware's web page.
* ``docs/adr/0013-procd-is-the-first-process.rst``,
  ``0014-one-control-plane.rst``, ``0015-names-of-the-sticks-own-programs.rst``
  and ``0016-the-panel-over-https.rst``: why the system is built this way.
* ``linux/Documentation/networking/pon.rst``: the kernel's interface to the
  ONU.
