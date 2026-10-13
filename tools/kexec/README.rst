.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===========
tools/kexec
===========

Starting a trial kernel on the stick from the running one, without the boot
loader, the console or a write to flash.

The board's flash is read-only from Linux, the switch driver is built in (so a
kernel module cannot be pushed), and the boot loader's prompt needs the serial
console. kexec writes nothing: ``kexecboot`` loads a new kernel into memory and
jumps to it, and a kernel that does not come up costs a reset, which returns to
the image of the bank the stick runs from. The page of the MIPS port
(``linux/Documentation/arch/mips/rtl9602c.rst``, "kexec") has what the kernel
does for it and what was measured.

What is here
============

``kexecboot.c``
    Loads a vmlinux ELF with ``kexec_load`` and starts it with ``reboot``. It
    reads the load addresses from the ELF's program headers, splits a segment
    that is larger than the kernel's per-segment ceiling (a quarter of RAM)
    into the fewest pieces that fit, and refuses what it cannot load before it
    asks the kernel for anything. ``-n`` loads and returns, so that ``reboot``
    starts the new kernel later.

``build``
    Makes the three files a trial needs, from an image build that has run (see
    "Building"): the RAM image with kexec on, a small target kernel and
    ``kexecboot``.

``tests/``
    The self-tests, part of ``make check``: the build script against a made-up
    image tree and, on a Linux host, ``kexecboot`` compiled with every warning
    an error and run on files it must refuse.

Building
========

On the Linux host (or the container) that built the image, after ``make
image``::

    tools/kexec/build

The files go to ``build/kexec``:

``ram.uImage``
    The RAM image (``make ram-image``) with ``CONFIG_KEXEC`` on and crash dumps
    off, since they reserve memory that moves the load address of the new
    kernel. It is also left as ``build/image/images/ram.uImage``, so the next
    ``make ram-image`` makes the ordinary one again.

``target.elf``
    The kernel to start: the board's configuration with a built-in initramfs of
    busybox and a four-line ``/init`` that prints the kernel's version, its
    command line and the time it came up, and then stops at a shell. Its
    command line, ``console=ttyS0,115200 panic=10``, is built in, since a
    kernel started with kexec on this platform is given none (the header of
    ``kexecboot.c`` says why).

``kexecboot``
    The program, linked statically: the image's libc is cut to what its
    programs use, and has no room for one more.

``tools/kexec/build --dry-run`` prints what it would run, and ``IMAGE_OUT``,
``KEXEC_OUT`` and ``JOBS`` name the image build, the output directory and the
parallelism.
The variables of ``make ram-image`` that are in the environment go on to it: a
bench's own files for the RAM image (``OPEN_DFP_34X_2C_LOCAL_OVERLAY``, the
ones that make its addresses, its ssh key and its console login) must be named
there, or the stick starts from the RAM image with none of them and the bench
cannot reach it.

Running it on the stick
=======================

1. Start the stick from the RAM image over TFTP (``make ramboot
   IMAGE=build/kexec/ram.uImage``, ``tools/bench/README.rst``), with the
   kernel's argument ``watchdog.stop_on_reboot=0`` among those the boot loader
   passes. The watchdog then keeps counting across the jump, so that a target
   that hangs ends in a chip reset (about a minute) and not in a stick that
   has to be power cycled.

2. Put the two files in memory on the stick; the image has no ``scp``::

       ssh root@STICK 'mkdir -p /kx && cat > /kx/kexecboot && chmod 755 /kx/kexecboot' < build/kexec/kexecboot
       ssh root@STICK 'cat > /kx/target.elf' < build/kexec/target.elf

3. Start the target, on the console, since the ssh session ends with the old
   kernel::

       /kx/kexecboot /kx/target.elf

   ``kexecboot`` prints the segments it loaded; the console then shows the new
   kernel's log, and the lines ``kexec-target: userspace up at ... s`` and its
   command line. On the lab stick (a build of 2026-10-13) it loaded one segment
   of 1506 pages and the target's ``/init`` ran 0.82 s after the jump.

4. The target leaves the watchdog unfed. The chip resets by itself when the
   watchdog's timeout ends, about a minute after the jump, and the boot loader
   starts the image of the bank the stick runs from.

Memory
======

The stick has 25 MB of RAM. The file ``/kx/target.elf`` is in memory (the
files of ``/kx`` are in a tmpfs), and ``kexec_load`` copies its segments into
pages of its own, so a target needs room twice. With the diagnostics left out
of the RAM image, as ``build`` does, about 6.4 MB were free after the boot, and
the 6.0 MB target fitted, just: with the diagnostics in, the file did not
(``cat: write error: No space left on device``). A larger target needs a
smaller RAM image or a smaller kernel; ``kexecboot`` says "segment 0 runs past
the end of the file" when it is given a file that was cut short.

What it does not do
===================

It does not give the new kernel a command line of its own (see the header of
``kexecboot.c``), it handles an ELF that is 32-bit, big-endian and MIPS and
linked in KSEG0 or KSEG1, and it does not start a kernel that needs more
segments than the kernel's limit of 16 or more memory than half of RAM.
