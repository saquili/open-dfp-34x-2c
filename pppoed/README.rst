.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

======
pppoed
======

A PPPoE client: the discovery of RFC 2516, then LCP, PAP or CHAP-MD5, and
IPCP and IPv6CP side by side. The session runs in the kernel's PPPoE and PPP
drivers, which the switch can offload; pppoed opens it, negotiates it and
keeps it.

::

   pppoed up -i IFACE -f FILE [-D]    # FILE holds "user secret"
   pppoed up -i IFACE -f FILE -4      # without IPv6
   pppoed discover -i IFACE           # the discovery alone
   pppoed sniff -i IFACE              # print frames, send nothing

Building and testing::

   make           # build/pppoed
   make check     # the tests

``up`` prints a line for each family that came up::

   up ppp0 local 192.0.2.2 peer 192.0.2.1 session 17 dns 198.51.100.53 -
   up6 ppp0 local fe80::... peer fe80::... session 17

The session carries PPP packets of 1492 bytes, what an Ethernet frame leaves
after PPPoE's 8 bytes (RFC 2516), unless the interface has room for more: then
pppoed asks the concentrator for PPP-Max-Payload, what the interface's MTU
leaves after those 8 bytes, in its PADI and PADR (RFC 4638), and when the
concentrator answers with the tag in its PADS, the session carries up to the
smaller of the two. An interface of MTU 1508 thus gets a session of 1500-byte
packets from a concentrator that agrees, and 1492 from one that does not. LCP
asks for that size as the MRU, and the interface ``pppN`` takes the peer's MRU
within it as its MTU.

IPv4 takes the address and the name servers that the peer gives. IPv6CP
(RFC 5072) agrees on the two interface identifiers, offering a random one,
and puts the link-local address on the interface; the global address and the
delegated prefix come after, from the router advertisements and DHCPv6, which
are not pppoed's. Either family may be refused by the peer and the link comes
up with the other; once IPCP is open, IPv6CP has two restart periods, six
seconds, to open too.

The tests run the negotiation against a peer in a child process, at the
other end of a socket pair: both families, a peer that refuses our
identifier, a peer that offers none, a peer without IPv6, a client without
IPv6, and a peer without IPv4.
