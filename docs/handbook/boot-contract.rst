.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=================
The boot contract
=================

The boot loader (the loader, for short) is a U-Boot of the 2011.12 release; its
banner says so after every reset. It sits in the ``boot`` partition, together
with a small first-stage loader that prints ``PRELOADER``, calibrates the memory
and loads it. The CPU's reset vector runs this partition, and the loader starts
the kernel. This page states what the loader needs from an image and what it
leaves behind for the kernel. It is written for a reader who builds images or
who would replace the loader.

The kernel's side of the contract (how the kernel is entered, what it is
handed, how it builds its command line and what it finds when it starts) is in
``linux/Documentation/arch/mips/rtl9602c.rst``, chapter "Boot contract", and is
not repeated here. The tool that relies on everything below is described in
``tools/bench/README.rst``.

Numbers typed to the loader are hexadecimal. The loader prints them without a
prefix, and the console lines on this page show them that way; in running text
they are written with ``0x``.

The flash
=========

The flash is 8 MiB (``0x800000`` bytes) of SPI NOR. The loader divides it into
eight partitions that cover the chip exactly, and the kernel's device tree
repeats the same offsets and sizes, except that this system splits both banks
its own way: the shipped firmware gives its kernel 1328 KiB and its root
filesystem 2512 KiB, and this system gives the same 3840 KiB as 1856 KiB and
1984 KiB. The loader knows nothing of this split, and needs nothing of it (see
"The banks' split and the loader").

.. list-table::
   :header-rows: 1
   :widths: 9 12 20 39 20

   * - Partition
     - Offset
     - Size
     - Holds
     - Written by
   * - ``boot``
     - ``0x000000``
     - 256 KiB (``0x040000``)
     - The first-stage loader and the boot loader.
     - Nobody.
   * - ``env``
     - ``0x040000``
     - 8 KiB (``0x002000``)
     - The boot loader's environment.
     - The boot loader, by ``saveenv``; ``onu-update`` the same way
       (``updating.rst``).
   * - ``env2``
     - ``0x042000``
     - 8 KiB (``0x002000``)
     - The environment's redundant copy.
     - The boot loader, by ``saveenv``; ``onu-update`` the same way.
   * - ``config``
     - ``0x044000``
     - 240 KiB (``0x03c000``)
     - The shipped firmware's JFFS2 file system (big-endian, 4 KiB erase
       blocks, zlib), with that firmware's files, the device's registration
       identity among them. This system mounts it on ``/config`` and keeps
       what the board keeps across updates, its settings, in a file of its
       own there, at most 64 KiB, and never writes the shipped firmware's
       files (ADR 0014, step 9; the file's keys are listed in
       ``onu-utils/README.rst``). A store of before step 9, two slots that
       took the whole partition, goes into that file at the first boot.
     - Linux: ``onu-config``.
   * - ``k0``
     - ``0x080000``
     - 1856 KiB (``0x1d0000``)
     - Bank 0 kernel: a legacy U-Boot image.
     - The bench: ``make flash IMAGE=<file> TARGET=kernel``; ``onu-update``
       while bank 1 runs; the shipped firmware's web page with
       ``web-install.tar`` while the shipped firmware runs from bank 1.
   * - ``r0``
     - ``0x250000``
     - 1984 KiB (``0x1f0000``)
     - Bank 0 root filesystem: a SquashFS image.
     - The bench: ``make flash IMAGE=<file> TARGET=rootfs``; ``onu-update``
       while bank 1 runs.
   * - ``k1``
     - ``0x440000``
     - 1856 KiB (``0x1d0000``)
     - Bank 1 kernel. On a stick as shipped, the shipped firmware may run from
       either bank, in its own split; the bank it does not run is the one
       ``web-install.tar`` writes, and the shipped firmware stays in the other
       until an update overwrites it.
     - ``onu-update`` while bank 0 runs; the shipped firmware's web page with
       ``web-install.tar`` while the shipped firmware runs from bank 0.
   * - ``r1``
     - ``0x610000``
     - 1984 KiB (``0x1f0000``)
     - Bank 1 root filesystem.
     - ``onu-update`` while bank 0 runs.

