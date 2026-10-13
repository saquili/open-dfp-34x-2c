.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

========
tools/vm
========

The kernel of this tree in qemu, for the tests that need a kernel that has the
features of the board's, and not the board.

``vm`` builds an arm64 kernel from the repository's own ``linux/`` on an
``allnoconfig`` with the options a test asks for, boots it on qemu's ``virt``
machine in software (no KVM) with the container's own root file system shared
as the guest's root over 9p, and runs a test as the guest's init. The guest
runs the programs the container has, so a test needs nothing installed on the
host but docker.

::

    tools/vm/vm omcid-kernel     # omcid's kernel backend test, make check-kernel
    tools/vm/vm pon-selftest     # the PON class selftest over the stand-in MAC
    tools/vm/vm kunit            # the KUnit tests of the RTL9602C drivers
    tools/vm/vm image            # build the container again
    tools/vm/vm clean            # forget the kernel builds

What each one is for:

``omcid-kernel``
    The test of omcid's kernel backend (``omcid/tests/kernel``) runs the
    daemon's netlink and ``tc`` calls against a real kernel, and needs the tc
    actions the daemon installs, the vlan action among them, which the kernel
    of a developer's laptop or of a container host often leaves out. The kernel
    here has them (``omcid.config``). The test is built from the working tree
    for the guest, and its status is the run's.

``pon-selftest``
    ``linux/tools/testing/selftests/drivers/net/pon/pon.py`` drives the PON
    class through the generic netlink family and rtnetlink, over the stand-in
    MAC ``pon_sim``. The kernel has the class, the stand-in and the checks that
    find a locking or sleeping mistake (``pon.config``), and a run in which the
    kernel found one fails even when every case passed. ``PYTEST_ARGS`` goes to
    the selftest, and ``VM_CMD`` runs a command in its directory instead, for a
    look inside the guest.

``kunit``
    The KUnit tests of the switch driver and of the PON driver run through the
    kernel's ``kunit.py`` on the same kind of kernel, from the options of
    ``rtl9602c.kunitconfig``.

Requirements
============

Docker, and an aarch64 host: Apple silicon, or an aarch64 Linux machine. The
guest kernel is built natively by the container, as a cross build would need a
toolchain this tool does not carry, and the guest's programs are the
container's. On another host ``vm`` refuses with that reason.

The container is built from ``Dockerfile`` at the first run: the Debian of the
distribution build (``docs/handbook/building.rst``) with qemu and the tools the
guest's tests call (iproute2, ethtool, Python with PyYAML). The kernel builds
are kept in the docker volume ``open-dfp-34x-2c-vm-kbuild`` (``VM_VOLUME``
names another), one directory for each of the first two, so the first run of
each takes minutes to build a kernel and the next ones seconds; ``REBUILD=1``
configures the kernel again (it is configured again by itself when a fragment changed), and ``tools/vm/vm clean`` drops the volume.

Environment
===========

``REBUILD``
    ``1`` configures the guest kernel again from the fragments, which a change
    of a fragment does by itself.
``JOBS``
    The parallelism of the kernel build; the container's processors by default.
``QEMU_TIMEOUT``
    The seconds the guest may take, 900 by default.
``VM_IMAGE``, ``VM_VOLUME``
    The names of the container and of the volume.
``PYTEST_ARGS``, ``VM_CMD``
    See ``pon-selftest``.

How a run is judged
===================

The guest's init runs the test and prints ``vm-rc=`` and its status, which the
run returns, so a test that failed fails the run. A guest that never printed
it, because it hung, panicked early or ran out of its time, is a failure too
(status 2), and so is a kernel option a fragment asks for that the
configuration lacks, which says that the fragment names an option the kernel
has not got.

Tests
=====

``tests/run`` checks without docker that the fragments name options the
kernel's Kconfig has and that the scripts parse. It is part of ``make check``.
