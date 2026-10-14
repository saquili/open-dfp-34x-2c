.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========
onu-utils
=========

The small programs of the ONU. A person at the stick runs ``onu-config``,
``onu-apply``, ``onu-update``, ``onu-svc``, ``pon``, ``onu-nl`` and
``passwd``, which are in ``/usr/sbin`` and ``/usr/bin``. The others only other
programs run, and they are in ``/usr/lib/onu``, out of a person's way (ADR
0015): ``ponid``, ``onu-mgmt``, ``onu-net``, ``onu-run``, ``onu-lane``,
``onu-change``, ``onu-trial``, ``onu-act``, ``onu-drop``, ``onu-call`` and
``onu-config.sh``. A script that runs one of them finds it there, and names it
by its whole path where procd starts it.

``ponid``
   prints the identity of the unit (serial number, vendor ID, version,
   equipment ID, PLOAM password and the values of the ONU-G and ONU2-G that
   G.988 has the ONU report) from the shipped firmware's files in the JFFS2
   file system of the partition ``config``. It is the identity ``onu-config`` puts in
   ``/run/onu-identity`` at boot, with the store's ``ID_*`` keys over it,
   which omcid reads and which gives the serial number to the PON driver. Its
   output holds the PLOAM password, a credential of the line: keep it
   private.

