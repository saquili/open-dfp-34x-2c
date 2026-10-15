.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

====================
Building the images
====================

How to build the two flash images of the stick from source, what each file is,
how big each may be and is, and how to put them on the board. The cross
toolchain is described in ``toolchain.rst``, the flash layout in
``boot-contract.rst``, and the bench commands that write the flash in
``flashing.rst``; this page uses all three. The build system and its external
tree are described in ``buildroot/README.rst`` and ``br2-external/README.rst``,
and the decision that the programs share one C library, cut to what they use,
in ``docs/adr/0011-target-executables-share-one-cut-c-library.rst``.

What the build makes
====================

``make`` in the top directory leaves these files in ``build/image/images``:

.. list-table::
   :header-rows: 1
   :widths: 18 56 26

   * - File
     - What it is
     - Size and budget
   * - ``k0.uImage``
     - The kernel of ``linux/``, built with the board's ``rtl9602c_defconfig``, as
       the legacy U-Boot image that the boot loader takes for bank 0: MIPS, Linux,
       kernel, LZMA, load address ``0x80100000``.
     - About 1.7 MB (see "Sizes and budgets").
   * - ``r0.squashfs``
     - The root filesystem for bank 0: a SquashFS image (xz, 64 KiB blocks,
       every file owned by root and carrying the same fixed time, no extended
       attributes) of BusyBox, dropbear, the programs of the ONU and the
       board's files.
     - About 1.3 MB (see "Sizes and budgets").
   * - ``update.bin``
     - The two images in one update file, which the stick's ``onu-update``
       writes into the bank it did not start from (``updating.rst``): a header
       with the board, the version and the build's time, the two images, and
       their SHA-256.
     - The two images and 288 bytes.
   * - ``web-install.tar``
     - The two images in the package that the web page of the stick's shipped
       firmware installs into the bank it did not start from
       (``updating.rst``): ``fwu.sh``, ``md5.txt``, and the bank cut where the
       shipped firmware cuts it, made by ``mkweb.py``, which refuses a package
       the page would refuse.
     - The two images, the kernel's padding and some 13 KB; the page takes
       fewer than 3932160 bytes.
   * - ``SHA256SUMS``
     - The SHA-256 digests of the two images, the update file and the package,
       in the form ``sha256sum -c`` reads.
     - 313 bytes.
   * - ``ram.uImage``
     - Made by ``make ram-image``, not by ``make``: the same kernel with the
       root filesystem built in as its initial RAM disk, and the tools of
       diagnosis with it, for a start from RAM that needs neither bank (see
       "Putting the images on the board").
     - About 3.7 MB, with the files of a bench; the boot loader takes up to
       12 MiB from RAM.

The images directory also holds ``uImage``, the kernel image as the build system
leaves it, which ``k0.uImage`` is a copy of.

Sizes and budgets
-----------------

.. list-table::
   :header-rows: 1
   :widths: 12 22 22 44

   * - Image
     - Size
     - Budget
     - Margin, and when it was measured
   * - ``k0``
     - 1698834
     - 1900544
     - 201710 bytes (10.6% of the partition), on 2026-10-11, once the
       kernel took JFFS2 for the partition ``config`` (ADR 0014).
   * - ``r0``
     - 1287869
     - 2031616
     - 743747 bytes (36.6% of the partition), on 2026-10-11, once iproute2
       left the flash's image (ADR 0014); the panel's TLS, which came after,
       adds about 22 KiB (ADR 0016).

