.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

============
tests/device
============

The tests that run on the stick itself. Each case checks one function of the
stick, says so in one sentence, and records the measured values that its
verdict rests on. A run writes a page with the table of every case and its
result, the log of every command each case ran, and the same results as JSON.
The pages of the runs that a change was tested with are kept in
``docs/testing/``.

What it needs
=============

* The stick, booted from an image of this tree, with the bench's ssh key in
  ``/root/.ssh/authorized_keys`` (a local overlay of the bench, see
  ``docs/handbook/building.rst``).
* A bench host: a Linux machine that reaches the stick's management address and
  through which ssh reaches the stick (``ssh -J``). For the data path, the bench
  host is also both far ends: one port is the far end of the stick's host link
  (the subscriber side) and one port is behind the OLT (the network side). The
  data-path cases send their bursts from a raw socket, with ``python3`` and
  ``sudo`` without a password for it.
* For the router-mode cases: network namespaces and macvlan interfaces on the
  bench host, which gets two namespaces, ``devtest-lan`` and ``devtest-wan``,
  for as long as the cases run. For the PPPoE case also a PPPoE concentrator on
  the network-side port, as a systemd unit, and the file of its secrets, whose
  first entry the case gives the stick: the entry goes from that file to the
  stick's status daemon on the bench host and is never recorded.
* Python 3.11 or later and an ssh client on the machine that runs the tests;
  ``python3`` and ``curl`` on the bench host.

Running
=======

From this directory::

    python3 -m devtest --bench BENCH --stick ADDRESS \
        --olt-port PORT --subscriber-port PORT \
        --token-file FILE --out ../../docs/testing/NAME

``--list`` lists the cases; naming cases or areas (``gpon``, ``data.upstream``)
runs only those. The cases that reset the stick run only with
``--destructive``, and after all the others; the cases that put the stick in
router mode and back run only with ``--router`` (``--pppoe-secrets FILE`` and
``--bng-service UNIT``, ``accel-ppp`` by default, for the PPPoE case). Without ``--olt-port`` and ``--subscriber-port`` the
data-path cases are skipped; without ``--token-file`` so is the case that reads
values from the status daemon. The settings can also come from the environment:
``DEVTEST_BENCH``, ``DEVTEST_STICK``, ``DEVTEST_OLT_PORT``,
``DEVTEST_SUBSCRIBER_PORT`` and ``DEVTEST_TOKEN_FILE``.

What a run records leaves out what names one unit or one bench: MAC addresses,
GPON serial numbers, the token, and the names of the bench host and the stick
are replaced by ``<mac>``, ``<serial>``, ``<token>``, ``<bench>`` and
``<stick>``.

Layout
======

``devtest/access.py``
    The two remote ends, ssh to them, and the redaction of what is recorded.
``devtest/core.py``
    Cases, outcomes and the loop that runs them.
``devtest/cases_*.py``
    The cases, by area: the system, the host side and management, the PON side
    and the optics, the data path.
``devtest/report.py``
    The page, the log and the JSON of a run.
``devtest/tests/``
    Unit tests, which need no stick: from this directory,
    ``python3 -m unittest discover -s devtest/tests -t .``.

Finer tests live with their component: the OMCI daemon's in ``omcid/tests/``,
the other programs' in their ``tests/`` directories, and the kernel's
selftests under ``linux/tools/testing/selftests/``.
