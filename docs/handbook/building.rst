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
and the decision to link every program statically in
``docs/adr/0010-target-executables-are-linked-statically.rst``.

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
     - 1783187 bytes; the partition ``k0`` takes 1359872.
   * - ``r0.squashfs``
     - The root filesystem for bank 0: a SquashFS image (gzip, 64 KiB blocks,
       every file owned by root and carrying the same fixed time, no extended
       attributes) of BusyBox, dropbear and the board's files.
     - 622859 bytes; the partition ``r0`` takes 2572288.
   * - ``SHA256SUMS``
     - The SHA-256 digests of the two images, in the form ``sha256sum -c``
       reads.
     - 154 bytes.

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
     - 1783187
     - 1359872
     - Over the budget by 423315 bytes.
   * - ``r0``
     - 622859
     - 2572288
     - 1949429 bytes (75.8 % of the partition).

The kernel image does not fit ``k0``. The bench refuses it (``is 1783187 bytes,
more than the 1359872 its destination takes``), so ``k0`` cannot be written until
the kernel is slimmed. The kernel can still be started from RAM with ``make
ramboot``, which takes images of up to 12 MiB (``boot-contract.rst``), and a
kernel started that way finds its root filesystem in ``r0`` as usual. The build
reports the excess and does not fail on it; it fails on ``r0`` over its budget
(``IMAGE_ENFORCE``, below). Where the kernel's size is, as measured, is in "The
kernel's size" below.

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
seven and a half minutes on 12 CPUs (four builds took 444 to 460 seconds). The
first build downloads the sources of the packages of the defconfig, 13 files of
41.8 MB together, besides the toolchain's (``br2-external/README.rst``); each is
checked against the hash that the build system carries, and a file that does not
match is refused. ``make`` makes the cross toolchain first, with static libraries
only, then the kernel with it, then BusyBox and dropbear, and the host tools the
images need (``mksquashfs``, ``mkimage``, the LZMA compressor; ``mksquashfs``
brings ``cmake`` with it, which takes a minute and a half to build).

These variables can be set on the command line or in the environment:

``IMAGE_OUT``
    The output directory (default ``build/image``). Two builds in two
    directories give the same images (see "Reproducibility").
``BR2_DL_DIR``, ``BUILDROOT_DL``
    Where the sources and the build system's tarball are kept (default
    ``build/dl``).
``IMAGE_ENFORCE``
    The images that must fit their partition for the build to pass (default
    ``r0``; set it to ``k0 r0`` when the kernel fits).
``OPEN_DFP_34X_2C_LOCAL_OVERLAY``
    A directory of the files of one bench, laid over the root filesystem image
    (below).

``make sdk`` builds the cross toolchain of ``toolchain_defconfig`` as a
relocatable tarball in ``build/toolchain/images``; ``toolchain.rst`` describes
how to use it.

What ``make`` judges
--------------------

#. ``image-budget --enforce r0``: ``r0`` has to fit its partition; ``k0`` is
   reported.
#. ``isa-gate`` over ``build/image/target``, every executable of the root
   filesystem: none may hold an instruction that the core cannot run
   (``toolchain.rst``). It reports 2 files, 2 objects, 272578 instructions and
   no findings. The gate can fail: the negative control of the toolchain tests
   compiles a stock MIPS32 object, and the gate refuses it for its architecture
   flags, for the load-linked and store-conditional pair and for the MIPS32
   instructions (``br2-external/tests/toolchain/negative-control``).
#. ``sha256sum -c SHA256SUMS`` in the images directory.

The root filesystem
===================

The root filesystem is the target directory of the build, packed. It holds:

BusyBox 1.37.0, static, 86 applets
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
    network
        ip ping netstat nc
    archives
        tar gzip gunzip zcat

    ``busybox --list`` prints them. The configuration is
    ``br2-external/board/dfp-34x-2c3/busybox.config``. To change it, run
    ``make -C build/image busybox-menuconfig`` and then
    ``make -C build/image busybox-update-config``. The second command writes the
    file with BusyBox's own header, so put the licence header (the first two
    lines) back before ``make check``.
dropbear 2026.93, static
    The ssh server (``dropbear``), its key tool (``dropbearkey``) and the
    server side of ``scp``. It takes keys only.
