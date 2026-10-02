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
``docs/adr/0010-target-executables-are-linked-statically.rst`` (proposed).

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
     - The kernel of ``linux/``, built with the board's ``rtl9602c_defconfig`` and
       the flash profile merged over it (see "The kernel"), as the legacy U-Boot
       image that the boot loader takes for bank 0: MIPS, Linux, kernel, LZMA,
       load address ``0x80100000``.
     - 1321490 bytes; the partition ``k0`` takes 1359872.
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
     - 1321490
     - 1359872
     - 38382 bytes (2.8 % of the partition).
   * - ``r0``
     - 622859
     - 2572288
     - 1949429 bytes (75.8 % of the partition).

Both images fit, and the build holds both to their partitions
(``IMAGE_ENFORCE``, below): it fails when either is over, and so does the
``image`` workflow. The kernel image of the board's complete configuration, the
one ``make linux`` builds, is 1783187 bytes and would not fit ``k0``; the bench
would refuse it (``is 1783187 bytes, more than the 1359872 its destination
takes``). What brings the kernel in the flash to 1321490 bytes is the flash
profile of the kernel, and "The kernel" below says what it gives up. A kernel
that does not fit ``k0`` can still be started from RAM with ``make ramboot``,
which takes images of up to 12 MiB (``boot-contract.rst``), and it finds its
root filesystem in ``r0`` as usual.

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
the sources in the download directory, a build from a fresh clone takes seven and
a half to eight minutes on 12 CPUs. The first build downloads the sources of the
packages of the defconfig, 13 files of 41.8 MB together, besides the toolchain's
(``br2-external/README.rst``); each is checked against the hash that the build
system carries, and a file that does not match is refused. ``make`` makes the
cross toolchain first, with static libraries only, then the kernel with it, then
BusyBox and dropbear, and the host tools the images need (``mksquashfs``,
``mkimage``, the LZMA compressor; ``mksquashfs`` brings ``cmake`` with it, which
takes a minute and a half to build).

These variables can be set on the command line or in the environment:

``IMAGE_OUT``
    The output directory (default ``build/image``). Two builds in two
    directories give the same images (see "Reproducibility").
``BR2_DL_DIR``, ``BUILDROOT_DL``
    Where the sources and the build system's tarball are kept (default
    ``build/dl``).
``IMAGE_ENFORCE``
    The images that must fit their partition for the build to pass (default
    ``k0 r0``, both). Naming fewer, ``IMAGE_ENFORCE=r0``, lets a build go on while
    the other image is only reported, for the day one is being slimmed; the
    ``image`` workflow holds both again in a step of its own.
``OPEN_DFP_34X_2C_LOCAL_OVERLAY``
    A directory of the files of one bench, laid over the root filesystem image
    (below).

``make sdk`` builds the cross toolchain of ``toolchain_defconfig`` as a
relocatable tarball in ``build/toolchain/images``; ``toolchain.rst`` describes
how to use it.

What ``make`` judges
--------------------

#. ``image-budget --enforce k0 r0``: both images have to fit their partitions;
   the margin of each is printed.
#. ``br2-external/tests/kernel/check-config`` on the kernel's final
   ``build/image/build/linux-custom/.config``: every option the product needs is
   set, and none that the flash profile drops (see "The kernel"). It reports 69
   required options and 7 settings of the profile.
#. ``isa-gate`` over ``build/image/target``, every executable of the root
   filesystem: none may hold an instruction that the core cannot run
   (``toolchain.rst``). It reports 2 files, 2 objects, 272578 instructions and
   no findings. The gate can fail: the negative control of the toolchain tests
   compiles a stock MIPS32 object, and the gate refuses it for its architecture
   flags, for the load-linked and store-conditional pair and for the MIPS32
   instructions (``br2-external/tests/toolchain/negative-control``).
#. ``sha256sum -c SHA256SUMS`` in the images directory.

The checks can fail. The size check is run on images of the sizes that matter,
either image one byte over its partition included, by the tests of
``tools/image-budget``, which also read this Makefile. The check of the kernel's
configuration is refused by the board's complete configuration, which sets the
seven options the profile drops, and the ``image`` workflow runs it so.

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

The kernel
==========

Two profiles
------------

