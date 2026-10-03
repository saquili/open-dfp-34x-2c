.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========
onu-utils
=========

The small programs of the ONU.

``ponid``
   prints the identity of the unit (serial number, vendor ID, version,
   equipment ID, PLOAM password and the values of the ONU-G and ONU2-G that
   G.988 has the ONU report) from its factory configuration, the JFFS2 file
   system in the partition ``config``. At boot it fills ``/run/onu-identity``,
   which omcid reads, and gives the serial number to the PON driver. Its output
   holds the PLOAM password, a credential of the line: keep it private.

``pon``
   shows the PON devices of the kernel and their ONU, through the kernel's
   ``pon`` generic netlink family: the activation state, the ONU-ID, the
   Alloc-ID and the Port-ID of the OMCI channel that the OLT assigned, the
   alarms, the GEM ports and T-CONTs, and the counters. The serial number, the
   credential the OLT takes the line by, is printed only with ``-s``.
   ``pon monitor`` prints every change the kernel announces, with the time it
   came, which is how the time to O5 and every fall out of it are seen::

      pon  GPON  O5 operation  ONU-ID 3  Alloc-ID 3  OMCC 3
         alarms none  GEM ports 2  T-CONTs 1  serial number set (-s shows it)
          PLOAM messages             120
          bandwidth maps             5000000
          T-CONT 0  Alloc-ID 1024

``onu-mode``
   puts the stick in the mode its settings name (``/run/onu.conf``, seeded from
   ``/etc/onu.conf``, which says what each key means). In bridge mode omcid
   connects the host link to the PON as the OLT provisions it. In router mode
   omcid leaves the OLT's bridge out (``omcid -r``) and the stick routes: the
   LAN is the host link ``sfp``, with an address and BusyBox's DHCP server;
   the WAN is a GEM port the OLT provisioned, or a VLAN on it, with an address
   by PPPoE (pppoed, with the user and secret of ``/run/onu-pppoe``; the PON
   port, the GEM interface and the VLAN get an MTU of 1508 first, so that a
   concentrator that agrees gives a session of 1500-byte packets, RFC 4638), by DHCP
   (BusyBox's client, with ``dhcp-wan`` as its script) or by hand; and an
   nftables table translates what leaves by the WAN, clamps the MSS of TCP
   connections to the route's MTU, and hands the flows to the switch through a
   flowtable. A watcher brings the WAN up once the OLT has
   provisioned its GEM port, and starts the PPPoE client again when its link
   ends::

      onu-mode start      # at boot
      onu-mode apply      # after /run/onu.conf changed
      onu-mode status

Building and testing::

   make           # build/ponid and build/pon
   make check     # the tests

``ponid`` needs zlib, ``pon`` libmnl and the kernel's headers, among them
``linux/pon.h`` of this repository's kernel; for a build on the host, point
CPPFLAGS at an installation of those headers (``make headers_install`` in
``linux/``). The tests of ``pon`` read and print messages that they build; the
tests of ``onu-mode`` and ``dhcp-wan`` run them against stand-ins for the
programs they call, in a scratch root.
