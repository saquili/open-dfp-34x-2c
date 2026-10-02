.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

======
pppoed
======

A PPPoE client: the discovery of RFC 2516, then LCP, PAP or CHAP-MD5 and
IPCP. The session runs in the kernel's PPPoE and PPP drivers, which the
switch can offload; pppoed opens it, negotiates it and keeps it.

::

   pppoed up -i IFACE -f FILE [-D]    # FILE holds "user secret"
   pppoed discover -i IFACE           # the discovery alone
   pppoed sniff -i IFACE              # print frames, send nothing

Building and testing::

   make           # build/pppoed
   make check     # the tests

IPv6CP is not done: the client rejects it, and the link carries IPv4 only.