The kernel is the tree of ``linux/``, and the configuration of both profiles of
the build starts from the board's own, ``arch/mips/configs/rtl9602c_defconfig``.

.. list-table::
   :header-rows: 1
   :widths: 10 34 34 22

   * - Profile
     - Built by
     - Configuration
     - Size of the image
   * - dev
     - ``make linux``, with the Debian cross compiler; the image is
       ``build/linux/arch/mips/boot/uImage``. The RAM boots of the bench start
       it (``make ramboot``), and so do the hardware tests.
     - ``rtl9602c_defconfig`` as it is: netfilter with NAT and flow offload, PPP
       and PPPoE, IPv6, QoS, debugfs and dynamic debug, the symbol table.
     - 1783187 bytes with the toolchain of the build, about 1783900 with the
       Debian cross compiler: 423315 over ``k0``. A RAM boot takes up to 12 MiB.
   * - flash
     - ``make``, with the toolchain of the build; the image is ``k0.uImage``.
     - ``rtl9602c_defconfig`` with ``br2-external/board/dfp-34x-2c3/kernel-flash.fragment``
       merged over it by the build system, and kbuild resolving the dependencies.
     - 1321490 bytes, 38382 under ``k0``; about 1322600 with the Debian cross
       compiler.

The dev profile has no size limit and keeps everything the hardware tests use.
The flash profile is what the product ships in the 1359872 bytes of ``k0``. The
product is a transparent GPON ONU: the bridge with VLAN filtering, the VLAN
translation (ME 171) done as ``tc`` flower rules that the switch carries out, the
PON class with the OMCI device, the optics through hwmon and I2C, the watchdog,
MTD, SquashFS and devtmpfs all stay. ``br2-external/tests/kernel/required-options.txt``
lists the 69 options the product needs, each with the value it must have and
the reason, and the build holds the kernel to the list.

What the flash profile gives up
-------------------------------

Every size below is an image in bytes, built from the committed tree the way
``make`` builds the kernel, with the toolchain of the build (a build takes about
half a minute, and gives the same bytes as the image of ``make``). *Alone* is the
complete configuration with the one group off, and *saved* is what that takes off
1783187. *Costs* is what keeping the group costs in the flash profile: the
profile with every group of the table dropped is 1294284 bytes, 65588 under
``k0``, and the cost is the growth when the one group is kept. *Margin* is what
would be left under ``k0`` then; the margin asked for is 24 KiB, 24576 bytes.

.. list-table::
   :header-rows: 1
   :widths: 14 9 8 8 8 9 44

   * - Group
     - Alone
     - Saved
     - Costs
     - Margin
     - Decision
     - Why
   * - ``KALLSYMS``
     - 1613893
     - 169294
     - 145562
     - -79974
     - dropped
     - The symbol table gives names to the addresses of an oops and fills
       ``/proc/kallsyms``. The addresses decode against the ``System.map`` of the
       same build, which is reproducible, and the dev profile keeps the names.
   * - ``IPV6``
     - 1665969
     - 117218
     - 101875
     - -36287
     - dropped
     - The management network is IPv4 and the stick has no IPv6 address of its
       own. The bridge forwards IPv6 frames like any other frames.
   * - ``NETFILTER``
     - 1675948
     - 107239
     - 94269
     - -28681
     - dropped
     - The product neither routes nor translates addresses: NAT and the flow
       table offload are the routed mode's. Its filtering is the switch's, flower
       rules.
   * - ``DEBUG_FS``, ``DYNAMIC_DEBUG``
     - 1725087
     - 58100
     - 50092
     - 15496
     - dropped
     - The views a person reads at a console: the PLOAM log, the tables, the
       decoded readings of the optics. The product's interfaces are the PON
       netlink family, hwmon and devlink. Kept alone, debugfs costs 24275 and
       dynamic debug 25522; each leaves less than the margin.
   * - ``BLK_DEV_INITRD``
     - 1744459
     - 38728
     - 37397
     - 28191
     - dropped
     - The root filesystem is the image in ``r0`` and the kernel has no initramfs
       of its own. It would fit alone, and would take the room of the four groups
       below it together.
   * - ``PACKET``
     - 1769984
     - 13203
     - 12057
     - 53531
     - kept
     - Packet sockets, for capture on the stick. It fits.
   * - ``PPP``, ``PPPOE``
     - 1772327
     - 10860
     - 10036
     - 55552
     - kept
     - PPPoE sessions. It fits.
   * - ``DCB``
     - 1777606
     - 5581
     - 4962
     - 60626
     - kept
     - The switch's default priority, DSCP map and trust order. It fits.
   * - ``KEXEC``
     - 1780325
     - 2862
     - 2393
     - 63195
     - dropped
     - A kernel started by kexec does not run on this board, so the system call
       can only cost a power cycle (``Documentation/arch/mips/rtl9602c.rst`` in
       ``linux/``). It would fit; it is dropped because it does nothing.
   * - ``SQUASHFS_LZ4``
     - 1782558
     - 629
     - 1175
     - 64413
     - kept
     - The bench takes a root filesystem image compressed with LZ4 as well as with
       gzip (``image.squashfs_compressions``). A kernel without the reader would
       be sent an image it cannot mount. It fits.
   * - ``NET_SCHED``
     - 1732182
     - 51005
     - 46422
     - 38382
     - kept
     - Traffic control, with the flower classifier and the actions of the VLAN
       translation and the filters. It is the product. The cost is measured against the profile
       without it (1275068).

