.. SPDX-License-Identifier: GPL-2.0-only

============================================
Passive optical network units: the PON class
============================================

Overview
========

An optical network unit (ONU) is the subscriber end of a passive optical
network (PON). The OLT at the operator's end grants the ONU its upstream time
slots and manages it through the ONU management and control interface (OMCI,
ITU-T G.988), and the subscriber's traffic travels in GEM ports (ITU-T
G.984.3). The PON class is the part of the kernel that gives the driver of an
ONU's MAC a place in the networking stack. It is software written for this
project, because no existing kernel framework covers the management channel
and the data path of an ONU. It holds no hardware facts of its own: what the
MAC does is on the pages of its driver (see "The hardware behind the class").

The class gives an ONU four things:

- A PON device, ``ponN``, that stands for one ONU MAC. The other three hang
  from it.
- An OMCI control port, the character device ``/dev/ponNomci0``. A userspace
  daemon reads the OMCI messages of the OLT from it and writes its answers to
  it; the class does not look inside a message.
- The ``gem`` rtnetlink link kind: one network interface for each GEM port.
  Management creates the interfaces (in practice the OMCI daemon does, from
  the OLT's GEM port network CTPs), and they are used like any Ethernet
  interface: they are bridge ports, routing interfaces and tc endpoints.
- The ``pon`` generic netlink family. It reports the ONU's activation state,
  the identifiers the OLT assigned to it, its alarms and counters, and its
  T-CONTs, and it announces changes.

A MAC driver uses the class through ``include/linux/pon.h``. It registers an
OMCI port and a data path against its own device, hands the class the OMCI
messages and the GEM frames it receives, and implements the operations the
class calls: create and delete a GEM port, transmit a frame, report status.
The class provides the rest: the devices, the queue in front of the daemon,
the network interfaces with their bridge offload, and the netlink family.

The class has no device tree node and no compatible string. It is created by
the MAC driver's own registrations, and the device tree describes only the
hardware. On the RTL9602C the device the class objects are registered against
is the switch node, compatible ``realtek,rtl9602c-switch`` (see "The RTL9602C
driver and the class").

Two names are easy to mix up on the RTL9602C. ``pon`` is the network interface
of the switch's PON port, a DSA user port, and the GEM interfaces stack on it.
``pon0`` is the PON device of the class, a device in sysfs that has no network
interface of its own, and ``pon0omci0`` is its OMCI port.

The parts, from user space down to the MAC driver::

             OMCI daemon             netlink clients         bridge, tc, routing
                  |                   |            |                  |
           /dev/pon0omci0       family "pon"  RTM_NEWLINK       gem0 gem1 ...
                  |                   |      (kind "gem")             |
                  |                   |            |                  |
           +------+-------------------+------------+------------------+-----+
           |                   PON class, the module pon                    |
           |             ponN: one PON device for each ONU MAC              |
           +----------+-----------------------------------------+-----------+
                      |                                         |
              pon_omci_port_ops                          pon_device_ops
                      |                                         |
           +----------+-----------------------------------------+-----------+
           |                     driver of the ONU MAC                      |
           +----------------------------------------------------------------+

Configuration
-------------

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Symbol
     - Effect
   * - ``CONFIG_PON``
     - Tristate, "Passive Optical Network (PON) ONU support". Builds
       ``drivers/net/pon/`` as ``pon.o``, linked from ``pon_core.o``,
       ``pon_gem.o``, ``pon_netlink.o`` and ``pon_nl.o``; as a module it is
       ``pon``. Drivers of ONU MACs select it, so it needs setting by hand
       only to build the class without one.
   * - ``CONFIG_NET_DSA_RTL9602C``
     - The RTL9602C switch driver, the only MAC driver in the tree. It selects
       ``PON``.
   * - ``CONFIG_PON_OMCI_INJECT``
     - The debugfs directory ``pon`` and the inject file of each OMCI port
       (see "The inject file"). It needs ``CONFIG_DEBUG_FS``, is off by
       default and off in the board configuration: it is for test images.
   * - ``CONFIG_NET_SWITCHDEV``
     - A frame received on an offloaded GEM interface carries
       ``offload_fwd_mark`` (see "Bridging and switchdev offload").

``rtl9602c_defconfig`` (``arch/mips/configs/``) builds the switch driver in
(``CONFIG_NET_DSA_RTL9602C=y``), which builds the class in, and sets
``CONFIG_DEBUG_FS=y``, ``CONFIG_BRIDGE=y`` and
``CONFIG_BRIDGE_VLAN_FILTERING=y``.

Where the code is
-----------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - File
     - Contents
   * - ``drivers/net/pon/pon_core.c``
     - The PON devices, their lifetime and lookups; the OMCI control port with
       its sysfs counters and its debugfs inject file; the start-up and exit
       of the module.
   * - ``drivers/net/pon/pon_core.h``
     - The internals the three parts share: ``struct pon_device`` and the
       lookups.
   * - ``drivers/net/pon/pon_gem.c``
     - The ``gem`` link kind: creation and deletion, transmit and receive,
       ethtool statistics, the netdev and switchdev notifiers, and the bridge
       offload.
   * - ``drivers/net/pon/pon_netlink.c``
     - The handlers of the ``pon`` family and ``pon_device_notify()``.
   * - ``drivers/net/pon/pon_nl.c``, ``pon_nl.h``
     - The policies, the operation table and the family, generated from the
       specification.
   * - ``drivers/net/pon/Kconfig``, ``Makefile``
     - The build entries.
   * - ``include/linux/pon.h``
     - The interface between the class and a MAC driver.
   * - ``include/uapi/linux/pon.h``
     - The uAPI of the family, generated from the specification.
   * - ``Documentation/netlink/specs/pon.yaml``
     - The specification of the family.
   * - ``include/uapi/linux/if_link.h``
     - ``enum ifla_gem_direction`` and the ``IFLA_GEM_*`` attributes.
   * - ``drivers/net/dsa/realtek/rtl9602c-gem.c``, ``rtl9602c-omci.c``,
       ``rtl9602c-gpon.c``
     - The calls the RTL9602C switch driver makes into the class: the data
       path, the OMCI port, and the status, notifications and OMCI channel
       state of the GPON MAC.
   * - ``drivers/net/dsa/realtek/rtl9602c-main.c``
     - ``rtl9602c_setup()`` creates the OMCI port and then registers the data
       path; ``rtl9602c_teardown()`` removes them in the opposite order.
   * - ``drivers/net/dsa/realtek/rtl9602c-offload.c``
     - Asks ``pon_gem_netdev_flow()`` whether a device is a GEM interface,
       when a flow offload names one.

Interfaces to user space
------------------------

- The character device ``/dev/pon0omci0``, the OMCI port (see "The OMCI control
  port"). Its major number is dynamic, and ``/proc/devices`` lists it under
  ``pon_omci``. Opening it needs ``CAP_NET_ADMIN``.
- The rtnetlink link kind ``gem`` and its network interfaces, called ``gemN``
  on this page (see "GEM interfaces"). Whoever creates an interface names it.
- The generic netlink family ``pon``, version 1, with the multicast group
  ``monitor`` (see "The pon generic netlink family"). The generic client of the
  kernel tree reads it::

    tools/net/ynl/pyynl/cli.py --spec Documentation/netlink/specs/pon.yaml \
        --dump dev-get

- sysfs: the class directory ``/sys/class/pon`` holds ``pon0`` and
  ``pon0omci0``, and the OMCI port has the read-only counters ``rx_messages``,
  ``rx_dropped``, ``rx_discarded``, ``tx_messages`` and ``tx_errors`` in its
  ``statistics`` directory (see "Counters of the port").
- debugfs, with ``CONFIG_PON_OMCI_INJECT``:
  ``/sys/kernel/debug/pon/pon0omci0/inject``, which puts a message into the
  port as if the MAC had received it (see "The inject file").
- ``ethtool -S gemN``: the MAC's counters for a GEM port, when the MAC keeps
  them (see "Statistics of a GEM interface").

The hardware behind the class
-----------------------------

The class is independent of the hardware. On the only board of this tree, the
ODI DFP-34X-2C3 with a Realtek RTL9602C, the hardware behind it is described
on other pages:

- The GPON MAC and the PON MAC, which run the activation, the Alloc-IDs, the
  GEM flows, the T-CONTs and the queues:
  Documentation/networking/device_drivers/pon/rtl9602c.rst.
- The CPU port and its DMA engine, the conduit through which the OMCI messages
  and the GEM frames reach the switch:
  Documentation/networking/device_drivers/ethernet/realtek/rtl9602c.rst.
- The switch and its DSA driver, which registers the class objects:
  Documentation/networking/dsa/rtl9602c.rst.
- The SoC and the board, including the device tree of the switch node:
  Documentation/arch/mips/rtl9602c.rst.

How to read the fact tables
===========================

Each block below has a short account of what the class does and why, followed
by fact tables with three columns: *Fact*, *Tag* and *Checked by*. The PON
class is software written for this project, so most facts are design decisions
and interface contracts, each with the reason it was made. The tag says how a
fact is established. **M**, measured on the board (the DFP-34X-2C3): *Checked
by* gives the command or observation, through the class's own interfaces
(``ip``, ``ethtool``, netlink, sysfs, debugfs, the kernel log), that shows it.
**T**, tested by function: the driver or the daemon depends on the fact, and
the named behaviour fails if it is wrong. **P**, public standard: *Checked by*
names the document and the clause. **I**, inferred, not verified. On this page
**I** covers two cases, and *Checked by* says in words which one a row is. A
*design choice* is a decision of the class that no check exercises; the row
says what would exercise it. An *assumption* is a belief about something the
class does not control, such as the OLT or the MAC, that no check settles; the
row says what would settle it. Where the driver depends on an **I** fact, the
row says "load-bearing". The last chapter collects the **I** facts.

A row with two tags is established in two ways, or in two parts; *Checked by*
says which part carries which tag. The behaviours that a tested fact depends on
are named in words:

- the board boots to a shell with the switch driver probed;
- bridged traffic passes both ways, sourced from the host;
- the downstream locks, and the ONU reaches O5 and holds it on an OLT;
- OMCI messages pass both ways over ``/dev/pon0omci0``: the daemon completes a
  MIB upload and the OLT keeps the ONU in O5;
- the *recorded exchange* is a recorded OMCI provisioning exchange of 459
  downstream messages, written message by message into the inject file (see
  "The inject file") while the ONU is in O5 on an OLT; the daemon answers
  through the real MAC;
- the *routed-traffic checks* are cases run with traffic on the board; a row
  that cites one says what it shows.

Module and start-up
===================

The class is one module, ``pon``. It has to exist before an ONU MAC driver
sets itself up, because a MAC driver creates its OMCI port and its data path
then, so the module starts at ``subsys_initcall``, ahead of the device drivers.
The three surfaces are one module because they share the device model: the
port, the link kind and the family all find the ONU through the same PON
device.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``CONFIG_PON`` is a tristate that ONU MAC drivers select; the RTL9602C
       switch driver does (``NET_DSA_RTL9602C``). As a module it is
       ``pon.ko``, built from ``pon_core.o``, ``pon_gem.o``,
       ``pon_netlink.o`` and ``pon_nl.o``. It is one module because the three
       surfaces share the device model.
     - T
     - The board boots to a shell with the switch driver probed against the
       core.
   * - The module provides three surfaces: the OMCI control port (a character
       device for each MAC), the ``gem`` rtnetlink link kind and the ``pon``
       generic netlink family. The help text of ``CONFIG_PON`` names all
       three.
     - T, M
     - T: OMCI both ways over ``/dev/pon0omci0`` for the port, and the
       recorded exchange for the link kind. M: a ``dev-get`` dump on the
       board returns the ONU.
   * - Start-up runs at ``subsys_initcall``, before device drivers probe,
       because MAC drivers create their ports and data paths when they set
       themselves up (see "Start-up, in order").
     - T
     - The board boots to a shell, and OMCI passes both ways over
       ``/dev/pon0omci0``.
   * - Exit runs the start-up in reverse: the ``gem`` link ops and both
       notifiers, the debugfs directory, the family, the character region and
       the class. The two allocators of numbers, for the ``N`` of ``ponN`` and
       for the minors of the ports, are destroyed last.
     - I
     - Design choice, not exercised: the module is built in, and unloading is
       never run. ``rmmod pon`` on a modular build would exercise it.
   * - ``MODULE_ALIAS_RTNL_LINK("gem")``: creating a link of kind ``gem``
       loads a modular core. The family has no alias, so a request to the
       ``pon`` family alone does not load it.
     - I
     - Design choice, not exercised: a modular build with the core not yet
       loaded, where ``ip link add ... type gem`` loads it and a ``dev-get``
       alone does not.

Start-up, in order
------------------

1. Register the class ``pon``. Purpose: PON devices and OMCI ports are devices
   of this class. (T)
2. With ``CONFIG_PON_OMCI_INJECT``, create the debugfs directory ``pon``.
   Purpose: each OMCI port gets its inject directory under it. (T)
3. Take a dynamic character major with 256 minors, named ``pon_omci``.
   Purpose: the OMCI ports are the character devices of this region, so the
   class has at most 256 ports. (T)
4. Register the generic netlink family ``pon``. Purpose: an ONU can be read as
   soon as a MAC registers it. (T)
5. Register a netdev notifier and a switchdev blocking notifier, then the
   ``gem`` link ops. Purpose: GEM interfaces follow their lower interface and
   receive the bridge's VLAN objects, and management can create them. (T)

A failure unwinds the steps before it, except the debugfs directory, which
stays. (T)

PON devices
===========

A PON device stands for one ONU MAC. It is a device of the class ``pon``
(device type ``pon``), named ``ponN`` with N the lowest number free when it is
created, so that the first MAC registered is ``pon0``. Its parent is the MAC
driver's own device. It has no device node. Its OMCI port has one, and the port
is its only child device.

A MAC registers two things against its own device, in either order: its OMCI
port (``pon_omci_port_create()``) and its data path (``pon_device_register()``).
The first registration creates ``ponN`` and the second finds it, so that both
share one device. The data path holds a reference on the PON device but is not
a child of it. When a registration is undone, the class drops its reference
and unregisters ``ponN`` if no child device is left.

The class finds a PON device in three ways: by network device, by number, and
as the one with the lowest number at or above a cursor. The first asks
``owns_netdev`` of each registered data path. The last is for dumps: the class
keeps its devices in the order they registered, and a dump needs them in the
order of their numbers.

On a board with the switch driver probed, the class directory lists the device
and its port::

    # ls /sys/class/pon
    pon0  pon0omci0

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A PON device stands for one ONU MAC: class ``pon``, device type
       ``pon``, name ``ponN``. N is the lowest free number, so the first MAC
       registered is ``pon0``. Its parent is the MAC driver's own device. It
       has no device node; only its OMCI port has one.
     - T
     - The board boots to a shell: ``/sys/class/pon/pon0`` exists, and the
       family reports number 0 (M).
   * - The MAC's first registration against a parent device creates the PON
       device. That is the OMCI port or the data path, whichever comes first.
       The second registration finds the same device, so both share one
       ``ponN``.
     - T
     - OMCI both ways over ``/dev/pon0omci0``: ``pon0omci0`` sits under
       ``pon0``, and ``pon0`` reports the ``pon`` interface's index (M).
   * - The PON device counts the MAC's two registrations, its OMCI port and its
       data path; each holds a reference to the device, and a put unregisters
       the device when neither is left. So the two may go in either order:
       the device stays registered, and found by the lookups, until the last
       of them goes.
     - I
     - Design choice, not exercised: the RTL9602C driver removes its data path
       first; removing the OMCI port first would exercise the other order.
   * - There are three lookups: by network device, by number, and "lowest
       number at or above a cursor". The lookup by network device asks the
       ``owns_netdev`` of each registered data path, under the registration
       lock. The cursor lookup exists for dumps, because the class keeps
       devices in registration order and a dump needs a stable order of
       numbers. Each lookup returns a reference the caller puts.
     - T
     - The family's dump returns the ONU (M). Every GEM interface finds its
       ONU through the ``pon`` interface (the recorded exchange).
   * - One mutex, the registration lock, serialises the creation and removal
       of PON devices and OMCI ports, the publication of a MAC's data-path
       operations, and every ``dev_status`` call. Where RTNL is held as well,
       RTNL is taken first. So ``owns_netdev`` and ``dev_status`` must not
       take RTNL.
     - I
     - Design choice, load-bearing: a MAC whose ``dev_status`` took RTNL would
       deadlock against the creation of GEM interfaces. A kernel with
       lockdep, a ``dev-get`` dump running while the daemon creates GEM
       interfaces, would show it.

The OMCI control port
=====================

The OMCI control port is the character device ``/dev/ponNomci0``. Through it a
daemon in user space runs the ONU's side of OMCI (ITU-T G.988): a read returns
one message from the OLT, a write sends one message to it, and the class does
not look inside either. The daemon builds every message whole, including the
message integrity check in its last four bytes.

The MAC driver creates the port with ``pon_omci_port_create()`` and gives it
three operations (``struct pon_omci_port_ops``): ``tx``, which is mandatory and
sends a message towards the OLT, and the optional ``start`` and ``stop``. It
hands each message it receives from the OLT to ``pon_omci_port_rx()``, and it
tells the class whether the OMCI channel exists with
``pon_omci_port_link_set()``. The channel, the OMCC of ITU-T G.984.3, exists
once the OLT has configured a Port-ID for it; the port starts without it. The
messages from the OLT wait in a queue of 64 for the reader.

Opening and closing
-------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The port is the character device ``/dev/ponNomci0``: class ``pon``,
       device type ``pon_omci``, a child of ``ponN``. It is modelled on the
       WWAN control ports, without what OMCI does not need (one port type, no
       terminal handling, no splitting of writes). The OMCI protocol itself
       lives in a userspace daemon. It is a control port and not a network
       interface because OMCI is not Ethernet, and a network interface would
       need an Ethernet encapsulation invented for it.
     - T
     - OMCI both ways over ``/dev/pon0omci0``.
   * - Open needs ``CAP_NET_ADMIN``, otherwise it fails with ``-EPERM``:
       whoever holds the port answers the OLT for the ONU and can reconfigure
       its data path through OMCI.
     - I
     - Design choice, not exercised: opening the port without the capability.
   * - The port takes one opener at a time; a second open fails with
       ``-EBUSY``. OMCI is request and response between one OLT and one
       management entity, and two readers would each see half the
       conversation.
     - I
     - Design choice, not exercised: a second open while the daemon runs.
   * - Open fails with ``-ENODEV`` once the MAC has removed the port.
       Otherwise it calls the MAC's ``start`` operation, if there is one, and
       empties the receive queue. The file is a stream: it has no position and
       cannot seek.
     - T
     - OMCI both ways over ``/dev/pon0omci0``: the daemon opens the port when
       it starts.

Reading and writing
-------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A read returns exactly one message. A buffer smaller than the message
       fails with ``-EMSGSIZE`` and leaves the message queued, so half a
       message is never delivered. A failed copy also leaves it queued, with
       ``-EFAULT``.
     - T, I
     - T: OMCI both ways over ``/dev/pon0omci0``, one message per read. I: the
       ``-EMSGSIZE`` branch is a design choice not exercised, because the
       daemon reads with a 1980-byte buffer; a short read would exercise it.
   * - A read on an empty queue fails with ``-EAGAIN`` under ``O_NONBLOCK``.
       Otherwise it waits interruptibly (``-ERESTARTSYS`` on a signal) for a
       message or for the removal of the port. Once the port has been removed,
       a read fails with ``-ENODEV``. A queue that the loss of the OMCI
       channel empties after the reader woke is an empty queue again: the
       read waits once more, or fails with ``-EAGAIN``.
     - T
     - OMCI both ways over ``/dev/pon0omci0``: the daemon reads non-blocking
       and waits in poll.
   * - A write sends exactly one message of 1 to 1980 bytes: 0 bytes fails with
       ``-EINVAL`` and more than 1980 with ``-EMSGSIZE``. The class does not
       look inside the message. The daemon builds all of it, including the
       integrity check in its last four bytes.
     - T
     - OMCI both ways over ``/dev/pon0omci0``: the OLT accepted every answer.
   * - A write is never split, because the largest message (1980 bytes) fits
       one GEM frame, whose 12-bit length field allows up to 4095 bytes of
       payload.
     - P
     - ITU-T G.984.3 clause 8.3.1.
   * - 1980 bytes is the largest OMCI message: the extended format of G.988,
       with ten octets of header, up to 1966 octets of contents and a
       four-octet message integrity check. The baseline format is 48 bytes.
       Both formats end in a four-byte integrity check, which in G.984 systems
       is a CRC.
     - P
     - ITU-T G.988 clause 11.1; clause 11.2, Tables 11.2-1 and 11.2-2.
   * - A write the MAC refuses returns the MAC's error and counts in
       ``tx_errors``. An accepted write counts in ``tx_messages`` and returns
       the byte count.
     - I
     - Design choice, not exercised: reading the counters (see "Counters of
       the port") around a refused write.

The OMCI channel and poll
-------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Without the OMCI channel, a write fails with ``-ENOLINK`` and poll
       reports no ``EPOLLOUT``. There is nobody to send to: OMCI travels only
       on the Port-ID the OLT has configured for it, and the OLT retransmits
       whatever it still wants answered.
     - I
     - Design choice, load-bearing: the daemon treats ``-ENOLINK`` as a
       dropped answer, and any write error other than ``-ENOLINK``,
       ``-ENETDOWN`` and ``-EAGAIN`` as the end of the channel. Writing to the
       port before the OLT has configured the OMCC exercises it. The reason is
       ITU-T G.984.3 clause 14.1.
   * - Poll reports ``EPOLLIN | EPOLLRDNORM`` while a message is queued, and
       ``EPOLLOUT | EPOLLWRNORM`` while the channel exists. Once the MAC has
       removed the port it reports ``EPOLLHUP | EPOLLERR``. ``EPOLLOUT`` does
       not promise room in the MAC: a write can still fail with ``-EAGAIN``.
     - T, I
     - T: OMCI both ways over ``/dev/pon0omci0``; the daemon waits for
       ``EPOLLIN``. I: ``EPOLLHUP`` is a design choice not exercised.
   * - A message that arrives while nobody holds the port open is dropped and
       counted in ``rx_dropped``, and open empties the queue. A new opener
       must not be handed requests the OLT has long since given up on.
     - I
     - Design choice, not exercised: an OLT message with the port closed, after
       which ``rx_dropped`` rises.
   * - At most 64 messages wait for a slow reader; further ones are dropped
       and counted. An OLT waits for each answer before it sends its next
       request, so a handful in flight is normal and 64 is generous.
     - I
     - Assumption about the OLT, not verified: a reader that stops reading
       during an OLT exchange would fill the queue.
   * - When the channel goes down, what is still queued is discarded and
       counted in ``rx_discarded``, apart from the messages a missing or slow
       reader loses. Those requests belonged to an association that ended:
       Deactivate_ONU-ID discards the OMCI Port-ID.
     - M
     - ITU-T G.984.3 clause 9.2.1. An OLT that deactivated the ONU 18 s into
       its first O5 session took one queued message with the channel, then
       counted with the reader's losses (docs/testing/2026-10-04-run13).

Counters of the port
--------------------

The port keeps five counters, read-only in the ``statistics`` group of its
sysfs device::

    # cat /sys/class/pon/pon0omci0/statistics/rx_messages

.. list-table::
   :header-rows: 1
   :widths: 24 76

   * - File
     - Counts
   * - ``rx_messages``
     - Messages queued for the reader.
   * - ``rx_dropped``
     - Messages dropped because the port was closed or the reader was 64
       messages behind.
   * - ``rx_discarded``
     - Messages still waiting for the reader when the OMCI channel went down.
   * - ``tx_messages``
     - Messages the MAC accepted.
   * - ``tx_errors``
     - Messages the MAC refused.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The port has the read-only sysfs statistics ``rx_messages`` (queued for
       the reader), ``rx_dropped`` (port closed or reader too slow),
       ``rx_discarded`` (channel gone with messages queued), ``tx_messages``
       (accepted by the MAC) and ``tx_errors`` (refused by the MAC), in its
       ``statistics`` group.
     - T
     - The device tests read them after the OMCI exchanges of a boot
       (``omci.channel``).

The inject file
---------------

With ``CONFIG_PON_OMCI_INJECT`` every OMCI port has a write-only file in
debugfs, in a directory named after the port. A write holds one OMCI message as
pairs of hexadecimal digits::

    # echo "<hex digit pairs>" > /sys/kernel/debug/pon/pon0omci0/inject

A recorded provisioning exchange can be written through it message by message,
and the daemon answers through the real MAC. A process that can write the file
can make the daemon act as if the OLT had asked for anything, so the option
that gives the file is off by default and in the board configuration.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - With ``CONFIG_PON_OMCI_INJECT``, each port has
       ``/sys/kernel/debug/pon/ponNomci0/inject``, mode 0200. Writing to it
       puts one message into the port as if the MAC had delivered it: the same
       queue, the same counters, the same wake-up. The daemon's answers still
       leave through the real MAC, so it tests the stack above the port and not
       a simulator. It exists because an OLT that provisions nothing leaves
       most of the daemon's provisioning paths unreachable.
     - M
     - The recorded exchange: 459 messages injected, 542 handled by the daemon,
       none rejected, the ONU held in O5, and six GEM interfaces created.
   * - Format: pairs of hexadecimal digits, with whitespace between pairs
       ignored. A write is 2 to 4024 characters and yields 1 to 1980 message
       bytes. An odd digit, a character that is not hexadecimal, or no bytes at
       all fails with ``-EINVAL``, and nothing is injected. A successful write
       returns its whole count.
     - T
     - The recorded exchange: one write per message.
   * - An injected message takes the way of a delivered one, so when it is
       dropped for want of a reader it counts in ``rx_dropped``: an injection
       that went nowhere is as visible as an OLT message that went nowhere.
       Injection bypasses whatever the MAC gates on (the activation state, the
       OMCC), so the daemon's answers can still fail with ``-ENOLINK``.
     - I
     - Design choice, not exercised: injecting with the port closed, and
       injecting before the OMCC exists.

GEM interfaces
==============

A GEM port is a network interface of the rtnetlink link kind ``gem``. It stacks
on the interface that carries the ONU's traffic, as a VLAN interface stacks on
its lower device: the ``IFLA_LINK`` of the request names that interface, and
the MAC's ``owns_netdev`` says whether it is the right one. On the RTL9602C it
is ``pon``. A GEM port carries traffic and is a bridge and tc endpoint, so it
is a network interface; a T-CONT only schedules upstream traffic, so it is
not. Management creates the interfaces, in practice the OMCI daemon from the
OLT's GEM port network CTPs. In ``ip`` syntax, for an ``ip`` that knows the
kind, a request looks like this, with N, P and T the interface number, the
Port-ID and the T-CONT index::

    ip link add gemN link pon type gem port-id P tcont T

The MAC creates the port behind the interface in ``gem_add``, which chooses the
*flow*, the MAC's own number for the GEM port. The interface sends a frame by
calling ``gem_xmit`` with the flow in a ``METADATA_HW_PORT_MUX`` dst, and the
MAC delivers a received frame with ``pon_gem_rx()`` by flow. When the MAC can
forward a GEM port in hardware, a GEM interface that joins a bridge is
offloaded to the switch through switchdev.

Attributes
----------

.. list-table::
   :header-rows: 1
   :widths: 28 6 8 58

   * - Attribute
     - No.
     - Type
     - Meaning
   * - ``IFLA_GEM_PORT_ID``
     - 1
     - u16
     - The Port-ID in the headers of the port's GEM frames, 0 to 4095.
       Required.
   * - ``IFLA_GEM_DIRECTION``
     - 2
     - u8
     - ``enum ifla_gem_direction``: ``IFLA_GEM_DIR_UPSTREAM`` (1),
       ``IFLA_GEM_DIR_DOWNSTREAM`` (2) or ``IFLA_GEM_DIR_BOTH`` (3). Both ways
       if absent.
   * - ``IFLA_GEM_TCONT``
     - 3
     - u32
     - The index of the T-CONT that schedules the port's upstream traffic.
       Required when the direction includes upstream.
   * - ``IFLA_GEM_MULTICAST``
     - 4
     - flag
     - A multicast GEM port. Allowed only with ``IFLA_GEM_DIR_DOWNSTREAM``.
   * - ``IFLA_GEM_QUEUE``
     - 5
     - u8
     - The port's upstream queue in its T-CONT, 0 the highest priority and
       below the ``num-tcont-queues`` the family reports. 0 if absent.
   * - ``IFLA_GEM_WEIGHT``
     - 6
     - u8
     - 0, the default, for a strict-priority queue; 1 to 255, the queue's
       weight among the T-CONT's weighted queues.
   * - ``IFLA_GEM_CIR``
     - 7
     - uint
     - The committed rate of the port's upstream traffic, in bytes per second.
       0, the default, for none.
   * - ``IFLA_GEM_PIR``
     - 8
     - uint
     - The peak rate of the port's upstream traffic, in bytes per second. 0,
       the default, for no limit but the T-CONT's grants.
   * - ``IFLA_GEM_PBITS``
     - 9
     - u8
     - The IEEE 802.1p priorities of the upstream frames the port takes from
       its VLANs, bit p for priority p, at least one. All eight if absent.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A GEM port is a network interface of the rtnetlink link kind ``gem``,
       stacked like a VLAN on the interface that carries the ONU's traffic
       (``IFLA_LINK``). A GEM port carries traffic and is a bridge and tc
       endpoint, so it is a network interface. A T-CONT only schedules, so it
       is not. Management creates GEM interfaces: the OMCI daemon, from the
       OLT's GEM port network CTPs. For example: ``ip link add gemN link pon
       type gem port-id P tcont T``.
     - T
     - The recorded exchange. A routed-traffic check: the daemon creates a GEM
       interface once the ONU is in O5.
   * - The link attributes are ``IFLA_GEM_PORT_ID`` (u16, 0 to 4095,
       required), ``IFLA_GEM_DIRECTION`` (u8, 1 to 3, default 3),
       ``IFLA_GEM_TCONT`` (u32, required when the direction includes upstream)
       and ``IFLA_GEM_MULTICAST`` (flag, allowed only with direction 2).
     - T
     - The recorded exchange: the daemon sends all four.
   * - The Port-ID range is 0 to 4095 because the Port-ID field of the GEM
       header has 12 bits.
     - P
     - ITU-T G.984.3 clause 8.3.1.
   * - Direction 1 is upstream (UNI to ANI), 2 is downstream (ANI to UNI) and 3
       is both. This is the encoding of the direction attribute of the GEM port
       network CTP: bit 0 towards the OLT, bit 1 from it.
     - P
     - ITU-T G.988 clause 9.2.3.
   * - A multicast GEM port carries downstream traffic only, because a
       multicast GEM interworking point reconstructs a stream from GEM packets.
       A multicast port with any other direction is refused.
     - P
     - ITU-T G.988 clause 9.2.5.
   * - ``IFLA_GEM_TCONT`` is a T-CONT index, starting at 0 and below the
       ``num-tconts`` the family reports. The class passes it through, and the
       MAC refuses an index it does not have. Several GEM ports may share one
       T-CONT.
     - M
     - The recorded exchange: two GEM interfaces on T-CONT 2. Sharing is
       allowed by ITU-T G.984.3 clause 8.3.1.
   * - The attribute numbers are ABI: ``IFLA_GEM_PORT_ID`` 1,
       ``IFLA_GEM_DIRECTION`` 2, ``IFLA_GEM_TCONT`` 3, ``IFLA_GEM_MULTICAST``
       4, ``IFLA_GEM_QUEUE`` 5, ``IFLA_GEM_WEIGHT`` 6, ``IFLA_GEM_CIR`` 7,
       ``IFLA_GEM_PIR`` 8 and ``IFLA_GEM_PBITS`` 9. ``enum
       ifla_gem_direction`` is ``IFLA_GEM_DIR_UPSTREAM`` 1,
       ``IFLA_GEM_DIR_DOWNSTREAM`` 2 and ``IFLA_GEM_DIR_BOTH`` 3.
     - T, I
     - T for 1 to 4: the recorded exchange, in which the daemon builds its
       requests with these numbers. I for 5 to 9: not yet sent by the daemon.

Upstream traffic
----------------

A port that carries upstream traffic takes frames from the VLANs it carries
in its bridge, and is scheduled in its T-CONT. ``IFLA_GEM_PBITS`` says which
priorities of its VLANs' frames it takes, so that the GEM ports of one VLAN can
divide them, as the GEM ports behind an IEEE 802.1p mapper do (ITU-T G.988
clause 9.3.10). The port sends through one of the T-CONT's queues, and the
queues share the OLT's grants to the T-CONT: ``IFLA_GEM_QUEUE`` names the
queue, ``IFLA_GEM_WEIGHT`` makes it a strict or a weighted one, and
``IFLA_GEM_CIR`` and ``IFLA_GEM_PIR`` give the port's traffic a committed and a
peak rate. In ``ip`` syntax, for an ``ip`` that knows the kind::

    ip link set gemN type gem pbits 0x30 queue 3 weight 0 cir 1250000 pir 12500000

The model follows the ONU's upstream traffic management of ITU-T G.988: a
priority queue per T-CONT and priority (clause 9.2.10), the T-CONT's policy,
strict priority or weighted round robin (clause 9.2.2), and the rates of a
traffic descriptor (clause 9.2.12).

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The upstream GEM ports of one bridge that carry one VLAN take disjoint
       priorities of its frames; a GEM port that would take a priority another
       one of the VLAN holds is refused by the MAC, in ``gem_vlan_add`` or
       ``gem_change``. A frame of a priority no GEM port of its VLAN takes
       goes up none, as a priority that a mapper points nowhere is discarded.
       ``IFLA_GEM_PBITS`` is at least 1 (``-ERANGE`` otherwise), so that each
       upstream port takes some priority of each of its VLANs.
     - P, T
     - P for the mapper, ITU-T G.988 clause 9.3.10. T on the RTL9602C: a
       priority mask of 0 was refused, "integer out of range", and a port
       asking for a priority another port of its VLAN held was refused.
   * - A T-CONT serves the committed rates of its queues first. What remains
       of its grant goes to its strict queues in order of priority, 0 the
       highest, and what they leave to its weighted queues in proportion to
       their weights. A queue sends nothing above its peak rate. A MAC that
       cannot schedule this way refuses the attributes it cannot honour in
       ``gem_add`` and ``gem_change``.
     - I
     - The class's model; whether a MAC follows it is on the MAC's page. ITU-T
       G.988 clauses 9.2.2 and 9.2.10 give strict priority and weighted round
       robin as a T-CONT's policies.
   * - The queue is a T-CONT index's queue, 0 to ``num-tcont-queues`` less
       one; the class refuses a higher one with ``-ERANGE``. GEM ports of one
       T-CONT that name the same queue share it, and the rates of the ports
       that share a queue add up.
     - T
     - On the RTL9602C, with 8 queues per T-CONT: queue 8 was refused, "No
       queue 8: a T-CONT has queues 0 to 7"; two loaded ports in queue 0
       shared it, 504 and 461 Mbit/s.
   * - The weight is one byte, as a priority queue's weight (ITU-T G.988
       clause 9.2.10); 0 stands for strict priority.
     - P
     - ITU-T G.988 clause 9.2.10.
   * - The rates are in bytes per second, as a traffic descriptor's committed
       and peak information rates (ITU-T G.988 clause 9.2.12). Both are
       ``NLA_UINT``: four bytes or eight. 0 means no committed rate and no
       peak rate, as a traffic descriptor's peak rate of 0 leaves the ONU's own
       policy. A committed rate above a peak rate that is not 0 is refused
       with ``-EINVAL``.
     - P, T
     - P for the unit and the 0, ITU-T G.988 clause 9.2.12. T for the refusal:
       a committed rate of 2000 with a peak rate of 1000.
   * - A port that carries no upstream traffic may not be given any of the
       five upstream attributes (``-EINVAL``), and reports none; an upstream
       port reports all five, its defaults included.
     - T, I
     - T for the refusal: a downstream-only port with queue 1. I for the
       reports, not read back.
   * - The five may change on an existing link, through ``changelink``, which
       hands the MAC the whole new configuration in ``gem_change``. A change
       that leaves them as they are calls nothing. Without ``gem_change`` a
       change is refused with ``-EOPNOTSUPP``, and so is a change, in the same
       request, of the Port-ID, the direction, the T-CONT or the multicast
       flag, which only deleting and creating the port again can make.
     - T, I
     - T on the RTL9602C: changes moved two loaded ports between queues and
       gave them weights, a peak rate and a committed rate, each followed by
       the traffic (the driver's page); a change to the queue a port already
       had succeeded. I for the refusals of a change, not exercised.

Creating and deleting
---------------------

Creation is an ``RTM_NEWLINK`` and deletion an ``RTM_DELLINK``, both with RTNL
held (see "Creating a GEM interface" and "Deleting a GEM interface" under
"Sequences"). A GEM interface is also deleted when its lower interface is
unregistered. The link kind's ``changelink`` changes the upstream attributes
only (see "Upstream traffic"): a GEM port whose Port-ID, direction or T-CONT
changes is deleted and created again.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``changelink`` changes no Port-ID, direction or T-CONT. A Port-ID or
       T-CONT changed under traffic would move a subscriber's frames to
       another service with nothing torn down, so such a port is deleted and
       created again.
       G.988 sets the CTP's Port-ID, T-CONT pointer and direction at creation
       but also lets the OLT write them later (clause 9.2.3), so the daemon
       turns every such write into a re-creation.
     - I
     - Design choice, not exercised: an OLT changing a provisioned GEM port,
       where the daemon deletes the interface and creates it again.
   * - Creation is refused, each time with an extack message, for: no
       ``IFLA_LINK`` (``-EINVAL``); an unknown link (``-ENODEV``); no Port-ID
       (``-EINVAL``); multicast without downstream only (``-EINVAL``);
       upstream without a T-CONT (``-EINVAL``); a link that carries no ONU's
       traffic (``-EOPNOTSUPP``); upstream attributes that the class refuses
       (see "Upstream traffic"); and whatever the MAC's ``gem_add``
       refuses.
       A flow that the MAC returns at or above ``num_flows`` (with a warning),
       or a flow that already has a GEM interface, gives ``-EEXIST``, and the
       MAC's port is removed again.
     - I
     - Design choice, not exercised: ``ip link add ... type gem`` with each
       fault.

The interface
-------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A GEM interface takes the lower interface's MAC address, which is the
       ONU's, and its MTU, both at creation. It does not follow later MTU
       changes of the lower. Its largest MTU is the lower's largest, and a
       change of its own MTU above the lower's MTU of the moment is refused
       with ``-ERANGE`` (``pon_gem_change_mtu()``), as for a VLAN device: the
       GEM port's frames cross the lower.
     - T, M
     - T: a routed-traffic check, in which frames addressed to the ONU's MAC
       reach the GEM interface. M: with ``pon`` at 1500, an MTU of 1508 on
       ``gem256`` was refused; ``pon`` raised to 1508 left ``gem256`` at 1500,
       which then took 1508.
   * - Setup: an Ethernet interface with no transmit queue (``IFF_NO_QUEUE``),
       lockless transmit, and per-CPU software counters; it is freed on
       unregistration. The dst and the ONU reference are released by a
       destructor that is installed only after registration succeeds.
       ``register_netdevice()`` runs a destructor on some of its failures and
       not on others, so until then the creation path releases them itself.
     - I
     - Design choice, not exercised: fault injection in
       ``register_netdevice()``.
   * - After registration the GEM interface is linked as an upper of its lower.
       If the link fails, the interface is unregistered and the MAC's port is
       removed.
     - I
     - Design choice, not exercised: as for the previous row.

Transmit and receive
--------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Transmit on a GEM interface without the upstream direction drops the
       frame and counts ``tx_dropped``. Otherwise the frame's dst is replaced
       by the interface's ``METADATA_HW_PORT_MUX`` dst (``port_id`` the flow,
       ``lower_dev`` the lower) and the frame goes to the MAC's ``gem_xmit``.
       It does not go through the lower's transmit, because the lower is a
       switch port and a frame sent there loses the dst before the MAC sees
       it.
     - T
     - Bridged traffic passes both ways, sourced from the host (upstream). A
       routed-traffic check: a routed burst leaves by the GEM interface.
   * - The transmit result is stored in a variable and evaluated once.
       ``net_xmit_eval()`` evaluates its argument twice: given the transmit
       call itself, it would send every frame twice and free the skb twice.
       ``NET_XMIT_SUCCESS`` and ``NET_XMIT_CN`` count as sent; anything else
       counts ``tx_dropped``. The interface always returns ``NETDEV_TX_OK``.
     - M
     - 40 of 40 upstream frames arrive, none twice. With the call inside the
       macro, every GEM frame left twice and the slab was corrupted.
   * - Receive looks up the flow's interface under RCU. A frame for a GEM
       interface that is down counts ``rx_dropped`` and is freed. Otherwise
       the frame's device and protocol are set, the software counters are
       updated, and the frame is passed to ``netif_receive_skb()``.
     - M
     - 80 of 80 downstream frames are delivered to a downstream-only multicast
       GEM interface. A routed-traffic check: downstream frames reach the GEM
       interface.
   * - With ``CONFIG_NET_SWITCHDEV``, a frame received on an offloaded GEM
       interface carries ``offload_fwd_mark``: the switch has already
       forwarded it where the bridge wanted it, and the bridge must not
       forward it again.
     - I
     - Assumption, load-bearing for bridged traffic without duplicates: that
       the switch has forwarded every such frame, so that none arrives twice,
       is not verified. A broadcast sent from the OLT side while a GEM
       interface is bridged, counted once at the subscriber side, would settle
       it.

State
-----

A GEM interface takes its operational state from its lower interface. The
RTL9602C driver keeps ``pon`` dormant outside O5, so the GEM interfaces are
dormant too until the ONU reaches O5; that they then show dormant and up is not
verified (see the second row below).

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Opening a GEM interface fails with ``-ENETDOWN`` while the lower is
       administratively down. So a GEM interface created with ``IFF_UP`` (as
       the daemon creates them) while the lower is down fails, and the kernel
       deletes it again.
     - I
     - Design choice, load-bearing for the order of provisioning: ``ip link
       set pon down``, then a creation with ``up``.
   * - The operational state follows the lower's: at creation, at open, and on
       the lower's CHANGE, UP and DOWN events. When the lower is unregistered,
       its GEM interfaces are deleted. On the RTL9602C, ``pon`` is dormant
       until O5 and up in O5, so its GEM interfaces are too.
     - I
     - Assumption, load-bearing for anything that waits for a GEM interface to
       be up: ``ip link show gemN`` before and after O5 would settle it.
   * - ``iflink`` is the lower's index, and the link's network namespace is the
       lower's.
     - I
     - Design choice, not exercised: ``ip -d link show gemN``.

Statistics of a GEM interface
-----------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The interface statistics (``ndo_get_stats64``) are the software
       counters: the frames this CPU sent and received. For a bridged GEM
       interface that is a handful of its traffic. They stay software on
       purpose: hardware numbers in the same structure would leave the drop
       fields beside them counting different traffic.
     - I
     - Design choice: ``ip -s link show gemN`` during a bridged burst shows a
       handful of frames.
   * - The MAC's own per-port counters are ethtool statistics under their own
       names, present only when the MAC implements ``gem_stats``; otherwise the
       set is empty. ``pon_tx_octets``: the octets of the GEM frames the port
       sent upstream, their headers included. ``pon_tx_frames``: the GEM frames
       it sent, a fragment counting as one. ``pon_rx_frames``: the GEM frames
       it received, a fragment counting as one. ``pon_rx_octets``: the payload
       octets of the GEM frames it received, their headers left out. These are
       the counts a GEM port network CTP's performance monitoring asks for.
     - I
     - Design choice; what the MAC counts is on the MAC's page.

tc and flowtable offload
------------------------

A GEM interface passes exactly one kind of tc setup to its lower interface:
flowtable offload, ``TC_SETUP_FT``. On the RTL9602C the lower is a switch port,
and the switch driver builds the hardware flows.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Only ``TC_SETUP_FT`` (flowtable offload) is passed to the lower's
       ``ndo_setup_tc``. Every other type, and a lower without
       ``ndo_setup_tc``, gets ``-EOPNOTSUPP``, which leaves blocks and qdiscs
       on the GEM interface in software.
     - T
     - A routed-traffic check: a flowtable naming ``sfp`` and the GEM
       interface loads with ``flags offload``, and conntrack reports
       ``HW_OFFLOAD``.
   * - ``TC_SETUP_FT`` is passed down because an offloaded flowtable needs
       every device in its set to accept it, and a flowtable naming a GEM
       interface that answered nothing was refused whole ("Operation not
       supported"). And the WAN device has to be the GEM interface. A
       flowtable naming ``pon`` is created but never offloads, because a frame
       the CPU sends on ``pon`` has no GEM flow: ARP for the gateway never
       resolves, no flow rule can be built, and the MAC driver sees only
       ``FLOW_CLS_DESTROY``.
     - M
     - ``nft`` with ``devices = { sfp, gemN }`` and ``flags offload``, with the
       GEM interface passing nothing down: refused. The same with ``pon`` in
       place of ``gemN``: loaded, no flow offloaded.
   * - Other types are not passed down because a tc block belongs to one
       interface. Passed down, a GEM interface's block was bound to the switch
       port's single ingress block. With ``pon`` holding a ``clsact``, ``tc
       qdisc add dev gemN clsact`` failed ("Driver ndo_setup_tc failed"). With
       ``pon``'s deleted, it succeeded, and then ``pon`` could not get its own
       back.
     - M
     - The two ``tc qdisc add ... clsact`` commands, in both orders, with
       blocks passed down.

Bridging and switchdev offload
------------------------------

A GEM interface is a bridge port like any other, and when the MAC can forward
between its GEM port and the rest of the bridge in hardware, the class offloads
it. The MAC's part is the optional pair ``gem_bridge_join`` and
``gem_bridge_leave``, the optional pair ``gem_vlan_add`` and ``gem_vlan_del``
for the bridge VLANs, and the optional ``gem_mrouter`` for the multicast routers
the bridge sees (see "The driver interface"). The class's part is the switchdev
offload of the port and the routing of the bridge's VLAN objects and of its
router state, described in the rows below and in "Joining a bridge" under
"Sequences".

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Joining a bridge. When a GEM interface is linked to a bridge master and
       the MAC has ``gem_bridge_join``, the class calls it. A refusal fails the
       join with the MAC's extack message. The class then offloads the port to
       switchdev with the lower as the hardware device, which puts the GEM
       interface in the switch's own hardware domain: the bridge forwards in
       software only what the hardware did not. If the switchdev offload
       fails, the class calls ``gem_bridge_leave`` and fails the join. Without
       ``gem_bridge_join``, or when the upper is not a bridge, the class does
       nothing and the stack bridges in software.
     - T, M
     - T: bridged traffic passes both ways, sourced from the host. M:
       downstream 1,271,133 frames at 1 Gb/s line rate with 65 reaching the
       CPU; upstream 1,030,770 of 1,600,050 offered (about 885 Mb/s against a
       grant of about 990 Mb/s) with 59 on the CPU; the ONU held O5 throughout.
   * - Leaving a bridge reverses the join. The switchdev offload is undone on
       ``NETDEV_PRECHANGEUPPER``, while the interface is still a bridge port;
       ``gem_bridge_leave`` follows on ``NETDEV_CHANGEUPPER``.
     - I
     - Design choice, not exercised: ``ip link set gemN nomaster`` during a
       bridged burst, after which the traffic falls back to the CPU and
       nothing is left programmed.
   * - The bridge VLAN objects of an offloaded GEM interface go to
       ``gem_vlan_add`` and ``gem_vlan_del``. Other objects, and any object for
       a GEM interface that is not offloaded, get ``-EOPNOTSUPP``, which keeps
       them in software. The VLANs have to reach the hardware: even a bridge
       without VLAN filtering gives each port its PVID, and a switch that
       learns per VLAN files every learned address under it. Without the
       VLANs, addresses learned on the PON side were filed under a different
       VLAN from the one subscriber lookups used, and every frame towards the
       network side was flooded, to the CPU too.
     - M
     - Of 150,000 upstream frames, 75 reached the CPU with the VLANs passed
       down; without them, all did.
   * - A VLAN is taken out of the MAC only when no other GEM interface of the
       same bridge still has it, because the switch has one PON port for all
       GEM ports.
     - I
     - Design choice, load-bearing for a bridge with several GEM interfaces:
       removing a VLAN from one of two GEM interfaces that share it must leave
       the traffic on the other going.
   * - The bridge's view of an offloaded GEM interface as a multicast router
       port (``SWITCHDEV_ATTR_ID_PORT_MROUTER``), which it takes from the
       queries it snoops and from the port's ``multicast_router`` setting, goes
       to ``gem_mrouter``. Other attributes, and every attribute of a GEM
       interface that is not offloaded, get ``-EOPNOTSUPP`` and stay in
       software. A MAC that forwards multicast in hardware needs the router
       state to send the multicast that no host asked for only where the bridge
       would: in a PON the router sits behind a GEM port.
     - M
     - On the RTL9602C, an IGMP general query from the network side made the
       multicast GEM interface a router port, and the PON port then dropped
       unregistered IPv4 multicast; turning snooping off made the bridge drop
       the router port and the PON port flood again (see the switch's
       *bridging* page, "Multicast snooping").
   * - The groups that the host itself joined, which the bridge hands to each
       of its ports as ``SWITCHDEV_OBJ_ID_HOST_MDB``, stop at an offloaded GEM
       interface: the class's switchdev notifier runs ahead of those of the
       default priority and ends the call chain for them. The switch's own
       ports in the bridge carry these groups already. A DSA switch cannot see
       that a GEM interface is offloaded through one of its ports, and counted
       each GEM interface's copy on its CPU port as a foreign interface's; the
       bridge replays nothing to a port that joins, so a group that the host
       had joined before the GEM interfaces were bridged was removed once for
       each GEM interface more than it was added, and every extra removal
       logged ``Failed to del Host Multicast Database entry`` with error -2.
     - M
     - On the RTL9602C, with two host groups joined before the two GEM
       interfaces were bridged with ``sfp``: before, four such lines in the
       kernel log 284 s after boot, when the groups expired; after, the groups
       were gone by 332 s and the log had none.

The pon generic netlink family
==============================

The ``pon`` family, version 1, reports what a network interface has no field
for: the activation of the ONU, the identifiers and allocations the OLT gave
it, its alarms and its counters. The traffic stays on the GEM interfaces, and
the OMCI messages on the OMCI port; neither passes through the family. The
family is specified in YAML (``Documentation/netlink/specs/pon.yaml``) and
generated with the kernel's YNL generator, which produces the policies, the
operation table and the uAPI header ``include/uapi/linux/pon.h``.

The objects are registered by the MAC driver, as in the kernel's DPLL family:
user space reads them and is told of changes, but can neither create nor change
any, because the kernel runs the transmission convergence layer and user space
only observes it. There is no set operation. The family has two objects: a
*dev* object for each ONU, and a *tcont* object for each of its T-CONTs. The
kernel tree's generic client reads them::

    tools/net/ynl/pyynl/cli.py --spec Documentation/netlink/specs/pon.yaml \
        --do dev-get --json '{"id": 0}'
    tools/net/ynl/pyynl/cli.py --spec Documentation/netlink/specs/pon.yaml \
        --dump dev-get
    tools/net/ynl/pyynl/cli.py --spec Documentation/netlink/specs/pon.yaml \
        --do tcont-get --json '{"id": 0, "index": 0}'
    tools/net/ynl/pyynl/cli.py --spec Documentation/netlink/specs/pon.yaml \
        --subscribe monitor --sleep 60

Operations
----------

.. list-table::
   :header-rows: 1
   :widths: 20 8 24 26 22

   * - Operation
     - No.
     - Request
     - Reply
     - Needs
   * - ``dev-get``, do
     - 1
     - ``id``
     - The attributes of the dev object.
     - ``CAP_NET_ADMIN``
   * - ``dev-get``, dump
     - 1
     - Nothing.
     - One message for each ONU.
     - ``CAP_NET_ADMIN``
   * - ``dev-change-ntf``
     - 2
     - Not a request: a notification.
     - The attributes of ``dev-get``.
     - Membership of the multicast group ``monitor``, which needs
       ``CAP_NET_ADMIN``.
   * - ``tcont-get``, do
     - 3
     - ``id``, ``index``
     - The attributes of the tcont object.
     - ``CAP_NET_ADMIN``
   * - ``tcont-get``, dump
     - 3
     - Nothing.
     - One message for each T-CONT of each ONU.
     - ``CAP_NET_ADMIN``
   * - ``tcont-set``, do
     - 4
     - ``id``, ``index``, and ``alloc-id`` to map, none to unmap
     - Nothing but the acknowledgment.
     - ``CAP_NET_ADMIN``

Attributes of the dev object
----------------------------

.. list-table::
   :header-rows: 1
   :widths: 22 6 22 50

   * - Attribute
     - No.
     - Type
     - Sent
   * - ``id``
     - 1
     - u32
     - Always. The N of ``ponN``.
   * - ``ifindex``
     - 2
     - u32
     - When known. The interface that carries the ONU's traffic, on which the
       GEM interfaces are created.
   * - ``type``
     - 3
     - u32, ``type``
     - When known. The kind of PON.
   * - ``onu-state``
     - 4
     - u32, ``onu-state``
     - When known. The activation state, 1 to 7 for O1 to O7.
   * - ``onu-id``
     - 5
     - u32
     - While the ONU has one. 0 to 253.
   * - ``alloc-id``
     - 6
     - u32, repeated
     - Once for each reported allocation, at most 32.
   * - ``omcc-port-id``
     - 7
     - u32
     - Once the OLT has configured the OMCI channel.
   * - ``alarms``
     - 8
     - u32, flags of ``alarm``
     - When the MAC can report alarms.
   * - ``stats``
     - 9
     - nest, ``dev-stats``
     - When the MAC keeps counters.
   * - ``serial-number``
     - 10
     - binary, exactly 8 bytes
     - While the MAC has one.
   * - ``num-gem-ports``
     - 11
     - u32
     - Always. The size of the MAC's flow space.
   * - ``num-tconts``
     - 12
     - u32
     - Always. The T-CONTs the ONU can be given for subscriber traffic.
   * - ``num-tcont-queues``
     - 13
     - u32
     - Always. The upstream queues of each of those T-CONTs.
   * - ``power-level``
     - 14
     - u32, ``power-level``
     - While the MAC transmits at a level the OLT can change: 0 low,
       1 medium, 2 high, as Serial_Number_ONU carries it.

The ``dev-stats`` nest holds fourteen counters numbered from 1 in this order:
``sn-requests``, ``ranging-requests``, ``ploam-rx``, ``ploam-crc-errors``,
``ploam-overflows``, ``bwmap-rx``, ``bwmap-crc-errors``, ``bwmap-overflows``,
``gem-frames``, ``bip-errors``, ``fec-corrected-bytes``,
``fec-corrected-codewords``, ``fec-uncorrectable-codewords`` and
``fec-codewords``. The tcont object has ``id`` (1), the ONU's
number, ``index`` (2), the T-CONT's index, and ``alloc-id`` (3), the Alloc-ID
bound to it. The C names are ``PON_A_DEV_*``, ``PON_A_DEV_STATS_*`` and
``PON_A_TCONT_*`` in ``include/uapi/linux/pon.h``; the commands are
``PON_CMD_DEV_GET`` (1), ``PON_CMD_DEV_CHANGE_NTF`` (2) and
``PON_CMD_TCONT_GET`` (3), and the group is ``PON_MCGRP_MONITOR``.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The ``dev`` attributes are ``id`` (the N of ``ponN``), ``ifindex`` (the
       interface the GEM ports stack on), ``type``, ``onu-state``, ``onu-id``,
       ``alloc-id`` (repeated), ``omcc-port-id``, ``alarms``, ``stats`` (a
       nest), ``serial-number`` (binary, exactly 8 bytes), ``num-gem-ports``,
       ``num-tconts``, ``num-tcont-queues`` and ``power-level``. Their
       numbers, the values of ``power-level``, and the command numbers
       (``dev-get`` 1, ``dev-change-ntf`` 2, ``tcont-get`` 3, ``tcont-set``
       4), are ABI.
     - M
     - The daemon reads the serial number and the alarms by these numbers and
       announces the serial number the driver holds.
   * - The ``stats`` counters are ``sn-requests``, ``ranging-requests``,
       ``ploam-rx``, ``ploam-crc-errors``, ``ploam-overflows``, ``bwmap-rx``,
       ``bwmap-crc-errors``, ``bwmap-overflows``, ``gem-frames``,
       ``bip-errors``, ``fec-corrected-bytes``, ``fec-corrected-codewords``,
       ``fec-uncorrectable-codewords`` and ``fec-codewords``, numbered from 1
       in that order (ABI).
     - I
     - Design choice; no check on the board reads the nest. A ``dev-get`` with
       ``stats`` decoded against the header would.

The family and its permissions
------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The family is ``pon``, version 1. It is specified in YAML and generated
       with the kernel's YNL generator, which produces the policies, the
       operation table and the uAPI header. It carries what a network interface
       has no field for: activation, ranging, identifiers and allocations. The
       traffic stays on GEM interfaces.
     - M
     - The family read on the board with the fibre connected to an OLT
       (``dev-get``; see "Status, alarms and notifications").
   * - The objects are registered by the MAC driver, as in the kernel's DPLL
       family: user space reads them and is told of changes, but cannot create
       any. The kernel runs the convergence layer, so the one set operation,
       ``tcont-set``, writes what management owns and the convergence layer
       does not: which T-CONT an Alloc-ID serves, a mapping the OLT makes
       through the OMCC (ITU-T G.984.3 clause 5.5.4).
     - I
     - Design choice; the operation table has no ``set``.
   * - The operations are ``dev-get`` (do needs ``id``; dump returns every
       ONU), ``tcont-get`` (do needs ``id`` and ``index``; dump returns every
       T-CONT of every ONU), ``tcont-set`` (do only, with ``id``, ``index``
       and an optional ``alloc-id``), and the notification ``dev-change-ntf``
       on the multicast group ``monitor``.
     - M, I
     - M: a ``dev-get`` dump and ``dev-change-ntf`` on the board. I:
       ``tcont-get`` is a design choice not exercised; a ``tcont-get`` dump
       would exercise it.
   * - ``dev-get`` and ``tcont-get``, the do and the dump of each, need
       ``CAP_NET_ADMIN`` (``admin-perm``), and so does joining ``monitor``
       (``GENL_MCAST_CAP_NET_ADMIN``, which the generator writes for a group
       with ``admin-perm``): its notifications carry the serial number, the
       credential the OLT takes the ONU by.
     - I
     - Design choice, not exercised: an unprivileged ``dev-get`` gets
       ``-EPERM``, and an unprivileged join of ``monitor`` is refused.
   * - ``netnsok``: the family answers in every network namespace. ONUs are not
       per namespace, and notifications go to the initial namespace only.
     - I
     - Design choice, not exercised: a request and a listener in another
       namespace.
   * - ``parallel_ops``: requests run without the generic netlink mutex; each
       one holds a reference on the device it reports.
     - I
     - Design choice, not exercised: concurrent dumps during registration.

Reports
-------

A report starts from "nothing known" and takes what the MAC's ``dev_status``
gives. What the MAC does not set is left out of the message and not sent as 0,
because 0 is a valid ONU-ID, Port-ID and Alloc-ID, and because "no alarms" is a
claim a MAC in reset cannot make. The counters are the exception: inside the
``stats`` nest every counter is sent.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``dev-get`` do fails with ``-EINVAL`` without ``id``, and with
       ``-ENODEV`` (extack "no ONU with that id") for an unknown one.
     - I
     - Design choice, not exercised: a request for number 1 on the one-ONU
       board.
   * - A report starts from "nothing known": ONU-ID and OMCC Port-ID -1,
       everything else 0. It then asks the MAC's ``dev_status``. A MAC without
       ``dev_status``, or one whose call fails, gives a report with only the
       number and the two capacities, because a half-filled status would mix
       measurements and zeros.
     - M
     - While the PON MAC was down, the report carried neither state nor
       alarms.
   * - Absent is not zero. An attribute the MAC did not set is left out:
       ``ifindex``, ``type`` and ``onu-state`` when 0; ``onu-id`` and
       ``omcc-port-id`` when negative; ``alarms`` without ``have_alarms``;
       ``stats`` without ``have_stats``; ``serial-number`` without
       ``have_serial``. ONU-ID 0, Port-ID 0 and Alloc-ID 0 are valid values,
       and "no alarms" is a claim a MAC in reset cannot make.
     - M
     - Notifications watched across ``ip link set pon down`` and ``up`` (see
       "Status, alarms and notifications"). The validity of 0 is in ITU-T
       G.984.3 clauses 9.1.1, 8.3.1 and 8.1.3.6.1.
   * - Inside ``stats`` every counter is sent, zeros included: a zero is a
       measurement, while a MAC that counts none of it leaves the whole nest
       out. The counters are ``uint``, which is 4 bytes while the value fits in
       32 bits and 8 after, so a counter can pass 2^32 without an ABI change
       and a reader must accept both widths.
     - I
     - Design choice, not exercised: a counter passing 2^32.
   * - ``num-gem-ports``, ``num-tconts`` and ``num-tcont-queues`` are always
       sent. They say what the hardware can hold, taken from the registration
       (``num_flows``, ``num_tconts``, ``num_queues``); a MAC that keeps flows
       for itself offers fewer GEM interfaces than ``num-gem-ports`` (see
       "Known limitations"). A daemon can then refuse an OLT's request for a
       GEM port network CTP (managed entity 268) or a T-CONT (managed entity
       262) beyond the hardware before half a service is built, and create a
       priority queue (managed entity 277) for each queue of each T-CONT. All
       are 0 before a data path is registered. After unregistration
       ``num-gem-ports`` returns to 0 but the other two keep their last
       values.
     - I
     - Design choice, not exercised: the daemon reads ``num-tconts`` when it
       starts and builds its T-CONT entities from it; a changed count would
       show in its MIB upload.
   * - ``power-level`` is sent while the MAC transmits at one of the levels
       the OLT sets with Change_Power_Level, coded as the power level of
       Serial_Number_ONU: 2 high, 1 medium, 0 low (ITU-T G.984.3
       clause 9.2.4.1). A daemon judges the transmitted power against limits
       that follow the level, as the transmit power alarms of the ANI-G
       (managed entity 263) need; each level is about 3 dB from the next.
     - I
     - Design choice. The 3 dB is the step this driver measures (see the
       driver's page).

Dumps
-----

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A ``dev-get`` dump keeps the number of the next ONU to report in
       ``cb->args[0]``. A dump resumed after a full message never repeats an
       ONU, and an ONU registered during the dump appears if its number is
       still ahead. The cursor moves past an ONU once its message is in the
       part being filled, so an ONU whose message does not fit is the first of
       the next part.
     - I
     - Design choice, not exercised: a dump across a registration.
   * - ``tcont-get`` do fails with ``-EINVAL`` without ``id`` or ``index``,
       with ``-ENODEV`` for an unknown ONU, and with ``-ENOENT`` when the ONU
       has no data path, the MAC has no ``tcont_get`` or the index is not
       below ``num_tconts``. ``alloc-id`` is absent while the T-CONT is idle.
       The MAC is asked under the registration lock, which keeps its data
       path registered for the call.
     - I
     - Design choice, not exercised: a ``tcont-get`` for index 16 on the
       RTL9602C.
   * - A ``tcont-get`` dump keeps a two-number cursor in ``cb->args``: the
       ONU's number and the T-CONT's index. An ONU without a data path, or
       whose MAC has no ``tcont_get``, is skipped.
     - I
     - Design choice, not exercised: a ``tcont-get`` dump.
   * - ``tcont-set`` maps T-CONT ``index`` to ``alloc-id``, 0 to 4095 (the
       policy refuses more with ``-ERANGE``), or unmaps it without
       ``alloc-id``. It fails with ``-EINVAL`` without ``id`` or ``index``,
       with ``-ENODEV`` for an unknown ONU or one without a data path, with
       ``-EOPNOTSUPP`` when the MAC has no ``tcont_set``, with ``-ENOENT``
       when the index is not below ``num_tconts``, and with ``-EBUSY`` when
       another T-CONT is mapped to the Alloc-ID, each with an extack message.
       The MAC is called under the registration lock.
     - T
     - On the RTL9602C: Alloc-ID 5000, "integer out of range"; T-CONT 8, "no
       such T-CONT on this ONU"; an Alloc-ID mapped to T-CONT 0, "Alloc-ID 256
       is mapped to T-CONT 0"; the default Alloc-ID, refused by the driver
       (its page).
   * - An Alloc-ID serves at most one user traffic T-CONT; the ONU rejects a
       second mapping. Mapping a T-CONT to an Alloc-ID makes it carry user
       traffic once the OLT has also assigned the Alloc-ID with
       Assign_Alloc-ID. G.988 lets the default Alloc-ID serve a user traffic
       T-CONT too.
     - P
     - ITU-T G.984.3 clause 5.5.4; ITU-T G.988 clause 9.2.2.

Notifications
-------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``pon_device_notify()`` sends nothing when nobody listens on
       ``monitor``. Otherwise it sends one ``dev-change-ntf`` message carrying
       the attributes of a get.
     - M
     - A listener on ``monitor`` on the board (see "Status, alarms and
       notifications").

ONU states, identifiers and counters
====================================

This chapter says what the values of the family's attributes mean. The
definitions are those of the ITU-T recommendations of the transmission
convergence layer; what the MAC of a board counts, and how, is its own
definition and is on the page of its driver.

The type of PON
---------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``type`` (enum, from 1) is ``gpon`` (G.984: 2.48832 Gbit/s downstream,
       1.24416 or 2.48832 Gbit/s upstream), ``xgpon`` (G.987: 9.95328 Gbit/s
       downstream, 2.48832 Gbit/s upstream), ``xgspon`` (G.9807.1: 9.95328
       Gbit/s both ways) or ``ngpon2`` (G.989: several TWDM wavelength
       channels). Only ``gpon`` is reported by a driver today.
     - P
     - ITU-T G.984.3 Summary; G.987.1; G.9807.1 Summary; the G.989 series.

Activation states
-----------------

``onu-state`` carries the activation state of the ONU. Its values are those of
``enum pon_onu_state`` in ``include/uapi/linux/pon.h``, named after the seven
states of ITU-T G.984.3, and the corresponding states of the other
transmission convergence layers. The RTL9602C driver's activation state
machine implements O1 to O5; its page is
Documentation/networking/device_drivers/pon/rtl9602c.rst.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``onu-state`` (enum, 1 to 7 for O1 to O7) holds the seven ONU
       activation states of G.984.3, and the corresponding states of the other
       transmission convergence layers.
     - P
     - ITU-T G.984.3 clause 10.2.1.
   * - The states:

       - O1, initial: LOS or LOF is asserted.
       - O2, standby: the downstream is synchronised, and the ONU waits for
         Upstream_Overhead.
       - O3, serial number: the ONU answers serial number requests and waits
         for Assign_ONU-ID.
       - O4, ranging: the ONU has its ONU-ID, answers ranging requests and
         waits for its equalisation delay (Ranging_Time).
       - O5, operation: the ONU sends data and PLOAM messages as the OLT
         directs.
       - O6, POPUP: entered from O5 on LOS or LOF, with upstream transmission
         stopped. A directed POPUP message returns the ONU to O5, a broadcast
         one to O4, and expiry of timer TO2 to O1.
       - O7, emergency stop: after Disable_Serial_Number the laser is off and
         the ONU sends nothing upstream.
     - P
     - ITU-T G.984.3 clauses 10.2.2 and 10.2.4 (Table 10-1).
   * - An ONU transmits upstream only in O3 (serial number responses), O4
       (ranging responses) and O5, and carries traffic only in O5.
     - P
     - ITU-T G.984.3 clause 10.2.2.

Alarms
------

``alarms`` is a set of flags, ``enum pon_alarm``: ``PON_ALARM_LOS`` (1),
``PON_ALARM_LOF`` (2), ``PON_ALARM_LOM`` (4) and ``PON_ALARM_PEE`` (8). The
flags say what is wrong now, not what happened.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``alarm`` flags ``los`` (1) and ``lof`` (2): loss of signal is no valid
       downstream signal; loss of frame is five consecutive invalid PSync
       fields. These are conditions present now, not events seen, and a MAC
       reports the bits it has.
     - P
     - ITU-T G.984.3 clause 11.1.2.
   * - ``alarm`` flag ``lom`` (4), loss of PLOAM, is a condition some MACs
       report: frames are synchronised but the PLOAM channel is not
       delineated. G.984.3 defines no ONU-side alarm by that name; loss of
       PLOAM appears there only as an OLT-side item.
     - I
     - Assumption about the MAC, not verified: the flag means what the MAC's
       own definition says (see
       Documentation/networking/device_drivers/pon/rtl9602c.rst). The ONU-side
       and OLT-side lists are in ITU-T G.984.3 clauses 11.1.1 and 11.1.2.
   * - ``alarm`` flag ``pee`` (8), physical equipment error: the OLT has said
       with Physical_Equipment_Error, about once a second while it lasts, that
       it cannot send GEM frames or OMCC frames. Each message raises the flag,
       and it clears when none has come for 3 s.
     - P
     - ITU-T G.984.3 clause 9.2.3.15 and clause 11.1.2.

Identifiers
-----------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``onu-id`` is assigned by Assign_ONU-ID in O3, which moves the ONU to
       O4. It takes a value from 0 to 253 (0xFF addresses every ONU, and in
       Serial_Number_ONU it means "no ONU-ID yet"). Deactivate_ONU-ID discards
       it.
     - P
     - ITU-T G.984.3 clause 9.2.4.1; also clauses 9.1.1, 9.2.1 and 10.2.4.
   * - Every Alloc-ID is 12 bits. An ONU's default Alloc-ID equals its ONU-ID
       and carries PLOAM and OMCI; further Alloc-IDs come from 256 to 4095. 254
       is the activation Alloc-ID and 255 means unassigned. Assign_Alloc-ID
       gives an ONU an Alloc-ID.
     - P
     - ITU-T G.984.3 clauses 8.1.3.6.1 and 9.2.1.
   * - ``alloc-id`` repeats once for each reported allocation, up to 32 in a
       report (``PON_MAX_ALLOC_IDS``); a MAC with more reports the first 32.
       No ONU is given anything like that many.
     - I
     - Assumption about the OLT, not verified: a MAC with more than 32
       allocations would exercise it.
   * - ``omcc-port-id`` is the Port-ID of the OMCI channel, once the OLT has
       configured it with Configure_Port-ID. Its messages reach the OMCI port,
       and it is never a ``gem`` interface.
     - P
     - ITU-T G.984.3 clauses 9.2.3.14 and 14.1.
   * - ``serial-number`` is 8 bytes (``PON_SERIAL_NUMBER_LEN``): 4 bytes of
       Vendor_ID in ASCII, then 4 bytes of vendor-specific serial number, as
       the ONU sends them in Serial_Number_ONU.
     - P
     - ITU-T G.984.3 clause 9.2.4.1.

Counters
--------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``stats`` holds totals since the MAC was brought up, and not register
       values. A MAC's counters may clear when read, and a register passed
       through would give each reader a number that depends on who read last.
     - I
     - Design choice. The RTL9602C's counters are measured to clear on read
       (see Documentation/networking/device_drivers/pon/rtl9602c.rst).
   * - What the counted events are. A serial number request is an allocation to
       Alloc-ID 254 with the PLOAMu flag. A ranging request is an allocation to
       the ONU's own ID with the PLOAMu flag, and in O5 it cannot be told from
       a regular PLOAM request. A downstream PLOAM message with a bad CRC is
       discarded. BIP is the bit-interleaved parity of the downstream frame.
     - P
     - ITU-T G.984.3 clause 10.2.4 (Table 10-1), clause 9.1.4, clause 8.1.3.4.
   * - ``bip-errors`` counts the bits that differed, which is what G.984.3
       accumulates as ERR for the bit error rate, and what REI reports.
     - P
     - ITU-T G.984.3 clause 11.2.2.
   * - The four ``fec-*`` counters are the downstream FEC statistics G.984.3
       asks of a receiver: the bytes and the codewords FEC corrected, the
       codewords it could not correct, and all the codewords it decoded. The
       fifth statistic there, the seconds with an uncorrectable codeword, is
       not a counter of the MAC: a reader that samples the uncorrectable count
       each second derives it.
     - P
     - ITU-T G.984.3 clause 13.1.3.
   * - The FEC counters stay at 0 while the downstream carries no FEC: the ONU
       decodes nothing until the downstream frames say they carry it.
     - P
     - ITU-T G.984.3 clause 13.2.3.2.
   * - What a MAC counts is otherwise its own definition. ``bwmap-crc-errors``
       may count whole maps, or allocation structures, each of which carries
       its own CRC. ``gem-frames`` means GEM frames that carried something, not
       idle frames. ``fec-codewords`` may be counted by the MAC, or derived by
       its driver from the frames received while the downstream carried FEC.
     - I
     - Assumption about the MAC, not verified: the RTL9602C's definitions are
       on the page of its driver. The allocation CRC is ITU-T G.984.3 clause
       8.1.3.6.5.

T-CONTs
-------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The ``tcont`` object is one transmission container, the unit the OLT
       grants upstream bandwidth to: every entry of the bandwidth map is an
       allocation to one T-CONT. Its attributes are ``id`` (the ONU's),
       ``index`` (starting at 0, below ``num-tconts``) and ``alloc-id``
       (absent while idle). G.988 models the same thing as the T-CONT managed
       entity. An ONU that cannot say which T-CONTs hold an Alloc-ID cannot tell
       an OLT that a T-CONT pointer names nothing.
     - P
     - ITU-T G.984.3 clause 8.1.3.6; ITU-T G.988 clause 9.2.2.

The driver interface
====================

A driver of an ONU MAC uses the class through ``include/linux/pon.h``. It
registers with the class in two independent steps, both against the same
``struct device``, its own:

1. ``pon_omci_port_create(parent, ops, drvdata)`` creates the OMCI control
   port, with the operations of ``struct pon_omci_port_ops``.
2. ``pon_device_register(parent, ops, num_flows, num_tconts, num_queues,
   drvdata)``
   registers the data path, with the operations of ``struct pon_device_ops``.
   From then on management can create GEM interfaces on the network interface
   that ``owns_netdev`` claims.

The two calls return the object or an ``ERR_PTR()``. Either step may come
first, and a MAC that has no OMCI channel, or no data path, does only the
other. The MAC undoes the steps with ``pon_device_unregister()`` for the data
path and ``pon_omci_port_remove()`` for the port, in either order. While registered, the MAC calls into the class from its receive
path (``pon_gem_rx()`` for a frame of a GEM port and ``pon_omci_port_rx()`` for
an OMCI message), tells it whether the OMCI channel exists
(``pon_omci_port_link_set()``) and reports a change of the ONU's status
(``pon_device_notify()``). The class calls the MAC through the two tables of
operations. The ``drvdata`` pointer of each registration comes back through
``pon_device_get_drvdata()`` and ``pon_omci_port_get_drvdata()``.

A *flow* is the MAC's own number for a GEM port: ``gem_add`` chooses it, the MAC
tags the GEM port's frames with it, and the class indexes the GEM interfaces by
it. A *T-CONT index* counts the ONU's T-CONTs from 0 (see "T-CONTs").

The full signatures and the description of every operation are in "Reference:
include/linux/pon.h" at the end of this chapter. The tables here give the
contract of each call, and how it is checked.

Registration and lifetime
-------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``pon_device_register(parent, ops, num_flows, num_tconts, num_queues,
       drvdata)`` registers a MAC's data path. It returns ``-EINVAL`` when
       ``owns_netdev``, ``gem_add``, ``gem_del`` or ``gem_xmit`` is missing,
       when ``num_flows`` or ``num_queues`` is 0, or when an optional pair is
       half given: a MAC
       that joins a bridge has to be able to leave it, and a MAC that takes a
       VLAN has to be able to drop it. A second data path on the same parent
       is ``-EBUSY``.
     - I
     - Design choice; only the success path runs (the board boots to a shell).
       A test registration with a half pair would show the refusal.
   * - ``num_flows`` is the size of the MAC's flow space. GEM interfaces are
       indexed by flow, and every flow a MAC hands back must be below it.
     - T
     - The recorded exchange: six GEM interfaces on six flows.
   * - ``num_tconts`` counts the T-CONTs the ONU can be given for subscriber
       traffic. It excludes a T-CONT the MAC keeps for its own management
       channel, because no T-CONT managed entity can point at that one: the
       default Alloc-ID carries the OMCC and can sit outside every T-CONT
       managed entity.
     - P
     - ITU-T G.988 clause 9.2.2; ITU-T G.984.3 clause 8.1.3.6.1 (the default
       Alloc-ID carries PLOAM and OMCI).
   * - ``num_queues`` counts the upstream queues of each of those T-CONTs,
       the same for all; the class checks ``IFLA_GEM_QUEUE`` against it and
       reports it as ``num-tcont-queues``.
     - I
     - Design choice, not exercised by the daemon yet.
   * - ``pon_device_unregister()`` first marks the data path as going, under
       RTNL and the registration lock, so that the lookup by network device,
       through which a new GEM interface finds its ONU, no longer finds it;
       then it deletes the ONU's GEM interfaces under the same RTNL. No GEM
       interface can be created between that and the end. It then clears the
       operations under the registration lock and frees the flow table. The
       MAC must have stopped calling ``pon_gem_rx()`` before it calls this.
     - I
     - Design choice; teardown is not exercised on the board. Unbinding the
       switch driver would exercise it.
   * - ``pon_device_get_drvdata()`` returns the pointer given at registration;
       every MAC operation reaches its driver state through it.
     - T
     - The board boots to a shell; bridged traffic passes both ways, sourced
       from the host.
   * - With ``CONFIG_PON`` off, the registration and creation calls return
       ``ERR_PTR(-EOPNOTSUPP)``, ``pon_omci_port_rx()`` frees the message,
       ``pon_gem_rx()`` and ``pon_gem_netdev_flow()`` return false, and the
       rest do nothing. So a MAC driver builds either way.
     - I
     - Design choice, not exercised: a build without ``CONFIG_PON``.

The data path operations
------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``owns_netdev(pon, dev)`` says whether ``dev`` is the interface that
       carries this ONU's traffic, the one GEM interfaces stack on. It is
       called under the registration lock, from GEM creation and from the
       netdev notifier.
     - T
     - The recorded exchange: GEM interfaces are created on ``pon``.
   * - ``gem_add(pon, cfg, flow, extack)`` creates a GEM port in the MAC and
       returns the hardware flow its frames are tagged with. It refuses, with
       an extack message, what the MAC cannot do. RTNL is held.
     - T
     - The recorded exchange: six created, none refused.
   * - ``gem_change(pon, flow, cfg, extack)``, optional, applies new upstream
       attributes to the GEM port of ``flow``: ``cfg`` differs from the port's
       configuration in ``pbits``, ``queue``, ``weight``, ``cir`` or ``pir``
       only, and the class has checked it as for ``gem_add``. The MAC refuses,
       with an extack message, what it cannot do, and leaves the port as it
       was. RTNL is held.
     - I
     - Design choice (see "Upstream traffic").
   * - ``gem_del(pon, flow)`` removes the port; RTNL is held. When a bridged
       GEM interface is deleted, ``gem_bridge_leave`` for the same flow arrives
       after ``gem_del``, from the bridge's own unregistration handling. So a
       MAC must accept the two calls in either order.
     - I
     - Design choice, load-bearing for a MAC that keeps per-bridge state (the
       RTL9602C one does; see "The RTL9602C driver and the class"). Deleting a
       bridged GEM interface, which the daemon does when an OLT re-provisions,
       would show whether both orders are handled.
   * - ``gem_xmit(pon, skb)`` sends one frame of an upstream GEM port. ``skb``
       carries a ``METADATA_HW_PORT_MUX`` dst whose ``port_id`` is the flow.
       The MAC owns ``skb`` in every case and returns a ``NET_XMIT_*`` code.
       The call comes from ``ndo_start_xmit`` with bottom halves disabled. The
       interface transmits locklessly, so the call can run on several CPUs at
       once.
     - T
     - Bridged traffic passes both ways, sourced from the host (upstream). A
       routed-traffic check: a routed burst leaves by the GEM interface.
   * - ``gem_bridge_join(pon, flow, br, extack)`` and ``gem_bridge_leave(pon,
       flow, br)`` are an optional pair; RTNL is held. Join makes the MAC
       forward in hardware between the GEM port and its other ports in ``br``,
       or refuses. Without the pair, a bridged GEM interface is bridged in
       software.
     - T
     - Bridged traffic passes both ways, sourced from the host.
   * - ``gem_vlan_add(pon, flow, vid, flags, extack)`` and
       ``gem_vlan_del(pon, flow, vid, last)`` are an optional pair; RTNL is
       held. They carry the bridge VLANs of an offloaded GEM port. ``flags``
       are the bridge's ``BRIDGE_VLAN_INFO_PVID`` and
       ``BRIDGE_VLAN_INFO_UNTAGGED``. ``last`` says that no other GEM interface
       of the same bridge still has the VID, so that a MAC that holds VLAN
       membership once for all its GEM ports drops the VLAN only then, while
       one that chooses a GEM port per VLAN and priority hears of each port.
     - M
     - With the VLANs passed down, 75 of 150,000 upstream frames reached the
       CPU; without them all did (see "Bridging and switchdev offload").
   * - ``gem_mrouter(pon, flow, mrouter)`` is optional; RTNL is held. It says
       that the bridge has started, or stopped, seeing a multicast router behind
       the GEM port of ``flow``, and comes only while the port is offloaded.
     - M
     - The query and the snooping switch-off above (see "Bridging and
       switchdev offload").
   * - ``pon_gem_rx(pon, flow, skb)`` is called from the MAC's receive path. It
       returns true when the flow has a GEM interface: the frame was
       delivered, or dropped because the interface is down. It returns false
       when the flow has none, and the frame is still the caller's.
     - T
     - A routed-traffic check: downstream frames reach the GEM interface. M:
       80 of 80 downstream frames are delivered to a downstream-only multicast
       GEM interface.
   * - ``pon_gem_netdev_flow(dev, flow)`` says whether ``dev`` is a GEM
       interface, and returns its flow if so. It exists for MAC code that is
       handed devices by stacks that know nothing of GEM: a flow offload names
       the egress device, a route names an interface. GEM interfaces are
       recognised by their netdev operations.
     - T
     - A routed-traffic check: the offloaded flow's stream is the GEM
       interface's.

Status and statistics
---------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``dev_status(pon, st)`` is optional and may sleep. It reports the
       transmission convergence layer: the activation state, the identifiers
       the OLT assigned, the alarms present now, and the totals. It is called
       under the registration lock, with ``st`` already set to "nothing known"
       (see "Reports").
     - M
     - ``dev-get`` on the board with the fibre connected to an OLT (see
       "Status, alarms and notifications").
   * - ``gem_stats(pon, flow, st)`` is optional and may sleep. It returns the
       MAC's own counters for one GEM port. Without it, the interface's ethtool
       statistics are empty rather than showing the CPU's numbers under the
       port's name.
     - I
     - Design choice, not exercised: ``ethtool -S gemN`` on a MAC with and
       without the operation.
   * - ``tcont_get(pon, index, alloc_id)`` is optional and may sleep. It
       returns the Alloc-ID bound to T-CONT ``index``, or ``-ENOENT`` when that
       T-CONT is idle. ``-ENOENT`` is used because zero is a valid Alloc-ID:
       the default Alloc-ID equals the ONU-ID, and ONU-ID 0 is valid.
     - P
     - ITU-T G.984.3 clauses 8.1.3.6.1 and 9.1.1.
   * - Any other error from ``tcont_get`` is the answer of a get, and a dump
       leaves that ONU's T-CONTs out from that index on. So a MAC returns only
       0 or ``-ENOENT`` for indices below ``num_tconts``.
     - I
     - Design choice, not exercised: a MAC returning another error for a valid
       index.
   * - ``tcont_set(pon, index, alloc_id, extack)`` is optional and may sleep.
       It maps T-CONT ``index``, below ``num_tconts``, to ``alloc_id``, 0 to
       4095, or unmaps it for a negative ``alloc_id``; the MAC binds the T-CONT
       to the Alloc-ID once the Alloc-ID is assigned too. It returns 0,
       ``-EBUSY`` when another T-CONT is mapped to ``alloc_id``, or another
       error, each refusal with a message in ``extack``, and leaves the
       mappings as they were on an error. The class itself refuses, with its
       own message, an ONU without a data path, a MAC without the operation
       and an index not below ``num_tconts``.
     - P, I
     - P for the one-to-one mapping, ITU-T G.984.3 clause 5.5.4. I for the
       contract.
   * - ``pon_device_notify(pon, st)`` reports a change with the status the MAC
       has just built. The class does not call back into the MAC, so the MAC
       may call it under the lock the change was made under. It allocates with
       ``GFP_KERNEL`` and so sleeps.
     - M
     - Notifications watched on ``monitor`` on the board (see "Status, alarms
       and notifications").

The OMCI port calls
-------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``pon_omci_port_create(parent, ops, drvdata)`` requires ``ops->tx``;
       without it the result is ``-EINVAL``. It creates ``ponN`` if needed,
       takes a minor (so the class has at most 256 ports) and registers the
       port as ``ponNomci0``. A MAC has one port and it is always number 0,
       because an ONU has at most one OMCI connection.
     - P
     - ITU-T G.984.3 clause 9.2.3.14 (at most one OMCI connection for each
       ONU).
   * - ``pon_omci_port_remove(port)`` requires that the MAC has stopped calling
       ``pon_omci_port_rx()``. It calls ``stop`` if the port is open, clears
       the operations, wakes readers (who then see ``-ENODEV``) and purges the
       queue. An open file stays valid until it is closed.
     - I
     - Design choice, not exercised: removing the switch driver while the
       daemon holds the port.
   * - ``pon_omci_port_rx(port, skb)`` takes exactly one message, linear, and
       the class owns it from then on. It may be called in any context the MAC
       receives in: it uses the queue's spinlock, atomic counters and a
       wake-up.
     - T
     - OMCI both ways over ``/dev/pon0omci0``.
   * - ``pon_omci_port_link_set(port, up)`` is how the MAC says whether the
       OMCI channel exists, that is, whether it has a GEM port for the OMCC. A
       repeated value is ignored. Going down drops what is queued. Both edges
       wake poll.
     - T
     - OMCI both ways over ``/dev/pon0omci0``: without the up edge every write
       fails with ``-ENOLINK`` and the OLT gets no answer.
   * - ``struct pon_omci_port_ops``. ``start`` is optional; it is called at
       open under the port's lock, and an error fails the open. ``stop`` is
       optional; it is called at close and at the removal of an open port.
       ``tx`` is mandatory. On success the MAC owns the skb; on error it must
       not touch it, and the class frees it. ``-EAGAIN`` means the transmit
       path is momentarily full. ``tx`` runs in process context under the
       port's mutex.
     - T, I
     - T: OMCI both ways over ``/dev/pon0omci0`` for ``tx``. I: ``start`` and
       ``stop`` are a design choice that the RTL9602C driver does not use, so
       they are not exercised.

Reference: include/linux/pon.h
------------------------------

.. kernel-doc:: include/linux/pon.h

Locking and lifetime
====================

Four mechanisms keep the class consistent.

- RTNL. The creation and deletion of GEM interfaces, the bridge and VLAN
  operations and the netdev and switchdev notifiers run with RTNL held. When a
  path holds RTNL and the registration lock, it takes RTNL first.
- The registration lock, a mutex in ``pon_core.c``. It serialises the creation
  and removal of PON devices, OMCI ports and data paths with each other, with
  the publication of a data path's operations, and with the calls of
  ``owns_netdev`` and ``dev_status``.
- The port lock of each OMCI port, a mutex. It serialises open, close and the
  removal of the port with each other and with the calls of ``start``,
  ``stop`` and ``tx``.
- RCU. The flow table of a PON device, which holds the GEM interface of each
  flow, is written under RTNL and read under RCU on the receive path. A GEM
  interface is published in it only after it is registered and linked to its
  lower interface, and it is removed from it first when the interface is
  deleted.

Lifetime follows the device model. A lookup returns a reference on the PON
device, which the caller drops. The data path holds one from its registration
to its unregistration, and the OMCI port is a child device of ``ponN``. A GEM
interface holds a reference on its ONU until the interface is freed. A file
that holds the OMCI port open holds a reference on the port until it is
closed, so the file stays valid after the MAC removed the port: a read then
fails with ``-ENODEV``, a write fails as well, and poll reports a hang-up.

What the class holds when it calls into the MAC, from the kernel-doc of
``include/linux/pon.h`` and the call sites:

.. list-table::
   :header-rows: 1
   :widths: 26 30 44

   * - Operation
     - The class holds
     - Notes
   * - ``owns_netdev``
     - RTNL and the registration lock.
     - Must not take RTNL.
   * - ``gem_add``, ``gem_change``, ``gem_del``
     - RTNL.
     - ``gem_del`` can come before ``gem_bridge_leave`` of the same flow.
   * - ``gem_bridge_join``, ``gem_bridge_leave``
     - RTNL.
     - ``gem_bridge_leave`` can come after ``gem_del``.
   * - ``gem_vlan_add``, ``gem_vlan_del``
     - RTNL.
     - ``gem_vlan_add`` only while the port is offloaded.
   * - ``gem_mrouter``
     - RTNL.
     - Only while the port is offloaded.
   * - ``gem_xmit``
     - No lock; bottom halves are disabled.
     - Can run on several CPUs at once. The MAC owns the skb.
   * - ``dev_status``
     - The registration lock.
     - May sleep. Must not take RTNL.
   * - ``gem_stats``
     - No lock of the class.
     - May sleep.
   * - ``tcont_get``, ``tcont_set``
     - The registration lock.
     - May sleep. Must not take RTNL.
   * - ``start``, ``stop`` of the OMCI port
     - The port lock.
     - ``start`` at open; ``stop`` at close and at the removal of an open port.
   * - ``tx`` of the OMCI port
     - The port lock.
     - Process context. The MAC owns the skb on success only.

What the MAC may call, and where:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Call
     - Context
   * - ``pon_omci_port_create()``, ``pon_omci_port_remove()``,
       ``pon_device_register()``
     - Process context; may sleep.
   * - ``pon_device_unregister()``
     - Process context, RTNL not held: the call takes RTNL itself.
   * - ``pon_gem_rx()``
     - The MAC's receive path, as for ``netif_receive_skb()``.
   * - ``pon_omci_port_rx()``
     - Any context the MAC receives in.
   * - ``pon_device_notify()``
     - Process context; allocates with ``GFP_KERNEL``. The MAC may hold the
       lock the change was made under.

The rules that follow, each with the fact table that holds its row:

- ``owns_netdev`` and ``dev_status`` do not take RTNL ("PON devices").
- A MAC stops calling ``pon_gem_rx()`` before ``pon_device_unregister()``, and
  ``pon_omci_port_rx()`` before ``pon_omci_port_remove()`` ("The driver
  interface").
- A MAC accepts ``gem_del`` and ``gem_bridge_leave`` in either order ("The
  driver interface").
- A MAC returns only 0 or ``-ENOENT`` from ``tcont_get`` for an index below
  ``num_tconts`` ("The driver interface").

The RTL9602C driver and the class
=================================

The RTL9602C switch driver is the only MAC driver in the tree. Its ONU sits
behind the switch's PON port, and it registers the class objects against its own
device, the switch. In its DSA ``setup`` operation, ``rtl9602c_setup()``, it
creates the OMCI port and then registers the data path; in
``rtl9602c_teardown()`` it removes them in the opposite order. The hardware the
class sees through it (the flows, the T-CONTs, the queues, the OMCC path and the
trailer of a received OMCI message) is on the pages listed in "The hardware
behind the class". This chapter holds only what the class sees.

The switch node has the compatible ``realtek,rtl9602c-switch`` and the binding
``Documentation/devicetree/bindings/net/dsa/realtek,rtl9602c-switch.yaml``. Its
``realtek,sysctl`` and ``memory-region`` properties decide whether the driver
registers the class objects at all (see "Registration").

Registration
------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The switch driver registers against its own device: first the OMCI port,
       then the data path. A failure of either is logged and not fatal, because
       a switch without its OMCI channel still switches. Teardown removes the
       data path first, then the OMCI port, which is the order the class
       requires (see "PON devices").
     - T
     - The board boots to a shell; OMCI both ways over ``/dev/pon0omci0``.
   * - Neither is registered when the device tree gives the PON MAC no system
       controller (``realtek,sysctl``) or no packet buffer (the ``pbo-us`` and
       ``pbo-ds`` memory regions), or when the CPU port has no conduit. Without
       them, no GEM frame and no OMCI message crosses the PON MAC.
     - I
     - Design choice, not exercised: a device tree without the packet buffer
       shows no ``/dev/pon0omci0``.
   * - Registration numbers: ``num_flows`` is ``RTL9602C_GPON_NUM_FLOW`` (128;
       see "Known limitations"). ``num_tconts`` is
       ``RTL9602C_GPON_DEFAULT_TCONT_IDX`` (16), because the scheduler has
       ``RTL9602C_PON_NUM_TCONT`` (17) T-CONTs and keeps the last for the
       OMCC.
     - T
     - The recorded exchange: six GEM interfaces on T-CONTs 0 to 2. OMCI both
       ways over ``/dev/pon0omci0``.
   * - ``owns_netdev`` is true only for the DSA user interface of
       ``RTL9602C_PORT_PON`` on this switch, named ``pon``.
     - T
     - The recorded exchange.

GEM ports
---------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``gem_add`` refuses: an upstream port on a T-CONT of 8 or above
       (``-ERANGE``); a PON MAC that is not up yet (``-ENETDOWN``); the OMCC's
       Port-ID (``-EEXIST``); a Port-ID that already has a GEM interface
       (``-EEXIST``); no free flow (``-ENOSPC``). G.988 allows several GEM
       port network CTPs with one Port-ID; this MAC does not. The flow is the
       lowest free one other than ``RTL9602C_GPON_OMCC_FLOW`` (64).
     - I
     - Design choice, not exercised: ``ip link add ... type gem`` with each
       fault.
   * - The driver takes the frames of a VLAN for each GEM port by priority, as
       the class's model says ("Upstream traffic"), with a classifier entry
       per claimed priority of a divided VLAN.
     - M
     - Two GEM ports dividing VLAN 3001's priorities, each priority's frames
       going up its own port (the driver's page, "Bridges and VLANs").
   * - The driver schedules as the class's model says ("Upstream
       traffic"): each GEM T-CONT has eight queues, the higher-numbered
       hardware queue going first, so priority 0 takes the highest; committed
       rates first, then strict queues, then weighted ones. ``gem_change``
       moves a port to its new queue and sets the queues' weights and rates
       again.
     - M
     - Two loaded queues of one T-CONT on the board (the driver's page,
       "Upstream streams, queues and T-CONTs").
   * - ``gem_add`` programs the downstream half at once. It programs the
       upstream half only when the T-CONT has an Alloc-ID; until then the
       upstream is parked with a warning, and it starts when Assign_Alloc-ID
       arrives. That is not an error: OMCI and PLOAM arrive in an order
       neither end controls.
     - T
     - A routed-traffic check: the GEM interface's T-CONT is one the OLT's
       bandwidth map grants, and its stream points at that T-CONT's queue.
   * - ``gem_xmit`` gives the frame to the CPU port's conduit on the queue of
       ``RTL9602C_PORT_PON``. The conduit writes the dst's ``port_id`` into the
       transmit descriptor.
     - M
     - 40 of 40 upstream frames at the far side, none twice.
   * - Every GEM interface must join the same bridge; a second bridge is
       refused (``-EBUSY``), because the switch isolates by port and the PON
       port is one port. A second upstream GEM interface in a VLAN of that
       bridge is refused (``-EBUSY``) when the VLAN is added, because the VLAN
       chooses the upstream stream.
     - I
     - Design choice, not exercised: a second bridge; two upstream GEM
       interfaces given one VLAN.
   * - The conduit hands frames from the PON port on any flow except the
       OMCC's to ``pon_gem_rx()``.
     - T
     - A routed-traffic check: downstream frames reach the GEM interface. M:
       80 of 80 downstream frames are delivered.

Status, alarms and notifications
--------------------------------

The driver fills the ONU's status from its GPON MAC state and reports a change
to the class whenever the state or an identifier changes. The activation state
machine, the Alloc-ID and Port-ID tables and the counters are on the page of
the GPON MAC (Documentation/networking/device_drivers/pon/rtl9602c.rst). A
listener on the ``monitor`` group sees the ONU walk through its states when the
PON is brought down and up again; ``dev-get`` reads the same report at any time.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``dev_status`` is filled under the driver's GPON lock, so it is one
       moment of the activation state machine. It gives ``type`` ``gpon``, the
       index of ``pon``, and the serial number once one is set. The state,
       ONU-ID (unless unassigned), OMCC Port-ID (while bound), the Alloc-IDs of
       T-CONTs 0 to 15, the totals and the alarms are given only while the PON
       is up. With the PON down there is no state and no alarm to report, not
       "O1, no alarms".
     - M
     - ``dev-get`` on the board with the ONU in O5: ``onu-state`` o5,
       ``onu-id`` 1, one ``alloc-id`` (257), ``omcc-port-id`` 1, ``alarms`` 0.
       With the PON down, neither state nor alarms.
   * - The default Alloc-ID (equal to the ONU-ID) is not among the reported
       ``alloc-id`` values. Only the bindings of the 16 traffic T-CONTs are.
     - M
     - ``onu-id`` 1 with the single ``alloc-id`` 257.
   * - The alarms are the level bits ``RTL9602C_GTC_DS_IRQ_LOS``, ``_LOF`` and
       ``_LOM`` of ``RTL9602C_GTC_DS_IRQ_STAT``, mapped to ``PON_ALARM_LOS``,
       ``_LOF`` and ``_LOM``. They are the current level, not the latched
       edges, because an alarm is what is wrong now.
     - M
     - ``alarms`` 0 in O5.
   * - ``pon_device_notify()`` is called after every state change, once the
       identifiers have been set up or taken down, and from the poll work every
       ``RTL9602C_GPON_POLL_MS`` (50 ms). A notification goes out only when the
       report, with its counters zeroed, differs from the last one sent:
       counters moving is not a change, and a steady O5 sends nothing.
     - M
     - A listener on ``monitor`` across ``ip link set pon down`` and ``up``
       saw: O1; a report with neither state nor alarms; O2; O3; O4 with the
       ONU-ID; O5; O5 with the Alloc-ID and the OMCC Port-ID.

T-CONTs and counters
--------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``tcont_get`` answers from the driver's record of what it programmed,
       not from a read-back of the MAC's table, which says what was written and
       not what the OLT still considers live. An idle T-CONT gives ``-ENOENT``.
     - I
     - Design choice, not exercised: a ``tcont-get`` dump before and after
       Assign_Alloc-ID.
   * - ``gem_stats`` gives the flow's upstream octets. The counter clears on
       read, and the driver keeps the total.
     - I
     - Design choice, not exercised: ``ethtool -S gemN`` across an upstream
       burst. The counter is described on the page of the PON MAC
       (Documentation/networking/device_drivers/pon/rtl9602c.rst).

The OMCI path
-------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - OMCI receive takes only control frames from ``RTL9602C_PORT_PON``. It
       hands each one to ``pon_omci_port_rx()`` as exactly one message, after
       removing what the hardware appends (``RTL9602C_OMCI_RX_TRAILER``, four
       bytes).
     - M
     - Every OLT message read from ``/dev/pon0omci0`` is 48 bytes, the last
       four a CRC over the first 44.
   * - Messages received outside O5 are dropped by the driver before they reach
       the port. It logs a debug line, and the port's ``rx_dropped`` does not
       count them.
     - I
     - Assumption, load-bearing and not verified: nothing measured shows that a
       message received outside O5 must be dropped. Delivering one would show
       whether any harm follows.
   * - OMCI transmit returns ``-ENOLINK`` while the OMCC is unbound; this
       closes the race with an unbind after the class's own link check.
       Otherwise the message goes to the conduit's control transmit for
       ``RTL9602C_PORT_PON`` and flow ``RTL9602C_GPON_OMCC_FLOW``. There are no
       ``start`` and ``stop`` operations.
     - T
     - OMCI both ways over ``/dev/pon0omci0``.
   * - ``pon_omci_port_link_set()`` is called with true when the OMCC is bound
       (Configure_Port-ID), and with false when it is unbound and when bring-up
       empties the MAC's tables.
     - T
     - OMCI both ways over ``/dev/pon0omci0``.
   * - The conduit's control transmit returns ``-EAGAIN`` when its ring is full
       and ``-ENETDOWN`` when the conduit is down. Both reach the writer
       unchanged, and the daemon treats them as a dropped answer. Other
       failures (for example ``-ENOMEM`` from a DMA mapping error) end the
       daemon's channel. The transmit descriptor's length field is 17 bits, so
       a 1980-byte message fits.
     - I
     - Design choice, load-bearing for the daemon surviving a full ring:
       CPU-sent traffic filling the conduit's ring while the daemon answers the
       OLT would exercise it.

Sequences
=========

The steps are in the order the code takes them. The tag in parentheses at the
end of a step is the tag of the fact the step rests on (see the tables above);
a step tagged I rests on an assumption or on a design choice that no check
exercises.

Setup of the RTL9602C driver
----------------------------

1. The class starts at ``subsys_initcall``. Purpose: the class, the character
   region, the family and the link kind exist before any MAC sets itself up.
   (T)
2. The switch driver creates its OMCI port, which creates ``pon0`` and
   ``pon0omci0``, and attaches its control-frame handler to the conduit.
   Purpose: OMCI messages have somewhere to go. (T)
3. It registers its data path on ``pon0`` (128 flows, 8 T-CONTs of 8 queues)
   and attaches its flow handler to the conduit. Purpose: GEM interfaces can be
   created and received frames delivered. (T)
4. ``ip link set pon up`` brings the PON up, and the ONU walks O1 to O5.
   Purpose: the OLT can activate the ONU. (T: the downstream locks, and the ONU
   reaches O5 and holds it on an OLT)
5. Configure_Port-ID binds the OMCC, the driver calls
   ``pon_omci_port_link_set(port, true)``, and writes are accepted from then
   on. Purpose: the daemon's answers can leave. (T)

An OMCI exchange
----------------

1. The daemon opens the port read-write and non-blocking. Open needs
   ``CAP_NET_ADMIN`` and is exclusive, and it empties the queue. Purpose: one
   management entity answers the OLT, and it starts from an empty queue. (T)
2. The daemon waits in poll for ``EPOLLIN``, then reads one message at a time
   with a 1980-byte buffer until ``-EAGAIN``. Purpose: every message is read
   whole. (T)
3. It writes one whole answer per call. ``-ENOLINK``, ``-EAGAIN`` and
   ``-ENETDOWN`` drop that answer and the OLT retransmits; any other error ends
   the exchange. Purpose: a lost answer is recovered by the OLT, and a broken
   channel is not. (T for the success path; the error paths are I)
4. A read returning ``-ENODEV`` means the MAC removed the port; the daemon
   closes it. Purpose: the file is released. (I)

Creating a GEM interface
------------------------

This is an ``RTM_NEWLINK`` of kind ``gem``, with RTNL held.

1. Validate the attributes: the ranges of the policy, then the direction rules
   (a multicast port is downstream only; an upstream port needs a T-CONT).
   Purpose: refuse a request that cannot be right before the MAC is asked. (T)
2. Find the ONU by the lower interface through ``owns_netdev``, taking a
   reference. Purpose: the interface belongs to the right ONU. (T)
3. Allocate the port-mux dst that the transmit path attaches to every frame.
   Purpose: the MAC takes the flow from it. (T)
4. Call the MAC's ``gem_add`` for a flow, then check that the flow is below
   ``num_flows`` and has no interface yet. Purpose: the MAC creates its port,
   and the flow table stays consistent. (T)
5. Put the flow and the lower into the dst, and take the lower's MAC address
   and MTU. Purpose: the frames carry their flow, and the interface has the
   ONU's address. (T)
6. Register the network device, then install the destructor. Purpose: the
   destructor runs only after a successful registration (see "The
   interface"). (I)
7. Link the interface as an upper of the lower. Purpose: the interface is
   stacked on its lower. (I)
8. Copy the lower's operational state, then publish the interface in the flow
   table under RCU. Purpose: receive can reach the interface only from this
   point. (T)

Deleting a GEM interface
------------------------

This is an ``RTM_DELLINK``, or the unregistration of the lower interface.

1. Unpublish the flow's interface (RCU). Purpose: receive stops reaching it.
   (I)
2. Unlink the interface from its lower and queue its unregistration. Purpose:
   the interface goes with the next batch of unregistrations. (I)
3. Call the MAC's ``gem_del``. Purpose: the MAC's port goes too; a port left in
   the MAC would make the next creation with the same Port-ID fail with
   ``-EEXIST``. (I)
4. During the unregistration, if the interface was bridged: the offload is
   undone on ``NETDEV_PRECHANGEUPPER``, then ``gem_bridge_leave`` is called on
   ``NETDEV_CHANGEUPPER``, after ``gem_del``. Purpose: the bridge lets go of a
   port only during its unregistration. (I)

Joining a bridge
----------------

1. On ``NETDEV_CHANGEUPPER`` (linking), call ``gem_bridge_join``; a refusal
   rolls the join back with the MAC's extack message. Purpose: the MAC forwards
   in hardware between the GEM port and the bridge's other ports, or says it
   cannot. (T)
2. Offload the port to switchdev with the lower as the hardware device. On
   failure, call ``gem_bridge_leave`` and roll back. Purpose: the GEM interface
   is in the switch's hardware domain. (T)
3. Mark the interface offloaded. Purpose: from here, received frames carry
   ``offload_fwd_mark`` and bridge VLAN objects reach the MAC. (T)
4. The bridge gives the new port its PVID, which reaches the MAC through
   ``gem_vlan_add``. Purpose: the switch files the addresses it learns under
   the VLAN the lookups use. (M)

Teardown of the RTL9602C driver
-------------------------------

1. Detach the conduit's flow handler, then call ``pon_device_unregister()``.
   The GEM interfaces are deleted under RTNL, then the operations are cleared.
   Purpose: no frame reaches the class while the data path goes. (I)
2. Detach the conduit's control handler, then call ``pon_omci_port_remove()``.
   Purpose: no message reaches the class while the port goes. (I)
3. The PON device is unregistered with its last child, the OMCI port. Purpose:
   nothing of the class is left behind. (I)

Known limitations
=================

- ``num-gem-ports`` is the size of the MAC's flow space: 128 on the RTL9602C.
  At most 127 GEM interfaces can exist there, because ``gem_add`` never gives
  out ``RTL9602C_GPON_OMCC_FLOW`` (64), which carries the OMCC. A daemon that
  sizes its GEM port entities by ``num-gem-ports`` gets ``-ENOSPC`` at the
  128th creation.

Open questions
==============

These are the facts tagged **I** in the tables above, each with what would
settle it. The first list holds the assumptions the driver depends on, the ones
the tables mark "load-bearing" and the ones behind the known limitations: the
driver or the daemon is correct on this board only if they hold. The second
list holds the questions to which the code is indifferent: it works whichever
way they resolve. Most of them are behaviours that the class implements by
design and that no check has run.

Assumptions the driver depends on
---------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Assumption
     - What would settle it
   * - That ``owns_netdev`` and ``dev_status`` do not take RTNL, so that they
       cannot deadlock against the creation of GEM interfaces ("PON devices").
     - A kernel with lockdep, running a ``dev-get`` dump while the daemon
       creates GEM interfaces.
   * - That a MAC accepts ``gem_del`` and ``gem_bridge_leave`` for one flow in
       either order ("The data path operations").
     - Deleting a bridged GEM interface, as the daemon does when an OLT
       re-provisions.
   * - That the daemon treats ``-ENOLINK`` as a dropped answer and any write
       error other than ``-ENOLINK``, ``-ENETDOWN`` and ``-EAGAIN`` as the end
       of the channel ("The OMCI channel and poll").
     - Writing to the port before the OLT has configured the OMCC.
   * - That an image of the board configuration cannot inject OMCI ("The
       inject file").
     - Listing ``/sys/kernel/debug/pon`` on an image built from the board
       configuration: it is not there.
   * - That the switch has forwarded every frame that arrives with
       ``offload_fwd_mark``, so that none arrives twice ("Transmit and
       receive").
     - A broadcast sent from the OLT side while a GEM interface is bridged,
       counted once at the subscriber side.
   * - That a GEM interface created with ``IFF_UP`` while ``pon`` is
       administratively down fails to open and is deleted again, so that the
       order of provisioning matters ("State").
     - ``ip link set pon down``, then a creation with ``up``.
   * - That ``pon`` is dormant until O5 and up in O5, so that its GEM
       interfaces are too ("State").
     - ``ip link show gemN`` before and after O5.
   * - That a VLAN is taken out of the MAC only when no other GEM interface of
       the bridge still has it ("Bridging and switchdev offload").
     - Removing a VLAN from one of two GEM interfaces that share it, and
       checking that the traffic on the other continues.
   * - That an unprivileged process cannot join ``monitor``.
     - An unprivileged listener trying to join ``monitor``.
   * - That ``tcont-get`` on an ONU without a registered data path answers
       ``-ENOENT``.
     - A ``tcont-get`` dump on an ONU without a data path.
   * - That a daemon sizing its GEM port entities by ``num-gem-ports`` copes
       with one entity fewer than the advertised number ("Known limitations").
     - A 128th creation, which fails with ``-ENOSPC``.
   * - That a message received outside O5 must be dropped ("The OMCI path").
     - Delivering a message received outside O5 and looking for harm.
   * - That CPU-sent traffic filling the conduit's ring costs the daemon no
       more than a dropped answer ("The OMCI path").
     - CPU-sent traffic filling the conduit's ring while the daemon answers the
       OLT.

Questions the code does not depend on
-------------------------------------

About start-up and lifetime
~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Question
     - What would settle it
   * - Whether the exit of the module undoes the start-up in reverse ("Module
       and start-up").
     - ``rmmod pon`` on a modular build.
   * - Whether creating a link of kind ``gem`` loads a modular core, and a
       request to the family alone does not ("Module and start-up").
     - A modular build with the core not yet loaded.
   * - Whether removing the OMCI port of a MAC whose data path is still
       registered leaves the PON device in place ("PON devices").
     - Removing the OMCI port of such a MAC.
   * - Whether registering a data path with a half pair of optional operations
       is refused with ``-EINVAL`` ("Registration and lifetime").
     - A test registration with a half pair.
   * - Whether unregistering the data path and removing the OMCI port are
       clean ("Registration and lifetime", "The OMCI port calls", "Teardown of
       the RTL9602C driver").
     - Unbinding the switch driver, and removing it while the daemon holds the
       port.
   * - Whether a build without ``CONFIG_PON`` builds a MAC driver
       ("Registration and lifetime").
     - A build without ``CONFIG_PON``.

About the OMCI port
~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Question
     - What would settle it
   * - Whether an open without ``CAP_NET_ADMIN`` fails with ``-EPERM``
       ("Opening and closing").
     - Opening the port without the capability.
   * - Whether a second open fails with ``-EBUSY`` ("Opening and closing").
     - A second open while the daemon runs.
   * - Whether a read with a buffer smaller than the message fails with
       ``-EMSGSIZE`` and leaves the message queued ("Reading and writing").
     - A short read.
   * - Whether a refused write counts in ``tx_errors`` and an accepted one in
       ``tx_messages`` ("Reading and writing").
     - Reading the counters around a refused write.
   * - Whether poll reports ``EPOLLHUP | EPOLLERR`` after the MAC removed the
       port ("The OMCI channel and poll").
     - Polling a port whose MAC was removed.
   * - Whether a message that arrives with the port closed is dropped and
       counted, and open empties the queue ("The OMCI channel and poll").
     - An OLT message with the port closed: ``rx_dropped`` rises.
   * - Whether 64 messages are enough for a slow reader, on the assumption that
       an OLT waits for each answer before it sends the next request ("The OMCI
       channel and poll").
     - A reader that stops reading during an OLT exchange.
   * - Whether messages still queued when the channel goes down are dropped and
       counted ("The OMCI channel and poll").
     - ``ip link set pon down`` with messages queued.
   * - Whether the sysfs counters of the port count as described ("Counters of
       the port").
     - Reading them before and after an OMCI exchange.
   * - Whether an injection with the port closed, or before the OMCC exists,
       behaves as described ("The inject file").
     - Injecting in both situations.
   * - Whether ``start`` and ``stop`` of the OMCI port work ("The OMCI port
       calls"). The RTL9602C driver has neither.
     - A MAC driver that supplies them.

About GEM interfaces
~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Question
     - What would settle it
   * - Whether a changed GEM port, deleted and created again by the daemon,
       leaves nothing behind ("Creating and deleting").
     - An OLT changing a provisioned GEM port.
   * - Whether each refusal of a creation gives the error and the message
       listed ("Creating and deleting").
     - ``ip link add ... type gem`` with each fault.
   * - Whether the error paths of the creation release everything ("The
       interface").
     - Fault injection in ``register_netdevice()`` and in the linking to the
       lower.
   * - Whether ``iflink`` and the link's network namespace are the lower's
       ("State").
     - ``ip -d link show gemN``.
   * - Whether the interface statistics of a bridged GEM interface show only
       the frames the CPU handled ("Statistics of a GEM interface").
     - ``ip -s link show gemN`` during a bridged burst.
   * - Whether ``ethtool -S gemN`` lists the four counters only when the MAC
       has ``gem_stats`` ("Statistics of a GEM interface", "T-CONTs and
       counters").
     - ``ethtool -S gemN`` on a MAC with and without the operation.
   * - Whether deleting a GEM interface stops receive reaching it, leaves no
       port in the MAC, and, for a bridged interface, undoes the offload and
       calls ``gem_bridge_leave`` after ``gem_del`` ("Deleting a GEM
       interface").
     - Deleting a GEM interface under downstream traffic; deleting one and
       creating another with the same Port-ID, where a leftover port in the MAC
       would give ``-EEXIST``; deleting a bridged GEM interface.
   * - Whether leaving a bridge undoes the offload and leaves nothing programmed
       ("Bridging and switchdev offload").
     - ``ip link set gemN nomaster`` during a bridged burst.
   * - Whether a MAC that returns another error from ``tcont_get`` for a valid
       index fails the report of that T-CONT ("Status and statistics").
     - A MAC that returns another error for a valid index.

About the family and its values
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Question
     - What would settle it
   * - Whether a reader that decodes the ``stats`` nest by the numbers of the
       header gets the counters they name ("Attributes of the dev object").
     - A ``dev-get`` with ``stats`` decoded against the header.
   * - Whether the family needs a set operation ("The family and its
       permissions"). It has none, because the kernel runs the convergence
       layer and user space only observes it.
     - None needed; the operation table has no set.
   * - Whether an unprivileged ``dev-get`` gets ``-EPERM`` ("The family and its
       permissions").
     - An unprivileged ``dev-get``.
   * - Whether the family answers in every network namespace and notifies the
       initial one only ("The family and its permissions").
     - A request and a listener in another namespace.
   * - Whether requests run correctly without the generic netlink mutex ("The
       family and its permissions").
     - Concurrent dumps during a registration.
   * - Whether ``dev-get`` for an unknown number fails with ``-ENODEV``
       ("Reports").
     - A request for number 1 on the one-ONU board.
   * - Whether a counter that passes 2^32 is sent in 8 bytes, which a reader
       must accept ("Reports").
     - A counter passing 2^32.
   * - Whether the two capacities are 0 before a data path registers, and
       ``num-tconts`` keeps its value after the data path unregisters
       ("Reports").
     - The daemon reads ``num-tconts`` when it starts and builds its T-CONT
       entities from it; a changed count would show in its MIB upload.
   * - Whether a dump resumed after a full message never repeats an ONU
       ("Dumps").
     - A dump across a registration.
   * - Whether ``tcont-get`` answers as described, and its dump keeps the
       cursor ("The family and its permissions", "Dumps").
     - A ``tcont-get`` dump, and a ``tcont-get`` for index 16 on the RTL9602C.
   * - Whether the ``lom`` flag is what a MAC's own definition says ("Alarms").
     - The definition on the page of the MAC.
   * - Whether a MAC can report more than 32 allocations, and whether no OLT
       assigns that many to one ONU ("Identifiers").
     - A MAC with more than 32 allocations.
   * - Whether ``stats`` holds totals since bring-up whatever the MAC's
       counters do ("Counters").
     - The RTL9602C's counters clear on read; a second reader would show the
       totals.
   * - Whether a MAC counts ``bwmap-crc-errors``, ``gem-frames`` and the
       ``fec-*`` counters as described ("Counters").
     - The definitions on the page of the MAC.

About the RTL9602C driver
~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Question
     - What would settle it
   * - Whether neither the OMCI port nor the data path is registered without
       the system controller, the packet buffer or a conduit ("Registration").
     - A device tree without the packet buffer: no ``/dev/pon0omci0``.
   * - Whether each refusal of ``gem_add`` gives the error listed ("GEM
       ports").
     - ``ip link add ... type gem`` with each fault.
   * - Whether a second bridge, and a second upstream GEM interface in one VLAN,
       are refused with ``-EBUSY`` ("GEM ports").
     - A second bridge, and two upstream GEM interfaces given one VLAN.
   * - Whether ``tcont_get`` follows Assign_Alloc-ID ("T-CONTs and counters").
     - A ``tcont-get`` dump before and after Assign_Alloc-ID.
   * - Whether the error paths of the daemon's exchange (a write refused with
       something other than the three errors, a read returning ``-ENODEV``)
       end the exchange as described ("Sequences").
     - Removing the switch driver while the daemon runs, and forcing a write
       error.