Both images fit, and the build fails when one stops fitting (``IMAGE_ENFORCE``,
below). The budget of an image is the size of its partition
(``boot-contract.rst``, "The flash"). They fit because bank 0 is split for
this system and not as the shipped firmware splits it, which left the kernel
too little room, and because the kernel and the root filesystem
leave out what the stick does not use ("The kernel's size" below, and "The root
filesystem"). The programs share one C library, cut to what they use; with
every program linked statically, as before ADR 0011, the root filesystem was
2004150 bytes on 2026-10-08.

The numbers are printed by ``tools/image-budget/image-budget``, which ``make``
runs, in the pinned container (below); the table keeps the latest that a
record of this repository gives. The other counts of this page, of files,
instructions, symbols and applets, were measured on 2026-10-08, before procd,
netifd, ``onu-nl`` and the panel's TLS entered the image, and are of that
image; ``make`` prints the current ones.

The build host
==============

Buildroot runs on Linux. The build is checked in the container that the
workflows use, Debian 13 named by digest, with the packages below; on macOS or
Windows work in a clone inside such a container (a volume of the container's own
file system, not a bind mount of a working tree: the kernel holds paths that
differ only in case, ``CONTRIBUTING.rst``).

::

    docker volume create open-dfp-34x-2c-work
    docker run -it --rm --cpus 12 -v open-dfp-34x-2c-work:/work \
        --mount type=bind,source="$(pwd)",target=/src,readonly \
        debian:trixie@sha256:f324c7ff54321e8d9c588493a20244965938ce0aa50bbd1022d38010e9ffc4b1

then, in the container::

    apt-get update
    apt-get install -y --no-install-recommends ca-certificates git python3 \
        patch xz-utils make gcc g++ libc6-dev binutils perl bzip2 gzip cpio \
        unzip rsync file bc findutils gawk wget sed diffutils tar debianutils \
        bison flex
    git config --global --add safe.directory '*'
    git clone /src /work/repo && cd /work/repo

The packages are those of ``br2-external/README.rst`` and the two the kernel
needs for its configuration tools (``bison`` and ``flex``). The build needs
``umask 022``, 9 GB of disk for an output directory, and network access to the
hosts that serve the sources, which Buildroot checks against the hashes it carries.

Building
========

::

    make

fetches the pinned Buildroot release and applies the carried patches (``make
buildroot``), archives the committed ``linux/`` tree
(``tools/kernel-archive``), configures ``odi_dfp_34x_2c3_defconfig`` in
``build/image``, builds, and then judges the result (``make image-check``). With
the sources in the download directory, a build from a fresh clone takes about
eight minutes on 12 CPUs. The first build downloads the sources of the packages
of the defconfig besides the toolchain's (``br2-external/README.rst``); each is
checked against the hash that the build system carries, and a file that does not
match is refused. The programs of this tree (``omcid``, ``onu-utils``,
``pppoed``, ``dhcp6d``, ``pond``, ``netifd``) are built from the working tree,
not downloaded. ``make``
makes the cross toolchain first, with shared libraries only, then the kernel
with it, then the programs of the image, and the host tools the images need
(``mksquashfs``, ``mkimage``, the LZMA compressor; ``mksquashfs`` brings
``cmake`` with it, which takes a minute and a half to build).

These variables can be set on the command line or in the environment:

``IMAGE_OUT``
    The output directory (default ``build/image``). Two builds in two
    directories give the same images (see "Reproducibility").
``BR2_DL_DIR``, ``BUILDROOT_DL``
    Where the sources and the build system's tarball are kept (default
    ``build/dl``).
``IMAGE_ENFORCE``
    The images that must fit their partition for the build to pass (default
    ``k0 r0``; empty, the sizes are reported and fail nothing).
``OPEN_DFP_34X_2C_LOCAL_OVERLAY``
    A directory of the files of one bench, laid over the root filesystem image
    (below).

``make sdk`` builds the cross toolchain of ``toolchain_defconfig`` as a
relocatable tarball in ``build/toolchain/images``; ``toolchain.rst`` describes
how to use it.

What ``make`` judges
--------------------

#. ``image-budget``: the sizes of ``k0`` and ``r0`` against their partitions,
   reported, and enforced for the images that ``IMAGE_ENFORCE`` names.
#. ``flash-layout``: the partitions of the device tree on the flash's 4 KiB
   sectors, and the kernel of this build with the flash driver built in and
   erasing in them (``boot-contract.rst``, "The erase sector").
#. ``isa-gate`` over ``build/image/target``, every executable of the root
   filesystem: none may hold an instruction that the core cannot run
   (``toolchain.rst``). It reports 26 files, 26 objects, 1749347 instructions
   and no findings. The gate can fail: the negative control of the toolchain
   tests compiles a stock MIPS32 object, and the gate refuses it for its
   architecture flags, for the load-linked and store-conditional pair and for
   the MIPS32 instructions (``br2-external/tests/toolchain/negative-control``).
#. ``sha256sum -c SHA256SUMS`` in the images directory.

The root filesystem
===================

The root filesystem is the target directory of the build, packed. It holds:

The C library
    uClibc-ng 1.0.59 as a shared library, with its dynamic loader; every
    program loads it. The build system installs it whole; the post-build script
    links it again with only the functions that the programs import
    (``tools/lib-trim``): 361 of the 1428 symbols it exports, 421516 bytes
    instead of 712556. It removes ``libgcc_s``, which the toolchain installs and
    no program loads, and fails the build when a program would miss a library
    or a symbol.