"Nobody" means that no tool and no procedure of this handbook writes the
partition, and that Linux cannot: the kernel's device tree marks it read-only,
and the flash driver refuses to open it for writing. The column describes the
policy of the bench and of the device tree, not a protection of the chip. The
page ``flashing.rst`` gives the reasons. ``onu-update`` never writes the bank
that runs (``updating.rst``).

The partitions appear in Linux as ``mtd0`` to ``mtd7`` in the order of the
table, and as the block devices 31:0 to 31:7. The order matters: the loader's
kernel arguments name the root filesystem by number, ``root=31:5`` for ``r0``
and ``root=31:7`` for ``r1``, so the order of the nodes in the kernel's device
tree is part of the contract. The device tree marks ``boot`` read-only: from
Linux the loader cannot be written, and its environment, the configuration
store and both banks can.

The names of a bank
-------------------

A bank goes by a different name in each place that speaks of it, and the names
count differently:

.. list-table::
   :header-rows: 1
   :widths: 40 30 30

   * - Where
     - The first bank
     - The second bank
   * - This handbook, ``onu-update`` (``RUNNING``, ``BANK<N>_STATE``), the
       bench (``BANK=``) and the programs: the bank, from 0
     - bank 0: ``k0``, ``r0``
     - bank 1: ``k1``, ``r1``
   * - The panel's System page: a system, by letter
     - system A
     - system B
   * - The kernel's command line: the root filesystem by number
     - ``root=31:5``
     - ``root=31:7``
   * - The loader's environment, counting from 0: ``sw_commit``,
       ``sw_tryactive``, ``sw_active``, ``sw_valid<N>``, ``sw_version<N>``,
       ``mtdparts<N>``, ``img<N>_kernel``, the macros ``b<N>`` and
       ``set_act<N>``
     - 0
     - 1
   * - The loader's layout variables, counting from 1: ``fl_kernel<N>``,
       ``fl_rootfs<N>`` and their sizes
     - 1
     - 2

The loader is the one that mixes the two counts; the rest of the handbook says
bank 0 and bank 1.

The erase sector
----------------

Linux erases the chip in 4 KiB sectors, and every partition of the table starts
and ends on one: the two copies of the environment are 8 KiB each. The kernel
does so only with ``CONFIG_MTD_SPI_NOR_USE_4K_SECTORS``, which is on by default
and which ``rtl9602c_defconfig`` states. Without it the kernel erases 64 KiB
blocks and forces read-only every partition that is not made of whole blocks
(``drivers/mtd/mtdpart.c``): no bank could be chosen and nothing saved, and the
JFFS2 of ``config`` was made with 4 KiB blocks. ``tools/flash-layout`` checks
the device tree for partitions that are off the sector, and the kernel of an
image build for the option (``make check`` and ``make image-check``); the boot
script warns on the console when ``/proc/mtd`` shows that ``config`` erases in
anything but 4 KiB.

The loader's window
-------------------

The loader maps the whole chip for reading at ``0x94000000`` (KSEG0; the
uncached alias is ``0xb4000000`` and the physical address ``0x14000000``). A
bank is booted from this window in place: ``k0`` is at ``0x94080000``, ``r0``
at ``0x94250000``, ``k1`` at ``0x94440000`` and ``r1`` at ``0x94610000``.
``crc32`` and ``bootm`` read through it. Nothing is ever written through it;
whether a store there does anything is not known.

The boot partition is visible a second time in the reset-vector window, at
physical address ``0x1fc00000`` (KSEG1 ``0xbfc00000``). The first-stage loader
reads the loader from there, and the start of that window is where the CPU's
reset vector runs.

The loader's names for the layout
---------------------------------

