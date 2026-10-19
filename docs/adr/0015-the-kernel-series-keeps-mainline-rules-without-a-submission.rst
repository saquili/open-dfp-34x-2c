.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

==========================================================================
ADR 0015: Hold the kernel series to mainline's rules without submitting it
==========================================================================

:Status: Accepted
:Date: 2026-10-15
:Deciders: the maintainer

Summary
=======

The drivers are written to the official Linux kernel's rules and split the
way its maintainers would expect, so that anyone can take them to mainline.
This project does not submit them itself.

Context
=======

ADR 0003 gives the repository a mainline structure: device tree, named
registers, the kernel's subsystems, parts that can be lifted out without a
new layout. It does not say whether the kernel series is meant for mainline.

The switch driver had grown as one module: ``rtl9602c.h`` had 3,779 lines,
``rtl9602c-offload.c`` 7,420, and nine objects went into the module. The
GPON MAC, the GEM ports, the OMCI channel and the I2C master of the optics
lived in the DSA switch driver. The PON interface
(``include/uapi/linux/pon.h``) was half an abstraction: the ONU's identity
was a devlink parameter of the switch driver; the GEM ports' attributes
``IFLA_GEM_{PBITS,QUEUE,WEIGHT,CIR,PIR}`` overlapped with what tc and
switchdev express; the XG-PON and XGS-PON values of its enums had no
implementation; and the counters in sysfs had no entry in
``Documentation/ABI``. The series' changes to the DSA core were not
collected in one place, and there were no KUnit or kselftest tests.

A submission is more than code that meets the rules: it means rounds of
review with the maintainers of networking, of DSA and of a new PON class
with its own uAPI, over months or years. The project is led by one person,
who builds what the stick needs and publishes it quickly, and has no time
for that.

Decision
========

The kernel series keeps mainline's rules, and this project does not offer
it to mainline.

* **The rules.** The kernel's coding style and checkpatch ``--strict`` over
  every commit of the series, the device-tree bindings, kernel-doc, KUnit
  and kselftest tests, the changes to the kernel's own files listed with
  their reasons (``docs/handbook/kernel-series.rst``), and the series
  exported on its own (``tools/kernel-series``, ``git subtree split -P
  linux``). The gates of continuous integration hold every commit to them.
* **The drivers are split as a submission would need.** The GPON MAC, the
  GEM ports and the OMCI channel are a driver of their own on an auxiliary
  device of the switch, under the PON class; the I2C master of the optics is
  a driver in ``drivers/i2c``; the DSA core changes are patches of their
  own, each with its reason, on which the drivers sit; pure functions have
  KUnit tests, and the PON family a kselftest through YNL with a stand-in
  MAC.
* **The PON uAPI** says what belongs to the PON class and what to devlink,
  tc and switchdev, without values that have no implementation. The GEM
  ports' queue attributes ``IFLA_GEM_{QUEUE,WEIGHT,CIR,PIR}`` stay in the
  link until the class is offered: they overlap with an offloaded HTB class
  of tc, but the queues belong to the T-CONT, which is not a network
  interface, and nothing yet chooses the class of a frame
  (``Documentation/networking/pon.rst``). ``IFLA_GEM_PBITS`` is the 802.1p
  mapper, not a queueing discipline, and stays.
* **Product-only code** (the bench's hooks, the debugfs views of test
  images) stays behind Kconfig options of its own, as
  ``NET_DSA_RTL9602C_PLOAM_INJECT`` and ``NET_DSA_RTL9602C_OPTICS_STORE`` do.
* **Anyone may take the series to mainline** under its licence, whole or in
  part. The project helps where it can, and does not lead the effort. What a
  submission would still need, the NAPT and flowtable paths reached and
  tested or removed and ``rtl9602c-offload.c`` split by function, is
  ordinary work, done when it is wanted.

Consequences
============

* No release waits for review outside the project, and no promise is made
  that the drivers will reach mainline.
* The PON class's interfaces, the ``pon`` generic netlink family, the
  ``gem`` link kind with its ``IFLA_GEM_*`` attributes and
  ``/dev/ponNomci0``, are this project's own and may change from one build
  to the next. A submission would change them again; whoever submits
  decides how.
* Because the rules stay, a person who wants to submit the series can start
  from it without a rewrite.
* Each step of the split keeps the board's behaviour as measured, which the
  device tests show; a step that changes it has a bug.

The split as it stands
======================

* ``i2c-rtl9602c`` in ``drivers/i2c`` drives the optics' I2C master, on the
  auxiliary device ``rtl9602c.i2c`` that the switch driver creates.
* ``rtl9602c-pon`` (``drivers/net/pon/rtl9602c/``) drives the GPON MAC, the
  PON MAC, the GEM ports and the OMCI channel, on the auxiliary device
  ``rtl9602c.pon``. The two drivers meet at
  ``include/linux/platform_data/rtl9602c-pon.h``: the switch's services to
  the MAC (the interrupt, the laser, the switch's half of the GEM ports) and
  the MAC's services to the switch, called under a read-write semaphore that
  the MAC's driver takes for writing to bind and unbind. Neither includes
  the other's header. The GPON interrupt source is masked while no driver
  handles it, since nothing else would read the GTC latches. The switch
  module went from 310 to 228 KiB, and the image's kernel grew by about
  1 KiB.
* ``tools/kernel-series`` checks that ``docs/handbook/kernel-series.rst``
  names every file of the kernel's own that the series changes, and exports
  the commits of the networking core as patches.
* The PON driver's pure calculations and the switch driver's NAPT hashes
  have KUnit tests. The PON class has a stand-in MAC, ``pon_sim``, and a
  kselftest through YNL (``tools/testing/selftests/drivers/net/pon/``) that
  runs on any architecture.