BusyBox 1.37.0
    The applets the boot script and a shell session use, by purpose, as of
    2026-10-08, when there were 94; the console's ``getty`` and ``login``,
    ``cryptpw``, and the DHCP server ``udhcpd`` and ``dumpleases`` came after
    (the DHCP client ``udhcpc`` served the bench's router of before and is out
    of the configuration):

    shell and init
        ash sh halt poweroff reboot
    files
        ls cat cp mv rm mkdir rmdir ln touch chmod chown dd df du stat find
        readlink realpath basename dirname pwd sync
    text
        echo printf head tail wc sort uniq cut tr tee xargs grep egrep fgrep
        sed awk test true false seq od hexdump md5sum sha256sum vi less clear
    system
        mount umount mountpoint ps top free uptime dmesg kill killall pidof
        sleep date uname hostname id whoami env which tty stty watch time
        syslogd klogd logger
    network
        ip ping netstat nc traceroute traceroute6 arping nslookup
    archives
        tar gzip gunzip zcat

    ``busybox --list`` prints them. The configuration is
    ``br2-external/board/dfp-34x-2c3/busybox.config``. To change it, run
    ``make -C build/image busybox-menuconfig`` and then
    ``make -C build/image busybox-update-config``. The second command writes the
    file with BusyBox's own header, so put the licence header (the first two
    lines) back before ``make check``.
dropbear 2026.93
    The ssh server (``dropbear``), its key tool (``dropbearkey``) and the
    server side of ``scp``. It takes root's password and keys.
The programs of the ONU, from this tree
    ``omcid``, the OMCI daemon, whose tools ``omcitap`` and ``omcivlan`` are
    in the RAM image only (``omcid/README.rst``); ``ponid``, which reads the
    identity of the unit from the shipped firmware's files in the partition
    ``config``, and ``onu-nl``, which sets the switch's
    filters and the drivers' parameters as tc and devlink would
    (``onu-utils/README.rst``); ``pppoed``, the
    PPPoE client and netifd's protocol ``pppoed`` (``pppoed/README.rst``),
    which links libubus to tell netifd; ``dhcp6d``, which hands a session's
    delegated IPv6 prefix on to the host (``dhcp6d/README.rst``); ``timed``,
    which sets the clock from the host by PTP or NTP, or from the OLT's date
    (``timed/README.rst``); ``pond``, the
    status daemon and its panel, over HTTPS (``pond/README.rst``); and
    ``onu-config``, ``onu-apply``, ``onu-update`` and the other programs of
    ``onu-utils`` (``onu-utils/README.rst``).
The network layer of OpenWrt
    ``netifd``, the network interface daemon, from the fork in ``netifd/``,
    and ``ubusd``, the message bus it answers on, with the libraries they load:
    libubox, libubus, libuci, libblobmsg_json and json-c; libnl-tiny is linked
    into ``netifd``. Buildroot builds them with
    ``buildroot/patches/0004-*.patch`` (``buildroot/README.rst``). The
    command-line tools of these packages are not in the image: the post-build
    script takes ``uci`` and ``jshn`` out, and sets ``ubus`` aside for the RAM
    image. procd starts both;
    netifd brings up the management VLAN, the addresses of the host side,
    the PPPoE sessions and the links that hand their addresses on to the
    router, with their routes, from the configuration that ``onu-net`` writes
    in ``/run/config`` (``onu-utils/README.rst``), and tells ``onu-apply`` of
    each interface that comes up or goes down.
procd
    OpenWrt's init and service supervisor, process 1, with the patches in
    ``br2-external/package/procd`` (``docs/adr/0013-procd-is-the-first-process.rst``).
iproute2
    Built for the RAM image alone (ADR 0014): there ``tc``, ``devlink`` and
    ``bridge``, and iproute2's ``ip`` as ``ip-full``, so that ``ip`` is
    BusyBox's from RAM as it is from the flash. No script of the stick runs
    ``ip``: netifd sets the addresses and routes, and ``onu-nl`` the links,
    the switch's classifier and the PON driver
    (``br2-external/board/dfp-34x-2c3/post-build.sh``); BusyBox's ``ip`` is
    built in for a person at the console.
