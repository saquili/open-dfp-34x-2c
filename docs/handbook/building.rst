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
     - 1652846 bytes; the partition ``k0`` takes 1900544.
   * - ``r0.squashfs``
     - The root filesystem for bank 0: a SquashFS image (xz, 64 KiB blocks,
       every file owned by root and carrying the same fixed time, no extended
       attributes) of BusyBox, dropbear, the programs of the ONU and the
       board's files.
     - 1516938 bytes; the partition ``r0`` takes 2031616.
   * - ``update.bin``
     - The two images in one update file, which the stick's ``onu-update``
       writes into the bank it did not start from (``updating.rst``): a header
       with the board, the version and the build's time, the two images, and
       their SHA-256.
     - The two images and 288 bytes; 3170072 bytes for the images above.
   * - ``SHA256SUMS``
     - The SHA-256 digests of the two images and the update file, in the form
       ``sha256sum -c`` reads.
     - 231 bytes.
   * - ``ram.uImage``
     - Made by ``make ram-image``, not by ``make``: the same kernel with the
       root filesystem built in as its initial RAM disk, for a start from RAM
       that needs neither bank (see "Putting the images on the board").
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
     - Margin
   * - ``k0``
     - 1652846
     - 1900544
     - 247698 bytes (13.0% of the partition).
   * - ``r0``
     - 1516938
     - 2031616
     - 514678 bytes (25.3% of the partition).

