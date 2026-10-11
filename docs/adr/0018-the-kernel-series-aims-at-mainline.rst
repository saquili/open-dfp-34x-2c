.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========================================================
ADR 0018: The kernel series aims at mainline
=========================================================

:Status: Accepted
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