The boot script
    ``/etc/init.d/rcS``, run by BusyBox init at ``sysinit`` (``/etc/inittab``):

    #. mounts ``/proc``, ``/sys``, ``/dev/pts`` and tmpfs on ``/tmp``, ``/run``
       and ``/var``, and ``/dev`` too if the kernel has not (it mounts devtmpfs
       itself when it starts the first program);
    #. sets the host name (``dfp-34x-2c3``);
    #. brings the interface ``sfp`` up with the address ``192.0.2.2/30``, and a
       VLAN interface ``mgmt`` of it (VLAN 4094) with ``198.51.100.2/30``. These
       are the addresses the bench tries (``flashing.rst``, "What you need"); it
       tries the second first, because that one answers whether or not a bridge
       has been built on the link (``tools/bench/README.rst``). The host has to
       have the VLAN on its interface too;
    #. makes an ed25519 host key in ``/run/dropbear`` and starts ``dropbear -s``
       (keys only);
    #. prints ``open-dfp-34x-2c: ready``.

    After it, BusyBox init keeps a shell on the console. There is no login
    prompt: the console of the stick is a serial line on the bench.

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
* No ssh key is in the image, so nobody can log in over ssh until one is put in
  (below). The console is the other way in, and it asks for nothing.
* There is no firewall, no DHCP client, no PON tool and no OMCI daemon. The
  daemon arrives as a package of the userspace work item.

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
are in the script). On a finished build this runs the last step only, a few
seconds, and it does not change the target directory: the next ``make`` without
the variable gives the plain image again. The image with a key in it is not the
image of this page, its digest differs, and it is not to be published.

The kernel's size
=================

The kernel image is 1783187 bytes, 423315 over its partition. It was built twice,
with the toolchain of the build and with the Debian cross compiler that ``make
linux`` uses. The images are 734 and 738 bytes apart in two builds of the second
(its banner holds the time and host of the build, so its size varies by a few
bytes). The two configurations differ in three lines that follow from the compiler
(its version text, its version number and the test that it can link). The 1063
compile commands carry the same architecture flags (``-EB``, ``-mabi=32``,
``-msoft-float``, ``-fno-pic``) but for one: ``-march=r3000`` with the toolchain of
the build and ``-march=mips1`` with the Debian compiler. The kernel's Makefile
tries ``r3000`` first, before it adds ``-msoft-float``, and the Debian compiler's
default floating-point mode refuses it; both mean MIPS I. The linked kernels hold
1283 and 1284 load-linked and store-conditional pairs, in code that runs only on a
CPU that is found to have them (``cpu_has_llsc``, which the kernel tests at run
time). The kernel is therefore not under the instruction-set gate yet
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

The features that the board's hardware paths use (netfilter's flow table for
the routing offload, queueing disciplines and classifiers for shaping and VLAN
translation, IPv6) are in the list, so which groups to give up is a decision
for the work that slims the kernel; the numbers above are what each costs.

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

Putting the images on the board
===============================

The bench (``tools/bench``, set up as in ``flashing.rst``) is the only tool that
writes the flash, and only bank 0.

Start the kernel from RAM, with the root filesystem it finds in ``r0`` (the
loader gives it ``root=31:5``)::

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
to check then, over ssh with the key above or by typing the commands on the
console, which asks for nothing::

    uname -r                 # 6.18.48
    cat /proc/cmdline        # panic=10, console=ttyS0,115200, ..., root=31:5
    grep ' / ' /proc/mounts  # /dev/root / squashfs ro,...
    busybox --list | wc -l   # 86
    ip addr                  # 192.0.2.2/30 on sfp, 198.51.100.2/30 on mgmt
    pidof dropbear
    cat /etc/issue           # open-dfp-34x-2c on the ODI DFP-34X-2C3

Open items
==========

* None of the images has been started on the board yet. The kernel of this tree
  has been started from RAM (``flashing.rst``, "Open items"); a root filesystem
  of this image has not been mounted by it. The boot script has been run in a
  scratch namespace with a native BusyBox, and the static programs under
  ``qemu-mips``; the first run on the board is the test.
* Whether the host's link comes up and the addresses answer depends on the host
  side having the matching addresses (``flashing.rst``); the VLAN of the second
  address has to exist on the host's interface too.
* The kernel has to be slimmed by 423315 bytes before ``k0`` can hold it.