The loader describes the same layout in variables, in hexadecimal without a
prefix. The bench compares them with its own table before it writes anything,
and stops with the variable's name if one differs. Mind the numbering: these
variables count the banks from 1 (see "The names of a bank").

.. list-table::
   :header-rows: 1
   :widths: 40 24 36

   * - Variable
     - Value
     - Meaning
   * - ``fl_boot_sz``
     - ``40000``
     - Size of ``boot`` (it starts at 0).
   * - ``fl_env``, ``fl_env2``, ``fl_env_sz``
     - ``40000``, ``42000``, ``2000``
     - The two environment copies and their size.
   * - ``fl_cfgfs``, ``fl_cfgfs_sz``
     - ``44000``, ``3c000``
     - ``config``.
   * - ``fl_kernel1``, ``fl_kernel1_sz``
     - ``80000``, ``14c000``
     - Bank 0 kernel as the shipped firmware splits the bank; ``k0`` of this
       system is ``1d0000`` long.
   * - ``fl_rootfs1``, ``fl_rootfs1_sz``
     - ``1cc000``, ``274000``
     - Bank 0 root filesystem as the shipped firmware splits the bank; ``r0`` of
       this system is ``1f0000`` long at ``250000``.
   * - ``fl_kernel2``, ``fl_kernel2_sz``
     - ``440000``, ``14c000``
     - Bank 1 kernel as the shipped firmware splits the bank; ``k1`` of this
       system is ``1d0000`` long.
   * - ``fl_rootfs2``, ``fl_rootfs2_sz``
     - ``58c000``, ``274000``
     - Bank 1 root filesystem as the shipped firmware splits the bank; ``r1`` of
       this system is ``1f0000`` long at ``610000``.

``img0_kernel`` and ``img1_kernel`` hold the window addresses of the two
kernels, ``94080000`` and ``94440000``.

The banks' split and the loader
-------------------------------

The loader rebuilds its layout variables, ``mtdparts0``, ``mtdparts1`` and
``bootargs`` from the first-stage loader's flash table at every start: a
``setenv`` of them lasts until the next reset, even when it is saved. Measured
on the board: after the split of this system was set and saved, the next start
read ``fl_kernel1_sz`` as ``14c000`` again, and the kernel's command line
carried the shipped table. So the loader keeps naming the shipped split, and
this system's split is the kernel's device tree and the bench's table alone.

Nothing of the split needs the loader: ``bootm`` starts the kernel at the start
of ``k0`` or ``k1``, the same in both splits, and takes the image's length from
its header; the kernel ignores the loader's partition table (see "The kernel
command line"), and ``root=31:5`` or ``root=31:7`` names ``r0`` or ``r1`` by its
number. Measured for bank 1 as well: a trial start booted the kernel at
``94440000`` and mounted the root filesystem from 31:7 at ``0x610000``. The bench
compares the loader's names with the shipped split, as a check that the board
is the one it expects, and writes by its own table. The shipped firmware's own
macros that write a bank (``upk``, ``upr``, ``yk``, ``yr``) do take the loader's
names: on a board that runs this system they would write a root filesystem
over the end of the kernel. The bench's guard runs none of them. The images
that were in bank 0 before the split do not fit it: flash both.

The environment
---------------

The environment is kept in two copies of 8 KiB, ``env`` and ``env2``. Each copy
has a CRC32 that is valid over its bytes 5 to 8191, and a flag byte at offset
4. ``saveenv`` writes the copies alternately: it erases and writes one copy and
then writes the flag byte of the other. ``Valid environment: 1`` follows a
write of ``env``, ``Valid environment: 2`` a write of ``env2``. The copy written
last carries flag 1, the other 0; of two copies whose CRC checks out the loader
takes the one flagged 1, and of two equal flags ``env2``. From Linux,
``onu-update`` saves the same way (``updating.rst``).

The image
=========

The kernel image
----------------

