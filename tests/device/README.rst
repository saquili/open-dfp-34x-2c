.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

============
tests/device
============

The hardware-in-the-loop test runner.

This drives the reference regression on the real board through the bench
tooling: boot to a shell, the watchdog life-cycle, the host link and its
recovery, bridged throughput both ways, VLAN and forwarding-database behaviour,
the classifier and shaper counters, the hardware clock, GPON downstream lock
and time-to-ready, the OMCI channel both ways, and the optics diagnostics and
laser-safety checks. Results are attached to each change.

Empty for now. It is populated by **WI-19** (with the kernel selftests that
live under ``linux/tools/testing/selftests/``).

Fine-grained functional tests live with their component: kernel selftests in
``linux/``, and the OMCI daemon's host unit tests in ``omcid/tests/``.