``onu-config``
   the stick's settings, their store in the flash and their rules, in one
   program (ADR 0015; it was ``onucfg`` and ``onu-store`` before). It reads
   and changes the configuration store: the file
   ``/config/open-dfp-34x-2c.conf`` of ``KEY=VALUE`` lines, at most 64 KiB,
   in the JFFS2 file system of the partition ``config``, beside the files the
   shipped firmware keeps there, which it never writes, so that the shipped
   firmware finds them after a switch of the bank (ADR 0014, step 9). A
   change writes the whole store to a new file and renames it over the old
   one, so that a power loss in the middle leaves the old store whole; a
   change that changes nothing writes nothing. ``--store`` or ``ONU_STORE``
   names another file::

      onu-config list
      onu-config get KEY
      onu-config set KEY VALUE
      onu-config unset KEY
      onu-config import <lines     # every KEY=VALUE and -KEY, one write

   Before step 9 the store took the whole partition, in two slots of its
   own format; ``onu-config slots DEVICE`` prints such a store and
   ``onu-config erase-slots DEVICE`` erases its partition, refusing one that
   holds none, for the boot to take it into the file (below).

   It also holds the one table of the data path's settings (ADR 0014,
   ``src/settings.h``): each key of ``/run/onu.conf``, the shape of its values
   and its default, the PPPoE account of each session, and the rules that bind
   them together, such as one session in a VLAN to the host handing its IPv4
   address down. ``onu-config check`` takes a set of them on its input, a later
   line over an earlier one, accounts among them, and prints the settings in
   full: every default filled in, the keys of before taken over (the WAN's
   VLAN, MAC address and GEM port as the first session's, ``LAN_VLAN`` as each
   session's link to the host), never an account. It refuses a set with a
   value not of its key's shape, a key that is no setting, or settings that
   make no sense together, and says why, naming the key and never the value.
   ``onu-config settings`` takes a set as it comes, for a reader: a line it cannot
   take is left out, said on the standard error, and nothing is judged.
   ``onu-config current`` reads the settings in force from the files of the
   running system, ``/run/onu.conf`` and the accounts, ``/run/onu-pppoeN``,
   which root alone may read, and prints them as ``settings`` does, with
   each account's ``PPPOEn_USER`` and ``PPPOEn_SECRET_SET``, never its
   secret; ``onu-config change`` takes a change on its input, judges it over
   them as ``check`` does, and as well against the management VLANs in use
   (no session's VLAN, nor the VLAN it hands down in, is one), and writes
   the settings and the accounts that changed aside, in
   ``/run/onu-pending``, or says why not and writes nothing; ``onu-apply
   try`` puts them in force under its lock, once it has kept the settings it
   goes back to, so that an ``onu-apply sync`` in between applies the
   settings in force alone. The status daemon has onu-act run them, and knows neither the
   rules nor the files. What came of the settings it asks of ``onu-apply
   state`` (the method ``settings_state``), and reads no file of ``/run``:
   the lines ``name=value`` of the mode, the name servers, the address handed
   to the router and where, what became of the settings in the store, each
   session's ``session.N.dev``, ``gem``, ``hands``, ``ipv6``, ``lan`` and
   ``gateway``, and ``trial.NAME`` for each change on trial, when it goes
   back, with ``trial.mgmt.address``. A fact that is not there has no line.
   The lines are the contract; the files under them may change::

      onu-config check <settings       # the settings in full, or why not
      onu-config settings <settings    # the same, without judging
      onu-config current               # the settings in force, no secret
      onu-config change <change        # the change written, or why not

``onu-config`` (the files)
   carries out, through ``onu-config.sh`` in ``/usr/lib/onu``, the verbs of the
   files the running system reads. It keeps in the store what must outlive a
   restart, as ``KEY=VALUE`` lines (the table below is the one list of them).
   ``onu-config load`` puts them in the files the running system reads, at
   boot; ``onu-config save`` writes the settings and the accounts in force
   back, which ``onu-apply reload`` does, and no other key. A load first
   makes ``/config`` the partition's JFFS2 file system, mounted read-only:
   each change of the store mounts it read-write for the write alone, one
   change at a time, under a lock of the directory, a socket of the abstract
   namespace named after it, as the stick's kernel has no ``flock``; a
   partition that still holds the store of before is read, erased and mounted
   empty, and that store goes into the file, since JFFS2 erases the blocks it
   does not know.
   The identity is the one ``ponid`` reads from the shipped firmware's files,
   with the store's ``ID_*`` keys over it, which hold only what was changed
   here. What became of the
   settings is one word in ``/run/onu-store`` (``loaded``, ``defaults``,
   ``saved``, ``failed``, ``none`` without ``/config``), which the status
   daemon shows. The status
   daemon also keeps two things there through it: ``onu-config identity`` takes
   the identity of ``KEY=VALUE`` lines on its input (``SERIAL``, ``PLOAM``, …)
   and ``onu-config password`` root's password, the line of its input, which it
   hashes in the form of ``crypt()`` (SHA-256 with 20000 rounds and a salt of
   16 random characters, by BusyBox's ``cryptpw``) into ``AUTH_HASH``;
   ``onu-config auth HASH`` stores a hash made elsewhere. Root's line of
   ``/run/shadow``, which ``/etc/shadow`` names, carries the hash for the
   console's login and the ssh server: one account, root, with one password
   for the console, ssh and the panel. Until one is set, the password is
   ``admin`` (a fixed hash of it); a hash of the panel's older form, which
   ``crypt()`` cannot check, leaves root without a password on the console and
   ssh (``*``), keys only, until a sign-in to the panel stores it again.
   Without a store that can be read (``/config`` not mounted, or the file
   damaged) the password is not known: ``/run/onu-auth`` holds ``!``, the
   panel refuses every sign-in, the ssh server takes keys only, and the
   console, which needs the stick opened, takes ``admin``, so that the store
   can be mended there.
   ``onu-config password-check`` says in its exit status whether the line of
   its input is root's password: 0 for the one in force, 3 for ``admin``
   while none is set, 4 for one kept in the older form, which the caller
   keeps again with ``onu-config password``, 5 for any while the password is
   not known, 1 for any other; ``onu-config password-state`` prints ``set``,
   ``default`` or ``unknown``. Both read the hash themselves
   (``src/password.c``), so that the status daemon, which has onu-act run
   them, never holds it.
   ``onu-config token new`` makes the token of scripts, which the status daemon
   takes in place of a sign-in (``pond/README.rst``): 64 hexadecimal digits,
   printed once, for the script. ``onu-config token`` prints it again
   (exit status 1 and no output when there is none), ``onu-config token off``
   takes it away, and ``onu-config token install`` hands the one in force to the
   status daemon, whose copy is ``/run/pond/token`` for the account ``pond``
   alone: the boot script after it made that directory, and ``token new`` and
   ``token off`` themselves. A person runs it, on the stick; the status daemon
   cannot (ubusd's access list has no method for it), so that a daemon taken
   over cannot make itself a token. Without a store the new token lasts until
   the next start.
   ``onu-config identity-current`` prints the identity of
   ``/run/onu-identity``, which root alone reads, without its PLOAM and
   logical passwords, with ``ploam_set`` and ``loid_password_set`` and where it
   came from, ``source``. A
   save with no account in force stores an empty ``PPPOEn_USER``, so that a
   removed account does not come back at the next boot, and takes out of the
   store each setting that ``/run/onu.conf`` no longer has, such as a key a
   later version replaced (an import's ``-KEY`` line), unless that file is
   missing or empty. ``onu-config system`` keeps the stick's own settings of
   ``KEY=VALUE`` lines on its input as ``SYS_*`` keys, each in the shape the
   boot script takes. Where the identity in force came from is ``store`` or
   ``factory`` in ``/run/onu-identity-source``. ``onu-config pd N`` keeps the
   host session N delegates its IPv6 prefix to, so that after a restart the
   prefix goes to that router at once, though it asks for nothing;
   ``onu-apply`` calls it when the line changes or goes, and a store that holds
   the same writes nothing.

   The keys of the store, and the file of ``/run`` a load puts each in (the
   files of secrets are for root only):

   .. list-table::
      :header-rows: 1
      :widths: 24 22 54

      * - Key
        - Kept by
        - In the file
      * - ``ID_<NAME>``
        - ``onu-config identity``
        - The line ``name=value``, the name in lower case, of
          ``/run/onu-identity``, over what ``ponid`` reads from the shipped
          firmware's files.
      * - ``PPPOEn_USER``, ``PPPOEn_SECRET``, n from 1 to 7
        - ``onu-config save``
        - The line ``user secret`` of ``/run/onu-pppoeN``; an empty user leaves
          the session without an account. ``PPPOE_USER`` and ``PPPOE_SECRET``,
          the single account of before, are the first session's.
      * - ``AUTH_HASH``
        - ``onu-config password``, ``auth``
        - ``/run/onu-auth``: the hash of root's password, which the console's
          login, the ssh server and the status daemon's sign-in all check.
          Without the key the default password is in force; without a store
          that can be read the file holds ``!``.
      * - ``SYS_<NAME>``
        - ``onu-config system``
        - The line ``NAME=value`` of ``/run/onu-system``, for ``HOSTNAME``,
          ``MGMT_VLAN``, ``MGMT_ADDRESS``, ``SFP_LANE``, ``NTP_SERVER`` (the
          address of the server the clock is set from) and ``LOG_SERVER`` (the
          address of a server of the host side that the system log goes to as
          well, with a port after it where it is not 514). The stick runs
          offline and resolves no name, so a name is refused.
      * - ``PDn_HOST``
        - ``onu-config pd``
        - ``/run/dhcp6d/pdN``: the host session N delegated its IPv6 prefix to,
          which ``dhcp6d`` takes back after a restart.
      * - ``TLS_KEY``
        - ``onu-config tls``
        - ``/run/onu-tls-key``: the status daemon's key, a scalar of P-256 in 64
          hexadecimal digits, made once on the stick.
      * - ``SSH_HOST_KEY``
        - ``onu-config ssh``
        - ``/run/onu-ssh-key``: the ssh server's host key in its own format, in
          hexadecimal digits, made once on the stick.
      * - ``POND_TOKEN``
        - ``onu-config token``
        - ``/run/onu-pond-token``: the token of scripts.
      * - every other key
        - ``onu-config save``, ``import``
        - A line of ``/run/onu.conf``: the settings (``src/settings.h``).

   A save writes the settings, the last row, and the accounts, the second; it
   leaves every other key as it is, and the panel never shows them.

``passwd``
   a new password for root, at a terminal: it asks twice without echo and
   hands it to ``onu-config password``, so that it holds after a restart and in
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
   run first by the boot script: after a start of a bank on trial, it commits
   the bank once omcid runs as one process across two looks, the status
   daemon answers on the address of ``/run/onu-mgmt`` (or one given) and the
   PON is in O5 (or has no light), and restarts into the committed bank when
   that does not happen within 300 s, forcing the restart after 30 s more.

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

``onu-nl``
   makes, through netlink, the requests of tc, ip (the links) and devlink that
   the boot script and the scripts here make, since the image carries
   neither: iproute2 is in the RAM image only (ADR 0014)::

      onu-nl clsact add|del DEV
      onu-nl filter add DEV [pref N] [vlan VID] [hw] trap|drop|redirect TO
      onu-nl filter del DEV pref N
      onu-nl filter prefs DEV
      onu-nl param NAME [VALUE]
      onu-nl link set DEV [up|down] [mtu N] [address MAC]
      onu-nl link add vlan DEV NAME VID
      onu-nl link del DEV
      onu-nl addr-wait DEV ADDRESS/LEN [SECONDS]

   A filter is on the ingress of DEV: without ``vlan`` it takes every frame
   (tc's matchall), with it the frames whose outer 802.1Q tag has that VID
   (flower); ``hw`` keeps it in the switch alone (tc's skip_sw); ``trap``
   sends what it takes to the CPU, ``drop`` drops it and ``redirect`` hands
   it to the ingress of TO. ``clsact add`` leaves a clsact qdisc that is
   there already. ``filter prefs`` prints the priorities of the filters on
   DEV's ingress, one a line, the lowest first. ``param`` prints a parameter
   of the switch's devlink instance in the runtime mode, or sets it: a number
   in decimal (or hexadecimal after ``0x``), ``true`` or ``false``, or a
   string (``Documentation/networking/devlink/rtl9602c.rst`` in the kernel
   lists them). ``link set`` takes DEV up or down and sets its MTU (68 to
   65535) and its MAC address, whichever it is given, once each, in any order,
   and leaves the rest as it is; ``link add vlan`` makes the interface NAME of
   VLAN VID, 1 to 4094, on DEV, and is refused when NAME is taken; ``link
   del`` removes DEV. ``addr-wait`` returns when DEV has the IPv4 address with that
   prefix length, at once if it has it, else when the kernel announces it, and
   fails after SECONDS (10): it listens and does not look in a loop. When the
   kernel refuses, its own message follows the error.
   It exits 0 when done, 1 when refused, 2 for a wrong command line::

      # onu-nl filter prefs sfp
      1
      # onu-nl param sfp_lane_now
      2500basex

``onu-apply``
   turns the settings into what the stick runs: procd and netifd carry out,
   but know nothing of sessions, of an address handed down or of omcid, and
   onu-apply tells them (ADR 0014; it was called ``onu-mode``, of the modes
   the stick no longer has). It gives the stick's data path the shape its
   settings give (``/run/onu.conf``, seeded from ``/etc/onu.conf``, which sets
   nothing and says what each key means; the default of a key is the table of
   ``onu-config``, which ``onu-config defaults`` prints); there is no
   mode to choose. Without a PPPoE account omcid connects the host link to the
   PON as the OLT provisions it. With accounts, the stick holds up to seven
   PPPoE sessions itself, each with the user and secret of
   ``/run/onu-pppoeN``, and each session runs on a GEM port the OLT
   provisioned, or on a VLAN on it (``wanVID``, shared by the sessions in that
   VLAN). A session's VLAN may be the subscriber's VID of a translation the
   OLT provisions, the one a router behind the stick would use: the session
   then runs in the VID that VID leads into on the fibre, as omcid publishes
   it in ``/run/omcid/xlate``. omcid goes on bridging the rest of what the
   OLT provisions, such as television and telephone, without the sessions'
   VLANs, which onu-apply writes to ``/run/onu-held-vlans`` before it has
   omcid build its bridge again; the frames of those VLANs that come down the
   other GEM ports go to the sessions' GEM interfaces, as an OLT may send a
   session's frames by any GEM port of its VLAN. A session untagged on its GEM
   port holds the whole port, and omcid then leaves the OLT's bridge out
   (``omcid -r``). Each session has its families: IPv4,
   IPv6 or both (``pppoed -4`` or ``-6``), as an operator that gives IPv6 on a
   session of its own asks. The PON port, the GEM interface and the VLAN get
   an MTU of 1508 first, so that a concentrator that agrees gives a session of
   1500-byte packets (RFC 4638). In each VLAN towards the host, the first
   session told to hand its address down (``SESSIONn_PASS=on``) that
   carries IPv4 hands its address on (``pppoed -P``): the address goes on no
   interface of the stick's, and the router behind the stick takes it from
   BusyBox's DHCP server on the link between them (the VLAN of ``sfp`` that
   ``SESSIONn_HOST_VLAN`` names, the session's own VLAN when it names none,
   or ``sfp`` itself, untagged; ``LAN_VLAN`` of before is taken as the first
   session's), as a /32 whose gateway is the concentrator's address, as a
   router that made the session itself would have it: the stick answers ARP
   for it on the link without taking it, so that a packet to it goes on to
   the concentrator, and its DHCP server answers from the stick's own
   address on the link, ``169.254.0.1``. The gateway is given as a
   classless static route too (RFC 3442),
   with the session's name servers and MTU and leases of a minute, so that
   the router finds the address of a new session within one. The stick routes
   the address there with a host route, until the session ends, and forwards
   from the link only what comes from that address, by a strict reverse-path
   check (BCP 38). netifd holds the links, their addresses and checks
   (onu-apply's part ``links``), and each session's route, proxy ARP entry
   and table (its part ``pass-N``), as it holds the sessions. The switch then routes with the TTL and hop limit
   unchanged (``l3_ttl_decrement``): the stick stands in for the operator's
   ONT, which is no hop, and the concentrator is the router's first. The first
   session that carries IPv6 hands its delegated prefix on in the same way,
   through ``dhcp6d`` (see ``dhcp6d/README.rst``), once its IPv6 is up: the
   link to the router gets IPv6, and the stick forwards it. onu-apply makes
   each session an interface of netifd, ``sesN`` of the protocol ``pppoed``
   (see ``pppoed/README.rst``), once the OLT has provisioned its GEM port,
   which omcid tells it of: omcid runs ``onu-apply omcid`` after each change
   of the GEM ports or of the VLANs they carry (its ``-x``), as dhcp6d runs
   ``onu-apply dhcp6d`` when the host's binding changes, which onu-apply
   keeps in the configuration store. netifd runs pppoed, puts what the session brought in place and starts
   pppoed again when its link ends, each session 3 s after the one before,
   as a concentrator may refuse one of two sessions that authenticate at
   once. netifd tells onu-apply when an interface comes up or goes down
   (``onu-apply iface``, netifd's ``-h``), and onu-apply hands the session's
   address and prefix on or takes them back; what a session has is in
   ``/run/session/N``. The stick translates no address and has no packet
   filter. The bench's router of before, ``MODE=router`` with its WAN and
   LAN settings, is gone: its keys in a store of before are left out::

      onu-apply start      # at boot
      onu-apply reload     # after /run/onu.conf changed; saves it too
      onu-apply try        # the same on trial, back unless confirmed in 90 s
      onu-apply confirm    # keep the change on trial, and save it
      onu-apply revert     # back to the settings of before the trial now
      onu-apply status
      onu-apply identity   # at boot: the identity to the PON driver, PON up
      onu-apply reidentify # after the identity changed: register again
      onu-apply iface      # netifd's, for an interface that came up or down
      onu-apply sync       # follow what changed outside the settings

   A change on trial (ADR 0014) is applied but not saved, and the settings
   and accounts applied before it are kept aside, in ``/run/onu-kept``,
   readable by root alone; unless ``confirm`` keeps it within 90 s
   (``TRIAL_LIMIT``), the stick goes back to them by itself, so that a
   change that cut off whoever made it comes undone. A change during a
   trial starts it again and goes back to the settings of before the first;
   ``reload`` saves, and so ends a trial. A restart during a trial starts
   with the settings saved before it.

   One ``onu-apply`` at a time changes the stick, and the others wait: the
   lock is ``/run/onu-apply.lock``, a symbolic link to the PID of its holder,
   as the stick's kernel has no ``flock``. A ``sync`` told while another
   waits leaves the change to that one.

   ``identity`` gives the PON driver the serial number and PLOAM password of
   ``/run/onu-identity`` (twenty hex digits of password as its ten octets) and
   brings the PON up; ``reidentify`` takes the PON down first and restarts
   omcid afterwards, so the OLT sees the stick leave and register anew. The
   MIB that omcid kept in ``/run/omcid/mib`` stays, and the new omcid goes on
   from it until the OLT provisions the stick again.

``onu-mgmt``
   the VLAN and the address the stick is managed on from the host side, and a
   change of them that cannot lock anyone out. ``onu-mgmt start`` records the
   pair of the boot; ``add`` puts a new pair beside the old one, with the
   switch's rules that keep its VLAN from the PON and an ssh server of its
   own, and refuses a VLAN a session or the WAN is given, and ``commit`` makes it the one in use and stores it, which a person
   can do only once the new pair answers; a change not committed goes again
   after ten minutes. netifd brings the pairs up: each is an interface of its
   own in onu-mgmt's part of netifd's configuration, so that the one a person
   manages the stick from is never taken down and up again when the other
   comes or goes. The address on ``sfp`` itself, ``DATA_ADDRESS``, is the
   interface ``data`` of the same part::

      onu-mgmt start VLAN ADDRESS [DATA_ADDRESS]   # at boot
      onu-mgmt add VLAN ADDRESS
      onu-mgmt commit
      onu-mgmt cancel
      onu-mgmt status

``onu-change``
   the changes on trial (ADR 0014), whichever program made them: the
   settings (``onu-apply try``), the setting of the SFP lane (``onu-lane
   try``) and the management pair (``onu-mgmt add``). Each is a file of
   ``/run/onu-trial.d`` named for it, with the moment it goes back; the
   program that made it keeps it or takes it back, and the keeper of the
   deadlines, ``onu-change watch``, one for all of them and a service of procd,
   takes it back when no one kept it in time: 90 s, and for the management
   pair ten minutes, the time to move a computer to the new VLAN and
   address. The status daemon confirms them for the panel, which does so as
   soon as it reaches the stick again; the management pair's only from a
   panel opened at its new address, which shows that the new pair answers::

      onu-change status        # name, when it goes back, the new address
      onu-change confirm [IP]  # keep them; the pair's only when IP is its own
      onu-change revert        # take them back now
      onu-change expire        # take back those whose time has come, once
      onu-change watch         # the keeper: expire, wait for the nearest, for ever

``onu-net``
   writes netifd's configuration, ``/run/config/network``, from parts: each
   script that sets something up through netifd writes a part of its own,
   ``/run/network.d/PART``, and the parts in the order of their names are the
   whole. A change is written to a new file and renamed over the old, and
   netifd is told with SIGHUP, upon which the fork in ``netifd/`` reads its
   configuration again; the image has no ``ubus`` command. ``onu-net wait``
   waits until an address netifd is to put on a device is there, as the
   kernel announces it (``onu-nl addr-wait``)::

      onu-net set PART <TEXT
      onu-net del PART
      onu-net wait DEV ADDRESS [SECONDS]

``onu-svc``
   gives procd the stick's programs as its services, over ubus, which procd
   starts, and again a set time after one ends; OpenWrt's shell scripts and
   jshn do that elsewhere. The boot script starts the log, netifd, the ssh
   server, ``pond`` and timed with it, each a service of its own. ``-r`` gives
   the threshold under which a run is a crash, the wait before the next start
   and the crashes in a row after which procd gives up, 60,2,0 unless given
   (0: never); ``-l`` sends the program's output to the system log, ``-o``
   appends it to a file, and ``-u`` runs it as an account. Its messages are
   procd's own form, blobmsg, built as they are put together, with no JSON
   text between (``src/svcmsg.c`` writes them through a writer, which the
   tests replace with one of text). ``sync`` gives procd a whole service at once, one
   program for each file of a directory, whose name is the instance's and
   whose line is what ``start`` takes after the name: procd starts what is
   new, ends what has no file any more, starts again only what changed, and
   leaves the rest alone (ADR 0014). ``onu-apply`` keeps its programs so, in
   ``/run/onu-svc.d``, as the service ``onu``: omcid, dhcp6d and udhcpd. A
   sync holds the directory while it reads
   it and tells procd, so that the last one sends what the directory holds
   then. ``list`` tells each service's or instance's state and how often
   procd started it again (a procd patch, ``br2-external/package/procd``)::

      onu-svc start NAME [-r THRESHOLD,TIMEOUT,RETRY] [-l | -o FILE] [-u USER] -- PROGRAM [ARG...]
      onu-svc sync NAME DIR
      onu-svc stop NAME
      onu-svc list [NAME]

``onu-act``
   does, as root, what a program that runs as an account of its own may ask
   of the stick, and nothing else (ADR 0014): the status daemon's changes of
   the SFP lane, of the management pair, of the name and of the identity, the
   confirmation of the changes on trial, the updates' status and trial, the
   restart, the settings of the data path (``onu-config current`` and
   ``change``, then ``onu-apply try``) and what came of them (``onu-apply
   state``), root's password (``onu-config
   password-check``, ``password-state`` and ``password``), and an update file,
   which streams through the descriptor the caller passes with the call, the
   reading end of a pipe into ``onu-update write -`` or the writing end out
   of ``onu-update read``. It is the object ``onu-act`` on ubus, with a method for each
   entry of its table (``src/act.c``): the method runs its program, with no
   shell, the caller's values in their places once each is short enough, of
   the characters its place allows and no option, and the caller's text,
   ``input``, on its standard input. The answer comes when the program ends,
   its exit status and what it wrote, ``{"code", "output"}``, or as soon as it
   starts for one run in the background (the restart, a new registration);
   a program that runs past its time is killed. A new password, an update
   written and a start of the other bank, what would give a caller root,
   run only when the call carries root's password in force, ``password``,
   which ``onu-config password-check`` judges; otherwise the call is refused
   with ``UBUS_STATUS_PERMISSION_DENIED``. The status daemon, taken over,
   cannot do them alone. ubusd's access lists say who may call which method. procd keeps it (``rcS``); it ends without
   ubusd, and procd starts it again. The status daemon of a build without
   libubus, which its tests make, runs the same table itself.

``onu-drop``
   starts a program as an account of its own, with only the powers it needs
   (ADR 0014)::

      onu-drop [-c CAP[,CAP...]] USER PROGRAM [ARGUMENT...]

   Started as root, it takes the account USER (not root's) and its groups,
   keeps of root's powers those named by ``-c``, from ``net_bind_service``,
   ``net_admin``, ``net_raw`` and ``sys_time``, and runs PROGRAM in its own
   place. Without ``-c`` it keeps none. The powers are kept for PROGRAM and
   what it runs (ambient), the rest leave the bounding set, so that nothing
   it runs can have them, and the program gains nothing from a file
   (``no_new_privs``). It exits 2 for a wrong command line, 1 when it cannot
   become the account or run the program. ``tests/test_onu_drop.sh`` reads
   the program's own ``/proc/self/status``; it needs root and the account
   ``nobody``, and says that it skipped without them.

``onu-call``
   asks ``onu-act`` for a method, from an account that runs no program of the
   stick as root::

      onu-call METHOD

   It prints what the method's program wrote and exits with its status; a
   method run in the background answers at once with 0. The programs that tell
   ``onu-apply`` of a change they publish run it with ``-x``, and it gets their
   name as the method (``omcid``, ``dhcp6d``): each is the method that runs
   ``onu-apply`` for that name, and ubusd's list lets each program call its own
   and no other (``tests/test_acl.py`` reads the lists).

``onu-run``
   starts a service of the stick in the foreground, as procd wants it, from
   the state the scripts keep: the ssh server and ``pond``, on the
   management pair in use and the host link's address, from
   ``/run/onu-mgmt``, so that one started again after a change of the pair
   listens on the new one; omcid, in the mode ``onu-apply`` last gave it
   (``/run/omcid.mode``), so that one started again after a crash comes up in
   the mode the stick is in; ``timed``, which sets the clock and keeps it, by
   NTP from ``NTP_SERVER`` of ``/run/onu-system`` or, without one, from the
   router at the other end of the management link, the other address of its
   /30 or /31, by PTP on the management link's interface, and from the date
   the OLT gave (``/run/omcid/clock``) when the host gives none, as the stick
   has no way of its own to the internet, with the hardware stamps of that
   interface turned on first (``timed -H``, as root) and the PTP hardware
   clocks ``/dev/ptp*`` given to the group ``ntp``; and the console's login prompt, or,
   on a bench whose ``/etc/board.conf`` has ``CONSOLE_AUTOLOGIN=root``, root's
   shell without the password::

      onu-run ssh
      onu-run pond
      onu-run omcid
      onu-run time
      onu-run console

Building and testing::

   make           # build/ponid, build/pon, build/onu-config, build/onu-update, build/onu-svc,
                  # build/onu-act, build/onu-nl
   make check     # the tests

``ponid``, ``onu-config`` and ``onu-update`` need zlib, ``onu-config`` and
``onu-update`` the kernel's MTD header
``mtd/mtd-user.h``, ``onu-svc`` and ``onu-act`` libubus and libubox (their
tests need neither), ``pon`` and ``onu-nl`` libmnl and the kernel's headers,
among them ``linux/pon.h`` of this repository's kernel; for a build on the
host, point CPPFLAGS at an installation of those headers (``make
headers_install`` in ``linux/``). The tests of ``pon`` read and print messages
that they build, and those of ``onu-nl`` build its requests and read them back;
``tests/test_onu_nl.sh`` runs ``onu-nl`` against the kernel it runs on and reads
the result back with tc, where it may make interfaces (root with
CAP_NET_ADMIN, as in a container started with ``--cap-add NET_ADMIN``), and
says that it skipped elsewhere. The
tests of ``onu-apply`` run it against stand-ins for the programs it calls, in a scratch root, and those of ``onu-config`` run it with
``build/onu-config`` on a store in a file and a stand-in for BusyBox's
``cryptpw``. ``onu-update`` is tested on a flash
made of files under ``ONU_ROOT``, with update files packed by the image build's
``mkupdate.py``, which needs python3; ``onu-trial`` against stand-ins.
