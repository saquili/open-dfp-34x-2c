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

   dhcp6d -w WAN -l LAN [-m IFACE] [-o DIR] [-b FILE] [-t TABLE]
          [-x PROGRAM] [-n NAME] [-v]              # the stick
   dhcp6d -c IFACE [-m IFACE] [-v]                 # the client alone
   dhcp6d -s PREFIX/LEN -l LAN [-d DNS] [-v]       # the server alone

``-m`` names the interface whose MAC address makes the client's DUID (a PPP
link has none); ``-o`` the directory where dhcp6d writes the files
``prefix``, the delegated prefix, and ``host``, the host's link-local address
once it has taken it; ``-b`` the file that keeps the host's binding for the
next start. The client alone prints the prefix and makes no route;
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

The host's binding outlives dhcp6d in the file of ``-b``, one line: the
host's DUID in hex, its IAID, its address and the prefix it had. A router
that holds its delegation asks for none when the stick comes back: measured,
NetworkManager kept handing out its /56 for a day after the stick restarted
and asked for the name servers only, so the prefix had no route back. dhcp6d
takes the binding of before back at its start, routes the prefix to that host
as soon as it is delegated again, and knows the host's Renew; a prefix other
than the one of before goes to the host with the old one at a lifetime of 0.
The file goes when the host releases its binding or the binding runs out, not
when dhcp6d stops. A route to the host that failed is tried again at the
host's next message. ``-x PROGRAM`` tells a program when the binding changes:
dhcp6d runs it with the argument ``dhcp6d`` and does not wait for it. On the
stick the program is ``onu-apply``, which keeps the binding in the
configuration store.

``-n NAME`` publishes the state on ubus (ADR 0014): the object
``dhcp6d.NAME``, whose method ``status`` gives the prefix delegated, the host
it went on to, and whether that host is bound and the prefix routed to it,
and the event ``dhcp6d`` with the same each time it changes. ``onu-apply``
names each session's dhcp6d ``sesN``. dhcp6d connects to ubusd again when
ubusd comes later or starts again. ``build/dhcp6d-none``, which ``make
check`` builds on a host without libubus, publishes nothing.

Building and testing::

   make           # build/dhcp6d
   make check     # the tests, and build/dhcp6d-none

``tests/netns.sh`` runs an operator, the stick and a host in three network
namespaces and has the host reach the operator's network from the delegated
prefix; it needs root, a privileged container for one (see the script).
