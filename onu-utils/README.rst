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
   and ``onu-store password`` root's password, the line of its input, which it
   hashes in the form of ``crypt()`` (SHA-256 with 20000 rounds and a salt of
   16 random characters, by BusyBox's ``cryptpw``) into ``AUTH_HASH``;
   ``onu-store auth HASH`` stores a hash made elsewhere. A load puts the hash in
   ``/run/onu-auth`` for root only, and root's line of ``/run/shadow``, which
   ``/etc/shadow`` names, carries it for the console's login and the ssh
   server: one account, root, with one password for the console, ssh and the
   panel. Until one is set, the password is ``admin`` (a fixed hash of it); a
   hash of the panel's older form, which ``crypt()`` cannot check, leaves root
   without a password on the console and ssh (``*``), keys only, until a
   sign-in to the panel stores it again. A
   save with no account in force stores an empty ``PPPOE_USER``, so that a
   removed account does not come back at the next boot. Where the identity in
   force came from is ``store`` or ``factory`` in ``/run/onu-identity-source``.
   ``onu-store pd N`` keeps the host session N delegates its IPv6 prefix to,
   the line ``dhcp6d`` keeps in ``/run/onu-pdN``, as ``PDn_HOST``, and a load
   puts it back, so that after a restart the prefix goes to that router at
   once, though it asks for nothing; ``onu-mode`` calls it when the line
   changes or goes, and a store that holds the same writes nothing.

``passwd``
   a new password for root, at a terminal: it asks twice without echo and
   hands it to ``onu-store password``, so that it holds after a restart and in
   the panel at once. It takes no user and no old password, the stick having
   one account, root, whoever runs it is root already.

``onu-update``
   the two banks of the flash and the updates that go into them
   (``docs/handbook/updating.rst``)::

      onu-update status          # which bank runs, which starts next, each bank
      onu-update check FILE      # check an update file, writing nothing
      onu-update write FILE      # into the bank that does not run, read back
      onu-update read [BANK]     # a bank as an update file, on the output
      onu-update try [BANK]      # start a bank once, the other by default
      onu-update commit          # keep the running bank
      onu-update env [KEY | KEY=VALUE]...   # the boot loader's environment

   FILE ``-`` is the standard input, so an update streams in without being held
   whole. The running bank, told by ``root=``, is never written; after a start
   from RAM, ``write`` needs the bank named. The update file's layout is in
   ``src/update.h``, the environment's in ``src/bootenv.h``.

``onu-trial``
   run last by the boot script with the host link's address: after a start of
   a bank on trial, it commits the bank once omcid runs, the status daemon
   answers and the PON is in O5 (or has no light), and restarts into the
   committed bank when that does not happen within 300 s.

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
   gives the stick's data path the shape its settings give (``/run/onu.conf``,
   seeded from ``/etc/onu.conf``, which says what each key means); there is no
   mode to choose. Without a PPPoE account omcid connects the host link to the
   PON as the OLT provisions it. With accounts, the stick holds up to seven
   PPPoE sessions itself, each with the user and secret of
   ``/run/onu-pppoeN``, and each session runs on a GEM port the OLT
   provisioned, or on a VLAN on it (``wanVID``, shared by the sessions in that
   VLAN). A session's VLAN may be the subscriber's VID of a translation the
   OLT provisions, the one a router behind the stick would use: the session
   then runs in the VID that VID leads into on the fibre, as omcid publishes
   it in ``/run/omcid-xlate``. omcid goes on bridging the rest of what the
   OLT provisions, such as television and telephone, without the sessions'
   VLANs, which onu-mode writes to ``/run/onu-held-vlans`` before it has
   omcid build its bridge again; the frames of those VLANs that come down the
   other GEM ports go to the sessions' GEM interfaces, as an OLT may send a
   session's frames by any GEM port of its VLAN. A session untagged on its GEM
   port holds the whole port, and omcid then leaves the OLT's bridge out
   (``omcid -r``). Each session has its families: IPv4,
   IPv6 or both (``pppoed -4`` or ``-6``), as an operator that gives IPv6 on a
   session of its own asks. The PON port, the GEM interface and the VLAN get
   an MTU of 1508 first, so that a concentrator that agrees gives a session of
   1500-byte packets (RFC 4638). The first session that carries IPv4 hands its
   address on (``pppoed -P``): the address goes on no interface of the
   stick's, and the router behind the stick takes it from BusyBox's DHCP
   server on the link between them (a VLAN on ``sfp``, ``LAN_VLAN``, or
   ``sfp`` itself), as a /32 whose gateway is the concentrator's address, as
   a router that made the session itself would have it: the stick answers
   ARP for it on the link without taking it, so that a packet to it goes on
   to the concentrator (``PASS_PEER=off`` gives the stick's own address on
   the link, ``PASS_GATEWAY``, from which the DHCP server answers either
   way), given as a classless static route too (RFC 3442),
   with the session's name servers and MTU and leases of a minute, so that
   the router finds the address of a new session within one. The stick routes
   the address there with a host route, until the session ends, and forwards
   from the link only what comes from that address, by a strict reverse-path
   check (BCP 38). The switch then routes with the TTL and hop limit
   unchanged (``l3_ttl_decrement``): the stick stands in for the operator's
   ONT, which is no hop, and the concentrator is the router's first. The first
   session that carries IPv6 hands its delegated prefix on in the same way,
   through ``dhcp6d`` (see ``dhcp6d/README.rst``), once its IPv6 is up: the
   link to the router gets IPv6, and the stick forwards it. A watcher
   brings each session up once the OLT has provisioned its GEM port, one
   session every 5 s, as a concentrator may refuse one of two sessions that
   authenticate at once, and starts its client again when its link ends,
   each session after a wait of its own; what a session has is in
   ``/run/session/N``. For the bench, ``MODE=router`` makes the stick route
   instead: the LAN is the host link ``sfp``, with an address and BusyBox's
   DHCP server; the WAN is a GEM port or a VLAN on it, with an address by
   PPPoE (session 1's account), by DHCP (BusyBox's client, with ``dhcp-wan``
   as its script) or by hand. The stick translates no address and has no
   packet filter::

      onu-mode start      # at boot
      onu-mode apply      # after /run/onu.conf changed; saves it too
      onu-mode status
      onu-mode identity   # at boot: the identity to the PON driver, PON up
      onu-mode reidentify # after the identity changed: register again

   ``identity`` gives the PON driver the serial number and PLOAM password of
   ``/run/onu-identity`` (twenty hex digits of password as its ten octets) and
   brings the PON up; ``reidentify`` takes the PON down first and restarts
   omcid afterwards, so the OLT sees the stick leave and register anew.

