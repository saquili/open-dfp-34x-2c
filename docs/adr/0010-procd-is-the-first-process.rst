.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===========================================================
ADR 0010: Run procd as the first process and the supervisor
===========================================================

:Status: Accepted
:Date: 2026-10-20
:Deciders: the maintainer

Summary
=======

procd, OpenWrt's init process, starts the stick and supervises its
programs: when one ends, procd starts it again. A crash of the panel, the
ssh server or the network daemon is over in about a second, without
restarting the stick.

Context
=======

The stick runs a dozen daemons: the ssh server, the status daemon, ubusd,
netifd, the log, omcid, pppoed, dhcp6d and udhcpd. Started in the
background by a boot script, as BusyBox's init does, nothing starts one
again when it ends: a crash of pond leaves the stick without its panel until
the next boot, and a crash of omcid leaves it without the OLT's
provisioning.

The image takes OpenWrt's network layer, netifd and ubus (``netifd/``,
``buildroot/patches/0004``). A project that writes for mainline would not
write a supervisor of its own, nor add a second init system unrelated to
the rest: it takes the one that comes with that network layer and fixes it
where the device needs it. That is procd, OpenWrt's first process, built
together with netifd and ubus, on which prplOS, the open software that
operators build for home gateways and ONTs, also runs.

Decision
========

procd is process 1.

* ``/sbin/init``, a script of the image, mounts what the kernel does not and
  execs procd. OpenWrt's own ``/sbin/init`` is not used: it mounts a tmpfs
  on ``/dev`` and runs OpenWrt's preinit and failsafe, which the image does
  not have. procd keeps the kernel's devtmpfs (patch 0002 of
  ``br2-external/package/procd``).
* procd feeds the watchdog from its start; BusyBox's init and watchdog are
  not built.
* procd starts ubusd and runs ``/etc/inittab``: the boot script at sysinit
  (``/etc/rc.d/S10boot``), then the console's login, which is started again
  half a second after it ends.
* The stick's programs are procd services. The boot script and
  ``onu-apply`` start them in order with ``onu-svc``, a small C program of
  this tree that hands procd a service over ubus. procd starts each again
  2 s after it ends and never gives up; a patch makes it report how often
  (``onu-svc list``).
* procd is OpenWrt's at its commit of 2026-10-07, with patches: udebug is
  left out, as the image carries neither OpenWrt's udebug library nor the
  daemon that collects its buffers; the kernel's devtmpfs is kept; a program
  of inittab without a terminal is started again too, which procd does not
  do by itself because OpenWrt's inittab holds only console entries; and an
  instance reports how often procd started it again. libubox and ubus are
  taken at OpenWrt's commits that match it; libubox is built whole, with the
  udebug code of its own, whose atomic operations the core runs (ADR 0021).
* A bench may give the console root's shell without a password
  (``CONSOLE_AUTOLOGIN=root`` in ``/etc/board.conf``), so that it can reach
  a stick whose network is down. An image without that file asks for the
  password (ADR 0009).

Consequences
============

* The image grows by some 46 KB: procd (38 KB compressed), libjson_script,
  which procd loads, and the C library's new symbols; BusyBox shrinks by
  3 KB (``r0`` 1,787,967 bytes with the ONT overlay, 243,649 left;
  2026-10-10).
* OpenWrt gives procd its services through shell scripts and jshn, which the
  image does not carry; ``onu-svc`` gives them from the command line, and the
  services exist in procd only, written in no file. ``onu-svc`` is 4 KB.
* A restarted program must not lose what its service needs: ADR 0011 keeps
  that state in ``/run``, puts pppoed under netifd, and keeps omcid's MIB.
* procd reads 127 characters of an inittab line and 7 words of a command;
  the tests keep the inittab within that.
* A restart goes through procd's shutdown entry in inittab, which the image
  must keep: without it procd waits for ever while feeding the watchdog.
* procd reports once at start that ``/etc/hotplug.d`` and the watch of
  hotplug rule files are missing (the kernel has no inotify). The image has
  no hotplug scripts or rules, and nothing depends on them.
