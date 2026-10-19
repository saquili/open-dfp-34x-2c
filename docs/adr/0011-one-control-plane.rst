.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===================================================
ADR 0011: Drive the stick through one control plane
===================================================

:Status: Accepted
:Date: 2026-10-12
:Deciders: the maintainer

Summary
=======

One part of the system owns the stick's settings. It checks a change, works
out from the settings which programs the stick runs and which interfaces it
has, and hands that to procd and netifd, which change only what differs. A
change of one connection does not drop the others, a change that cuts the
panel off is undone by itself, and a program that procd restarts carries on
where it was.

Context
=======

procd keeps the stick's programs (ADR 0010) and netifd its interfaces. What
turns the settings into programs and interfaces has to meet these needs:

* A change of one session must not drop the others; on a stick in service
  they include the operator's.
* What the settings mean must be written once. A panel that works out on
  its own what the stick will do with a setting drifts apart from the stick.
* A change that may lock someone out, of the management address, the host
  lane's mode or the sessions, must be tried and undone the same way when it
  fails.
* Each device has one owner. netifd removes from a device it owns whatever
  it did not set itself, so no script may set addresses or routes beside it.
* A program that procd restarts must not start from nothing. An omcid that
  loses the MIB the OLT gave it carries no service until the OLT provisions
  it again, which an OLT that keeps the stick in operation (O5) does not do
  by itself.
* The status daemon takes HTTP from the host side and must not run as
  root.

Options considered
==================

One script that applies everything
    The simplest form: on each change, take the whole configuration down and
    bring it up again, with ``ip`` and loops of its own. It drops every
    session on any change and cannot share devices with netifd. Rejected.
OpenWrt's layer
    One model of the settings (UCI), procd's reload of the services a change
    concerns, rpcd between the web interface and the system with access
    lists, and a web interface that applies a change on trial and undoes it
    unless the browser confirms it. The image carries neither UCI nor jshn
    (ADR 0010), so the layer cannot be taken whole.
A small equivalent of our own
    procd and netifd already do the hard part. procd's ``service set``
    replaces a service's list of instances: it starts the new ones, stops
    those no longer listed, restarts only one whose command changed and
    leaves the rest alone. netifd, when it rereads its configuration,
    changes only the interfaces whose configuration changed. Chosen.

Decision
========

The stick's settings drive it through one control plane of five parts.

The settings
    The store keeps them (``onu-config``). One table in C, in
    ``onu-utils``, describes each key: its type, its range and its default.
    ``onu-config check`` tests a whole set of settings against the table and
    against the rules that bind keys together, such as no two slots on one
    VLAN. pond asks it rather than knowing the rules, and shows what the
    stick did, from its state, not what it reckons the stick will do.
    ``onu-config`` hands the settings on with every default filled in, so
    that nothing else keeps defaults of its own.

The generator
    ``onu-apply`` is a function. From the settings and the facts of the
    moment (the VLANs the OLT lets through each GEM port, the interfaces
    netifd has up, the delegated prefixes) it writes what the stick is to
    be, and takes nothing down itself:

    * netifd's configuration, through the parts of ``onu-net``: the
      management VLAN, each session, each link to the host, and their
      routes, tables, rules and proxy entries;
    * one procd service, ``onu``, whose instances are all the programs the
      settings ask for: omcid, a dhcp6d for each session with IPv6 and a
      udhcpd for each local network; ``onu-svc`` gives procd the whole list
      at once;
    * the firewall's table and rules (``onu-fw``, ADR 0019), which go to the
      kernel in one netlink batch, all or nothing.

    Run twice on the same input, it changes nothing. It runs after a change
    of the settings, on netifd's events (netifd runs it for each event of an
    interface, ``netifd -h``), and when omcid or dhcp6d report a change. The
    generator is a shell script: what it writes is text, which the tests
    compare on any host.

pppoed under netifd
    A protocol handler of the netifd fork, ``pppoed``, starts pppoed for an
    interface, on the code that the fork's external protocols share
    (``proto-ext.c``). pppoed reports the session's address, peer, routes
    and name servers to netifd over ubus (``notify_proto``), as OpenWrt's
    PPP scripts do, but in C and without jshn. netifd sets them, and starts
    pppoed again when it ends.

One transaction
    A change of the settings is tried before it is kept: the stick keeps the
    settings it had, applies the new ones and waits for a confirmation,
    which the panel gives by itself as soon as it reaches the stick again.
    Without one within 90 seconds, the stick goes back to the settings it
    had and applies them. Only a confirmed change is written to the flash.
    The management address and the host lane's mode are cases of it; an
    image's trial stays with the boot loader (``onu-trial``).

The state
    omcid and dhcp6d each publish an object on ubus with the method
    ``status``; a session's state is netifd's, which publishes it already.
    When what they publish changes they run ``onu-apply``, through
    ``onu-call``, which asks ``onu-act`` for the method of their own name
    and nothing else; no events are sent on ubus. The state that no program
    publishes, which ``onu-apply`` keeps in files (the mode, each session's
    facts, the address handed on, the changes on trial), is reported by
    ``onu-apply state``, which ``onu-act`` runs for pond, as ``name=value``
    lines. Those lines are the contract; the files beneath them may change.
    pond reads the state there and reads no file under ``/run``.

Two rules hold for every program of the stick:

* **A restart goes unnoticed.** A program keeps the state its service needs
  in ``/run``, which outlives the program but not the stick, and takes it up
  again when it starts: dhcp6d does (``-b``), and omcid keeps its MIB there.
  pppoed is the exception: a session ends with its process, and netifd opens
  a new one.
* **No program has more than it needs.** pond, ntpd, dhcp6d, pppoed and
  omcid run as accounts of their own, uid 200 to 204, without a password or
  a shell, started by ``onu-drop`` with only the capabilities each uses
  (``net_admin``, ``net_raw``, ``net_bind_service``, ``sys_time``), the rest
  removed from the bounding set and ``no_new_privs`` set. ubusd's access
  lists (``/usr/share/acl.d``) let each publish its objects and call what it
  needs, and nothing else; a client running as root is not bound by them.
  Root stays the one account to log in with (ADR 0009). netifd stays root's:
  it starts the sessions as pppoed, writes sysctls and runs ``onu-apply`` for
  the events of the interfaces, so an unprivileged netifd would need the
  power to start a program as another account and a way to request
  ``onu-apply iface`` without root. ``docs/handbook/system.rst`` lists which
  program runs as which account.

Consequences
============

* A change of one slot leaves the other sessions up, and a change that cuts
  the panel off is undone by itself.
* What the settings mean is in one place, the table, and pond and the
  generator take it from there.
* netifd alone sets addresses and routes; the scripts make no ``ip`` calls
  (ADR 0017).
* pond reads ubus as well as the kernel's interfaces. libubus is in the
  image for procd and netifd; pond, omcid and dhcp6d add only their calls to
  it.
* The settings live in a file of the ``config`` partition's own file
  system, beside the shipped firmware's files (ADR 0016).