The start
    ``/sbin/init``, the first program, mounts ``/proc``, ``/sys``,
    ``/dev/pts`` and tmpfs on ``/tmp`` (2 MiB at most), ``/run`` (4 MiB) and
    ``/var`` (1 MiB; ``/var/run`` is ``/run``), and ``/dev`` too if the kernel
    has not (it mounts devtmpfs
    itself when it starts the first program), and execs ``procd``, which is
    process 1 from then on. procd feeds the watchdog (a timeout of 30 s, fed
    every 5 s), starts ``ubusd`` and runs ``/etc/inittab``:

    #. the boot script, ``/etc/init.d/rcS`` as ``/etc/rc.d/S10boot`` (below),
       whose output goes to the log but for the lines on the console at its
       end;
    #. once it is done, the console's login prompt (``onu-run console``).

    The boot script starts the stick's programs as procd's services
    (``onu-svc``): the log first, ``netifd`` once its configuration is
    written, the ssh server, ``pond`` and ``timed`` once the management
    address is up. The stick has no clock that runs while it is off, and keeps
    no time in the flash: ``timed`` sets the time by PTP from a master on the
    management link, with the switch's hardware stamps over layer 2, or by
    NTP from the router at the other end of it or from
    the server that the stick's settings name (``NTP_SERVER``, ``onu-config
    system``), or, when the host gives none, from the date the OLT gives with
    OMCI, if it gives one; until then the clock reads 1970. When the settings
    name a server of the host side for the log (``LOG_SERVER``), the log goes
    there as well,
    the stick keeping its own copy. Both are addresses, not names: the stick
    runs offline, has no name server to ask and no ``/etc/resolv.conf``.
    procd starts a service again 2 s after it ends and never gives up;
    ``onu-svc list`` tells how each is doing and how often procd started it
    again. The console's login prompt is started again half a second after it
    ends.
    ``reboot`` signals procd, which ends every program and restarts the
    stick. What ``halt`` and ``poweroff`` leave the stick in has not been
    tried.
The boot script
    ``/etc/init.d/rcS``:

    #. starts ``onu-trial`` in the background, which keeps a bank started on
       trial once the system is up, and restarts into the committed bank when
       it is not, a boot script that hangs included (``updating.rst``);
    #. starts the system log, ``syslogd`` and ``klogd``, which keep
       ``/run/messages`` and one older copy of 64 KiB each;
    #. reads the unit's settings (``onu-config load``): it mounts the
       partition ``config`` on ``/config``, reads the identity of the unit from
       the shipped firmware's files there, with what the store changed over it,
       into ``/run/onu-identity`` (readable by root only: it holds the PLOAM
       password), and the saved settings, the PPPoE accounts and root's
       password into ``/run``; ``onu-config tls`` gives the panel its key and
       ``onu-config ssh`` the ssh server its host key, each made and kept in
       the store at the first start; a token of scripts, if root made one, is
       handed to the status daemon with the servers below;
    #. sets the host name, turns IPv6 off on the PON port, on the CPU port and
       on every interface made after them, the bridge and the GEM interfaces of
       ``omcid`` among them (the host side keeps it), and has ARP answer only
       for an address of the interface it is asked on: the stick has no address
       on the PON side, sends nothing of its own to the OLT and answers nothing
       from it;
    #. brings the switch's conduit ``cpu`` and the interface ``sfp`` up, and
       starts ``netifd``, which gives ``sfp`` the address ``192.0.2.2/30`` and a
       VLAN interface ``mgmt`` of it (VLAN 4094) the address
       ``198.51.100.2/30``, from the configuration ``onu-mgmt`` writes; it waits
       up to ten seconds for each. netifd alone sets them, and procd starts it
       again if it ends. These are the addresses the bench tries
       (``flashing.rst``, "What you need"); it tries the second first, because
       that one answers whether or not a bridge has been built on the link
       (``tools/bench/README.rst``). The host has to have the VLAN on its
       interface too;
    #. keeps that VLAN off the PON both ways: the switch sends the frames of
       it that come in on ``sfp`` to the CPU alone, and drops those that come
       in on ``pon`` (``onu-nl``), so the network behind the OLT neither sees
       the management link nor reaches it;
    #. starts the servers on the two addresses above and nowhere else, as
       procd's services: ``dropbear`` (root's password and keys, with the
       ed25519 host key that ``onu-config ssh`` keeps in the store),
       ``onu-act``, and ``pond``,
       the status daemon and its panel, over HTTPS on port 443 and on port 80
       to send a browser there (ADR 0016), as an account of its own, ``pond``,
       with which no one logs in; ``onu-act`` does, as root, what pond asks of
       the stick from a fixed list, and ubusd's access list lets pond ask
       nothing else (ADR 0014). Then ``timed``. The stick has no packet filter,
       so its servers keep off the PON side by where they listen;
    #. gives the PON driver the serial number and the PLOAM password through
       its devlink parameters (``onu-apply identity``, with ``onu-nl``) and
       brings the PON up;
    #. starts ``omcid``, which answers the OLT, and what the settings ask for:
       per session ``dhcp6d``, per local network ``udhcpd``, as the instances
       of one service of procd, ``onu``, which ``onu-apply`` gives procd whole
       (``onu-svc sync``): procd starts what is new, ends what is gone and
       starts again only what changed. ``omcid`` keeps what the OLT set up in
       ``/run/omcid/mib``, and one that procd starts again goes on from it,
       unseen by the OLT. netifd holds the PPPoE sessions;
    #. prints on the console the addresses of ssh, the panel's address and the
       SHA-256 fingerprint of its certificate, and last
       ``open-dfp-34x-2c: ready``.

    procd then starts the console's login prompt (``getty`` and ``login``).

    The stick has one account, root, with one password for the console, ssh
    and the panel (``docs/adr/0012-one-account-root.rst``). Until one is set,
    the password is ``admin``; the panel asks for a new one at the first
    sign-in, and ``passwd`` sets one on the console or over ssh. Either way
    ``onu-config`` keeps its hash in the configuration store and writes root's
    line of ``/run/shadow``, which ``/etc/shadow`` names, at every boot. A
    forgotten password is cleared from the boot loader's console: a RAM boot
    (``flashing.rst``) whose kernel arguments end with ``rdinit=/bin/sh``
    starts a shell instead of the system, with no login, and there::

        mount -t proc proc /proc
        mount -t devtmpfs devtmpfs /dev
        onu-config load                    # /config, the partition "config"
        onu-config unset AUTH_HASH
        umount /config
        reboot -f

    The next boot has the default password, ``admin``, again; nothing else in
    the store changes.

