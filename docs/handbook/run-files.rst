.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===================
The files of /run
===================

``/run`` is the stick's memory-backed directory: what the programs of the
stick tell each other in files, and what they keep across the start again of
one of them (ADR 0011). It is empty at power-up and gone at the next one. This
page lists every path of it that the programs of this tree write or read, who
does which, and how far a reader may count on it.
``tools/run-files`` fails ``make check`` when a program of the tree names a
path of ``/run`` that this page does not.

What may be counted on
======================

Three kinds of file, in order of how much may rest on them.

**Told.** What the panel and a person need to know of what ``onu-apply`` keeps
in files is told by ``onu-apply state``, which ``onu-act`` runs for the panel
(``settings_state``), in lines ``name=value``. The lines are the contract and
``onu-utils/README.rst`` lists them. The state of ``omcid``, of ``dhcp6d`` and
of netifd's interfaces is told by their objects on ubus, the method
``status``. A reader outside the tree, or the panel, takes the state from
these two and from no file below.

**Shared.** A file that one component writes and another reads is an agreement
between them: its format is the same on both sides, and a change goes into
both in one commit with the tests of both. The first table is these files.

**Private.** A file that one program (or the scripts of ``onu-utils``)
writes and reads itself, to keep its work between runs. It may change with
the program, and nothing else may read it. The second table is these.

Where the table gives a mode, it is the one on the board (M: ``ls -l /run``
on the DFP-34X-2C3 in O5, 2026-10-12). The files of secrets are for root, or
for the one account that uses them, and for no one else.

Shared by two components
========================