The loader boots a legacy U-Boot image: a header of 64 bytes, all fields
big-endian, followed by the data. For this board the data is the raw kernel
compressed with LZMA. The loader verifies both CRCs and decompresses the data
itself, so the kernel needs no self-extracting wrapper. ``make linux`` builds
the image with ``mkimage`` as ``build/linux/arch/mips/boot/uImage``.

.. list-table::
   :header-rows: 1
   :widths: 10 8 24 58

   * - Offset
     - Size
     - Field
     - Value for the kernel of this board
   * - 0
     - 4
     - Magic
     - ``0x27051956``
   * - 4
     - 4
     - Header CRC32
     - Over the 64 header bytes, with this field taken as zero.
   * - 8
     - 4
     - Creation time
     - Seconds since 1970 (UTC). ``bootm`` prints it as ``Created:``.
   * - 12
     - 4
     - Data size
     - The bytes that follow the header.
   * - 16
     - 4
     - Load address
     - ``0x80100000``
   * - 20
     - 4
     - Entry point
     - The address of ``kernel_entry``.
   * - 24
     - 4
     - Data CRC32
     - Over the data.
   * - 28
     - 1
     - Operating system
     - Linux (5)
   * - 29
     - 1
     - Architecture
     - MIPS (5)
   * - 30
     - 1
     - Image type
     - Kernel (2)
   * - 31
     - 1
     - Compression
     - LZMA (3)
   * - 32
     - 32
     - Name
     - Text, for example ``Linux-<release>``.

The field values are those of U-Boot's public ``include/image.h``. The bench
reads the header before it sends anything, and refuses a file whose magic,
architecture, compression, load address or CRCs are wrong or that is larger
than its target.

The load address ``0x80100000`` is the KSEG0 address of physical 1 MiB. The
kernel has to lie above the fixed exception vectors at ``0x80000000`` to
``0x800003ff``; linked at ``0x80000000`` it stops in ``trap_init()``. After any
change of the kernel configuration, read the load address from the header of
the built image: turning on crash dumps, which kexec turns on by default, moves
it to ``0x84000000``, above this board's memory, and the board configuration
keeps them off. The kernel page named above says why.

The root filesystem image
-------------------------

The root filesystem of a bank is a SquashFS image in ``r0`` (or ``r1``), which
the kernel mounts from the block device that ``root=`` names. The bench checks
the magic ``hsqs`` at offset 0 and the compression id, a 16-bit little-endian
value at offset 20, against the set the kernel supports (xz, 4, in the current
kernel configuration), and refuses a file that does not fit ``r0``.

Where an image goes in RAM
--------------------------

.. list-table::
   :header-rows: 1
   :widths: 18 82

   * - Address
     - Use
   * - ``0x80000000``
     - Staging area for an image that is to be flashed, received by YMODEM or
       by TFTP. The largest root filesystem image, 1.9 MiB, fits.
   * - ``0x80100000``
     - The kernel's load address: where ``bootm`` decompresses it (physical 1
       MiB).
   * - ``0x81000000``
     - Staging area for a RAM-boot image received by TFTP (physical 16 MiB). It
       may be up to 12 MiB, because the loader keeps its own code in the top of
       RAM, relocated to ``0x81c00000``.

A kernel can be started from RAM without writing the flash. The loader fetches
the image by TFTP to ``0x81000000``, checks it, decompresses it to
``0x80100000`` and enters it::

    tftpboot 0x81000000 uImage
    bootm 0x81000000

The compressed copy sits at 16 MiB while the decompressed kernel grows from 1
MiB, so the decompressed kernel has to end below physical ``0x01000000``, which
leaves 15 MiB. The loader may also cap the decompressed size, at a limit that
is not known. A development image that carries its user space is about 4 MB
and decompresses to about 8.1 MB; it does not fit ``k0`` and boots from RAM
only. The steps around the two commands (the host link, the boot arguments)
are on the page ``flashing.rst``.

How a bank is chosen
====================