``onu-mgmt``
   the VLAN and the address the stick is managed on from the host side, and a
   change of them that cannot lock anyone out. ``onu-mgmt start`` records the
   pair of the boot; ``add`` puts a new pair beside the old one, with the
   switch's rules that keep its VLAN from the PON and an ssh server of its
   own, and ``commit`` makes it the one in use and stores it, which a person
   can do only once the new pair answers; a change not committed goes again
   after ten minutes. netifd brings the pairs up: each is an interface of its
   own in onu-mgmt's part of netifd's configuration, so that the one a person
   manages the stick from is never taken down and up again when the other
   comes or goes::

      onu-mgmt start VLAN ADDRESS [DATA_ADDRESS]   # at boot
      onu-mgmt add VLAN ADDRESS
      onu-mgmt commit
      onu-mgmt cancel
      onu-mgmt status

``onu-net``
   writes netifd's configuration, ``/run/config/network``, from parts: each
   script that sets something up through netifd writes a part of its own,
   ``/run/network.d/PART``, and the parts in the order of their names are the
   whole. A change is written to a new file and renamed over the old, and
   netifd is told with SIGHUP, upon which the fork in ``netifd/`` reads its
   configuration again; the image has no ``ubus`` command. ``onu-net wait``
   waits until an address netifd is to put on a device is there::

      onu-net set PART <TEXT
      onu-net del PART
      onu-net wait DEV ADDRESS [SECONDS]

``onu-svc``
   gives procd the stick's programs as its services, over ubus, which procd
   starts, and again a set time after one ends; OpenWrt's shell scripts and
   jshn do that elsewhere. The boot script starts the log, netifd, the ssh
   server and ``pond`` with it, and ``onu-mode`` omcid, its watcher, dhcp6d
   and udhcpd. ``-r`` gives the threshold under which a run is a crash, the
   wait before the next start and the crashes in a row after which procd
   gives up, 60,2,0 unless given (0: never); ``-l`` sends the program's
   output to the system log, ``-o`` appends it to a file. ``list`` tells
   each service's state and how often procd started it again (a procd
   patch, ``br2-external/package/procd``)::

      onu-svc start NAME [-r THRESHOLD,TIMEOUT,RETRY] [-l | -o FILE] -- PROGRAM [ARG...]
      onu-svc stop NAME
      onu-svc list [NAME]

``onu-run``
   starts a service of the stick in the foreground, as procd wants it, from
   the state the scripts keep: the ssh server and ``pond``, on the
   management pair in use and the host link's address, from
   ``/run/onu-mgmt``, so that one started again after a change of the pair
   listens on the new one; omcid, in the mode ``onu-mode`` last gave it
   (``/run/omcid.mode``), so that one started again after a crash comes up in
   the mode the stick is in; and the console's login prompt, or, on a bench
   whose ``/etc/board.conf`` has ``CONSOLE_AUTOLOGIN=root``, root's shell
   without the password::

      onu-run ssh
      onu-run pond
      onu-run omcid
      onu-run console

Building and testing::

   make           # build/ponid, build/pon, build/onucfg, build/onu-update, build/onu-svc
   make check     # the tests

``ponid``, ``onucfg`` and ``onu-update`` need zlib, ``onucfg`` and
``onu-update`` the kernel's MTD header
``mtd/mtd-user.h``, ``onu-svc`` libubus, libubox and libblobmsg_json (its
tests need none of them), ``pon`` libmnl and the kernel's headers, among them
``linux/pon.h`` of this repository's kernel; for a build on the host, point
CPPFLAGS at an installation of those headers (``make headers_install`` in
``linux/``). The tests of ``pon`` read and print messages that they build; the
tests of ``onu-mode`` and ``dhcp-wan`` run them against stand-ins for the
programs they call, in a scratch root, and those of ``onu-store`` run it with
``build/onucfg`` on a store in a file and a stand-in for BusyBox's
``cryptpw``. ``onu-update`` is tested on a flash
made of files under ``ONU_ROOT``, with update files packed by the image build's
``mkupdate.py``, which needs python3; ``onu-trial`` against stand-ins.