The profile gives up the seven options of the groups marked dropped and keeps the
rest. Built, it is 1321490 bytes, 38382 under ``k0`` (2.8 % of the partition),
which is 13806 bytes more than the margin asked for. The four groups that stay
cost 28230 bytes one at a time on the image with everything dropped, and 27206
all at once. Debugfs (24275 in the profile), dynamic debug (25522) and the
initial RAM disk (37397 on the image with everything dropped) are larger than
the 13806 that are left; kexec (2393) is not, and is dropped because it does
nothing. The weighing of what else could have been done:

Loadable modules
    The defconfig has no modules, and a large optional subsystem could live in
    ``r0``, which has 1.9 MB free, instead of being left out. It does not pay.
    Switching ``MODULES`` on, with nothing built as a module, takes the profile
    from 1321490 to 1513860 bytes (192370 more). The kernel is linked with
    ``LD_DEAD_CODE_DATA_ELIMINATION``, and once it can have modules the linker no
    longer drops the functions that are exported: ``.text`` of ``vmlinux`` grows by
    449456 bytes and the export tables add 177091 (``size -A``). With the pieces of
    netfilter as modules the image is 1519604 bytes.
    IPv6 as a module does not link: the bridge depends on ``IPV6 || IPV6=n`` and
    follows it into a module, and the PON class, which is built in, calls the
    bridge (``undefined reference to br_vlan_get_info``). A module also needs the
    tools in the root filesystem and a boot script that loads it.
Function and data sections with a garbage-collecting link
    ``LD_DEAD_CODE_DATA_ELIMINATION`` is already on in ``rtl9602c_defconfig``, as is
    ``CC_OPTIMIZE_FOR_SIZE``; there is nothing more to take from them. The kernel's
    link-time optimization is for clang only, and this architecture does not offer it.
The netlink interface of ethtool
    Switching ``ETHTOOL_NETLINK`` off saves 22471 bytes, which would pay for
    debugfs. It is not taken: ethtool netlink is an interface of user space (the
    current ethtool and any daemon that reads the ports use it) and debugfs is a
    view for a person, which a RAM boot of the dev profile gives.
Zeroing of stack variables
    ``INIT_STACK_NONE`` instead of ``INIT_STACK_ALL_ZERO`` saves 10072 bytes. It is
    a hardening, and the room did not need it.
PACKET dropped as well
    Without ``PACKET`` the image is 1309630 bytes, 50242 under ``k0``. It was
    not needed for the margin.

kbuild has the last word
------------------------

The build system merges the fragment with the kernel's ``merge_config.sh`` and
then runs ``make olddefconfig``, so an option that another option selects comes
back whatever the fragment says, and an option whose dependency is gone
disappears. The build therefore checks the file it made, not the fragment:
``br2-external/tests/kernel/check-config`` reads the final ``.config`` and holds
it to the list of required options and to the settings of the fragment. On the
kernel of the build all 69 required options hold and none of the seven dropped
options is set; on the board's complete configuration, the check fails on all
seven. ``br2-external/README.rst`` describes the check and its tests.

