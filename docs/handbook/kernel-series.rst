.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========================================
The kernel series and the kernel under it
=========================================

``linux/`` is Linux 6.18.48 with this project's commits on top (ADR 0018: the
series aims at mainline). This page says what those commits change in code
that the kernel owns, and why, so that a maintainer who is offered one of them
sees its reason, and so that nothing changes upstream's code without a line
here. Files the series adds are its own and are not listed.

Layers
======

From the bottom:

#. The import of the release, one commit, "linux: import Linux 6.18.48".
#. The platform: the Lexra LX5281 core and the RTL9602C SoC, its device-tree
   bindings, and the small changes to the Otto drivers (timer, GPIO, SPI,
   watchdog) that the SoC shares with the RTL838x family.
#. The networking core: the few changes to DSA, flow offload, the PTP core
   and the YNL generator that the drivers need. These are offered first, each
   alone, with its reason in the commit message.
#. The PON class, ``drivers/net/pon``, its uAPI and its genetlink family: new
   code, with the GEM link attributes in ``include/uapi/linux/if_link.h``.
#. The drivers, which sit on the layers below: the switch (DSA), the GPON MAC,
   the CPU-port DMA engine and the I2C master of the optics. The switch and
   the CPU-port engine share one contract, ``include/linux/dsa/rtl9602c.h``;
   the switch and the I2C master share ``include/linux/platform_data/
   i2c-rtl9602c.h``.

What the series changes in the kernel's own files
=================================================

Paths are relative to ``linux/``. ``tools/kernel-series/kernel-series check``
fails when the series changes a file that is not named below.