Which bank a plain reset boots is set in the loader's saved environment.
``onu-update`` changes it from Linux, as the loader saves it (``updating.rst``).

.. list-table::
   :header-rows: 1
   :widths: 24 76

   * - Variable
     - Meaning
   * - ``sw_commit``
     - The committed bank, 0 or 1. It is the bank a plain reset boots, and the
       fall-back after a trial.
   * - ``sw_tryactive``
     - The one-shot trial: 2 means none is pending, 0 a trial of bank 0, 1 a
       trial of bank 1.
   * - ``sw_active``
     - The bank that was booted last. The loader writes it.
   * - ``bootcmd``
     - Runs ``boot_by_commit`` when ``sw_tryactive`` is 2 and
       ``boot_by_tryactive`` otherwise.
   * - ``bootdelay``
     - The length of the countdown, in ticks. It is 5 as shipped. The tick is
       taken to be one second.
   * - ``boot_by_commit``, ``boot_by_tryactive``
     - The macros that ``bootcmd`` chooses between.
   * - ``b0``, ``b1``
     - Boot bank 0 or bank 1 for this boot only, changing nothing.
   * - ``set_act0``, ``set_act1``
     - Record the active bank.
   * - ``en_wdt``
     - Arms the watchdog (see "The watchdog").
   * - ``mupgrade_en``
     - 1 as shipped: when the countdown ends, the loader first listens on the
       host link for a firmware image sent to it by multicast, and writes what
       it receives. Nothing asks who sends it. 0 switches the listener off; the
       bench's board runs with 0, and ``onu-update commit`` sets 0. The listener also sets up the loader's
       network on the way, which used to hide the restart fault described under
       "What the loader leaves behind".

After a reset the loader works as follows:

1. It prints ``Hit any key to stop autoboot:`` and counts ``bootdelay`` ticks
   down to 0. Any byte it receives stops the countdown, and is consumed: a
   command typed during the countdown loses its first character.
2. At the end of the countdown, or when ``boot`` is typed, it runs
   ``bootcmd``.
3. With ``sw_tryactive`` at 2 that is ``boot_by_commit``: the committed bank
   boots.
4. With any other value it is ``boot_by_tryactive``, which consumes the trial.
   It sets ``sw_tryactive`` to 2 and ``sw_active`` to the bank, saves the
   environment, arms the watchdog and boots the bank. The flag is therefore
   spent before the kernel runs, and the next reset takes the committed bank.
5. A bank is booted from the loader's window in place: ``94080000`` for bank 0,
   ``94440000`` for bank 1, with the arguments of that bank (next section).

The end of the countdown is the form measured. That typing ``boot`` at the
prompt does the same is expected and not yet confirmed on the board.

The fall-back of a trial is the committed bank. A trial protects the board only
while ``sw_commit`` names the other bank, as it does for an update
(``updating.rst``): a failed trial then costs one reset. With ``sw_commit``
naming the tried bank itself, a failed trial is followed by the same kernel,
and the way back is the loader prompt and ``run b0`` or ``run b1``. Measured
with ``sw_commit`` naming the other bank: a tried system that panicked, and a
tried kernel that the loader could not unpack, which the watchdog armed by
``en_wdt`` reset, were both followed by the committed bank.

The kernel command line
=======================

For bank 0 the loader hands the kernel three arguments:

* ``console=ttyS0,115200``, from the variable ``bootargs_base``;
* an ``mtdparts=`` table in the loader's own naming of the flash, which this
  kernel ignores, because its partitions come from its device tree and no
  command-line partition parser is built;
* ``root=31:5``, the last words of the variable ``mtdparts0``.

Bank 1's arguments are the same with ``root=31:7`` (the variable
``mtdparts1``). The macros assemble the arguments into ``bootargs``;
``rst2dfl_flg`` takes part in the assembly and is not defined, so it expands to
nothing. The loader passes no device tree: the kernel carries its own.