The root filesystem is read-only, and what the system writes in ``/run``,
``/tmp`` and ``/var`` lives in memory. Three things it writes to the flash:
its own file in the partition ``config``, at most 64 KiB, with the settings,
the accounts, the password's hash, the panel's key and the ssh host key
(``onu-config``); the boot loader's environment and the bank that does not run,
at an update (``onu-update``). The partition is mounted read-only, and
``onu-config`` mounts it read-write for each change of its file alone, one
change at a time.

Known limitations
-----------------

* The ssh host key and the panel's key are made once, at the first start, and
  kept in the configuration store (``onu-config ssh``, ``onu-config tls``);
  each is made only after the kernel says its random number generator is
  seeded, which a read of ``/dev/random`` waits for (about 3 s after the
  kernel's start). Without a store that can be written they are made again at
  every start, and an ssh client that keeps host keys will say so.
* No ssh key is in the image, so ssh takes root's password alone until a key is
  put in (below). Until a password is set, it is ``admin`` on the console, ssh
  and the panel alike, and anybody who reaches the stick's addresses can use it;
  the shells of root say so at every login.
* The status daemon has no token for scripts until root makes one on the stick
  (``onu-config token new``, over ssh or on the console), so a script signs in
  as root with root's password, as a person does. The token is kept in the
  configuration store, like the keys above, and stays across restarts and
  updates; ``onu-config token`` prints it again and ``onu-config token off``
  takes it away. It is for ``https://`` addresses only (``pond/README.rst``,
  "The token of scripts").
* The panel answers over HTTPS, on port 443, with a key the stick makes at its
  first start and keeps in its store, and a certificate it signs itself (ADR
  0016); port 80 sends a browser there. The browser warns at the first visit:
  the boot prints the certificate's fingerprint on the console, and the panel
  shows it under System, for a person to compare with the browser's.
* If the configuration store cannot be read, or the partition ``config`` is
  not mounted, root's password is not known: the panel refuses every sign-in
  and says why, the ssh server takes keys only, and the console takes
  ``admin``, so that the store can be mended there. A new password cannot be
  kept until the store is back.
* The update file has a checksum and no signature (``updating.rst``).

Files of one bench
------------------

An image that a bench uses needs its ssh key and perhaps its own addresses, which
do not belong in the repository. ``OPEN_DFP_34X_2C_LOCAL_OVERLAY`` names a
directory whose files the post-image script lays over a copy of the root
filesystem tree before it packs ``r0.squashfs``::

    mkdir -p local/root/.ssh local/etc
    cp ~/.ssh/id_ed25519.pub local/root/.ssh/authorized_keys
    chmod 700 local/root/.ssh; chmod 600 local/root/.ssh/authorized_keys
    printf 'DATA_ADDRESS=203.0.113.2/30\n' > local/etc/board.conf
    make OPEN_DFP_34X_2C_LOCAL_OVERLAY=$PWD/local

``/etc/board.conf`` is read by the boot script and may set ``DATA_ADDRESS``,
``MGMT_VLAN`` and ``MGMT_ADDRESS`` (the value above is an example; the defaults
are in the script). An empty ``DATA_ADDRESS`` leaves the host link itself
without an address: the stick is then managed on its VLAN alone, which the
switch keeps from the PON both ways, and has no address on the subscriber's
data path, as an operator's ONT has none. On a finished build this runs the last step only, a few
seconds, and it does not change the target directory: the next ``make`` without
the variable gives the plain image again. The image with a key in it is not the
image of this page, its digest differs, and it is not to be published.

