.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========================================================
ADR 0018: The kernel series aims at mainline
=========================================================

:Status: Accepted, amended (see "Amendments"), and amended by 0021
:Date: 2026-10-11
:Decided-by: the owner, 2026-10-11

.. note::

   In plain words: the drivers are written to go into the official Linux one
   day. They are split the way the kernel's maintainers would want them,
   before they grow further.

Context
=======

ADR 0003 gives the repository a mainline structure: device tree, named
registers, the kernel's subsystems, parts "liftable without re-layout". It
did not say whether the kernel series is to be offered to mainline, and the
switch driver has grown as one module: ``rtl9602c.h`` has 3779 lines,
``rtl9602c-offload.c`` 7420, and nine objects go into the module. The GPON
MAC, the GEM ports, the OMCI channel and the I2C master of the optics live
in the DSA switch driver.

The PON interface (``include/uapi/linux/pon.h``) is half an abstraction. The
ONU's identity is a devlink parameter of the switch driver; the GEM ports'
attributes ``IFLA_GEM_{PBITS,QUEUE,WEIGHT,CIR,PIR}`` overlap with what tc
and switchdev express; the XG-PON and XGS-PON values of its enums have no
implementation; the counters in sysfs have no entry in
``Documentation/ABI``. The changes of the series to the DSA core (a tag
protocol number, the order of ``TC_SETUP_FT``, ``port_set_pcp_rewr``) are
not collected in one place, and there are no KUnit or kselftest tests.

Decision
========

The kernel series aims at mainline. New work on the kernel side keeps to
what a submission needs, and the existing drivers are split towards it:

* The GPON MAC, the GEM ports and the OMCI channel leave the DSA driver for
  a driver of their own on an auxiliary device of the switch, under the PON
  class.
* The I2C master of the optics becomes a driver of ``drivers/i2c``.
* The PON uAPI gets a record of its own before it changes again: what
  belongs to the PON class, what to devlink, tc and switchdev, and the
  values without an implementation removed.
* The DSA core changes are kept as patches of their own, each with its
  reason, on top of which the drivers sit.
* Pure functions get KUnit tests, and the PON family a kselftest through YNL
  with a stand-in MAC.

Consequences
============

* The split is large work and comes in steps; until a step is done, the code
  stays where it is and works as it does.
* Each step keeps the board's behaviour as measured, which the device tests
  show; a step that changes it is a bug of that step.
* Code that only the stick's product needs (the bench's hooks, the debugfs
  views of test images) stays behind Kconfig options of its own, as
  ``NET_DSA_RTL9602C_PLOAM_INJECT`` and ``NET_DSA_RTL9602C_OPTICS_STORE`` do.
* Dead paths, such as NAPT and the flowtable without netfilter in the
  board's configuration, are either reached and tested or removed before a
  submission.

Amendments
==========

**2026-10-12, the owner: the GEM queue attributes and the NAPT paths stay.**

The GEM ports' attributes ``IFLA_GEM_{QUEUE,WEIGHT,CIR,PIR}`` stay in the link
until the PON class is offered. They overlap with an offloaded HTB class of tc
(rate, ceil, priority, quantum), but the queues belong to the T-CONT, which is
no network interface, and nothing yet chooses the class of a frame;
``Documentation/networking/pon.rst`` says so where the attributes are
described. When the PON class is offered they move to tc and the attributes
go, ``IFLA_GEM_PBITS`` apart, which is the 802.1p mapper and not a queue
discipline. The NAPT and flowtable paths stay until the NAT backend can reach
and test them, which is before a submission, as above.

**2026-10-12: what the split has done.**

Done in the same clean-up:

* The I2C master of the optics is a driver of ``drivers/i2c``,
  ``i2c-rtl9602c``, on the auxiliary device ``rtl9602c.i2c`` that the switch
  driver makes.
* The GPON MAC, the PON MAC, the GEM ports and the OMCI channel are a driver
  of their own, ``rtl9602c-pon`` (``drivers/net/pon/rtl9602c/``), on the
  auxiliary device ``rtl9602c.pon``. The two drivers meet at
  ``include/linux/platform_data/rtl9602c-pon.h``: the services of the switch
  to the MAC (the interrupt, the laser, the switch's half of the GEM ports) and
  the services of the MAC to the switch, called under a read-write semaphore
  that the MAC's driver takes for writing to bind and to unbind. Neither
  includes the other's header. The switch module went from 310 to 228 KiB, the
  image's kernel grew by about 1 KiB, and the behaviour on the board is as
  measured before. The step found a fault that only an unbind shows: with no
  driver to read the GTC latches the GPON interrupt source asserted again at
  once and the interrupt thread spun until the watchdog reset the stick. The
  source is now masked when no driver handles it.
* The changes to files the kernel owns are listed, each with its reason, in
  ``docs/handbook/kernel-series.rst``, and ``tools/kernel-series`` checks that
  the page names every such file and exports the commits of the networking
  core as patches.
* The pure calculations of the PON driver are in one file and have KUnit
  tests, as the NAPT hashes of the switch driver do. The PON class has a
  stand-in MAC, ``pon_sim``, and a kselftest through YNL
  (``tools/testing/selftests/drivers/net/pon/``) that runs on any
  architecture. It found a fault of the class itself, the missing selection of
  switchdev, which is fixed, and with lock debugging a sleeping lock in the
  stand-in's own transmit path.

What stays before a submission is the NAPT and flowtable paths, as above, and
the size of ``rtl9602c-offload.c``, which holds the offloads of the switch and
may be split by function.