The kernel puts ``panic=10`` from its device tree in front of the loader's
arguments, so a panic restarts the board after 10 seconds. That order has not
yet been read from a boot of the current kernel configuration. The page named
above describes how the kernel builds the line.

The watchdog
============

The watchdog is a block of the SoC, driven by the kernel. Its control register
is at physical address ``0x18003268``, which a loader prompt reaches as
``0xb8003268``; it reads back what was written.

What a restart does
-------------------

The kernel's restart (``reboot``, SysRq ``b``, a panic with ``panic=10``) goes
through the watchdog driver's restart handler, which writes ``0x80000000`` to
the control register: the watchdog enabled with its shortest deadline, 335.5
milliseconds, and a full chip reset as its action. The loader then starts on a chip as a power cycle
leaves it, the switch and the SerDes lane to the host included, and no count
is left running.

Older kernels, which a bank may still hold, restarted by a jump to the CPU's
reset vector, with ``0xe7c00000`` written first: a deadline of 44.3 seconds,
as a safety net for a jump that never reached the loader. That count keeps
running through the jump, through the loader and into the next boot, although
the control register then reads 0. Writing 0 to the register stops it. The
loader does not. The chip therefore resets about 46 seconds after such a
restart (45.9 seconds were measured from arming to the loader's first output,
and 46.9 and 47.0 seconds after restarts) unless something clears the count in
time. The rest of this section is about that count and the one of a trial
boot.

Who clears it
-------------

* The kernel's watchdog driver, at its probe, when it finds the watchdog
  disabled in the register, as after such a restart: it writes its own word
  with the enable clear, which stops the count. A kernel whose driver probes
  within the 46 seconds survives the count of the restart that started it.
* A person or the bench at the loader prompt, with ``mw.l b8003268 0``. The
  bench types this one memory write before it flashes, because the transfer,
  the erase and the write take longer than the 46 seconds that the count
  allows.
* The chip reset itself, which starts everything again.

