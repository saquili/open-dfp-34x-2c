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
   pppoed up -i IFACE -f FILE -6      # without IPv4
   pppoed up -i IFACE -f FILE -D -P   # the IPv4 address for a host behind
   pppoed discover -i IFACE           # the discovery alone
   pppoed sniff -i IFACE              # print frames, send nothing

Building and testing::

   make           # build/pppoed, with libubus, libubox and libblobmsg_json
   make check     # the tests, with build/pppoed-none, which prints what it
                  # would tell netifd

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
within it as its MTU. With ``-M`` a concentrator that agreed to larger packets
in its PADS but asks in LCP for the MRU of plain PPPoE, 1492, is taken at the
discovery's word, and ``pppN`` sends packets as large as the discovery agreed
to: an operator's concentrator did both and carried 1500-byte packets each
way, IPv4 and IPv6, with the don't-fragment bit (measured). A smaller MRU is
the peer's own limit, and holds.

IPv4 takes the address and the name servers that the peer gives. With ``-P``
the address goes on no interface here: the ``up`` line names it, for a host
behind this one to take, and a default route that ``-D`` asks for goes
through the link all the same. IPv6CP (RFC
5072) agrees on the two interface identifiers, offering a random one, and puts
the link-local address on the interface, letting it carry IPv6 first: a system
that keeps IPv6 off the interfaces made after its start, as the stick does on
its PON side, keeps it off ``pppN`` too. The global address and the delegated
prefix come after, from the router advertisements and DHCPv6, which are not
pppoed's. Either family may be refused by the peer and the link comes up with
the other; once IPCP is open, IPv6CP has two restart periods, six seconds, to
open too. ``-4`` and ``-6`` leave a family out: pppoed does not ask for it and
refuses the peer's request with a Protocol-Reject, as for an operator that
gives IPv4 and IPv6 on two sessions of one line.

Under netifd
============

On the stick a session is an interface of netifd, of the protocol
``pppoed`` (ADR 0014), whose handler is pppoed itself, as
``/lib/netifd/proto/pppoed.sh``: netifd runs it as ``pppoed.sh '' dump`` for
what the protocol takes (the session, its family, whether its address is
handed on, whether the default route goes through it), and as ``pppoed.sh
pppoed setup|teardown IFACE CONFIG DEVICE`` for an interface. At setup it has
netifd run ``pppoed up`` for the session on the interface's device, with the
account of ``/run/onu-pppoeN``, so that netifd keeps it and starts it again
when it ends. netifd starts it through ``onu-drop``, as the account
``pppoed`` with the two powers it uses, ``net_raw`` for the discovery and
``net_admin`` for the PPP unit, its interface and its addresses; the file of
the account belongs to the group ``pppoed``, ``/dev/ppp`` too, and the
directory of the session's facts to the group ``onu``, and ubusd's list
(``acl.json``) lets it call ``notify_proto`` of netifd's interfaces and
nothing else; at teardown it has netifd end it, which pppoed does as a peer
expects. The messages go over ubus (``notify_proto``), as JSON text built in
``src/nproto.c``, which the tests check on any host.

``-N IFACE`` is what makes ``up`` netifd's: the link comes up with its MTU and
its IPv6 link-local address, and the IPv4 address (unless ``-P`` hands it on),
the default route that ``-D`` asks for and the name servers go to netifd, which
puts them in place and holds them; netifd takes away from an interface it
holds what it did not put there itself. ``-o DIR`` writes what the session
brought to the files of DIR while it lasts: ``ppp``, ``ipv4``, ``peer``,
``dns`` and ``ipv6``. ``-w`` waits before the discovery, so that sessions that
ended together start again apart; a session that does not come up waits five
seconds before it ends, so that netifd, which starts it again at once, does
not do so in a loop.

SIGTERM, SIGINT and SIGHUP end a session as a peer expects: an LCP
Terminate-Request, then a PADT, so that the concentrator frees the session
at once rather than after its echo timeout; a session that does not come up
after its discovery gets a PADT too.

The tests run the negotiation against a peer in a child process, at the
other end of a socket pair: both families, a peer that refuses our
identifier, a peer that offers none, a peer without IPv6, a client without
IPv6, a peer without IPv4, a client without IPv4, and a client told to stop.