``/etc/board.conf`` may also set ``OMCI_SW_VERSION`` and ``OMCI_PACK_VERSION``,
the versions that the two software images and the circuit packs report to the
OLT, up to 14 characters, for a unit that is to show the OLT what the firmware
it came with showed. With the first, both images report it and both are valid;
without it the images report the release of the running kernel and only the
committed one is valid. Without the second the circuit packs report the unit's
version.

``/etc/board.conf`` may set ``CONSOLE_AUTOLOGIN=root``: the console then gives
root's shell without asking for the password, so that a bench reaches the stick
when its network does not (``onu-run console``). An image without the file asks
for root's password there, as the ssh server and the panel do.

The kernel's size
=================

When this section was measured, the kernel image was 1783187 bytes, 423315 over
its partition. It was built twice, with the toolchain of the build and with the
Debian cross compiler that ``make linux`` uses. The images are 734 and 738 bytes
apart in two builds of the second (its banner holds the time and host of the
build, so its size varies by a few bytes). The two configurations differ in
three lines that follow from the compiler (its version text, its version number
and the test that it can link). The 1063 compile commands carry the same
architecture flags (``-EB``, ``-mabi=32``, ``-msoft-float``, ``-fno-pic``) but
for one: ``-march=r3000`` with the toolchain of the build and ``-march=mips1``
with the Debian compiler. The kernel's Makefile tries ``r3000`` first, before it
adds ``-msoft-float``, and the Debian compiler's default floating-point mode
refuses it; both mean MIPS I. The linked kernels hold 1283 and 1284 load-linked
and store-conditional pairs, in code that runs only on a CPU that is found to
have them (``cpu_has_llsc``, which the kernel tests at run time). The kernel is
therefore not under the instruction-set gate yet
(``tools/isa-gate/README.rst``).

Linked, ``vmlinux`` holds 4571004 bytes of code and read-only data, 1125132 of
data and 71396 of bss (``size vmlinux``). The raw image is 5700108 bytes, which
LZMA makes 31 % of that. By directory of the source tree (the text, read-only
data and data of the linked objects before compression, summed by
``size -B`` over the members of ``vmlinux.a``, so the numbers are of the objects
and a little more than the image holds) the largest are:

.. list-table::
   :header-rows: 1
   :widths: 14 14 72

   * - Directory
     - Bytes
     - Of which
   * - ``net``
     - 2526371
     - ``net/ipv4`` 540576, ``net/core`` 479479, ``net/ipv6`` 371923,
       ``net/netfilter`` 315175, ``net/sched`` 168188, ``net/devlink`` 141489,
       ``net/ethtool`` 139193, ``net/bridge`` 114717, ``net/dsa`` 78290
   * - ``drivers``
     - 984024
     - ``drivers/net`` 289148, ``drivers/base`` 158751, ``drivers/tty`` 144063,
       ``drivers/of`` 78573, ``drivers/clk`` 70083, ``drivers/gpio`` 68596,
       ``drivers/mtd`` 66829
   * - ``kernel``
     - 763386
     - ``kernel/sched`` 146613, ``kernel/printk`` 96876, ``kernel/time`` 94527,
       ``kernel/irq`` 87632
   * - ``fs``
     - 595652
     - ``fs/proc`` 95759, ``fs/squashfs`` 41170
   * - ``lib``
     - 457811
     - ``lib/zstd`` 72804
   * - ``mm``, ``block``, ``arch``, ``init``
     - 399371, 211508, 133661, 36667
     -

The largest symbols (``nm -S --size-sort -r vmlinux``) are static buffers: the
printk ring (``_printk_rb_static_infos`` 45056 bytes, ``__log_buf`` 16384,
``_printk_rb_static_descs`` 6144) and ``irq_desc`` (36864), then the functions
``___bpf_prog_run`` (13856), ``__dev_ethtool`` (9772) and the zstd decoder
(``ZSTD_decompressSequencesLong_default`` 9056). No symbol is a tenth of the
excess; it is spread over the network stack.

What each group of options is worth is measured by switching it off in
``rtl9602c_defconfig`` and building the image again, with ``make linux`` (a
build takes about half a minute with eight jobs)::

    linux/scripts/config --file build/linux/.config -d IPV6
    make -C linux O=$PWD/build/linux ARCH=mips CROSS_COMPILE=mips-linux-gnu- olddefconfig
    make -C linux O=$PWD/build/linux ARCH=mips CROSS_COMPILE=mips-linux-gnu- -j8 uImage

The size of the compressed image from the baseline of 1783910 bytes, with one group
off at a time (the baseline varies by about 20 bytes between two builds, because
the banner holds the build time and host):