A RAM boot does not stop the count, and the bench leaves it running on purpose.
After an older kernel's restart, the boot has to reach the kernel within about
45 seconds. The first transfer after such a restart sometimes fails, and
waiting does not cure it; the reset that the count brings does (see "When
something goes wrong" on ``flashing.rst``). The jump had left the switch as the
kernel set it up, and behind one of the host's cages none of the loader's
frames left the board until the chip reset.

A trial boot
------------

The one-shot path arms the watchdog before it boots the bank (the macro
``en_wdt``), so that a failing image costs one reset: the chip resets, and the
committed bank boots. A committed boot leaves the watchdog disarmed.

The kernel leaves the loader's count running. The watchdog driver finds it
enabled at its probe, restarts it with the driver's own timeout and feeds it
until user space opens ``/dev/watchdog``, for 60 seconds at most; procd, the
first process, opens it as it starts and feeds it for as long as it runs (ADR
0013). Once procd feeds the watchdog, what ends a failed trial is
``onu-trial``, which the boot script starts as its first step and which
restarts the stick into the committed bank when the system does not come up
(``updating.rst``), a boot script that hangs after it included. So a trial
kernel that hangs before the driver's probe is reset by the loader's count, and
one that never starts user space by the end of the 60 seconds (M: a RAM boot
with the loader's watchdog armed and no init process reset the chip 69 s after
``bootm``, 2026-10-11).

What the loader leaves behind
=============================

At the moment the kernel is entered:

* UART0 is at 115200 baud, divisor 109.
* Timer 1 is running, with the control word ``0x11000032``. The kernel's timer
  driver programs it anew as its clocksource, with its interrupt disabled.
* The SerDes lane to the host is off: the mode field of ``RTL9602C_SERDES_CTRL``
  (offset ``0x000214`` of the switch register file) reads ``0x1f``, and the
  whole register ``0x0004801f``. It stays off unless the lane macro ran before
  ``bootm``. The kernel's switch driver brings it up itself.
* The system controller's packet-buffer enable for the PON MAC is clear: the
  register at physical ``0x1800063c`` reads ``0x10``.
* The watchdog is disarmed, unless a trial boot or an older kernel's restart
  armed it. A count armed before survives into the kernel, as the section
  above says.

A restart resets the whole chip; the switch's table memories are the one state
known to survive it, and the drivers empty them at bring-up. The older restart,
a jump to the reset vector, was no reset: the CPU's state, its caches and TLB
and every on-chip block kept the state they had. One block it left running was
the CPU port's DMA engine, which went on receiving into the rings of the kernel
that was going away, in memory that the next kernel owned by then; on the
board, about half of the boots from the flash that followed a ``reboot`` lost
their first processes, the boot script among them. The kernel stops the engine
on an orderly shutdown and on a panic, and the chip reset stops it in any case,
SysRq ``b`` included.

Timings
=======

.. list-table::
   :header-rows: 1
   :widths: 56 44

   * - Step
     - Time
   * - A restart, to the loader's countdown
     - About 3.2 s (the first-stage banner after 1.9 s, the ``U-Boot`` banner
       after 3.1 s).
   * - An older kernel's restart through the reset vector, to the loader's
       countdown
     - 1.81 s (the first-stage banner after about 0.3 s, the ``U-Boot`` banner
       after about 1.5 s).
   * - An older kernel's restart that only the watchdog completes
     - About 46 s.
   * - The countdown
     - ``bootdelay`` ticks, taken to be one second each.
   * - TFTP transfer and start of an image by the loader
     - About 4 s for images of 0.8 to 1.2 MB. The images of about 4 MB were not
       timed apart.
   * - The kernel's banner to user space
     - About 2 s.

The times of whole procedures (a RAM boot, a flash) are on the page
``flashing.rst``.

If you replace the loader
=========================

What the kernel and the bench rely on, from the sections above:

* The loader runs from the start of ``boot`` when the CPU is at its reset
  vector, after a power-up and after a chip reset; the kernel's restart is a
  chip reset by the watchdog.
* It boots a legacy U-Boot image with LZMA compression at the image's load
  address, ``0x80100000``, and enters it at the image's entry point with the
  arguments in the registers that the kernel page describes. It passes no
  device tree; the kernel carries its own.
* It passes the arguments of the bank: ``console=ttyS0,115200`` and
  ``root=31:5`` for bank 0.
* It leaves UART0 at 115200 baud, 8N1.
* After an older kernel's restart, a count that the restart armed is still
  running when the loader starts. A loader that does not stop it has the chip
  reset about 46 seconds after the restart, unless the kernel clears it first.
* A loader that uses the network needs the SerDes lane to the host up; the
  lane macro on ``flashing.rst`` says what the bring-up is.
* The bench types to the dialogue of U-Boot 2011.12: its prompt, its
  countdown, its commands and its output. It reads the variables above. With a
  different loader it needs a different driver.

Open items
==========

* The order of the kernel command line after ``panic=10`` has not yet been read
  from a boot of the current configuration.
* The decompressed kernel is assumed to stay below 15 MiB and below whatever
  cap the loader has. It has not been tested at the limit.
* Nothing is written through the loader's flash window, and whether a store
  there does anything is not known.
* A bad write to ``boot`` is assumed to be beyond repair from the loader. It
  has never been tried.
* The read-only flag of ``boot`` in the device tree has not been tested: no
  test tries to open it for writing, though the flash driver writes the other
  partitions (``onu-update``, ``onu-config``).
* That a trial boot is not reset by the watchdog when the kernel hangs between
  the clearing of the count and the probe of the watchdog driver has not been
  observed. A trial boot that stops there would show it.
* That the countdown tick is one second is taken from the figures of the
  countdown and has not been timed.
* That ``boot`` typed at the prompt consumes a pending trial exactly as the end
  of the countdown does has not yet been confirmed on the board.
