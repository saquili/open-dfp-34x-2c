.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

======
dhcp6d
======

The IPv6 of a PPPoE session handed on to the host behind the stick, as the
IPv4 address of a session is. On the session's link dhcp6d is a DHCPv6
client (RFC 8415) that asks for a delegated prefix and the name servers
(RFC 3646); on the link to the host it is a DHCPv6 server that delegates that
prefix to the one host there, and a router that advertises itself (RFC 4861)
with the session's MTU and the name servers (RFC 8106)::

   dhcp6d -w WAN -l LAN [-m IFACE] [-o DIR] [-t TABLE] [-v]   # the stick
   dhcp6d -c IFACE [-m IFACE] [-v]                 # the client alone
   dhcp6d -s PREFIX/LEN -l LAN [-d DNS] [-v]       # the server alone

``-m`` names the interface whose MAC address makes the client's DUID (a PPP
link has none); ``-o`` the directory where dhcp6d writes the files
``prefix``, the delegated prefix, and ``host``, the host's link-local address
once it has taken it. The client alone prints the prefix and makes no route;
the server alone delegates a fixed prefix, as an operator would, for a bench.

The client's Solicit offers Rapid Commit; without it, the best Advertise of
the first wait is taken. The prefix is renewed with its server at T1, with
any at T2, and lost when its valid lifetime ends. The server answers one
host: a new one takes the binding over, as a new router behind the stick
would. It gives the prefix with what is left of its lifetimes and T1 and T2
of at most ten and sixteen minutes, so that a new prefix from the operator
reaches the host soon; the prefix of before goes with the new one at a
lifetime of 0. The advertisements carry no prefix, only the Other flag: the
host takes the delegated one by DHCPv6.

The routes dhcp6d makes are of protocol ``dhcp``: the default route through
the server that delegated the prefix, the prefix as unreachable, so that
what is not the host's goes nowhere rather than back out, and the prefix
through the host. With ``-t`` the default route goes in the routing table
TABLE, with a rule that routes by it what comes from the prefix: where the
stick hands the prefixes of two sessions to two hosts, each host's traffic
leaves by the session its prefix came from. SIGTERM, SIGINT and SIGHUP take
them away, advertise the stick with a router lifetime of 0, so that the host
drops it, and release the prefix.

Building and testing::

   make           # build/dhcp6d
   make check     # the program and the tests of the protocol

``tests/netns.sh`` runs an operator, the stick and a host in three network
namespaces and has the host reach the operator's network from the delegated
prefix; it needs root, a privileged container for one (see the script).