.. list-table::
   :header-rows: 1
   :widths: 24 20 20 36

   * - Path
     - Written by
     - Read by
     - Holds
   * - ``/run/onu.conf``
     - ``onu-config``
     - ``onu-apply``, ``onu-mgmt``, ``onu-config``
     - The settings in force, ``KEY=value`` lines with every default filled in
       (``onu-utils/src/settings.h``). The panel does not read it: it asks
       ``onu-config current`` through ``onu-act``.
   * - ``/run/onu-pppoeN``
     - ``onu-config``
     - ``pppoed`` (``-f``), ``onu-apply``, ``onu-config``
     - The account of PPPoE session N, 1 to 7: ``user secret``. For root and,
       by its group, ``pppoed``; ``onu-apply`` sets mode 0640 and the group
       when the session comes up and whenever it puts an account in place.
   * - ``/run/session/N/``
     - ``pppoed`` (``ppp``, ``ipv4``, ``peer``, ``dns``, ``ipv6``);
       ``onu-apply`` (``dev``, ``gem``, ``family``, ``hands``, ``role``,
       ``lan`` and the rest)
     - ``onu-apply``
     - What session N has and what it brought. The directory belongs to the
       group ``onu``, so that ``pppoed`` can write in it
       (``onu-utils/scripts/onu-apply``, its header).
   * - ``/run/ipoe/K/``
     - ``onu-dhcpc``, udhcpc's script (``ipv4``, ``router``, ``dns``,
       ``until``, of a lease); ``onu-apply`` (``dev``, ``gem``, ``kind``,
       ``makings``, ``net``, and the addresses of a static connection)
     - ``onu-apply``
     - What IPoE connection K has and what it brought. The directory belongs
       to the group ``onu``, so that the DHCP client can write in it.
   * - ``/run/onu-identity``
     - ``onu-config`` (what ``ponid`` reads from the shipped firmware's
       files, with the store's ``ID_*`` keys over it), and the boot script
       (``sw_version``, ``pack_version``)
     - ``onu-apply``, ``onu-config``
     - The unit's identity, ``KEY=value`` lines; mode 0600, since it holds
       the PLOAM password.
   * - ``/run/omcid/identity``
     - ``onu-apply``, as ``identity.new`` and then moved in place
       (``/run/omcid/identity.new``)
     - ``omcid``
     - The identity without the PLOAM password; mode 0640, group ``omcid``.
   * - ``/run/onu-identity-source``
     - ``onu-config``
     - ``onu-config``
     - ``store`` or ``factory``: where the identity in force came from.
   * - ``/run/onu-held-vlans``
     - ``onu-apply``
     - ``omcid``
     - The VIDs of the sessions, one per line, which ``omcid`` leaves out of
       its bridge (``omcid/README.rst``).
   * - ``/run/omcid/applied``
     - ``omcid``
     - ``onu-apply``
     - ``bridge`` or ``router``: the shape ``omcid`` programmed after
       ``onu-apply`` sent it SIGUSR2.
   * - ``/run/omcid/xlate``, ``/run/omcid/gem-vlans``
     - ``omcid``
     - ``onu-apply``
     - The OLT's VLAN translations, and a ``VID PORT-ID`` line for each VID a
       GEM port lets through (``omcid/README.rst``).
   * - ``/run/omcid/clock``
     - ``omcid``, as a file beside it that is then moved in place
     - ``timed``
     - The date and time the OLT gave with Synchronize time, and the
       monotonic time they came at: ``utc=`` and ``monotonic_ns=`` lines
       (``timed/README.rst``). Written only when the OLT gives a date.
   * - ``/run/timed/state``
     - ``timed``, as a file beside it that is then moved in place
     - ``pond``
     - What sets the clock and how near it is: the source in use, the state,
       the offset, and what each source last gave, ``KEY=value`` lines
       (``timed/README.rst``).
   * - ``/run/omcid/ctl``
     - ``omcid``
     - ``omcivlan`` and ``omcitap`` of the RAM image
     - The control socket of ``omcid`` (``omcid/include/omcid/ctl_proto.h``).
   * - ``/run/onu-mgmt``
     - ``onu-mgmt``
     - ``onu-apply``, ``onu-trial``, ``onu-config``, ``omcid``
     - The management pair in use: ``VLAN``, ``ADDRESS``, ``IFACE``, ``NAME``,
       ``PREF`` and ``DATA_ADDRESS``.
   * - ``/run/onu-system``
     - ``onu-config``
     - the boot script, ``onu-run``, ``onu-lane``
     - The stick's own settings, ``NAME=value``: ``HOSTNAME``, ``MGMT_VLAN``,
       ``MGMT_ADDRESS``, ``SFP_LANE``, ``NTP_SERVER``, ``LOG_SERVER``.
   * - ``/run/onu-auth``
     - ``onu-config``
     - ``onu-config``, ``onu-run``, ``onu-mgmt``, the console's profile
     - The hash of root's password, or ``!`` when the password is not known
       (the store cannot be read); empty while the default is in force. Mode
       0600.
   * - ``/run/shadow``
     - ``onu-config``
     - ``login``, ``getty`` and ``dropbear``, through ``/etc/shadow``
     - Root's entry; mode 0600.
   * - ``/run/dropbear/host_key``
     - ``onu-config``
     - ``dropbear`` (``-r``)
     - The ssh server's host key in its own format; mode 0600.
   * - ``/run/pond/tls.key``
     - the boot script, from ``/run/onu-tls-key``
     - ``pond`` (``-k``)
     - The panel's key, a P-256 scalar; mode 0400, account ``pond``.
   * - ``/run/pond/token``
     - ``onu-config token install``
     - ``pond`` (``-t``), at each request that has no session
     - The token of scripts, 64 hexadecimal characters; for the account
       ``pond`` alone (``pond/README.rst``, "The token of scripts").
   * - ``/run/dhcp6d/pdN``
     - ``dhcp6d`` (``-b``)
     - ``dhcp6d``, started again; ``onu-config pd N``
     - The host's binding, one line: its DUID in hex, its IAID, its address
       and the prefix it had (``dhcp6d/README.rst``).
   * - ``/run/config/network``, ``/run/network.d/PART``
     - ``onu-net``
     - netifd (``-c /run/config``)
     - netifd's configuration, and the parts it is made of, in the order of
       their names (``onu-utils/README.rst``, "onu-net").
   * - ``/run/onu-svc.d/NAME``
     - ``onu-apply``
     - ``onu-svc sync``
     - The instances of procd's service ``onu``, one file for each: the
       options and the command of the instance.
   * - ``/run/ubus/``
     - ``ubusd`` (its socket, ``ubus.sock``)
     - every program that calls ubus
     - The socket of ubus; the access lists are ``/usr/share/acl.d``. A pond
       built without libubus reads the files ``OBJECT`` of this directory
       instead, as the stand-in its tests use; the stick has none.

Private to one program or to onu-utils
======================================

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Path
     - What it keeps
   * - ``/run/onu-store``
     - What became of the settings: ``loaded``, ``defaults``, ``saved``,
       ``failed`` or ``none``. Told as the line ``store``.
   * - ``/run/onu-applied/``, ``/run/onu-kept``, ``/run/onu-pending``
     - The settings and accounts last applied, those a change on trial goes
       back to, and a change that ``onu-config change`` wrote and ``onu-apply
       try`` puts in force.
   * - ``/run/onu-mode``, ``/run/onu-mode.conf``
     - The shape in force, ``bridge`` or ``passthrough``, and the settings it
       was made from. Told as the line ``mode``.
   * - ``/run/onu-dns``, ``/run/onu-pass``, ``/run/onu-pass-proxy``,
       ``/run/onu-lan``
     - The name servers, the address handed on to the host, the gateway the
       peer is given and the link it is on. Told as the line ``dns`` and
       the lines whose names begin with ``pass.``.
   * - ``/run/onu-feeds``, ``/run/onu-feeds.key``
     - The GEM interfaces that carry a rule handing their frames to a
       session's GEM interface, one per line, to take them off again, and the
       key of those rules, which says whether they are as they should be.
   * - ``/run/onu-fw.conf``, ``/run/onu-fw.conf.new``
     - The description of ``onu-fw``'s table of netfilter for the local
       networks (NAT, the forwards, the DMZ and the firewall's defaults), as
       ``onu-apply`` put it in place, and the next one while it is written;
       gone with the table.
   * - ``/run/onu-guard.conf``, ``/run/onu-guard.conf.new``
     - The description of the guard of the stick's own programs, ``onu-fw``'s
       table of netfilter that keeps them to the management VLAN, as
       ``onu-apply`` put it in place, from the boot on, and the next one while
       it is written; gone when ``onu-fw`` refused it, so that the next apply
       tries again.
   * - ``/run/onu-fw-acl.conf``, ``/run/onu-fw-acl.conf.new``, ``/run/onu-fw.acl``
     - The firewall's rules as ``onu-fw acl`` puts them into the switch's ACL
       (ADR 0019), the next set while it is written, and what became of each
       rule, ``ID in|part|out ENTRIES [WHY]`` a line; gone with the last rule
       in force.
   * - ``/run/onu-omcid``, ``/run/omcid.mode``
     - What ``omcid`` was last told (the held VLANs) and the shape it starts
       in, so that one started again after a crash comes up as it was.
   * - ``/run/onu-pdN.kept``
     - The copy of ``/run/dhcp6d/pdN`` that ``onu-apply`` keeps across a
       change of the sessions.
   * - ``/run/onu-trial.d/``, ``/run/onu-trial``, ``/run/onu-trial.pon``,
       ``/run/onu-trial.omcid``, ``/run/onu-trial.reply``
     - The changes on trial, each with the moment it goes back (told as the
       lines ``trial.NAME``), and what the trial of a new image found:
       ``trial``, ``committed`` or ``failed``.
   * - ``/run/onu-apply.lock``, ``/run/onu-apply.again``
     - The lock of the one ``onu-apply`` that changes the stick, a symbolic
       link to its holder's PID, and the note that a change came while it ran.
   * - ``/run/onu-store.before``
     - The lines of a two-slot store of older builds, while they are in RAM
       only
       (``updating.rst``).
   * - ``/run/onu-tls-key``, ``/run/onu-ssh-key``, ``/run/onu-pond-token``
     - The keys and the token as ``onu-config load`` took them out of the
       store, before the boot script hands each to its program; mode 0600.
   * - ``/run/omcid/mib``
     - The MIB the OLT gave ``omcid``, kept so that a start again goes
       unseen; mode 0600, account ``omcid``.
   * - ``/run/omcid/``, ``/run/dhcp6d/``, ``/run/pond/``, ``/run/timed/``
     - The directories the boot script makes for the accounts of ``omcid``,
       ``dhcp6d``, ``pond`` and ``ntp`` (``timed``), each owned by its
       account. The one of ``pond`` is closed to all others (mode 0700).
   * - ``/run/udhcpd-lanV.conf``, ``.leases`` and ``.pid``
     - The DHCP server that hands a session's address down on the host link
       of VLAN V (``lanV``): its configuration, its one lease and its PID.
   * - ``/run/udhcpd-netN.conf``, ``.leases`` and ``.pid``
     - The DHCP server of local network N: its configuration, its leases,
       written every ten seconds and read by ``pond``, and its PID.
   * - ``/run/dropbear.pid``, ``/run/dropbear-pending.pid``
     - The ssh server's PID, and that of the one a management pair on trial
       starts beside it.
   * - ``/run/timed.pid``
     - The PID of the clock's daemon, which ``onu-run`` writes and
       ``onu-mgmt`` ends when the management link changes. BusyBox's own
       daemons write their PID files here too (``klogd.pid``,
       ``syslogd.pid``).
   * - ``/run/messages``, ``/run/omcid.log``, ``/run/dhcp6d.log``,
       ``/run/pond/log``, ``/run/onu-trial.log``
     - The system log (``syslogd``, one older copy beside it) and the logs of
       the programs that write their own. In memory, as a bench reads them.
   * - ``/run/resolv.conf.auto``
     - The name servers netifd's interfaces got (``-r``). Nothing in this
       tree reads it.

Changing a file
===============

* A **shared** file changes in both programs in one commit, with a test of
  each side that reads the same example. A new one is added to the first
  table in the commit that makes it.
* A **private** file may change with its program; a person who looks at it on
  a stick is reading the program's working notes.
* What a panel or a script needs of a private file is **told**: a new line of
  ``onu-apply state``, with its name in ``onu-utils/README.rst``, not a new
  reader of the file.
