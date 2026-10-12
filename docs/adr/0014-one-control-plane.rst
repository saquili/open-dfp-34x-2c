.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==============================================================================
ADR 0014: One control plane turns the settings into what procd and netifd keep
==============================================================================

:Status: Accepted, amended by 0019 (step 9), by 0020 (iproute2) and by the
         entries under "Amendments"
:Date: 2026-10-10
:Decided-by: the owner, 2026-10-10

.. note::

   In plain words: the stick gets one part that owns its settings. It checks
   a change, works out from the settings which programs the stick is to run
   and which interfaces it is to have, and hands that to procd and netifd,
   which change only what differs. A change of one connection no longer
   drops the others, a change that cuts the panel off goes back by itself,
   and a program that procd starts again goes on where it was.

Context
=======

procd keeps the stick's programs (ADR 0013) and netifd its interfaces, but
netifd has only the management VLAN so far. The rest of the network is
``onu-mode``'s, a shell script that is at once the keeper of what the
settings mean, the one that applies them, and a watcher:

* A change in the panel writes the settings and runs ``onu-mode apply``,
  which takes the whole mode down and brings it up again (``pass_down``,
  then ``pass_up``). A change of one session's slot drops every session; on
  a stick in service that includes the operator's.
* What the settings mean is written twice: in shell in ``onu-mode``, and in
  C in pond, which works out for its pages what onu-mode will do with them
  (``pond/src/prov_settings.c``: "as onu-mode takes it", "as onu-mode
  decides"). A new rule needs both, and the two can part.
* A change that may lock someone out is tried and undone in three places,
  each in its own way: ``onu-mgmt`` for the management address,
  ``onu-lane`` for the host lane's mode, ``onu-trial`` for an image. A
  change of the sessions has no such thing: a wrong one stays until someone
  repairs it.
* ``onu-mode`` sets addresses, routes, rules and proxy entries with ``ip``
  itself, some forty times, keeps pppoed in loops of its own, and looks for
  changes every five to eight seconds (``onu-mode watch``). netifd takes off
  a device it owns what it did not set itself, so the two cannot share one.
* The programs' state is in files of ``/run``, each in a format of its own,
  which pond reads one by one. A program that procd starts again starts from
  nothing: omcid loses the MIB the OLT gave it, and the stick carries no
  service until the OLT provisions it again, which an OLT that keeps the
  stick in operation (O5) does not do by itself.
* Every program runs as root, pond too, which takes HTTP from the host side.

OpenWrt has a layer for this: one model of the settings (UCI), procd's
reload of the services a change concerns, rpcd between the web interface
and the system, with access lists, and a web interface that applies a
change on trial and undoes it unless the browser confirms it. The image
carries neither UCI nor jshn (ADR 0013), and needs a small equivalent of
its own. Its two daemons already do the hard part. procd's ``service set``
replaces the list of a service's instances: it starts the new ones, stops
those no longer listed, starts again only one whose command changed, and
leaves the rest alone. netifd, when it reads its configuration again,
changes only the interfaces whose configuration changed.

Decision
========

The stick's settings drive it through one control plane of five parts.

The settings
   The store keeps them, as today (``onucfg``, ``onu-store``). One table in
   C, in ``onu-utils``, describes each key: its type, its range and its
   default. ``onucfg check`` tests a whole set of settings against it and
   against the rules that bind keys together, such as no two slots on one
   VLAN. pond asks it rather than knowing the rules, and shows what the
   stick did, from its state, not what it reckons onu-mode would do.
   ``onucfg`` hands the settings on with every default filled in, so that
   nothing else keeps defaults of its own.

The generator
   ``onu-mode`` becomes a function: from the settings and the facts of the
   moment (the VLANs the OLT lets through each GEM port, the interfaces
   netifd has up, the prefixes delegated) it writes what the stick is to be,
   and takes nothing down itself:

   * netifd's configuration, through the parts of ``onu-net``: the
     management VLAN, each session, each link to the host, and their
     routes, tables, rules and proxy entries;
   * one procd service, ``onu``, whose instances are all the programs the
     settings ask for: omcid, a dhcp6d for each session with IPv6, a udhcpd
     for each local network; ``onu-svc`` gives procd the whole list at once;
   * later, the firewall's rules, which go to the kernel in one netlink
     batch, all or nothing.

   Run twice on the same input, it changes nothing. It runs after a change
   of the settings, on netifd's events (netifd runs it for each event of an
   interface, ``netifd -h``), and when omcid publishes a change.
   ``onu-mode watch`` and its loops go. The generator stays a shell script:
   what it writes is text, which the tests compare on any host.

pppoed under netifd
   A protocol of the fork, ``pppoed``, starts pppoed for an interface, on
   the code the fork's external protocols share (``proto-ext.c``), and
   pppoed tells netifd the session's address, peer, routes and name
   servers over ubus (``notify_proto``), as OpenWrt's PPP scripts do, but
   in C and without jshn. netifd sets them, and starts pppoed again when it
   ends.

One transaction
   A change of the settings is tried before it is kept: the stick keeps the
   settings it had, applies the new ones and waits for a confirmation,
   which the panel gives by itself as soon as it reaches the stick again.
   Without one in 90 seconds the stick goes back to the settings it had and
   applies them. Only a confirmed change is written to the flash.
   ``onu-mgmt``'s pending pair and ``onu-lane``'s trial become cases of it;
   an image's trial stays the boot loader's (``onu-trial``).

The state on ubus
   omcid and dhcp6d each publish an object on ubus with their state, and
   send an event when it changes; a session's state is netifd's, which
   publishes it already. pond reads the state there. A file of ``/run``
   stays while something still reads it.