The networking core and the tools
---------------------------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - File
     - Change and reason
   * - ``include/net/dsa.h``, ``net/dsa/Kconfig``, ``net/dsa/Makefile``
     - A tag protocol, ``DSA_TAG_PROTO_RTL9602C``, and its tag driver. The
       switch's CPU tag is in the DMA descriptors, never in the frame, and
       the driver moves no bytes, but DSA needs a tag driver for the conduit
       to hand it frames at all.
   * - ``net/dsa/user.c``
     - ``TC_SETUP_FT`` is offered to the switch's ``port_setup_tc()`` before
       the conduit's, so that a switch with a flow engine of its own sees the
       flow block; every switch in the tree answers ``-EOPNOTSUPP`` and
       behaves as before.
   * - ``include/net/dsa.h``, ``net/dsa/user.c``
     - A switch operation, ``port_set_pcp_rewr``, and the DCB rewrite table
       (``dcb rewr ... prio-pcp``) on the user ports, which had none, so a
       switch with an egress PCP remark can be told its map the standard way.
   * - ``include/net/flow_offload.h``, ``net/core/flow_offload.c``
     - ``flow_rule_match_num_of_vlans()``: flower matches the number of VLAN
       tags, and every other key of a rule has a helper for drivers.
   * - ``drivers/ptp/ptp_clock.c``
     - ``-ERANGE`` for a frequency that does not fit a ``long``. On a 32-bit
       architecture ``clock_adjtime()`` cut it first, and the cut value
       passed the range check and was applied.
   * - ``tools/net/ynl/pyynl/ynl_gen_c.py``
     - The generator writes ``GENL_MCAST_CAP_NET_ADMIN`` for a multicast
       group with ``admin-perm``, as the schema allows, so a family can keep
       a group to privileged listeners (the ``pon`` family's alarms).
   * - ``include/uapi/linux/if_link.h``
     - The ``gem`` link kind of the PON class: its attributes and the
       direction enum. Part of the class, which is new code; the file is
       upstream's.

The platform
------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - File
     - Change and reason
   * - ``arch/mips/include/asm/cpu.h``, ``arch/mips/kernel/cpu-r3k-probe.c``
     - The Lexra LX5281 (``PRID_IMP_LEXRA``) in the R3000 probe. Without a
       case for its PRId ``cpu_probe()`` stops the boot at a ``BUG_ON()``
       before any console exists. The core runs as an R3000 with a 64-entry
       TLB, without ``MIPS_CPU_LLSC`` (``sc`` never stores) and without the
       FPU probe.
   * - ``arch/mips/include/asm/tlbmisc.h``, ``arch/mips/mm/tlb-r3k.c``
     - ``r3k_wired_entries``: the number of entries a full flush leaves
       alone, 8 on the R3000 and 0 on this core. And a fix that holds for the
       R3000 code in general: the ranged and single-page flushes retired
       every entry at the same ``EntryHi``, which leaves several entries with
       one value, undefined by the architecture; each now gets an unmapped
       KSEG0 address of its own.
   * - ``arch/mips/include/asm/cacheflush.h``, ``arch/mips/mm/cache.c``,
       ``arch/mips/mm/Makefile``
     - Cache maintenance for the Lexra core, through coprocessor 0 register
       20 (``c-lexra.c``). The R3000 code isolates the cache with
       ``Status.IsC``/``SwC``, and on this core the boot stops silently at
       the first code generated at run time.
   * - ``arch/mips/kernel/relocate_kernel.S``
     - ``__SYNC()`` instead of a bare ``sync``, which is not a MIPS I
       instruction, so that the file builds for the core; and the Lexra
       caches maintained before the jump, without which a kexec'd kernel
       never comes up.
   * - ``arch/mips/Kconfig``, ``arch/mips/Kbuild.platforms``,
       ``arch/mips/boot/dts/Makefile``, ``arch/mips/boot/dts/realtek/Makefile``
     - The platform, its symbol and the board's device tree in the build.
   * - ``drivers/clocksource/timer-rtl-otto.c``
     - ``rttm_cs`` is static: the platform's code no longer reaches it.
   * - ``drivers/gpio/gpio-realtek-otto.c``, ``drivers/gpio/Kconfig``
     - The RTL9602C entry: line N is bit N of a 32-bit word in the CPU's
       byte order (plain 32-bit accesses, ``GPIO_PORTS_REVERSED``), interrupts
       off, and the platform builds the driver.
   * - ``drivers/spi/spi-realtek-rtl.c``, ``drivers/spi/Kconfig``,
       ``drivers/spi/Makefile``
     - The RTL9602C controller, and a symbol of its own. And a fix that
       holds for every user: ``init_hw()`` cleared both chip-select bits,
       which are active low, so CS0 stayed asserted until the first transfer
       and CS1 for good; both are now deasserted at probe and the select bit
       stays as the boot loader left it.
   * - ``drivers/watchdog/realtek_otto_wdt.c``, ``drivers/watchdog/Kconfig``
     - The RTL9602C, which raises no phase 1 interrupt (so no pretimeout is
       advertised). And, for every user, a watchdog the boot loader started
       is kept running instead of being stopped by the default control word
       at probe.
   * - ``Documentation/devicetree/bindings/gpio/realtek,otto-gpio.yaml``,
       ``Documentation/devicetree/bindings/spi/realtek,rtl-spi.yaml``,
       ``Documentation/devicetree/bindings/timer/realtek,otto-timer.yaml``,
       ``Documentation/devicetree/bindings/watchdog/realtek,otto-wdt.yaml``
     - The RTL9602C compatibles of the Otto devices.
   * - ``Documentation/devicetree/bindings/mips/cpus.yaml``,
       ``Documentation/devicetree/bindings/mfd/syscon.yaml``,
       ``Documentation/devicetree/bindings/vendor-prefixes.yaml``
     - The Lexra core, the SoC's system control block and the vendor prefixes
       of the board.
   * - ``Documentation/arch/mips/index.rst``
     - The SoC and board reference in the index.

The hooks of new code
---------------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - File
     - Change and reason
   * - ``drivers/net/Kconfig``, ``drivers/net/Makefile``
     - The PON class directory.
   * - ``drivers/net/dsa/realtek/Kconfig``, ``drivers/net/dsa/realtek/Makefile``
     - The RTL9602C switch driver, beside upstream's Realtek switch drivers.
   * - ``drivers/net/ethernet/realtek/Kconfig``,
       ``drivers/net/ethernet/realtek/Makefile``
     - The CPU-port DMA engine driver.
   * - ``drivers/i2c/busses/Kconfig``, ``drivers/i2c/busses/Makefile``
     - The I2C master of the optics.
   * - ``Documentation/i2c/busses/index.rst``,
       ``Documentation/networking/device_drivers/ethernet/index.rst``,
       ``Documentation/networking/device_drivers/index.rst``,
       ``Documentation/networking/devlink/index.rst``,
       ``Documentation/networking/dsa/index.rst``,
       ``Documentation/networking/index.rst``
     - The pages of the drivers, the PON class and the devlink parameters in
       the indexes.
   * - ``MAINTAINERS``
     - The entries of the new drivers and the class.

Looking at the series
=====================

::

    tools/kernel-series/kernel-series list              # the files above
    tools/kernel-series/kernel-series check             # none is missing here
    tools/kernel-series/kernel-series export -o DIR     # the core patches

``export`` writes, in the order of the series, the commits that touch the
networking core, the PTP core or the YNL generator, each as a patch file for
its maintainers. A commit that adds a driver beside such a change, as the tag
driver's commit does, is exported whole. The ``check`` runs in ``make check``,
so a change to a file of the kernel's that this page does not name fails the
build until it is named, with its reason.