Both images fit, and the build fails when one stops fitting (``IMAGE_ENFORCE``,
below). They fit because bank 0 is split for this system, 1856 KiB of kernel
and 1984 KiB of root filesystem where the shipped firmware had 1328 KiB and
2512 KiB (``boot-contract.rst``), and because the kernel and the root filesystem
leave out what the stick does not use ("The kernel's size" below, and "The root
filesystem"). The programs share one C library, cut to what they use; with
every program linked statically, as before ADR 0011, the root filesystem was
2004150 bytes on 2026-10-08.

The numbers are printed by ``tools/image-budget/image-budget``, which ``make``
runs, and are those of ``make`` on the commit this page was written against, in
the pinned container (below).

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

fetches the pinned Buildroot release and applies the carried patch (``make
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
BusyBox 1.37.0, 94 applets
    The ones the boot script and a shell session use, by purpose:

    shell and init
        ash sh init halt poweroff reboot
    files
        ls cat cp mv rm mkdir rmdir ln touch chmod chown dd df du stat find
        readlink realpath basename dirname pwd sync
    text
        echo printf head tail wc sort uniq cut tr tee xargs grep egrep fgrep
        sed awk test true false seq od hexdump md5sum sha256sum vi less clear
    system
        mount umount mountpoint ps top free uptime dmesg kill killall pidof
        sleep date uname hostname id whoami env which tty stty watch time
        syslogd klogd logger watchdog
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
    ``omcid``, the OMCI daemon, with its tools ``omcitap`` and ``omcivlan``
    (``omcid/README.rst``); ``ponid``, which reads the identity of the unit
    from its factory configuration (``onu-utils/README.rst``); ``pppoed``, the
    PPPoE client (``pppoed/README.rst``); ``dhcp6d``, which hands a session's
    delegated IPv6 prefix on to the host (``dhcp6d/README.rst``); ``pond``, the
    HTTP status daemon (``pond/README.rst``).
The network layer of OpenWrt
    ``netifd``, the network interface daemon, from the fork in ``netifd/``,
    and ``ubusd``, the message bus it answers on, with the libraries they load:
    libubox, libubus, libuci, libblobmsg_json and json-c; libnl-tiny is linked
    into ``netifd``. Buildroot builds them with
    ``buildroot/patches/0004-*.patch`` (``buildroot/README.rst``). The
    command-line tools of these packages, ``ubus``, ``uci`` and ``jshn``, are
    not in the image: the post-build script takes them out. procd starts both;
    netifd brings up the management VLAN and its address, from the
    configuration that ``onu-net`` writes in ``/run/config``
    (``onu-utils/README.rst``), and takes the other links over one at a time
    as the scripts that set them up move to it.
procd
    OpenWrt's init and service supervisor, process 1, with the patches in
    ``br2-external/package/procd`` (``docs/adr/0013-procd-is-the-first-process.rst``).
iproute2
    ``ip``, ``tc`` and ``devlink``, which configure the links, the switch's
    classifier and the PON driver, and ``bridge``, which shows the bridge's
    VLANs. The post-build script takes the other programs of the package out
    (``br2-external/board/dfp-34x-2c3/post-build.sh``).
The start
    ``/sbin/init``, the first program, mounts ``/proc``, ``/sys``,
    ``/dev/pts`` and tmpfs on ``/tmp``, ``/run`` and ``/var`` (``/var/run`` is
    ``/run``), and ``/dev`` too if the kernel has not (it mounts devtmpfs
    itself when it starts the first program), and execs ``procd``, which is
    process 1 from then on. procd feeds the watchdog (a timeout of 30 s, fed
    every 5 s), starts ``ubusd`` and runs ``/etc/inittab``:

    #. the boot script, ``/etc/init.d/rcS`` as ``/etc/rc.d/S10boot`` (below),
       whose output goes to the log but for the lines on the console at its
       end;
    #. once it is done, the console's login prompt (``onu-run console``).

    The boot script starts the stick's programs as procd's services
    (``onu-svc``): the log first, ``netifd`` once its configuration is
    written, the ssh server and ``pond`` once the management address is up.
    procd starts a service again 2 s after it ends and never gives up;
    ``onu-svc list`` tells how each is doing and how often procd started it
    again. The console's login prompt is started again half a second after it
    ends.
    ``reboot``, ``halt`` and ``poweroff`` signal procd, which ends every
    program and restarts.
The boot script
    ``/etc/init.d/rcS``:

    #. sets the host name (``dfp-34x-2c3``);
    #. turns IPv6 off on the PON port, on the CPU port and on every interface
       made after that, the bridge and the GEM interfaces of ``omcid`` among
       them (the host side keeps it), and has ARP answer only for an address
       of the interface it is asked on: the stick has no address on the PON
       side, sends nothing of its own to the OLT and answers nothing from it;
    #. brings the interface ``sfp`` up with the address ``192.0.2.2/30``, and,
       through ``netifd``, a VLAN interface ``mgmt`` of it (VLAN 4094) with
       ``198.51.100.2/30``; when netifd has not brought that address up within
       ten seconds, the script brings it up itself, so that the stick stays
       reachable. The switch's conduit ``cpu`` is brought up first, so that
       netifd, which takes down only what it brought up, never takes it down.
       These are the addresses the bench tries (``flashing.rst``, "What you need"); it
       tries the second first, because that one answers whether or not a bridge
       has been built on the link (``tools/bench/README.rst``). The host has to
       have the VLAN on its interface too. No frame of that VLAN crosses the
       PON in either direction: the switch sends those that come in on ``sfp``
       to the CPU alone, and drops those that come in on ``pon``, so the
       network behind the OLT neither sees the management link nor reaches it;
    #. reads the identity of the unit into ``/run/onu-identity`` (readable by
       root only: it holds the PLOAM password), with the versions that
       ``/etc/board.conf`` may name (below), gives the PON driver the
       serial number and the PLOAM password through devlink and brings the PON
       up;
    #. starts ``omcid``, which answers the OLT, as procd's service, and with
       sessions the watcher and, per session, ``dhcp6d`` (``onu-mode``);
    #. prints ``open-dfp-34x-2c: ready`` on the console.

    After it, procd starts ``dropbear`` (root's password and keys), with an
    ed25519 host key made in ``/run/dropbear``, and ``pond``, whose API stays
    locked without ``/etc/pond.token``, on the two addresses above and nowhere
    else: the stick has no packet filter, so its servers keep off the PON side
    by where they listen. On the console it keeps a login prompt (``getty``
    and ``login``).

    The stick has one account, root, with one password for the console, ssh
    and the panel (``docs/adr/0012-one-account-root.rst``). Until one is set,
    the password is ``admin``; the panel asks for a new one at the first
    sign-in, and ``passwd`` sets one on the console or over ssh. Either way
    ``onu-store`` keeps its hash in the configuration store and writes root's
    line of ``/run/shadow``, which ``/etc/shadow`` names, at every boot. A
    forgotten password is cleared from the boot loader's console: a RAM boot
    (``flashing.rst``) whose kernel arguments end with ``rdinit=/bin/sh``
    starts a shell instead of the system, with no login, and there::

        mount -t proc proc /proc
        mount -t devtmpfs devtmpfs /dev
        grep '"config"' /proc/mtd          # mtd3: ... "config"
        onucfg --store /dev/mtd3 unset AUTH_HASH
        reboot -f

    The next boot has the default password, ``admin``, again; nothing else in
    the store changes.

Everything that the system writes lives in memory and is gone at the next
restart; the root filesystem is read-only.

Known limitations
-----------------

* The ssh host key is made at every boot, in memory (``/run``). The host
  therefore sees a new key at every boot, and an ssh client that keeps host keys
  will say so. A key that survives restarts needs a place to keep it, which the
  stick does not have yet.
* The host key is made from the kernel's random pool a few seconds after power-on,
  before the system has collected much entropy.
* No ssh key is in the image, so ssh takes root's password alone until a key is
  put in (below). Until a password is set, it is ``admin`` on the console, ssh
  and the panel alike, and anybody who reaches the stick's addresses can use it;
  the shells of root say so at every login.
* The status daemon has no token in this image, so its API answers only
  ``/api/health``; a token is a file of one unit, like the ssh key (below).
* There is no DHCP client and no DHCP server.

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
read. The kernel image is 1652846 bytes, and bank 0 as this system splits it
gives the kernel 1856 KiB (``boot-contract.rst``).

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

The bench (``tools/bench``, set up as in ``flashing.rst``) is the only tool that
writes the flash, and only bank 0.

Start the kernel from RAM with the root filesystem built in, which needs
neither bank: ``make ram-image`` makes ``ram.uImage`` from the build (the files
of ``OPEN_DFP_34X_2C_LOCAL_OVERLAY`` included), and::

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

When ``k0`` fits, write it and start the trial of bank 0 (``flashing.rst``, "Arm a
one-shot trial")::

    make flash IMAGE=build/image/images/k0.uImage TARGET=kernel ARGS=--arm

The markers of a good boot on the console are the loader's lines for the image
(``Image Type: MIPS Linux Kernel Image (lzma compressed)``, ``Load Address:
80100000``), ``Starting kernel ...``, the kernel's banner ``Linux version 6.18.48``,
``VFS: Mounted root (squashfs filesystem) readonly on device 31:5.``, and last
the three lines of the boot script, ending with ``open-dfp-34x-2c: ready``. What
to check then, over ssh with the key above or on the console, after a login as
root::

    uname -r                 # 6.18.48
    cat /proc/cmdline        # panic=10, console=ttyS0,115200, ..., root=31:5
    grep ' / ' /proc/mounts  # /dev/root / squashfs ro,...
    busybox --list | wc -l   # 94
    ip addr                  # 192.0.2.2/30 on sfp, 198.51.100.2/30 on mgmt
    pidof dropbear omcid pond watchdog syslogd
    netstat -ltn             # ssh and the status daemon, on the two addresses only
    cat /etc/issue           # open-dfp-34x-2c on the ODI DFP-34X-2C3

A kernel started from ``ram.uImage`` has no ``VFS: Mounted root`` line: its
root filesystem is the initial RAM disk, a tmpfs. ``tests/device`` runs the full
check of a running stick and records it.

Open items
==========

* The image whose programs share the cut C library (ADR 0011) has been started
  from the flash on the bench's board only, written into its spare bank by
  ``onu-update`` and tried once: the boot script reached its ready line,
  dropbear, ``omcid``, ``pond``, the watchdog daemon and the system log ran,
  ``pond`` answered HTTP (``licenses.txt`` too), the kernel log held no fault,
  the processor was 90% idle, and ``onu-trial`` committed the bank 5 seconds
  after the boot script. The board had no light on its fiber at the time, so
  the PON stayed at O1; the PON side of the image is not tested yet.
* The image with the programs of the ONU has not been started on the board as a
  whole yet. ``omcid`` and ``ponid`` of this tree have been run on the board,
  copied to a running stick: it reached O5 and answered the OLT's MIB upload.
  The first start of ``ram.uImage`` and a run of ``tests/device`` are the test.
* Whether the host's link comes up and the addresses answer depends on the host
  side having the matching addresses (``flashing.rst``); the VLAN of the second
  address has to exist on the host's interface too.
* The images were started from the flash on one board, the bench's: the boot
  from bank 0 reaches the boot script's ready line in about 16 seconds after
  the loader's reset, the PON reaches O5 and ``omcid`` answers the OLT. Other
  boards of the same kind have not been tried.
