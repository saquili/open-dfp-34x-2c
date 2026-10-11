.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

====================================================================
ADR 0013: procd is the first process and watches the stick's programs
====================================================================

:Status: Accepted, amended by 0014 (pppoed under netifd, omcid's MIB kept)
:Date: 2026-10-10
:Decided-by: the owner, 2026-10-10

.. note::

   In plain words: the program that starts the stick, procd from OpenWrt,
   also watches its programs, and starts one again when it ends. A crash of
   the panel, the ssh server or the network daemon is over in a second,
   without a restart of the stick.

Context
=======

BusyBox's init started the boot script, and the script started the stick's
daemons in the background: the ssh server, the status daemon, ubusd, netifd,
the log, and through onu-mode omcid, pppoed and dhcp6d. Nothing started one
again when it ended. A crash of pond left the stick without its panel until
the next start, one of omcid without the OLT's provisioning. Only pppoed had
a loop of its own in onu-mode.

The image takes OpenWrt's network layer, netifd and ubus (``netifd/``,
``buildroot/patches/0004``), and the owner asked how a team that writes code
for mainline would give the stick a supervisor. Such a team would not write
one of its own, nor add a second system that has nothing to do with the
rest: it takes the one of the layer the system already comes from, and fixes
it where the device needs it. That is procd, OpenWrt's first process, which
is made with netifd and ubus, and on which prplOS, the open software that
operators build for their home gateways and ONTs, runs as well.

Decision
========

procd is process 1.

* ``/sbin/init``, a script of the image, mounts what the kernel does not and
  execs procd. OpenWrt's own ``/sbin/init`` is not used: it mounts a tmpfs on
  ``/dev`` and runs OpenWrt's preinit and failsafe, which the image has none
  of. procd keeps the kernel's devtmpfs (patch 0002 of
  ``br2-external/package/procd``).
* procd feeds the watchdog, from its start; BusyBox's init and watchdog are
  not built.
* procd starts ubusd and runs ``/etc/inittab``: the boot script at sysinit
  (``/etc/rc.d/S10boot``) and then the console's login, which is started
  again half a second after it ends.
* The stick's programs are procd's services, which the boot script and
  ``onu-mode`` start in their order with ``onu-svc``, a small C program of
  this tree that gives procd a service over ubus: the log, netifd, the ssh
  server and the status daemon; omcid, onu-mode's watcher, dhcp6d and udhcpd.
  procd starts each again 2 s after it ends and never gives up; a patch has
  it tell how often (``onu-svc list``). omcid started again takes the mode
  the stick is in from ``/run/omcid.mode`` (``onu-run omcid``).
* procd is OpenWrt's at its commit of 2026-10-07, with patches: udebug, whose
  ring buffer needs atomic operations, is left out; the kernel's devtmpfs is
  kept; and a program of inittab without a terminal is started again too,
  which procd did not do, as OpenWrt's inittab has only the console's
  entries; and an instance tells how often procd started it again. libubox
  and ubus are taken at OpenWrt's current commits to match.
* A bench may give the console root's shell without the password
  (``CONSOLE_AUTOLOGIN=root`` in ``/etc/board.conf``), so that it reaches a
  stick whose network is down. The image without that file asks for the
  password, as ADR 0012 has it.

Consequences
============

* The image grows by some 46 KB: procd, 38 KB compressed, libjson_script,
  which it loads, and the C library's new symbols; BusyBox is 3 KB smaller
  (r0 1787967 bytes with the ONT overlay, 243649 left).
* OpenWrt gives procd its services with shell scripts and jshn, which the
  image does not carry; onu-svc gives them from the command line, and the
  services live in procd only, written in no file. onu-svc is 4 KB.
* pppoed stays in onu-mode's loop, which reads its events and sets up the
  session's routes and addresses: that is netifd's to do, and pppoed moves
  under netifd, not under procd.
* An omcid that procd starts again has lost its MIB: the OLT has to provision
  the stick again, as after any restart of omcid.
* procd reads 127 characters of an inittab line and 7 words of a command;
  the tests keep the inittab within that.
* A restart goes through procd's shutdown entry of inittab, which the image
  must keep: without it procd waits for ever, feeding the watchdog.
* procd says once at start that ``/etc/hotplug.d`` and the watch of hotplug
  rule files are missing (the kernel has no inotify); the image has no
  hotplug scripts or rules, and nothing depends on them.