Two rules hold for every program of the stick:

* A start again goes unseen. A program keeps in ``/run`` the state its
  service needs, which outlives the program but not the stick, and takes
  it up again when it starts: dhcp6d does (``-b``), and omcid keeps its MIB
  there. pppoed is the exception: a session ends with its process, and
  netifd opens a new one.
* No program has more than it needs. pond runs as an account of its own,
  without a password or a shell, with which no one logs in; root stays the
  one account to log in with (ADR 0012). procd starts pond as that account,
  and ubusd's access lists (``/usr/share/acl.d``) let it read the state and
  ask the control plane for a change, and nothing else. A client running as
  root is not bound by them.

The way there
=============

Each step is a commit with its tests, and none leaves a device with two
owners: what moves to netifd moves whole.

1. The table of the settings and ``onucfg check``; pond asks it, and its
   copy of the rules goes.
2. ``onu-svc`` gives procd a whole service with its instances; onu-mode
   gives its programs as one list.
3. The fork's ``pppoed`` protocol and pppoed's reports over ubus; the
   sessions become netifd's interfaces, and onu-mode's loops go.
4. The links to the host, with their routes, tables, rules and proxy
   entries, become netifd's; netifd's events run the generator.
   ``onu-mode watch`` goes, and so does the boot script's fallback that
   sets the management address with ``ip`` when netifd has not.
5. The transaction and the panel's confirmation; ``onu-mgmt`` and
   ``onu-lane`` take it up.
6. The state on ubus: omcid and dhcp6d publish it, and pond reads it.
7. omcid keeps its MIB in ``/run``.
8. pond as an account of its own, under ubusd's access lists.
9. The settings in the ``config`` partition's own file system, beside the
   shipped firmware's files (added on 2026-10-10; ADR 0019 has it).

Consequences
============

* A change of one slot leaves the other sessions up, and a change that cuts
  the panel off is undone by itself.
* What the settings mean is in one place, the table, and pond and the
  generator take it from there.
* netifd alone sets addresses and routes. Most of the image's uses of
  ``ip`` go with onu-mode's; iproute2 left the image in a step of its own
  (ADR 0020).
* pond reads ubus as well as the kernel's interfaces, which it did not.
  libubus is in the image already (procd, netifd); pond, omcid and dhcp6d
  add only their calls to it. Each step measures what it adds to the image.
* ADR 0013 kept pppoed in onu-mode's loop until it moved under netifd; this
  record moves it. The boot script's services (the log, netifd, the ssh
  server, the status daemon) stay services of their own, and onu-mode's
  become the instances of ``onu``.
* While the move goes on, onu-mode still sets what has not moved yet; the
  tests of each step check that netifd and onu-mode never touch one device.

Amendments
==========

**2026-10-10, the owner: step 6, the state on ubus.**

omcid's part of step 6 goes with step 8: until pond leaves root, nothing reads
omcid's state on ubus, and then pond needs what it reads of the MIB there too,
under ubusd's access lists.

**2026-10-10, the owner: a ninth step.**

The settings go into the file system that the shipped firmware keeps in the
config partition, instead of taking the partition. This is ADR 0019, which has
the decision and what it cost; the step's text left this record on 2026-10-12.

**2026-10-11, the owner: iproute2 leaves the image.**

The decision and its consequences are ADR 0020, which also holds the decision
of 2026-10-12 that the scripts run no ip. The text left this record on
2026-10-12.

**2026-10-12, the owner: accounts of their own (second rule).**

The second rule of the decision is carried out: pond (before), ntpd, dhcp6d,
pppoed and omcid run as accounts of their own, uid 200 to 204, started by
``onu-drop`` with only the powers each uses (``net_admin``, ``net_raw``,
``net_bind_service``, ``sys_time``), which they keep for what they run, with
the rest out of the bounding set and ``no_new_privs``; ubusd's lists let each
publish its objects and call what it needs. Since only root may run
``onu-apply``, omcid and dhcp6d tell it of a change by ``onu-call``, which
asks onu-act for a method of their own name, and nothing else. omcid writes in
``/run/omcid`` and reads an identity without the PLOAM password; dhcp6d keeps
the host's binding in ``/run/dhcp6d``; a session's directory belongs to the
group ``onu``, and the file of its account to the group ``pppoed``. netifd
stays root's: it starts the sessions as pppoed, writes sysctls and runs
``onu-apply`` for the events of the interfaces, so a netifd of its own would
need the power to start a program as another account and a way to ask
``onu-apply iface`` from an account that is not root's. The handbook lists who
runs as whom (``docs/handbook/system.rst``).

**2026-10-12, the owner: no events, no state files (fifth part, step 6).**

The events of "The state on ubus" are not sent, and pond reads no file of
``/run``. omcid and dhcp6d run the program of ``-x``, ``onu-apply``, when what
they publish changes, which is how the stick follows it; nothing listened to
the events on ubus, and a second notice of one change is one more thing to
keep in step. Their objects stay, with the method ``status``. What no program
of the stick publishes, the state that ``onu-apply`` keeps in files (the mode,
each session's facts, the address handed on, the changes on trial), is told by
``onu-apply state``, which onu-act runs for pond (``settings_state``), in
lines ``name=value`` that are the contract; the files under them may change.

**2026-10-12: the notes of trials.**

The record had gathered the notes of trials among its paragraphs. They are out
of it: the trial of the shipped firmware's packages is in
docs/handbook/updating.rst, and the others (steps 3, 4, 5 and 7 on a stick
with live sessions, and the removal of iproute2) stand in the history of this
file.