.. list-table::
   :header-rows: 1
   :widths: 24 46 15 15

   * - Off
     - What it is
     - Image
     - Change
   * - BLK_DEV_INITRD
     - the initial RAM disk and its seven decompressors
     - 1746013
     - -37897
   * - IPV6
     - IPv6
     - 1666267
     - -117643
   * - NETFILTER
     - netfilter: nf_tables, connection tracking, the flow table and NAT
     - 1677552
     - -106358
   * - NET_SCHED
     - queueing disciplines, classifiers and actions (``tc``)
     - 1733096
     - -50814
   * - PPP, PPPOE
     - PPP and PPP over Ethernet
     - 1773411
     - -10499
   * - DEBUG_FS, DYNAMIC_DEBUG
     - debugfs and dynamic debug
     - 1726540
     - -57370
   * - KALLSYMS
     - the symbol table of the running kernel
     - 1614435
     - -169475
   * - KEXEC
     - kexec
     - 1781343
     - -2567
   * - SQUASHFS_LZ4
     - LZ4 in SquashFS
     - 1783588
     - -322
   * - PACKET
     - ``AF_PACKET`` sockets
     - 1771186
     - -12724
   * - DCB
     - data center bridging
     - 1778606
     - -5304

and together:

.. list-table::
   :header-rows: 1
   :widths: 40 20 20 20

   * - Off
     - Image
     - Change
     - Against k0
   * - all of those but NETFILTER and PPP
     - 1352544
     - -431366
     - fits k0 by 7328 bytes
   * - all of those but PPP
     - 1258164
     - -525746
     - fits k0 by 101708 bytes
   * - all of those
     - 1248354
     - -535556
     - fits k0 by 111518 bytes

The measurements above are against the shipped split of bank 0, whose ``k0``
took 1359872 bytes. The board's configuration now leaves out netfilter (the
stick translates no address and keeps no packet filter: a router behind it does
both), the initial RAM disk and kexec, and smaller parts that the board does not
use, which the configuration's history names; it keeps IPv6, the symbol table,
debugfs and dynamic debug, through which a problem on the operator's line is
read. The kernel image was 1652846 bytes then, and 1698834 once it took JFFS2
for the partition ``config`` (ADR 0014); bank 0 as this system splits it gives
the kernel room for it (``boot-contract.rst``).

Reproducibility
===============

``BR2_REPRODUCIBLE`` is on: the build writes no time of its own. The kernel
builds as ``buildroot@buildroot`` at the time the build system's release was
made, ``mkimage`` writes the same time into the image header, and the post-image
script gives it to ``mksquashfs`` for every file.
Two builds from clean, each from a fresh clone of the repository in a container
of the pinned image, in directories whose paths are 21 and 60 characters long,
gave the same ``k0.uImage``, ``r0.squashfs`` and ``SHA256SUMS``, byte for byte,
and the same root filesystem tree: 138 entries with the same modes, sizes,
SHA-256 digests and link targets. The comparison is::

    cmp build/image-a/images/k0.uImage build/image-b/images/k0.uImage
    cmp build/image-a/images/r0.squashfs build/image-b/images/r0.squashfs
    br2-external/tests/image/tree-digest build/image-a/target > a.txt
    br2-external/tests/image/tree-digest build/image-b/target > b.txt
    diff a.txt b.txt

The ``image`` workflow makes it (``.github/workflows/README.rst``). The host
directory of a build (``build/image/host``) is not compared: the programs that
run on the build machine record the directory they were built in, as
``br2-external/README.rst`` says of the toolchain, and none of them is in an
image.

The licence information
=======================

::

    make legal-info

runs the build system's ``legal-info`` for the board's configuration, after a
build, into ``build/image/legal-info``:

``manifest.csv``
    Every package of the image: its version, its licence, its licence files and
    its source. ``host-manifest.csv`` is the same for the programs of the build
    machine.
``licenses/``
    The licence texts, each checked against its hash in the package's hash file.
    The packages of this repository take theirs from ``LICENSES/`` at the top
    (``br2-external/external.mk``).
``sources/``
    The sources a release hands on, among them archives of this repository's
    own packages.
``pond-panel-licenses.txt``
    The licences of the npm packages whose code the panel holds, as the panel's
    build wrote them (``pond/web/licences.ts``); the image carries the same file
    beside the panel, ``/usr/share/pond/www/licenses.txt``. This is why the
    command wants a built image.

A build from scratch collects 383 MB, nearly all of it sources. Buildroot's own
source is not saved; its release is the one pinned in ``buildroot/VERSION``.

Putting the images on the board
===============================