Measuring another change
------------------------

The sizes above can be reproduced with the Debian cross compiler in half a minute
a build, and the differences between two options come out the same to within a
few hundred bytes::

    make linux
    linux/scripts/kconfig/merge_config.sh -m -O build/linux build/linux/.config \
        br2-external/board/dfp-34x-2c3/kernel-flash.fragment
    make -C linux O=$PWD/build/linux ARCH=mips CROSS_COMPILE=mips-linux-gnu- olddefconfig
    make -C linux O=$PWD/build/linux ARCH=mips CROSS_COMPILE=mips-linux-gnu- -j8 uImage
    br2-external/tests/kernel/check-config build/linux/.config

The image is 1322613 bytes that way, and the check passes. To switch another
option off, add its ``# CONFIG_X is not set`` line to the fragment under a reason,
to switch one on, put it in ``required-options.txt``; ``make`` then judges the
real image.

The two toolchains
------------------

The dev profile was built twice from the same tree, with the toolchain of the
build and with the Debian cross compiler that ``make linux`` uses. The images are
734 and 738 bytes apart in two builds of the second (its banner holds the time and
host of the build, so its size varies by a few bytes). The two configurations
differ in three lines that follow from the compiler (its version text, its version
number and the test that it can link). The 1063 compile commands carry the same
architecture flags (``-EB``, ``-mabi=32``, ``-msoft-float``, ``-fno-pic``) but for
one: ``-march=r3000`` with the toolchain of the build and ``-march=mips1`` with the
Debian compiler. The kernel's Makefile tries ``r3000`` first, before it adds
``-msoft-float``, and the Debian compiler's default floating-point mode refuses it;
both mean MIPS I. The linked dev kernels hold 1283 and 1284 load-linked and
store-conditional pairs, in code that runs only on a CPU that is found to have
them (``cpu_has_llsc``, which the kernel tests at run time). The kernel is
therefore not under the instruction-set gate yet (``tools/isa-gate/README.rst``).

Reproducibility
===============

``BR2_REPRODUCIBLE`` is on: the build writes no time of its own. The kernel
builds as ``buildroot@buildroot`` at the time the build system's release was
made, ``mkimage`` writes the same time into the image header, and the post-image
script gives it to ``mksquashfs`` for every file.
Two builds from clean, each from a fresh clone of the repository in a container
of the pinned image, in directories whose paths are 24 and 63 characters long,
gave the same ``k0.uImage``, ``r0.squashfs`` and ``SHA256SUMS``, byte for byte,
and the same root filesystem tree: 138 entries with the same modes, sizes,
SHA-256 digests and link targets. The comparison is::

    cmp build/image-a/images/k0.uImage build/image-b/images/k0.uImage
    cmp build/image-a/images/r0.squashfs build/image-b/images/r0.squashfs
    br2-external/tests/image/tree-digest build/image-a/target > a.txt
    br2-external/tests/image/tree-digest build/image-b/target > b.txt
    diff a.txt b.txt

The kernel's vDSO carries the directory it was built in in its debug
information, and the toolchain of the build maps the output directory to a fixed
name (``-ffile-prefix-map``). A kernel built in the output directory is therefore
the same wherever the output directory is, and a kernel built anywhere else, as
``make linux`` builds it, varies by a few dozen bytes from one directory to
another.

The ``image`` workflow makes the comparison (``.github/workflows/README.rst``).
The host directory of a build (``build/image/host``) is not compared: the programs that
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

Write the kernel to ``k0`` and start the trial of bank 0 (``flashing.rst``, "Arm a
one-shot trial")::

    make flash IMAGE=build/image/images/k0.uImage TARGET=kernel ARGS=--dry-run
    make flash IMAGE=build/image/images/k0.uImage TARGET=kernel ARGS=--arm

The bench checks the image against the partition, its header and its load
address before it types anything; the first line shows what it would do.

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
* The kernel of the flash profile is the dev profile's less seven options
  (``kernel-flash.fragment``), and it has not been started on the board, from RAM
  or from ``k0``; the trial above is the first run.
* The room is 38382 bytes under ``k0``. A change to the kernel that grows the image
  past 1359872 minus 24576 is a decision to take with this page's table in hand:
  the build fails only when the image is over the partition.
