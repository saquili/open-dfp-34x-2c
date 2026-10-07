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
   system in the partition ``config``. Until the configuration store holds an
   identity, it is the one ``onu-store`` puts in ``/run/onu-identity`` at boot,
   which omcid reads and which gives the serial number to the PON driver, and
   the one the store's first write takes in. Its output holds the PLOAM
   password, a credential of the line: keep it private.

``onucfg``
   reads and changes the configuration store: the partition ``config`` split
   into two slots, each a header and ``KEY=VALUE`` lines. A change writes the
   slot that is not current and then makes it current, so that a power loss in
   the middle leaves the old configuration whole; a change that changes nothing
   writes nothing::

      onucfg --store /dev/mtd3 list
      onucfg --store /dev/mtd3 get KEY
      onucfg --store /dev/mtd3 set KEY VALUE
      onucfg --store /dev/mtd3 unset KEY
      onucfg --store /dev/mtd3 import <lines     # every KEY=VALUE, one write

``onu-store``
   keeps the unit's identity (keys ``ID_*``), its settings (the keys of
   ``/run/onu.conf``) and its PPPoE account (``PPPOE_USER``,
   ``PPPOE_SECRET``) in the store. ``onu-store load`` puts them in the files
   the running system reads, at boot; ``onu-store save`` writes the settings
   and the account in force back, which ``onu-mode apply`` does. The store
   shares the partition with the shipped firmware's configuration, and its
   first write overwrites the start of it: so the first save takes the
   identity ``ponid`` reads into the same write, and ``onu-store migrate`` does
   that alone, refusing when there is no identity to take. What became of the
   settings is one word in ``/run/onu-store`` (``loaded``, ``defaults``,
   ``saved``, ``failed``, ``none``), which the status daemon shows. The status
   daemon also keeps two things there through it: ``onu-store identity`` takes
   the identity of ``KEY=VALUE`` lines on its input (``SERIAL``, ``PLOAM``, …)
   and ``onu-store auth HASH`` the administrator's password hash
   (``AUTH_HASH``), which a load puts in ``/run/onu-auth`` for root only. A
   save with no account in force stores an empty ``PPPOE_USER``, so that a
   removed account does not come back at the next boot.

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
   (BusyBox's client, with ``dhcp-wan`` as its script) or by hand. The stick
   translates no address and has no packet filter: a LAN host reaches beyond
   the WAN with an address that the network behind the WAN routes back to the
   stick. A watcher brings the WAN up once the OLT has
   provisioned its GEM port, and starts the PPPoE client again when its link
   ends. Passthrough mode is router mode with the WAN's PPPoE session opened
   by the stick and its IPv4 address handed on (``pppoed -P``): the address
   goes on no interface of the stick's, and the router behind the stick takes
   it from BusyBox's DHCP server on the link between them (a VLAN on ``sfp``,
   ``LAN_VLAN``, or ``sfp`` itself), as a /32 whose gateway is the stick's
   address on that link (``PASS_GATEWAY``), given as a classless static route
   too (RFC 3442), with the session's name servers and MTU and leases of a
   minute, so that the router finds the address of a new session within one.
   The stick routes the address there with a host route, until the session
   ends::

      onu-mode start      # at boot
      onu-mode apply      # after /run/onu.conf changed; saves it too
      onu-mode status

Building and testing::

   make           # build/ponid, build/pon and build/onucfg
   make check     # the tests

``ponid`` and ``onucfg`` need zlib, ``onucfg`` the kernel's MTD header
``mtd/mtd-user.h``, ``pon`` libmnl and the kernel's headers, among them
``linux/pon.h`` of this repository's kernel; for a build on the host, point
CPPFLAGS at an installation of those headers (``make headers_install`` in
``linux/``). The tests of ``pon`` read and print messages that they build; the
tests of ``onu-mode`` and ``dhcp-wan`` run them against stand-ins for the
programs they call, in a scratch root, and those of ``onu-store`` run it with
``build/onucfg`` on a store in a file.