A stick as shipped takes this system through its shipped firmware's web page
and ``web-install.tar`` (``updating.rst``), or through the bench
(``tools/bench``, set up as in ``flashing.rst``), which writes bank 0 alone.
Once this system runs, it updates itself into the other bank with
``update.bin``.

A stick is diagnosed started from RAM; the flash holds the image of the final
test. The tools of diagnosis, ``omcitap``, ``omcivlan``, ``ubus``, the
flash's ``flashcp``, ``flash_erase`` and ``mtd_debug``, ``ethtool``, and
iproute2's ``tc``, ``devlink``, ``bridge``, ``dcb`` and ``ip``, the last as
``ip-full``, are in the RAM image only: the post-build script sets them aside in
``build/image/diag`` with the C library whole, since the image's own is cut to
what the image's programs use, and ``make ram-image`` lays that directory over
its root.

A RAM image is the flash's image plus those tools, which is not what the flash
holds. ``make ram-image RAM_DIAG=no`` leaves them out, and the device tests
(``tests/device/README.rst``) put the ones that they need on the stick, in
memory, for the run (``--tools build/image/diag``): that run tests the
flash's root filesystem and kernel without a write to the flash.

Start the kernel from RAM with the root filesystem built in, which needs
neither bank: ``make ram-image`` makes ``ram.uImage`` from the build (the tools
of diagnosis and the files of ``OPEN_DFP_34X_2C_LOCAL_OVERLAY`` included),
and::

    make ram-image OPEN_DFP_34X_2C_LOCAL_OVERLAY=$PWD/local
    make ramboot IMAGE=build/image/images/ram.uImage \
        ARGS='--ready "open-dfp-34x-2c: ready"'

Or start ``k0.uImage`` from RAM, with the root filesystem it finds in ``r0``
(the loader gives it ``root=31:5``)::

    make ramboot IMAGE=build/image/images/k0.uImage \
        ARGS='--ready "open-dfp-34x-2c: ready"'

Write the root filesystem to ``r0``. See the plan first; it names no board::

    make flash IMAGE=build/image/images/r0.squashfs TARGET=rootfs ARGS=--dry-run
    make flash IMAGE=build/image/images/r0.squashfs TARGET=rootfs

Write the kernel to ``k0`` and start the trial of bank 0 (``flashing.rst``, "Arm
a one-shot trial")::

    make flash IMAGE=build/image/images/k0.uImage TARGET=kernel ARGS=--arm

The markers of a good boot on the console are the loader's lines for the image
(``Image Type: MIPS Linux Kernel Image (lzma compressed)``, ``Load Address:
80100000``), ``Starting kernel ...``, the kernel's banner ``Linux version 6.18.48``,
``VFS: Mounted root (squashfs filesystem) readonly on device 31:5.``, and last
the lines of the boot script, ending with ``open-dfp-34x-2c: ready``. What
to check then, over ssh with the key above or on the console, after a login as
root::

    uname -r                 # 6.18.48
    cat /proc/cmdline        # panic=10, console=ttyS0,115200, ..., root=31:5
    grep ' / ' /proc/mounts  # /dev/root / squashfs ro,...
    busybox --list           # the applets above
    ip addr                  # 192.0.2.2/30 on sfp, 198.51.100.2/30 on mgmt
    pidof procd netifd dropbear omcid pond syslogd
    onu-svc list             # procd's services, and how often each restarted
    netstat -ltn             # ssh and the status daemon, on the two addresses only
    cat /etc/issue           # open-dfp-34x-2c on the ODI DFP-34X-2C3

A kernel started from ``ram.uImage`` has no ``VFS: Mounted root`` line: its
root filesystem is the initial RAM disk, a tmpfs. ``tests/device`` runs the full
check of a running stick and records it.

Open items
==========

* The data-path cases of ``tests/device`` (``data.*``) and the search of the
  host link have not run on the image as it is now, with procd, netifd, the
  panel over HTTPS, ``onu-nl`` and the store in the partition ``config``.
  They send frames from two ports of a bench, one behind the OLT and one at
  the far end of the stick's host link, and the lab as it is has none. Their
  latest record, ``docs/testing/2026-10-07-run23.rst``, is of an image
  of before those, whose programs were linked statically. The other cases
  are recorded on the current image
  (``docs/testing/2026-10-12-fbf6474956e2.rst``).
* Whether the host's link comes up and the addresses answer depends on the host
  side having the matching addresses (``flashing.rst``); the VLAN of the second
  address has to exist on the host's interface too.
* The images were started from the flash on one board, the bench's: the boot
  from bank 0 reaches the boot script's ready line in about 16 seconds after
  the loader's reset, the PON reaches O5 and ``omcid`` answers the OLT. Other
  boards of the same kind have not been tried.
