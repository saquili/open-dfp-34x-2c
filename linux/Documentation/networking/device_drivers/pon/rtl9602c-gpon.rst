.. SPDX-License-Identifier: GPL-2.0-only

=========================
Realtek RTL9602C GPON MAC
=========================

Overview
========

The GPON MAC is the block of the RTL9602C that runs the ONU's side of the
transmission convergence layer of ITU-T G.984.3, the GTC layer. Downstream it
finds the 125 us frame, reads the PLOAM channel and the bandwidth map, and sorts
the GEM frames by Port-ID into flows. Upstream it sends a burst in each
allocation that the OLT grants to one of the ONU's Alloc-IDs: the physical layer
overhead, a PLOAM message if the allocation asks for one, and the GEM frames
that the PON MAC queues for the allocation's T-CONT. Its registers, tables and
counters are in the switch's register window, at offsets ``0x700000`` to
``0x706cfc``.

The MAC does not run the activation by itself. It takes its activation state
from a register that the driver writes, and classifies the OLT's grants by that
state; the driver runs the state machine of G.984.3 from O1 to O7 on the PLOAM
messages that the MAC queues for it. The MAC answers the OLT's serial-number and
ranging requests by itself, from message templates that the driver arms. G.984.3
gives the ONU 750 us to act on a PLOAM message, so the driver dispatches PLOAM
messages from the switch's interrupt thread and not from a poll.

``ip link set pon up`` starts the activation and ``ip link set pon down`` stops
it. The ONU's serial number comes from the devlink parameter ``gpon_serial``;
the driver has no default, and the interface does not come up until one is set.

The block between the fibre and switch port 2::

       fibre
         |
       optics
         |
       PON SerDes: analog front end, GPON mode
         |
       +-------------------------------------------------------------+
       | GPON MAC                                    (this page)     |
       |  downstream: GTC framing, PLOAM queue, bandwidth map,       |
       |              GEM framing, Port-ID table -> flows            |
       |  upstream:   burst overhead, PLOAM queues and templates,    |
       |              GEM framing, Alloc-ID table -> T-CONTs         |
       +-------------------------------------------------------------+
         |
       PON MAC: packet buffer, streams, queues, T-CONT scheduler
         |
       switch port 2 (pon)

This page describes the block as the driver sets it up: the boot report, the
bring-up and take-down, the PON SerDes and its analog front end, the downstream
lock, the activation state machine, PLOAM in both directions, the burst overhead
and the upstream timing, the bandwidth map, the Alloc-ID and Port-ID tables, GEM
framing, the counters, FEC and encryption, and what the driver reports to the
PON class, through devlink and in debugfs. The PON MAC and the packet buffer are
on Documentation/networking/device_drivers/pon/rtl9602c.rst, which also
introduces the set of pages for the PON hardware; the optics are on
Documentation/networking/device_drivers/pon/gn25l95.rst.

Activation in brief
-------------------

The driver implements the states O1 to O7 of ITU-T G.984.3 (clause 10.2.1). A
normal activation runs as follows; "The activation state machine" has the facts
of each step.

1. Bring-up leaves the ONU in O1, with TX_DISABLE asserted, the message
   templates armed and the GPON interrupt source enabled, and the ONU moves on
   to O2 once LOS and LOF have cleared.
2. The OLT broadcasts Upstream_Overhead. The driver programs the burst overhead
   and the pre-assigned delay from it, asks the optics for the transmit power
   level it names, releases TX_DISABLE and moves to O3. The MAC answers each
   discovery grant, an allocation to Alloc-ID 254 with the PLOAMu flag, with
   the Serial_Number_ONU template. After every ten answers with no ONU-ID the
   ONU takes the next power level itself.
3. The OLT sends Assign_ONU-ID carrying the ONU's serial number. The driver
   writes the ONU-ID, binds the default Alloc-ID, which equals the ONU-ID, and
   moves to O4. The OLT then sends the new ONU-ID a ranging request, which the
   MAC answers by itself.
4. The OLT sends Ranging_Time. The driver writes the equalization delay and the
   burst overhead length of the ranged state, and moves to O5.
5. Next the OLT sends Configure_Port-ID, and Assign_Alloc-ID for any T-CONT
   beyond the default one. The driver acknowledges both: the Port-ID binds the
   OMCC, the ONU's OMCI channel, and each Alloc-ID goes into one of T-CONTs 0 to
   15. OMCI messages then cross the PON MAC to ``/dev/pon0omci0``.

The table lists every event that changes the state. TO1 is the timer of G.984.3
that bounds the time in O3 and O4, and TO2 the one that bounds the time in O6;
the driver's use of them is described with the state machine. An ONU that the
OLT disabled stays disabled, in O7, even across a power-off, until the OLT
enables it. The driver keeps the disable, the devlink parameter
``gpon_disabled``, across a take-down and a bring-up of the PON; across a
restart user space keeps it and sets the parameter again before the PON comes
up.

.. list-table::
   :header-rows: 1
   :widths: 30 12 10 48

   * - Event
     - From
     - To
     - What the driver does
   * - ``ip link set pon up``
     - any
     - O1
     - Bring-up sets O1, builds and programs everything (see "Bring-up and
       take-down") and ends with no ONU-ID.
   * - ``ip link set pon up`` with ``gpon_disabled`` true
     - any
     - O7
     - As above, and the ONU goes on to O7 before the GPON interrupt is
       enabled: the OLT disabled it before the PON went down or the system
       restarted.
   * - LOS and LOF both clear
     - O1
     - O2
     - Nothing more: the ONU waits for Upstream_Overhead.
   * - LOS or LOF
     - O2, O3 or O4
     - O1
     - Asserts TX_DISABLE and gives back the ONU-ID, the Alloc-IDs and the OMCC
       if they are held.
   * - LOS or LOF
     - O5
     - O6
     - Asserts TX_DISABLE and arms TO2; the ONU-ID, the Alloc-IDs and the OMCC
       stay.
   * - Upstream_Overhead, broadcast
     - O2
     - O3
     - Programs the burst overhead and the pre-assigned delay, asks for the
       default transmit power level mode it names, arms the Serial_Number_ONU
       template again, releases TX_DISABLE and arms TO1.
   * - Assign_ONU-ID carrying the ONU's serial number
     - O3
     - O4
     - Writes the ONU-ID, binds the default Alloc-ID into T-CONT 16 and arms a
       bandwidth-map capture; TO1 runs on from O3.
   * - Ranging_Time for the main path, directed
     - O4
     - O5
     - Writes the equalization delay and the burst overhead length of the ranged
       state, flushes the upstream PLOAM queues and disarms TO1.
   * - TO1 expires
     - O3 or O4
     - O2
     - Gives back the ONU-ID, the Alloc-IDs and the OMCC if they are held. The
       next Upstream_Overhead programs everything again.
   * - Deactivate_ONU-ID, directed or broadcast
     - O4, O5 or O6
     - O2
     - Gives back the ONU-ID, the Alloc-IDs and the OMCC.
   * - POPUP, directed
     - O6
     - O5
     - Releases TX_DISABLE; the ranging held stays in force.
   * - POPUP, broadcast
     - O6
     - O4
     - Releases TX_DISABLE and arms TO1; the OLT ranges the ONU again.
   * - Swift_POPUP, broadcast
     - O6
     - O5
     - As a directed POPUP.
   * - TO2 expires
     - O6
     - O1
     - Gives back the ONU-ID, the Alloc-IDs and the OMCC, and goes on to O2 at
       once if LOS and LOF have cleared.
   * - Disable_Serial_Number disabling this ONU's serial number
     - any but O7
     - O7
     - Asserts TX_DISABLE, gives back the ONU-ID, the Alloc-IDs and the OMCC,
       sets ``gpon_disabled``, and asks for transmit power level mode 0, as on
       entering O1 and O2.
   * - Disable_Serial_Number enabling this ONU's serial number, or every ONU
     - O7
     - O2
     - Clears ``gpon_disabled``; the OLT discovers the ONU again.
   * - ``gpon_disabled`` set false by hand
     - O7
     - O2
     - As the OLT's enable: the ONU leaves O7 without it.
   * - ``ip link set pon down``
     - any
     - O1
     - Asserts TX_DISABLE and gives back the ONU-ID, the Alloc-IDs and the OMCC.
       The MAC is not reset, and ``gpon_disabled`` stays as it is.

Configuration
-------------

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Symbol
     - Effect
   * - ``CONFIG_NET_DSA_RTL9602C``
     - Tristate. Builds ``rtl9602c-gpon.o`` into the module ``rtl9602c`` with
       the other blocks of the switch driver; the GPON MAC cannot be left out of
       it. It selects ``PON``.
   * - ``CONFIG_PON``
     - The PON class (Documentation/networking/pon.rst). The GPON MAC code
       reports the ONU's status and the state of the OMCI channel to it through
       ``include/linux/pon.h``.
   * - ``CONFIG_DEBUG_FS``
     - The seven debugfs views of the GPON MAC exist only with it (see "The
       debugfs views").
   * - ``CONFIG_DYNAMIC_DEBUG``
     - The per-message PLOAM trace and a few more lines are ``pr_debug()``
       lines. They print once enabled through dynamic debug (see "The kernel
       log").
   * - ``CONFIG_RTL9602C_OPTICS_RO``
     - With it the optics' I2C adapter refuses every write, and the modulation
       hold of the bring-up writes nothing (see
       Documentation/networking/device_drivers/pon/gn25l95.rst).

``rtl9602c_defconfig`` (``arch/mips/configs/``) builds the switch driver in
(``CONFIG_NET_DSA_RTL9602C=y``) and sets ``CONFIG_DEBUG_FS=y`` and
``CONFIG_DYNAMIC_DEBUG=y``.

Device tree
-----------

The GPON MAC has no device tree node of its own. It is part of the register
window of the switch node, compatible ``realtek,rtl9602c-switch``, which
Documentation/networking/dsa/rtl9602c.rst describes with its binding. The GPON
code depends on these properties of that node.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Property
     - What the GPON code does with it
   * - ``reg``
     - The register window. The GPON MAC is at offsets ``0x700000`` to
       ``0x706cfc`` of it.
   * - ``interrupts``
     - The switch's one interrupt, shared by all its sources. The GPON MAC is
       one of them, ``RTL9602C_IRQ_SRC_GPON`` (bit 10 of
       ``RTL9602C_IRQ_ENABLE``, ``0x01d00c``): the bring-up enables it, and the
       interrupt thread dispatches PLOAM messages. The binding requires the
       property.
   * - ``realtek,pon-optics``
     - Optional. The optics the MAC drives (``semtech,gn25l95``, with the
       laser's TX_DISABLE line, which the state machine releases in O3 to O5
       and asserts in every other state). The MAC's driver waits for their
       driver; without the property every TX_DISABLE step is skipped.
   * - ``realtek,sysctl``, ``memory-region``
     - Read at probe for the PON MAC, which the bring-up builds (see
       Documentation/networking/device_drivers/pon/rtl9602c.rst).

Where the code is
-----------------

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - File
     - Contents
   * - ``drivers/net/pon/rtl9602c/rtl9602c-gpon.c``
     - The GPON MAC: the boot report, the PON SerDes and analog set-up, bring-up
       and take-down, the activation state machine, PLOAM receive and transmit,
       the burst overhead and the upstream timing, the Alloc-ID, Port-ID and
       multicast tables, the counters, the status report to the PON class and
       the debugfs views.
   * - ``drivers/net/pon/rtl9602c/rtl9602c-pon-regs.h``, ``rtl9602c-pon.h``
     - The register map of the GPON MAC, and ``struct rtl9602c_pon``, whose
       fields hold its state (their names begin ``gpon_`` and ``ploam_``).
   * - ``drivers/net/dsa/realtek/rtl9602c-main.c``, ``rtl9602c-pon-host.c``
     - The GPON source of the switch interrupt (``rtl9602c_irq()`` notes the
       time, ``rtl9602c_irq_thread()`` calls the PON operation ``irq``, which
       is ``rtl9602c_gpon_irq()``), ``rtl9602c_port_enable()`` and
       ``rtl9602c_port_disable()`` of the PON port, which start and stop the
       MAC through the PON operations, the devlink parameter ``gpon_serial``
       and the info item ``gpon.mac``. The MAC's boot report and its debugfs
       views are made by the PON driver when it binds
       (``rtl9602c_pon_probe()``).
   * - ``drivers/net/pon/rtl9602c/rtl9602c-gem.c``
     - The PON class's data path. This block calls ``rtl9602c_gem_restore()``
       and ``rtl9602c_gem_tcont_ready()`` in it. It calls back
       ``rtl9602c_gpon_port_bind()`` and ``rtl9602c_gpon_port_unbind()`` for the
       Port-ID table and ``rtl9602c_gpon_fill_status()`` for the status, and it
       answers the T-CONT query from the driver's record of the Alloc-IDs.
   * - ``drivers/net/pon/rtl9602c/rtl9602c-omci.c``
     - The OMCI port. It reads the activation state and whether the OMCC is
       bound; this block reports the OMCI channel's link to the port.
   * - ``rtl9602c-ponmac.c`` in the same directory, and the optics of the PON
       class
     - Called from this block: ``rtl9602c_ponmac_init()`` and
       ``rtl9602c_ponmac_sid_valid()`` of the PON MAC, and
       ``pon_optics_tx_disable()``, ``pon_optics_start()`` and, from
       ``rtl9602c_gpon_power_work()``, ``pon_optics_set_power_level()`` of the
       optics (``gn25l95_tx_disable()``, ``gn25l95_start()`` and
       ``gn25l95_set_power_level()``). The ``ponpath`` view of the PON MAC calls
       ``rtl9602c_gpon_port_get()``, ``rtl9602c_gpon_cntr_sync()`` and
       ``rtl9602c_gpon_flow_cntr()``.

Locks
-----

The state of the block is kept in ``struct rtl9602c_priv`` and protected by
three locks.

.. list-table::
   :header-rows: 1
   :widths: 20 14 66

   * - Lock
     - Type
     - What it serialises
   * - ``gpon_lock``
     - mutex
     - PLOAM dispatch from the interrupt thread and the worker, the Alloc-ID and
       Port-ID tables, bring-up and take-down, and the status report.
   * - ``ploam_lock``
     - mutex
     - The writers of upstream PLOAM messages.
   * - ``ploam_log_lock``
     - spinlock
     - The ``ploamlog`` ring.

Interfaces to user space
------------------------

- ``ip link set pon up`` starts the activation and ``ip link set pon down``
  stops it. Bringing the interface up fails with ``ENXIO`` until a serial number
  has been set. The driver keeps the interface dormant in every state but O5;
  that ``ip link show pon`` then shows it so is not verified.
- The devlink parameter ``gpon_serial`` on ``platform/1b000000.switch`` sets the
  serial number, and ``devlink dev info`` reports the MAC's revision word as the
  fixed version ``gpon.mac``. The parameter ``gpon_disabled`` reads whether the
  OLT has disabled the ONU, and sets it before the PON comes up: user space
  keeps it across a restart. The parameter list is on
  Documentation/networking/devlink/rtl9602c.rst.
- Through the PON class (Documentation/networking/pon.rst): the OMCI port
  ``/dev/pon0omci0``, which carries the OMCC; the ``gem`` interfaces; and the
  ``pon`` generic netlink family, which reports the activation state, the
  ONU-ID, the Alloc-IDs, the OMCC's Port-ID, the alarms, the counters and the
  serial number, and announces changes.
- Seven debugfs views in ``/sys/kernel/debug/rtl9602c/``: ``gpon``, ``gpontbl``,
  ``ploamlog``, ``bwmap``, ``usburst``, ``uscntr`` and ``gemusintr`` (see "The
  debugfs views"); with ``CONFIG_PON_RTL9602C_PLOAM_INJECT``, a test
  image's option, also the file ``ploaminject``.
- The kernel log: the lines of this block begin ``rtl9602c-switch: <GPON>`` (see
  "The kernel log").

The serial number is a per-device setting. An example in the form the driver
accepts, with a made-up number::

    devlink dev param set platform/1b000000.switch name gpon_serial \
            value ABCD0000A001 cmode runtime
    ip link set pon up

The kernel log
--------------

The lines of this block begin ``rtl9602c-switch: <GPON>``, with exceptions noted
in the table. The transitions and the reports of the bring-up are at information
level. The per-message trace of the PLOAM channel is at debug level: dynamic
debug switches it on, with ``echo 'file rtl9602c-gpon.c +p' >
/sys/kernel/debug/dynamic_debug/control``. It prints before the message is acted
on, and a console line costs about 9 ms at 115200 baud, against the 750 us that
G.984.3 gives the ONU. The table gives the lines a maintainer meets; the facts
name the others.

.. list-table::
   :header-rows: 1
   :widths: 46 14 40

   * - Line, after the prefix
     - Level
     - Meaning
   * - ``<GPON> MAC rev 0x11110320, self test passed, data retention included``
     - info
     - The boot report (see "The register block").
   * - ``<GPON> MAC: nothing answers below the top level yet``, or ``<GPON> MAC:
       the transmission-convergence layer is answering (0x...)``
     - info
     - The boot report: the MAC is in reset, or it is running.
   * - ``<GPON> MAC brought up``
     - info
     - The bring-up returned 0.
   * - ``the PON has no serial number: set one before bringing the interface
       up``
     - error
     - Bring-up refused with ``-ENXIO``. No ``<GPON>`` tag.
   * - ``PON MAC not built (err): the PON stays down``
     - error
     - The PON MAC failed to build, and the bring-up fails. No ``<GPON>``
       tag.
   * - ``<GPON> MAC did not leave reset (reset control ...)``
     - error
     - The MAC reset timed out.
   * - ``<GPON> tables emptied: N Alloc-IDs (...), N Port-IDs, N multicast``
     - info
     - The tables held entries at bring-up.
   * - ``<GPON> O2 standby -> O3 serial-number``, and the other transitions;
       ``<GPON> O5 operation -> O2 standby after S.mmm s in O5``
     - info
     - A state change, logged after it is made.
   * - ``<GPON> burst: guard ...``, ``<GPON> Upstream_Overhead raw: ...``
     - info
     - Upstream_Overhead was acted on in O2.
   * - ``<GPON> EQD: d + c * 128 = s -> mf m, in-frame b``
     - info
     - The equalization delay was written.
   * - ``<GPON> burst length: p B pre-ranging, r after; overhead x/y``
     - info
     - Extended_Burst_Length was acted on in O3.
   * - ``<GPON> 10 serial-number requests answered and no ONU-ID: transmit
       power level mode m -> n``
     - info, rate-limited
     - The ONU's own levelling took the next mode in O3.
   * - ``<GPON> Change_Power_Level up: transmit power level mode m -> n``, or
       ``down``, or ``neither``
     - info
     - Change_Power_Level was acted on in O4 or O5.
   * - ``<GPON> transmit power level mode n not set (err); the ONU stays at
       mode m``
     - warning, rate-limited
     - The optics did not set the mode asked for; ``-61`` before O4 when they
       have not measured it since probe.
   * - ``<GPON> ONU-ID N assigned; written T us after the interrupt``
     - info
     - Assign_ONU-ID was acted on in O3, its writes done T us after the
       interrupt.
   * - ``<GPON> Ranging_Time raw: ... -- eqd N``
     - info
     - Ranging_Time took the ONU from O4 to O5.
   * - ``<GPON> BER interval N ms: an REI every interval`` or ``no REI``
     - info
     - BER_Interval was acted on.
   * - ``<GPON> Ranging_Adjustment: eqd A -> B``
     - info
     - Ranging_Adjustment changed the equalization delay; the ``EQD`` line
       follows.
   * - ``<GPON> Ranging_Adjustment out of range, not applied: ...``
     - warning
     - The change would take the delay below 0 or out of the register.
   * - ``<GPON> <message> is not supported, ignored: ...``
     - info
     - The first message of a type the driver does not act on.
   * - ``<GPON> key N sent`` or ``<GPON> key N was not taken by the MAC``
     - info, warning
     - Request_Key was answered; the second when a key word did not complete.
   * - ``<GPON> the keys were not cleared``
     - warning
     - At bring-up or when the ONU gave up its ONU-ID, a word of the zero keys
       did not complete.
   * - ``<GPON> GEM port N encrypted`` or ``no longer encrypted``
     - info
     - Encrypted_Port-ID changed the Port-ID's state.
   * - ``<GPON> the OLT deactivated this ONU in O5``
     - info
     - Deactivate_ONU-ID was acted on.
   * - ``<GPON> TO1 expired in O4; trying again``
     - info
     - TO1 took the ONU back to O2.
   * - ``<GPON> the OLT disabled this ONU`` or ``<GPON> the OLT enabled this
       ONU again``
     - info
     - Disable_Serial_Number took the ONU to O7, or out of it to O2.
   * - ``<GPON> the OLT disabled this ONU before: it stays in O7 until the OLT
       enables it``
     - info
     - The bring-up ended in O7, with ``gpon_disabled`` set.
   * - ``<GPON> kept disabled: the PON comes up in O7`` or ``<GPON> no longer
       kept disabled``
     - info
     - ``gpon_disabled`` was set or cleared while the PON was down.
   * - ``<GPON> enabled by hand, not by the OLT``
     - info
     - ``gpon_disabled`` was cleared while the PON was up.
   * - ``<GPON> signal detect disagrees with the burst (latch 0x...)``
     - warning, rate-limited
     - The upstream latch reported bit 8 or 9. The device name replaces the
       prefix.
   * - ``<GPON> upstream FEC on`` or ``<GPON> upstream FEC off``
     - info
     - The upstream FEC state the MAC reports changed; once a change, from off
       at each bring-up (see "FEC and encryption").
   * - ``<GPON> upstream PLOAM queue N full``
     - warning, rate-limited
     - A message was refused with ``-EBUSY``. The device name replaces the
       prefix.
   * - ``<GPON> serial number set``
     - info
     - The devlink parameter was accepted.

Where to read next
------------------

The other pages of the port, for what this page leaves to them:

- Documentation/networking/device_drivers/pon/rtl9602c.rst: the PON MAC and the
  packet buffer, the upstream streams, queues and T-CONT scheduler, the GEM
  ports as network interfaces and the OMCI path.
- Documentation/networking/device_drivers/pon/gn25l95.rst: the I2C
  masters of the optics, the laser driver's pages, the digital diagnostics,
  TX_DISABLE and the loss-of-signal input.
- Documentation/networking/pon.rst: the PON class, its OMCI port, the ``gem``
  interfaces and the ``pon`` netlink family.
- Documentation/networking/dsa/rtl9602c.rst: the switch, its register window,
  its interrupt and the device tree node.
- Documentation/networking/devlink/rtl9602c.rst: the devlink parameters, the
  region and the info reply.
- Documentation/networking/device_drivers/ethernet/realtek/rtl9602c.rst: the CPU
  port and the OMCI frames that reach it.
- Documentation/arch/mips/rtl9602c.rst: the CPU core, the platform and the
  board.

How to read the fact tables
===========================

Each block below has a short account of what the hardware does and what the
driver does with it, followed by fact tables with three columns: *Fact*, *Tag*
and *Checked by*. The tag says how the fact is established. **M**, measured on
the board (the DFP-34X-2C3): *Checked by* gives the command or observation,
through the driver's own interfaces (the kernel log, the debugfs views, ``ip``,
``devlink``), or the register reads that show it. **T**, tested by function: the
driver depends on the fact, and the named behaviour fails if it is wrong. **P**,
public standard: *Checked by* names the document and the clause. **I**,
inferred, not verified: the fact is an assumption, and *Checked by* says what
would confirm or refute it. Where the driver writes a value that nothing shows
to be needed, *Required: unverified* means that removing the write and repeating
the named behaviour is what would settle it. The last chapter collects the **I**
facts and says which of them the driver depends on.

A row with two tags is established in two ways, or in two parts; *Checked by*
says which part carries which tag. Register offsets are from the base of the
switch register window, physical ``0x1b000000``, and values are 32-bit words as
``readl()`` and ``writel()`` see them on this big-endian CPU. Bit positions are
those of the macros. PLOAM octets are numbered as G.984.3 numbers them: octet 1
is the ONU-ID, octet 2 the message identifier, octets 3 to 12 the data and octet
13 the CRC. The driver's ``RTL9602C_PLOAM_*`` offset macros count from 0, so the
offset n is octet n + 1. "The test OLT" is the OLT that the board was measured
against; what a row says it sent is what the driver received from it, not a
requirement.

The behaviours that a tested fact depends on are named in words: the board boots
to a shell; the downstream locks, and the ONU reaches O5 on an OLT and holds it;
OMCI messages pass both ways over ``/dev/pon0omci0``; the optics diagnostics
read, the laser is off before O3 and TX_DISABLE is honoured. Four further checks
ran with traffic on the board: downstream GEM frames reach the SFP port;
upstream GEM frames reach the OLT while the ONU's state is held; OMCI messages
keep being answered while data flows; and a routed flow leaves by a GEM port on
the T-CONT whose Alloc-ID the OLT assigned, which shows that the bandwidth map
grants that T-CONT. "Every debugfs view reads" means that each view of this page
can be read without an error.

The register block
==================

The GPON MAC occupies offsets ``0x700000`` to ``0x706cfc`` of the switch's
register window. Its top region, ``0x700000`` to ``0x700280``, answers at any
time: it holds the reset control, the revision word, the block's interrupt
enable and status, and the result of a memory self-test. Every other register,
from ``0x701000`` up, answers only once the MAC has been taken out of reset;
until then a read returns ``RTL9602C_GPON_NO_RESPONSE``, which is the bus's
reply when nothing decodes the access and not register content. Reading it is
harmless. The table gives the layout in outline; the registers are named where
the facts use them.

.. list-table::
   :header-rows: 1
   :widths: 14 24 62

   * - Offsets
     - Sub-block
     - What the driver uses there
   * - ``0x700000``
     - Top region
     - Reset control, revision, interrupt enable and status, memory self-test.
   * - ``0x701000``
     - Downstream GTC and GEM
     - The downstream interrupt latch, enable and status; ONU control (state and
       ONU-ID); downstream control; LOS; the superframe counter; the PLOAM
       receive queue; the Alloc-ID and Port-ID tables; the per-flow counters;
       the GTC counters from ``0x701180`` to ``0x7011fc``; the OMCI PTI match;
       the frame-derived pulse; and each flow's downstream type from
       ``0x701400``.
   * - ``0x702000``
     - Bandwidth-map capture
     - Capture control, status and data.
   * - ``0x704000``
     - Downstream GEM filter
     - The multicast filter and the multicast table.
   * - ``0x705000``
     - Upstream GTC
     - The upstream interrupt latch and enable; the upstream ONU-ID and control;
       the write key; the response and equalization delays; the laser timing;
       the burst overhead; the PLOAM transmit slots; the upstream counters; the
       signal-detect limits; the transmit mode and the power save.
   * - ``0x706000``
     - Upstream GEM
     - The upstream GEM interrupt latch and status; the PTI codes; the power
       save; the burst merge threshold; each flow's upstream Port-ID from
       ``0x706400`` and upstream octet counter from ``0x706800``.

After a cold start the boot report has two lines, each with the prefix
``rtl9602c-switch:``. The prefix is left out here::

    <GPON> MAC rev 0x11110320, self test passed, data retention included
    <GPON> MAC: nothing answers below the top level yet

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The GPON MAC occupies offsets ``0x700000`` to ``0x706cfc`` of the switch
       register window. Its top region, ``0x700000`` to ``0x700280``, holds the
       reset control ``RTL9602C_GPON_RESET_CTRL`` (``0x70000c``), the revision
       word ``RTL9602C_GPON_REVISION`` (``0x700010``), the interrupt enable
       ``RTL9602C_GPON_IRQ_EN`` (``0x700040``) and status
       ``RTL9602C_GPON_IRQ_STAT`` (``0x700044``), and the memory self-test
       result ``RTL9602C_GPON_MEM_SELFTEST`` (``0x700280``). The GTC, GEM,
       bandwidth-map and table registers start at ``0x701000``.
     - M
     - Register reads through the switch window: the whole range read in one
       pass without a hang, and two passes read alike.
   * - The top region answers at any time. From ``0x701000`` up every register
       returns ``RTL9602C_GPON_NO_RESPONSE`` (``0xca0eca0f``) until the MAC has
       been taken out of reset (see "Bring-up and take-down"): the bus's reply
       when nothing decodes the access, not register content. Reading it is
       harmless. Once the MAC runs, every register answers.
     - M
     - With the MAC never brought up, eight top-region registers answered and
       the 1000 words from ``0x701000`` up all read ``0xca0eca0f``; with the ONU
       in O5, all of them answered and none read the pattern.
   * - ``RTL9602C_GPON_REVISION`` reads ``0x11110320`` on this chip.
       ``rtl9602c_gpon_report()`` prints it at setup, and
       ``rtl9602c_devlink_info_get()`` reports it as the fixed version
       ``gpon.mac``.
     - M
     - The register read; ``devlink dev info platform/1b000000.switch`` shows
       ``gpon.mac 0x11110320``.
   * - ``RTL9602C_GPON_MEM_SELFTEST`` reads ``0x00000007`` at setup on a working
       chip: ``RTL9602C_GPON_MEM_SELFTEST_DONE`` (bit 0) and
       ``RTL9602C_GPON_MEM_SELFTEST_RETAIN_DONE`` (bit 1) among the set bits,
       ``RTL9602C_GPON_MEM_SELFTEST_FAIL`` (bit 3) and
       ``RTL9602C_GPON_MEM_SELFTEST_RETAIN_FAIL`` (bit 4) clear.
     - M
     - The boot log line ``MAC rev ..., self test passed, data retention
       included``.
   * - The bits mean: the memory test finished, the data-retention test
       finished, the memory test failed, the retention test failed. A set
       failure bit has never been seen.
     - I
     - A part with a failing MAC memory. Only log lines depend on it.
   * - ``rtl9602c_gpon_report()``, called once at switch setup, prints an error
       when either failure bit is set, a warning when
       ``RTL9602C_GPON_MEM_SELFTEST_DONE`` is clear, and otherwise the revision,
       adding "data retention included" when
       ``RTL9602C_GPON_MEM_SELFTEST_RETAIN_DONE`` is set. It then reads
       ``RTL9602C_GTC_DS_IRQ_STAT`` and prints whether the transmission
       convergence layer answers, that is whether the value is anything but
       ``RTL9602C_GPON_NO_RESPONSE``. It writes nothing.
     - T
     - The board boots to a shell: both lines are in the boot log.

Bring-up and take-down
======================

``ip link set pon up`` brings the GPON MAC up from whatever state the hardware
was left in. The chip may have been reset by the watchdog or restarted by a warm
reset, the MAC may still hold an activation, and its tables keep their contents
through the MAC reset and the chip reset. The bring-up therefore starts from O1,
builds the PON MAC, puts the PON SerDes in GPON mode, resets the MAC, empties
its tables, and only then programs the registers of the activation path,
enabling the interrupt last. It runs under ``gpon_lock``. If it fails the
interface does not open.

Bring-up, in order
------------------

The steps of ``rtl9602c_gpon_bringup()``, in order. The tags in parentheses are
those of the facts that carry each step.

1. Set O1. Purpose: end an activation that survived a warm reset, which restarts
   the CPU but not the GPON MAC. (M)
2. Build the PON MAC (``rtl9602c_ponmac_init()``, see
   Documentation/networking/device_drivers/pon/rtl9602c.rst). Purpose: every
   OMCI message crosses it. A failure ends the bring-up with its error, the
   PON staying down: activation does not depend on the PON MAC, but an ONU
   without it answers no OMCI and the OLT deactivates it. That the PON MAC has
   to be built before step 3 is an assumption. (M for the need, I for the
   order)
3. Put the PON SerDes in GPON mode (``rtl9602c_pon_serdes_gpon()``, see "PON
   SerDes and analog front end"): park it, reset it, enable the burst-enable
   output driver, set GPON mode, override the bit-error indication and force no
   burst enable. Purpose: the MAC reset of step 5 completes only once the SerDes
   is in GPON mode. (M for the mode and the burst-enable force, I for the rest)
4. Write the analog front end (``rtl9602c_pon_analog_init()``), ending with the
   burst-enable path out of power-down. Purpose: the receiver recovers the clock
   and detects signal, and the burst output works. (M for the signal-detect bit,
   I for the rest)
5. Reset the MAC (``rtl9602c_gpon_reset()``). Purpose: from here the GTC
   registers answer. A timeout fails the bring-up. (M)
6. Empty the Alloc-ID, Port-ID and multicast tables
   (``rtl9602c_gpon_tables_clear()``). Purpose: neither the MAC reset nor the
   chip reset clears them, and what an earlier run left in them breaks the next
   activation. (M)
7. Program the GEM ports that exist again (``rtl9602c_gem_restore()``). Purpose:
   they live in the tables that step 6 emptied. (M)
8. Let the optics prepare the transmitter (``pon_optics_start()``: the
   modulation hold of Documentation/networking/device_drivers/pon/gn25l95.rst).
   Purpose: before the laser is first enabled in O3. A failure is logged and
   bring-up goes on. (I)
9. Set the burst merge threshold. (I)
10. Write the response delay under the write key. Purpose: upstream timing. (M)
11. Write the period of the frame-derived pulse. (I)
12. Set the signal-detect mismatch limit. (I)
13. Enable the optical LOS input and select its polarity. Purpose: downstream
    lock. (M)
14. Write the FEC detection threshold. (I)
15. Keep only the bandwidth-map allocations for this ONU. Purpose: the ONU
    answers the discovery grants and its own grants only. (M)
16. Write the OMCI PTI match. Purpose: the OMCC downstream. (T)
17. Write the downstream GEM filter. Purpose: GEM data downstream. (T)
18. Enable the downstream GTC interrupts, then the two sub-blocks at the top
    level. Purpose: PLOAM messages from the interrupt. (T, M)
19. Write the laser lead and lag. Purpose: burst timing. (M)
20. Enable the upstream GTC interrupts. Purpose: among them a change of the
    upstream FEC state (see "FEC and encryption"). (M, I for the FEC event)
21. Enable the burst power save, in the GTC under the write key and then in the
    GEM framer. (I)
22. Write the upstream PTI codes. Purpose: GEM data upstream. (T)
23. Write the upstream control word under the write key. Purpose: the PLOAMu
    field, the scrambler and the burst-enable polarity. (T)
24. Write the transmit mode under the write key. Purpose: bursts that start in
    the previous frame. (M)
25. Keep only the allocations for this ONU, once more. (M)
26. Mark the ONU-ID unassigned and set O2. Purpose: wait for Upstream_Overhead.
    (P)
27. Arm the No message, Dying_Gasp and Serial_Number_ONU templates. Purpose:
    they are ready before the first discovery grant. (M, P)
28. Enable the GPON source of the switch interrupt and run the worker at once.
    Purpose: activation starts, and the first PLOAM message dispatched finds
    every register of the activation path programmed. (T)

The steps tagged M are measured, each in the facts of the section that describes
it. The steps tagged T are checked by the ONU reaching O5 on an OLT and OMCI
messages passing both ways over ``/dev/pon0omci0``. The steps tagged I are
written on inference; the last chapter lists them.

The MAC reset
-------------

The MAC reset, ``rtl9602c_gpon_reset()``, is a pulse on one bit and a wait for
another:

1. Set ``RTL9602C_GPON_RESET_CTRL_TRIGGER`` (bit 0 of
   ``RTL9602C_GPON_RESET_CTRL``). Purpose: start the reset. (M)
2. Clear the bit again. (M)
3. Poll ``RTL9602C_GPON_RESET_CTRL_DONE`` (bit 8) every 100 us for at most 20
   ms. Purpose: the GTC registers answer once it is set. A timeout is logged
   with the register's value (``MAC did not leave reset``) and fails the
   bring-up. (M)

Take-down and the facts of the bring-up
---------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``ip link set pon up`` reaches ``rtl9602c_port_enable()``, which calls
       ``rtl9602c_gpon_start()``. It returns ``-ENXIO`` while no serial number
       has been set; 0 at once when the PON is already up, because a second
       bring-up would reset the MAC under a live activation; otherwise it zeroes
       the running totals of the counters and runs ``rtl9602c_gpon_bringup()``.
       The PON counts as up only when that returns 0, and an error fails the
       interface's open.
     - M
     - With no serial number set, ``ip link set pon up`` failed with ``ENXIO``;
       with one set, the interface came up and the ONU reached O5.
   * - ``ip link set pon down`` reaches ``rtl9602c_port_disable()``, which calls
       ``rtl9602c_gpon_stop()``: the state machine goes to O1, which asserts
       TX_DISABLE and gives back what O4 and O5 built (see "The activation state
       machine"), and the PON is marked down. The MAC is not reset; the GPON
       interrupt source and the worker stay active, and in O1 no received
       message is acted on.
     - M
     - ``down`` took the ``onu`` line of the ``gpon`` view from ``00000105``
       (O5, ONU-ID 1) to ``0000ff01`` (O1, ONU-ID 255), and ``up`` walked O3 and
       O4 back to O5; from a cold boot a down and up re-ranged in 1 to 3 s, 20
       times of 20.
   * - A warm reset restarts the CPU but not the GPON MAC, which keeps the
       previous activation: ranged, in O5, transmitting in its bursts. The PON
       MAC cannot be built beneath a MAC in that state, because the drain of the
       OMCC's queue that its build starts with does not complete. Bring-up
       therefore begins by setting O1.
     - M
     - The drain completed in 0 us after cold boots and was still busy 0.5 s
       later after every warm reset (4 of 4); the ONU still reached O5 and no
       OMCI message crossed; ``down`` then ``up``, which sets O1 first, repaired
       it.
   * - The PON MAC (``rtl9602c_ponmac_init()``) is built next, and a failure
       ends the bring-up: activation does not depend on the PON MAC, but every
       OMCI message does, and the OLT deactivates an ONU that answers none.
     - M
     - With the PON MAC never built, the ONU reached O5, no OMCI message reached
       the CPU, and the OLT sent Deactivate_ONU-ID about 12 s later.
   * - The PON MAC is built before the SerDes is put in GPON mode, not after.
     - I
     - Required: unverified; reversing the two and checking that OMCI messages
       pass both ways.
   * - The MAC reset completes only once the SerDes is in GPON mode, so it comes
       after it. Afterwards ``RTL9602C_ONU_CTRL`` reads ``0x0000ff01``: ONU-ID
       255, O1.
     - M
     - With the SerDes in GPON mode the reset completed and the ``onu`` line
       read ``0000ff01``; without it the trigger bit read back set and the done
       bit stayed clear through 1000 polls.
   * - The GEM ports that exist are programmed again
       (``rtl9602c_gem_restore()``, see
       Documentation/networking/device_drivers/pon/rtl9602c.rst) right after the
       tables are emptied, because they live in those tables.
     - M
     - After a PON down and up, a GEM port created before it was restored, and
       its upstream restarted when the Alloc-ID was assigned again (the kernel
       log line ``upstream started``).
   * - The optics' modulation is held (``pon_optics_start()``, see
       Documentation/networking/device_drivers/pon/gn25l95.rst) before the
       laser is first enabled in O3; a failure is logged and bring-up goes
       on.
     - I
     - Whether activation needs the hold is open: activations have been clean
       with the hold on and off.
   * - The GPON source of the switch interrupt (``RTL9602C_IRQ_SRC_GPON``, bit
       10 of ``RTL9602C_IRQ_ENABLE``, ``0x01d00c``) is enabled last and the
       worker is run at once, so that the first PLOAM message dispatched finds
       every register of the activation path programmed.
     - T
     - The downstream locks and the ONU reaches O5 on an OLT.

PON SerDes and analog front end
===============================

The PON SerDes sits between the optics and the GPON MAC. Its analog front end
holds the receiver, the clock recovery, the line-rate profile and the
burst-enable output to the optics. Its registers are in the switch's register
window, outside the GPON MAC's range: ``0x0001d0``, ``0x000104`` and the SerDes
block from ``0x022034`` to ``0x022a74``. The bring-up writes them in two
functions. ``rtl9602c_pon_serdes_gpon()`` parks the SerDes, resets it and puts
it in GPON mode. ``rtl9602c_pon_analog_init()`` then writes the analog front
end: five tuning words, the clock recovery (CDR) with its loop setting, the
line-rate profile, the burst-enable output level, the transmit amplitude and the
receiver enable, and at the end takes the burst-enable path out of power-down.

Two of these are known to matter: the GPON mode, without which the MAC reset
does not complete, and the state of ``RTL9602C_SERDES_ANA_RX_CFG_SD_POR``, which
has to be set for the downstream to frame with a module whose loss-of-signal
output has its maker's polarity. The
others are written because the
downstream locks and the ONU reaches O5 with all of them in place; none has been
removed on its own to see whether it is needed, and the rows say so.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Park the PON SerDes: ``RTL9602C_SERDES_PON_CTRL_MODE`` (bits 4:0 of
       ``RTL9602C_SERDES_PON_CTRL``, ``0x0001d0``) is set to
       ``RTL9602C_SERDES_PON_CTRL_MODE_OFF`` (``0x1f``), then
       ``RTL9602C_SERDES_PON_RESET`` (``0x022034``) is written 0, so that the
       analog side is configured while it is stopped.
     - I
     - Required: unverified; the ONU reaches O5 on an OLT.
   * - Set ``RTL9602C_CHIP_RESET_PON_SERDES`` (bit 0 of ``RTL9602C_CHIP_RESET``,
       ``0x000104``) and wait 10 to 11 ms; the driver never clears the bit. It
       resets the PON SerDes; that it resets the GPON MAC as well is not
       established.
     - I
     - A MAC register written before this step and read after it.
   * - Set ``RTL9602C_SERDES_PON_OPTIC_CTRL_BURST_OE`` (bit 12 of
       ``RTL9602C_SERDES_PON_OPTIC_CTRL``, ``0x022090``), the output driver of
       the burst-enable signal to the optics; clear
       ``RTL9602C_SERDES_PON_TX_CFG_BURST_DLY`` (bits 6:4, the burst enable's
       delay against the data) and ``RTL9602C_SERDES_PON_TX_CFG_D2A_SEL`` (bits
       3:0, the transmitter's data source) of ``RTL9602C_SERDES_PON_TX_CFG``
       (``0x02203c``).
     - I
     - Required: unverified; the ONU reaches O5 on an OLT and the OLT hears its
       bursts.
   * - ``RTL9602C_SERDES_PON_CTRL_MODE`` is set to
       ``RTL9602C_SERDES_PON_CTRL_MODE_GPON`` (``0x08``): the PON SerDes in GPON
       mode. The MAC reset completes only after it.
     - M
     - The MAC reset completes with it and not without it (see "Bring-up and
       take-down").
   * - Set ``RTL9602C_SERDES_ANA_FORCE_BER_VAL`` (bit 13) and
       ``RTL9602C_SERDES_ANA_FORCE_BER_EN`` (bit 12) of
       ``RTL9602C_SERDES_ANA_FORCE_BER`` (``0x022508``), an override of the
       SerDes's bit-error indication.
     - I
     - Required: unverified; the ONU reaches O5 on an OLT.
   * - Clear ``RTL9602C_SERDES_PON_BURST_FORCE_EN`` (bit 0 of
       ``RTL9602C_SERDES_PON_BURST_FORCE``, ``0x0220e4``), so that no burst
       enable is forced. Set, it does not make the module transmit; what it
       forces is not established.
     - M
     - With the bit set and TX_DISABLE released, the module's own diagnostics
       (see Documentation/networking/device_drivers/pon/gn25l95.rst)
       read zero bias and zero power; forcing the module's burst enable instead
       read 14.1 mA and +2.3 dBm.
   * - Write whole, in this order: ``RTL9602C_SERDES_ANA_TUNE2`` (``0x022588``)
       = ``RTL9602C_SERDES_ANA_TUNE2_INIT`` (``0x6df8``);
       ``RTL9602C_SERDES_ANA_PLL_CFG`` (``0x02258c``, the clock multiplier) =
       ``RTL9602C_SERDES_ANA_PLL_CFG_INIT`` (``0x8941``);
       ``RTL9602C_SERDES_ANA_TUNE8`` (``0x0225a0``) =
       ``RTL9602C_SERDES_ANA_TUNE8_INIT`` (``0x0713``);
       ``RTL9602C_SERDES_ANA_TUNE25`` (``0x0225e4``) =
       ``RTL9602C_SERDES_ANA_TUNE25_INIT`` (``0x001f``);
       ``RTL9602C_SERDES_ANA_TUNE24`` (``0x0225e0``) =
       ``RTL9602C_SERDES_ANA_TUNE24_INIT`` (``0x8001``).
     - I
     - Required: unverified; the ONU reaches O5 on an OLT with them.
   * - ``RTL9602C_SERDES_ANA_CDR_LOOP_1G25`` (``0x0226b8``),
       ``RTL9602C_SERDES_ANA_CDR_LOOP_EPON`` (``0x0227b8``) and
       ``RTL9602C_SERDES_ANA_CDR_LOOP_GPON`` (``0x022738``) are each set to
       ``RTL9602C_SERDES_ANA_CDR_LOOP_INIT`` (``0x80c5``): one clock-recovery
       loop-filter setting for all three line-rate profiles, read as integral
       gain 1 and proportional gains 4 and 2.
     - I
     - The value and its decoding are unverified; the ONU reaches O5 on an OLT
       with them.
   * - Set ``RTL9602C_SERDES_ANA_RX_CFG_CDR_AFE`` (bit 14 of
       ``RTL9602C_SERDES_ANA_RX_CFG``, ``0x0225b0``), the enable of the clock
       recovery's analog front end, in the same write that sets bit 15
       (``RTL9602C_SERDES_ANA_RX_CFG_SD_POR``, see "Downstream lock, LOS and
       alarms").
     - I
     - Required: unverified; the ONU reaches O5 on an OLT.
   * - ``RTL9602C_SERDES_ANA_FORCE_RATE`` (``0x022504``):
       ``RTL9602C_SERDES_ANA_FORCE_RATE_VAL`` (bits 7:5) is set to
       ``RTL9602C_SERDES_ANA_FORCE_RATE_VAL_GPON`` (1) and
       ``RTL9602C_SERDES_ANA_FORCE_RATE_EN`` (bit 4) is set: the analog
       line-rate profile is forced to GPON's 2.48832 Gbit/s downstream and
       1.24416 Gbit/s upstream instead of following the digital side.
     - I
     - Required: unverified; the register reads ``0x30`` after bring-up and 0
       before.
   * - Set ``RTL9602C_SERDES_ANA_BURST_OUT_TTL`` (bit 14 of
       ``RTL9602C_SERDES_ANA_BURST_OUT``, ``0x022584``): the burst-enable output
       to the optics at TTL level.
     - I
     - Required: unverified; the register reads ``0x73a4`` after bring-up and
       ``0x33a4`` before.
   * - ``RTL9602C_SERDES_ANA_TX_CFG_AMP`` (bits 5:3 of
       ``RTL9602C_SERDES_ANA_TX_CFG``, ``0x0225d8``) is set to
       ``RTL9602C_SERDES_ANA_TX_CFG_AMP_GPON`` (5), the upstream transmit
       amplitude; it resets to 7.
     - I
     - Required: unverified; the register reads ``0x39`` before and ``0x29``
       after.
   * - Clear ``RTL9602C_SERDES_ANA_RX_FILTER_CFG`` (bits 7:0 of
       ``RTL9602C_SERDES_ANA_RX_FILTER``, ``0x0225ac``), the receiver filter
       configuration.
     - I
     - Required: unverified; the ONU reaches O5 on an OLT.
   * - Set ``RTL9602C_SERDES_ANA_CDR_CTRL_RESET`` (bit 14) and
       ``RTL9602C_SERDES_ANA_CDR_CTRL_LPF_EN`` (bit 10) of
       ``RTL9602C_SERDES_ANA_CDR_CTRL`` (``0x0225cc``), manual controls of the
       clock recovery's reset and loop filter. Which level of the reset bit
       holds the clock recovery in reset is not known.
     - I
     - Required, and the polarity, unverified; the ONU reaches O5 on an OLT.
   * - ``RTL9602C_SERDES_ANA_FORCE_RX`` (``0x022500``) is written
       ``RTL9602C_SERDES_ANA_FORCE_RX_OFF`` (``0x0010``), then
       ``RTL9602C_SERDES_ANA_FORCE_RX_ON`` (``0x0030``), then the driver waits
       50 ms: the receiver-enable override on with the receiver off, then on.
     - I
     - Required: unverified; the ONU reaches O5 on an OLT.
   * - Last, ``RTL9602C_SERDES_PON_PDOWN_BURST`` (bit 10 of
       ``RTL9602C_SERDES_PON_PDOWN``, ``0x022038``) is cleared: the burst-enable
       path is out of power-down.
     - I
     - Required: unverified; the ONU reaches O5 on an OLT and the OLT hears its
       bursts.
   * - Written on their own, the tuning words, the loop setting, the clock
       recovery's front-end enable, the receiver filter, the manual
       clock-recovery controls, the receiver-enable override and the burst-path
       power-down changed no reading. The downstream framed only once the
       line-rate profile, the burst-enable output level, the transmit amplitude,
       the optical LOS input (see "Downstream lock, LOS and alarms"), the FEC
       detection threshold, the OMCI PTI match and the interrupt enables were
       written as well. It framed in that measurement with
       ``RTL9602C_SERDES_ANA_RX_CFG_SD_POR`` set (see the next section).
     - M
     - The ``gpon`` view and register reads before and after each group of
       writes.

Downstream lock, LOS and alarms
===============================

A downstream GTC frame lasts 125 us. The receiver recovers the clock from the
2.48832 Gbit/s stream, the MAC finds the frame by its PSync field and tracks the
superframe counter in the Ident field, and from then on it reads the PLOAM
channel and the bandwidth map of every frame. The driver's part is to set the
SerDes up (see the previous section), to let the optical module's LOS input act
on the receiver, and to read the result: the superframe counter advances 8000
times a second while the downstream is framed, and the levels in
``RTL9602C_GTC_DS_IRQ_STAT`` say whether the MAC reports LOS, LOF or LOM, and
whether the line carries FEC.

To see whether the downstream is framed, read the ``gpon`` view twice, ten
seconds apart::

    cat /sys/kernel/debug/rtl9602c/gpon
    sleep 10
    cat /sys/kernel/debug/rtl9602c/gpon

The ``frame`` line advances by about 80000 while the downstream is framed and
stands still otherwise; ``signok 1`` on the ``sig`` line says that the receiver
detects a signal.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A downstream GTC frame lasts 125 us (38880 bytes at 2.48832 Gbit/s). The
       Ident field of its PCBd carries a 30-bit superframe counter, one larger
       in each frame, which the ONU tracks with a local counter.
     - P
     - ITU-T G.984.3 clause 8.1 and clause 8.1.3.2.
   * - ``RTL9602C_GTC_DS_SUPERFRAME`` (``0x701048``), field
       ``RTL9602C_GTC_DS_SUPERFRAME_CNT`` (bits 29:0), follows the received
       superframe counter: it advances 8000 times a second while the downstream
       is framed and not at all otherwise. The ``gpon`` view prints it on its
       ``frame`` line, read from the register at every read.
     - M
     - The ``frame`` line read twice: 7996.7 to 8004.7 a second over five 10 s
       intervals against the uptime clock, and no advance before the downstream
       framed; 80169 in 10.0 s.
   * - ``RTL9602C_SERDES_PON_RX_STAT`` (``0x022a74``):
       ``RTL9602C_SERDES_PON_RX_STAT_SIGNAL`` (bit 8) is 1 while the PON
       receiver detects a signal. The ``gpon`` view prints it on its ``sig``
       line as ``signok``.
     - M
     - The ``sig`` line read 0 and then 1 across the change of the signal-detect
       bit in the next row, the OLT transmitting.
   * - ``RTL9602C_SERDES_ANA_RX_CFG_SD_POR`` (bit 15 of
       ``RTL9602C_SERDES_ANA_RX_CFG``) must be set for a module whose
       loss-of-signal output has its maker's polarity (the optics' A2h table 2
       byte BBh, ``LOS_POLARITY`` bit 0, clear): clear, the receiver's signal
       detect reports no signal and the downstream does not frame. The driver
       sets it in the analog set-up.
     - M
     - One stick, one fibre, one OLT, only this bit changed: set, the ONU
       reached O5 4.7 s after the kernel started, ``signok`` 1 and the
       superframes counting; clear, ``signok`` 0, the ``frame`` line still and
       the ONU in O1 with LOS, LOF and LOM.
   * - A module whose BBh had been corrupted to ``0xa1`` (``LOS_POLARITY`` set,
       the output as signal detect) framed only with the bit clear. Its table 2
       also held ``0xa1`` or ``0xa0`` at B8h, BDh and C1h, where an intact
       module of the same model holds ``00``, ``3d`` and ``08``.
     - M
     - On that module, with the OLT transmitting and only this bit changed:
       set, ``signok`` 0 and the ``frame`` line still; clear, ``signok`` 1 and
       80169 superframes in 10.0 s. The tables of both modules, read through
       the ``tables`` view.
   * - What bit 15 selects inside the SerDes is not established; the two rows
       above fix only which value goes with which polarity of the module's
       output.
     - I
     - The bit changed with the module's ``LOS_POLARITY`` changed.
   * - ``RTL9602C_GTC_DS_LOS`` (``0x701040``): ``RTL9602C_GTC_DS_LOS_OPTIC``
       (bit 8) is the level of the optical module's LOS input and
       ``RTL9602C_GTC_DS_LOS_CDR`` (bit 10) the clock recovery's loss
       indication, both 1 for loss; ``RTL9602C_GTC_DS_LOS_OPTIC_EN`` (bit 0)
       lets the input act on the receiver and ``RTL9602C_GTC_DS_LOS_OPTIC_POL``
       (bit 1) selects which level of it means loss. Both configuration bits
       reset clear; the driver sets both.
     - M
     - The register read ``0x00000100`` (bit 8 set on live light) before and
       ``0x00000003`` after the bring-up writes; the ``los`` line of the
       ``gpon`` view prints the two loss bits.
   * - Whether ``RTL9602C_GTC_DS_LOS_OPTIC_EN`` (bit 0) and
       ``RTL9602C_GTC_DS_LOS_OPTIC_POL`` (bit 1) are needed with the module's
       output at its maker's polarity is not established.
     - I
     - Clearing each and reading the ``los`` line and the superframe counter.
   * - ``RTL9602C_SERDES_PON_RX_STAT_SYNC`` (bit 9) read 0 on a framed
       downstream; what it reports is not established. The ``gpon`` view prints
       it beside bit 8 as ``lecsync``.
     - I
     - The ``sig`` line read ``00000100``; a reading where bit 9 is 1 would
       settle its meaning.
   * - ``RTL9602C_GTC_DS_IRQ_STAT`` (``0x701008``) holds the present condition:
       ``RTL9602C_GTC_DS_IRQ_LOS`` (bit 0), ``RTL9602C_GTC_DS_IRQ_LOF`` (bit 1),
       ``RTL9602C_GTC_DS_IRQ_FEC`` (bit 2, the downstream carries FEC) and
       ``RTL9602C_GTC_DS_IRQ_LOM`` (bit 3). The driver reports LOS, LOF and LOM
       from it as alarms (see "Reporting: the PON class and devlink"); FEC is a
       property of the line, not an alarm.
     - M
     - The ``ds`` line of the ``gpon`` view read ``0000000a`` (LOF and LOM) with
       the SerDes reporting no signal, and ``00000000`` on a framed downstream.
   * - ``RTL9602C_GTC_DS_IRQ_LATCH`` (``0x701000``) latches the same events at
       the same bit positions, with ``RTL9602C_GTC_DS_IRQ_SN_REQ`` (bit 8, a
       grant classified as a serial-number request),
       ``RTL9602C_GTC_DS_IRQ_RANGING_REQ`` (bit 9, one classified as a ranging
       request), ``RTL9602C_GTC_DS_IRQ_PLOAM_RX`` (bit 10, a PLOAM message
       waits) and ``RTL9602C_GTC_DS_IRQ_SECOND`` (bit 11, the pulse of
       ``RTL9602C_GTC_DS_PPS``). Reading it clears it, and the read is the
       acknowledgment. ``RTL9602C_GTC_DS_IRQ_EN`` (``0x701004``) uses the same
       positions; the bring-up enables the first seven (``0x70f``; 0 at
       reset), and SECOND while the switch driver wants the seconds
       (``rtl9602c_gpon_second_enable()``, the PON operation
       ``second_enable``).
     - T
     - The ONU reaches O5 on an OLT: activation runs from this interrupt, and it
       does not assert again once the latch is read (see "PLOAM receive").
   * - G.984.3's ONU detects LOS when no valid downstream signal is received and
       LOF after five consecutive invalid PSync fields, cleared by two
       consecutive correct ones; on either it switches its laser off and changes
       state per clause 10.
     - P
     - ITU-T G.984.3 clause 11.1.2 and clause 8.1.3.1.
   * - LOM is not among G.984.3's ONU alarms. What the MAC's LOM bit detects is
       not established; the driver reports it as loss of PLOAM
       (``PON_ALARM_LOM`` in ``include/uapi/linux/pon.h``).
     - I
     - A downstream with frames and a corrupted PLOAM channel.
   * - The PEE alarm (``PON_ALARM_PEE``) is the driver's own: every
       Physical_Equipment_Error raises it, with a warning, ``the OLT reports a
       physical equipment error``, when it was not raised; the worker clears it
       once none has come for ``RTL9602C_GPON_PEE_MS`` (3 s), saying ``the
       OLT's physical equipment error has cleared``; and O1 clears it. Each
       change is sent to the PON class.
     - T
     - Physical_Equipment_Error injected once a second for 5 s in O5: ``PEE``
       among the alarms of ``pon`` from the first, still 2 s after the last,
       gone 3 s after it.
   * - ``RTL9602C_GTC_DS_PPS`` (``0x70126c``): the driver writes
       ``RTL9602C_GTC_DS_PPS_PERIOD`` (bits 12:0) =
       ``RTL9602C_GTC_DS_PPS_PERIOD_1S`` (7999: a pulse every 8000 downstream
       frames, one second at 125 us a frame) and ``RTL9602C_GTC_DS_PPS_OFFSET``
       (bits 31:26) = 1. The pulse latches ``RTL9602C_GTC_DS_IRQ_SECOND``,
       which the switch's PTP clock reports as the OLT's seconds (see "The
       OLT's seconds" in Documentation/networking/dsa/rtl9602c-ptp.rst).
     - M, I
     - M: 120 pulses in a row, 8000 frames apart by the superframe counter
       read in the interrupt. I: what else consumes the pulse and what the
       offset counts are not identified.

The upstream write key
======================

Some upstream GTC registers are locked: a write to one is dropped without any
error unless the key was written first. The driver opens the key around each
write to a register that it believes locked, and closes it again.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_GTC_US_WRITE_KEY`` (``0x705018``): writing
       ``RTL9602C_GTC_US_WRITE_KEY_UNLOCK`` (``0xcc19``) opens the locked
       upstream GTC registers to writes; writing
       ``RTL9602C_GTC_US_WRITE_KEY_LOCK`` (0) closes them. A write to a locked
       register without the key is dropped without any error.
     - M
     - A write to ``RTL9602C_GTC_US_CTRL`` without the key read back unchanged.
   * - The driver writes ``RTL9602C_GTC_US_RESP_DELAY`` between the two key
       writes, and ``RTL9602C_GTC_US_CTRL``, ``RTL9602C_GTC_US_TX_MODE`` and
       ``RTL9602C_GTC_US_POWERSAVE`` through ``rtl9602c_gpon_us_locked_rmw()``
       (unlock, read-modify-write, lock). That the response delay, the transmit
       mode and the power save are locked like the control register, and that no
       other register the driver writes is, are not established; a key that is
       not needed costs nothing.
     - I
     - A write to each without the key, read back (the ``usburst`` view shows
       them; the equalization delay write reads the response delay back).

The activation state machine
============================

G.984.3 defines seven ONU states and two timers, and the driver implements O1 to
O5 and TO1 ("Known limitations" says what it leaves out). The state is
software's: ``rtl9602c_gpon_apply_state()`` writes it into ``RTL9602C_ONU_CTRL``
on every transition and the MAC follows what is written. The MAC classifies the
OLT's grants by it: in O1 it counts no grant as a serial-number request, and in
O3 it takes the discovery grants.

The code that causes a transition writes the registers that the new state needs,
and then asks for the transition. The transition
(``rtl9602c_gpon_apply_state()``) does the following, in this order.

1. Record the new state for the rest of the driver. (M)
2. Make the ``pon`` interface dormant, or not dormant in O5. (I)
3. Record the event in the ``ploamlog`` ring. (M)
4. Arm TO1 on entering O3, and on entering O4 from O6; leave it running on
   entering O4 from O3, and disarm it on entering any other state. Arm TO2 on
   entering O6 and disarm it on entering any other state. (I: read from the
   code; the time from O3 to O5 on the bench is far under TO1)
5. Write the state into ``RTL9602C_ONU_CTRL``. Purpose: the MAC follows it. (M)
6. Below O4, and in O7, if an ONU-ID is held: write 255 into the ONU-ID
   registers and unbind the default Alloc-ID, the Alloc-IDs in T-CONTs 0 to 15
   and the OMCC. Purpose: deactivation and an emergency stop discard them. (M)
7. Drive TX_DISABLE: released in O3 to O5, asserted in every other state. (M)
8. Below O3 and in O7, ask for transmit power level mode 0 if the ONU is at a
   lower mode or has asked for one. Purpose: the ONU sends nothing there, and
   a lower level outlasts no session (see "Transmit power level"). (I)
9. Log the transition, and on leaving O5 how long the ONU was in it. Purpose: a
   console line costs about 9 ms, so the log comes after the writes that have to
   meet the 750 us. (M)

The report to the PON class follows the log line
(``rtl9602c_gpon_set_state()``).

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - G.984.3 defines seven ONU states, O1 Initial, O2 Standby, O3
       Serial_Number, O4 Ranging, O5 Operation, O6 POPUP and O7 Emergency Stop,
       and two timers: TO1 bounds the time in O3 and O4 together (recommended 10
       s), TO2 the time in O6 (recommended 100 ms).
     - P
     - ITU-T G.984.3 clause 10.2.1.
   * - ``RTL9602C_ONU_CTRL`` (``0x701010``): ``RTL9602C_ONU_CTRL_STATE`` (bits
       3:0) holds the state as the numbers 1 to 7 (``RTL9602C_ONU_O1_INITIAL``,
       ``RTL9602C_ONU_O2_STANDBY``, ``RTL9602C_ONU_O3_SERIAL_NUMBER``,
       ``RTL9602C_ONU_O4_RANGING``, ``RTL9602C_ONU_O5_OPERATION``,
       ``RTL9602C_ONU_O6_POPUP``, ``RTL9602C_ONU_O7_EMERGENCY_STOP``), and
       ``RTL9602C_ONU_CTRL_ONUID`` (bits 15:8) the ONU-ID, 255 until one is
       assigned. The state is software's: ``rtl9602c_gpon_apply_state()`` writes
       it on every transition and the MAC follows what is written.
     - M
     - Writing O2 and O3 read back ``0x0000ff02`` and ``0x0000ff03``, and O7
       ``0x0000ff07``; the ``onu`` line of the ``gpon`` view prints the
       register.
   * - The state gates the MAC's classification of grants: in O1 no grant is
       counted as a serial-number request; in O3 the discovery grants are.
     - M
     - The serial-number request count (``sn-requests`` on the ``cntr`` line of
       the ``gpon`` view) stayed 0 for 100 s in O1 while bandwidth maps arrived
       8000 a second, and counted 8 in 8 s once O3 was written.
   * - Bring-up ends in O1 with no ONU-ID, and the ONU goes on to O2 once LOS
       and LOF have cleared. Upstream_Overhead, acted on only in O2, takes the
       ONU to O3 and starts TO1.
     - P, M
     - ITU-T G.984.3 clause 10.2.5.1 a) and Table 10-1; the kernel log shows
       ``O1 initial -> O2 standby`` once the PON comes up, and the ONU reaches
       O5 on an OLT.
   * - The downstream's synchronisation moves the state
       (``rtl9602c_gpon_ds_sync()``): with LOS or LOF set in
       ``RTL9602C_GTC_DS_IRQ_STAT``, an ONU in O2, O3 or O4 goes to O1 and one
       in O5 to O6; with both clear, one in O1 goes to O2; O6 and O7 wait for
       the OLT. The interrupt acts on a change of either bit at once, and the
       worker reads the levels every 50 ms, so a missed change is still
       followed.
     - P, I
     - ITU-T G.984.3 clause 10.2. The step from O1 to O2 at bring-up is the
       row above; the steps on a loss of the downstream have not been taken on
       the board, which needs the fibre pulled with the ONU in each state.
   * - O6 keeps the ONU-ID, the Alloc-IDs and the OMCC, with the laser off. A
       POPUP directed to the ONU takes it back to O5, a broadcast one to O4,
       where the OLT ranges it again and TO1 runs; when TO2
       (``RTL9602C_GPON_TO2_MS``, 100 ms) expires first, the ONU goes to O1,
       giving them back, and on to O2 at once if the downstream is back.
     - P, I
     - ITU-T G.984.3 clause 10.2.5.1. Not exercised on the board, which reaches
       O6 only when the downstream is lost in O5; a POPUP injected in O5 was
       ignored.
   * - Disable_Serial_Number with octet 3 at ``0xff`` and the ONU's serial
       number in octets 4 to 11 takes the ONU to O7 from any state the
       downstream reaches, laser off and ONU-ID given back; octet 3 at ``0x00``
       with its serial number, or at ``0x0f`` for every ONU, takes an ONU in O7
       to O2, from which the OLT discovers it again.
     - P, M
     - ITU-T G.984.3 clause 9.2.3.6 and clause 10.2.5.1. Injected through
       ``ploaminject``: the disable took O5 to O7, the module's bias and
       transmitted power then reading 0; the enable, and in a second run the
       enable for every ONU, took O7 to O2, and the OLT ranged and provisioned
       the ONU again in about 2 s, after which bridged traffic crossed both
       ways. A disable for another serial number, and Deactivate_ONU-ID in O7,
       changed nothing.
   * - An ONU whose last state before a power-off was O7 powers up in O7, and
       a disabled ONU stays disabled even after a power-off, until the OLT
       enables it with Disable_Serial_Number.
     - P
     - ITU-T G.984.3 clause 10.2.4, Table 10-1, the event "ONU power up", and
       clause 11.1.2, the alarm DIS.
   * - ``gpon_disabled``, the devlink parameter of that name, is set when
       Disable_Serial_Number disables the ONU and cleared when it enables it
       (``rtl9602c_gpon_disable_sn()`` decides the state); while the PON is up
       it is set exactly while the ONU is in O7. A bring-up with it set ends in
       O7 before the GPON interrupt is enabled, so that the first message
       dispatched finds the ONU stopped; a take-down keeps it. A set to true
       while the PON is up is refused with ``-EBUSY``, and a set to false then
       takes an ONU in O7 to O2, as the OLT's enable would. The driver keeps no
       state across a restart or a rebind: user space sets the parameter
       again before the PON comes up.
     - I
     - Read from the code; the KUnit case ``rtl9602c_test_disable_sn`` checks
       the decision on the message. On the board: a disable injected through
       ``ploaminject`` in O5, then ``ip link set pon down`` and ``up``, then a
       restart with the parameter set before ``up``; each time O7 with the
       module's bias and transmitted power 0; then the enable injected, and O2
       and O5 again.
   * - Assign_ONU-ID carrying the ONU's own serial number, acted on only in O3,
       takes it to O4.
     - P
     - ITU-T G.984.3 clause 10.2.5.1 c); the ONU reaches O5 on an OLT.
   * - Ranging_Time for the main path, directed to the ONU's ONU-ID, is acted on
       in O4 and O5: in O4 it sets the equalization delay and takes the ONU to
       O5, stopping TO1; in O5 it only changes the delay.
     - P
     - ITU-T G.984.3 clause 10.2.5.1 d) and Table 10-1; the ONU reaches O5 on an
       OLT.
   * - Deactivate_ONU-ID, directed to the ONU's ONU-ID or broadcast, is acted on
       in O4, O5 and O6 and takes the ONU to O2; in O7 it is ignored.
     - P
     - ITU-T G.984.3 clause 10.2.5.1 h); the OLT's Deactivate_ONU-ID after
       unacknowledged messages took the ONU to O2.
   * - TO1 is armed for ``RTL9602C_GPON_TO1_MS`` (10 s) on entering O3, runs on
       into O4, is armed on entering O4 from O6, and is disarmed in every other
       state. The worker, which runs every 50 ms, checks it, and TO2 with it. On
       expiry the ONU goes to O2 and the next Upstream_Overhead programs
       everything again.
     - M
     - ``TO1 expired in O4; trying again`` in the kernel log, then a new pass
       through O3.
   * - Whenever the state drops below O4 while an ONU-ID is held (TO1, TO2,
       Deactivate_ONU-ID, a loss of the downstream, the interface going down),
       or enters O7, the driver writes
       ``RTL9602C_ONU_ONUID_UNASSIGNED`` (255) into ``RTL9602C_ONU_US_ONUID``
       (``0x705010``) and into ``RTL9602C_ONU_CTRL_ONUID``, clears the default
       Alloc-ID's table entry (T-CONT 16) and every T-CONT from 0 to 15 in use,
       and unbinds the OMCC (``rtl9602c_gpon_o5_teardown()``).
     - M
     - With the ONU-ID of a failed ranging left in ``RTL9602C_ONU_US_ONUID``,
       discovery never succeeded again (O3, TO1, O2, O3 ...); writing 255 into
       it brought Assign_ONU-ID back within one TO1. ITU-T G.984.3 clause 9.2.1
       and clause 5.5.4: deactivation discards the ONU-ID, the OMCI Port-ID and
       every Alloc-ID.
   * - Which upstream field takes ``RTL9602C_ONU_US_ONUID``, the burst header's
       ONU-ID (G.984.3 clause 8.2.2.2), octet 1 of every upstream PLOAM message
       whatever the template holds there, or both, is not established; the row
       above shows only that the ONU identifies itself by it.
     - I
     - A capture of the upstream at the OLT with the register at 255 and at an
       assigned value.
   * - TX_DISABLE follows the state: it is released in O3 to O5 and asserted in
       O1, O2, O6 and O7 (``pon_optics_tx_disable()``, see
       Documentation/networking/device_drivers/pon/gn25l95.rst). Only
       the hard block is lifted: the burst enable still confines emission to the
       allocations that the OLT grants.
     - M
     - The laser is off before O3; in O7 the line read high and the module's
       bias and transmitted power 0, and both came back in O5. The module
       measures those only in a burst, so a test build forced the laser driver
       on through the module's own configuration table: in O5 its indication
       of light (table 2 ``0xb7`` bits 7 and 5) rose within 10 ms, and asserting
       TX_DISABLE cleared it with the driver still forced on; in O7, after an
       injected Disable_Serial_Number, it stayed clear in all 130 reads, 10 ms
       apart, through a second forced on and after. ITU-T G.984.3 clause 10.2.2 (no upstream transmission
       before O3); clause 9.2.1 and clause 11.1.2 (Deactivate_ONU-ID and loss of
       signal switch the laser off).
   * - The PON port's operational state is dormant in every state but O5, and up
       in O5 (``rtl9602c_gpon_operstate()``, on each transition and when the
       port is enabled): up but waiting for an external event, as RFC 2863
       defines dormant.
     - I
     - ``ip link show pon`` below O5 and in O5; RFC 2863 section 3.1.12.
   * - A transition is logged only after the state, the ONU-ID and TX_DISABLE
       are written, and the PON class report follows the log line. The
       per-message PLOAM trace is debug-level; the lines left at information
       level come after the writes they describe. G.984.3 gives the ONU 750 us,
       and a console line costs about 9 ms at 115200 baud.
     - M
     - An Assign_ONU-ID queued behind two logged Upstream_Overhead lines was
       acted on 20 ms after its interrupt, and the interrupt thread printing at
       real-time priority starved the watchdog's feeder until the board reset.
       ITU-T G.984.3 clause 9.2.
   * - G.984.3 gives the ONU 750 us to process a downstream message and prepare
       the upstream action. After Assign_ONU-ID the OLT issues a ranging
       request, a grant to the new ONU-ID, which the MAC answers by itself from
       the ONU-ID, the state and the default T-CONT entry already written; those
       writes have to land within that time, which is why PLOAM messages are
       taken from the interrupt.
     - M
     - Dispatched from the 50 ms worker, the ranging request was never counted
       and TO1 expired in O4 on every attempt; a poll every 4 ms did no better;
       from the interrupt every activation reached O5. ITU-T G.984.3 clause 9.2
       and clause 10.2.5.2.

Transmit power level
--------------------

The ONU's transmitter has three power levels, the level modes of ITU-T G.984.2:
0, the normal level, and 1 and 2, 3 dB and 6 dB below it. The driver keeps two
modes: the one it asks the optics for, ``gpon_power_mode``, and the one the
optics last said they are at, ``gpon_power_set``, which Serial_Number_ONU
declares and the PON class reports. ``rtl9602c_gpon_power_work()`` has the
optics set the mode asked for, outside ``gpon_lock``, and
``rtl9602c_gpon_power_applied()`` takes what they set. In O4 and O5, where the
ONU sends often, the optics measure the power as they set it; in the other
states the ONU sends at most its answers to serial-number requests, and the
optics set only what they can without measuring (see "Transmit power
levelling" in Documentation/networking/device_drivers/pon/gn25l95.rst).

.. list-table::
   :header-rows: 1
   :widths: 40 12 48

   * - Event
     - State
     - Mode asked for
   * - Bring-up
     - O1
     - 0; the optics' preparation puts it back too.
   * - Upstream_Overhead
     - O2, then O3
     - The default mode of its flags octet, pp; the count of answered
       serial-number requests starts from none.
   * - ``RTL9602C_GPON_SN_REQUEST_THRESHOLD`` (10) serial-number requests
       answered at the present level and no Assign_ONU-ID
     - O3
     - The next mode, modulo 3: 0, 1, 2, 0 (``rtl9602c_gpon_power_sn()``,
       from the worker); the count starts again from none once the optics
       have answered.
   * - Change_Power_Level
     - O4, O5
     - One mode up or down, within 0 to 2 (see "PLOAM receive").
   * - Entering O1, O2 or O7
     - O1, O2, O7
     - 0, when the ONU is at a lower mode or has asked for one.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Upstream_Overhead carries the default ONU transmit power level mode in
       bits 1:0 of its flags octet, pp (``RTL9602C_PLOAM_OVERHEAD_POWER_MODE``):
       ``00`` mode 0, normal; ``01`` mode 1, normal less 3 dB; ``10`` mode 2,
       normal less 6 dB; ``11`` reserved. Mode 0 is the highest, the reverse of
       the coding of Serial_Number_ONU. The ONU takes it on Upstream_Overhead
       in O2, with the delimiter and the pre-assigned delay.
     - P
     - ITU-T G.984.3 clause 9.2.3.1 (note 4), clause 10.1.2 and clause 10.2.2
       b).
   * - The driver asks for the mode pp names, and for mode 0 when pp is the
       reserved ``11`` (``rtl9602c_gpon_power_mode_of()``). The test OLT names
       mode 0 (flags ``0x20``).
     - T
     - ``rtl9602c_test_power_mode_of`` (KUnit); the ONU reaches O5 on the test
       OLT at mode 0. A mode other than 0 has not been run on the board.
   * - The ONU-activated power levelling: in O3 the ONU counts the
       serial-number requests it answers without receiving Assign_ONU-ID.
       Initially it uses the mode Upstream_Overhead named; once the count meets
       or exceeds SN_Request_Threshold, recommended 10, it increments its
       operating power level modulo 3 (..., 0, 1, 2, 0, 1, 2, ...) and goes on
       answering, and it repeats this until it receives Assign_ONU-ID or a
       disable message. The OLT-activated power levelling is
       Change_Power_Level, in O4 and O5.
     - P
     - ITU-T G.984.3 clause 10.5.1, clause 10.5.2, clause 10.2.1,
       clause 10.2.5.3 a) and Table 10-1.
   * - The driver adds the serial-number requests of
       ``RTL9602C_GPON_CNT_ACTIVATION_SN`` that come in O3 into
       ``gpon_sn_answered`` (``rtl9602c_gpon_cntr_sync()``); the worker, every
       50 ms, asks for the next mode once the count reaches
       ``RTL9602C_GPON_SN_REQUEST_THRESHOLD`` and sets the count to 0
       (``rtl9602c_gpon_power_sn_step()``). Requests counted beyond the
       threshold, and those answered before the optics have answered for the
       new mode, do not count towards the next step. Upstream_Overhead starts
       the count from none; leaving O3 ends the levelling, for O4 on
       Assign_ONU-ID and for O2, O1 or O7 on TO1, a loss of the downstream or
       Disable_Serial_Number.
     - T
     - ``rtl9602c_test_power_sn_step`` (KUnit). Not run on the board: the test
       OLT assigns the ONU-ID at the first answered request.
   * - The requests the MAC counts in O3 are those it answered with the
       Serial_Number_ONU template; a request counted and left unanswered would
       make the ONU step early.
     - I
     - The ``ploam-auto`` count of the ``uscntr`` view, the PLOAMs sent from
       the templates, rising by as many as ``sn-requests`` in O3.
   * - "Increments its operating power level modulo 3" is read as counting the
       mode numbers of Upstream_Overhead, 0 to 1 to 2 and back to 0: normal,
       3 dB down, 6 dB down, normal. Clause 10.5.1 starts from
       Upstream_Overhead's mode and does not say which coding it counts in; in
       the coding of Serial_Number_ONU the cycle from the normal level would
       run high, low, medium.
     - I
     - An OLT that relies on the ONU's own levelling, or a later edition of the
       clause.
   * - The ONU answers serial-number requests at the mode the optics are at,
       and Serial_Number_ONU's TT declares that mode, not the one asked for:
       before the optics have answered, and when they could not set a mode, the
       ONU stays at the mode they are at and says so.
     - P
     - ITU-T G.984.3 clause 9.2.4.1: TT is the transmit power level mode used
       by the ONU.
   * - Before O4 the optics set a lower mode only at a set point kept from a
       measurement since probe, which only Change_Power_Level in O4 or O5
       makes; until then a default mode of 1 or 2, or a step, leaves the ONU at
       mode 0 with ``transmit power level mode <n> not set (-61); the ONU stays
       at mode 0``.
     - I
     - Not run on the board: an injected Upstream_Overhead naming mode 1 in O2,
       before and after an injected Change_Power_Level has measured mode 1 in
       O5.
   * - Below O3 and in O7 the ONU sends nothing, so the level does not matter
       there; the driver asks for mode 0 on entering O1, O2 or O7, so that a
       lower level is kept only while the ONU sends, from O3 to O6.
     - P, I
     - P: ITU-T G.984.3 clause 10.2.2 a), b) and g). I: not run on the board;
       the ``power`` line of the ``gpon`` view after an injected
       Disable_Serial_Number at mode 1 would show it.
   * - TO1 bounds O3 and O4 at 10 s, and Upstream_Overhead starts each O3 at
       the default mode, so a step needs ten answered requests within one pass
       through O3: an OLT that asks less often than once a second never makes
       the ONU step. The test OLT's requests counted 8 in 8 s once O3 was
       written (see "The activation state machine").
     - P, M
     - P: ITU-T G.984.3 clause 10.2.1. M: the ``sn-requests`` count of the
       ``gpon`` view.

PLOAM receive
=============

The OLT sends PLOAM messages in the PLOAMd field of the downstream frame. The
MAC checks each message's CRC, counts the failures and queues the good ones; the
driver pops a message, which the queue presents in seven 16-bit words, and acts
on it in ``rtl9602c_gpon_ploam_rx()``. The MAC raises the PLOAM-received event
in ``RTL9602C_GTC_DS_IRQ_LATCH``, and the switch's interrupt thread pops and
dispatches up to 32 messages under ``gpon_lock``. The 50 ms worker does the same
as a safety net and normally finds nothing.

The interrupt path, in order:

1. The hard interrupt handler (``rtl9602c_irq()``) notes the time, and while
   the PTP clock wants the OLT's seconds the superframe counter and the
   clock, and wakes the thread, leaving the GPON source pending and the line
   masked. Purpose: the source asserts again until the GTC latches are read.
   (M)
2. The thread (``rtl9602c_irq_thread()``, which calls ``rtl9602c_gpon_irq()``)
   reads ``RTL9602C_GPON_IRQ_STAT``. (M)
3. If ``RTL9602C_GPON_IRQ_GTC_US`` is set, it reads the upstream latch (see
   "Upstream timing"). Purpose: a latch left unread keeps the source asserted.
   (M)
4. If ``RTL9602C_GPON_IRQ_GTC_DS`` is set, it reads
   ``RTL9602C_GTC_DS_IRQ_LATCH``, which clears it, records any bit other than
   PLOAM_RX and SECOND in the ``ploamlog`` ring, hands SECOND to the switch
   driver (the host operation ``second``), and on a change of LOS or LOF moves the
   state under ``gpon_lock`` (``rtl9602c_gpon_ds_sync()``). On SN_REQ in O3 it
   notes, under the same lock, that a serial-number request has come, which
   ends the taking of Extended_Burst_Length. Purpose: the note is made before
   any message of the same interrupt is dispatched. (M for the latch, I for
   the move)
5. On ``RTL9602C_GTC_DS_IRQ_PLOAM_RX`` it pops and dispatches up to 32 PLOAM
   messages, under ``gpon_lock``. (M)
6. It writes the source's bit into ``RTL9602C_IRQ_PENDING`` to clear it. (M)

A message is acted on only when its ONU-ID is 255 or the ONU's own, and each
type needs a state. The table lists what the driver does with each type; a type
not in it is recorded in the ``ploamlog`` ring and dropped.

.. list-table::
   :header-rows: 1
   :widths: 22 26 22 30

   * - Message (identifier)
     - Addressed to
     - Acted on in
     - What the driver does
   * - Upstream_Overhead (1)
     - All ONUs
     - O2
     - Burst overhead, pre-assigned delay and default transmit power level
       mode; O3.
   * - Extended_Burst_Length (20)
     - All ONUs
     - O3
     - Type 3 preamble lengths before and after ranging.
   * - Assign_ONU-ID (3)
     - All ONUs; octets 4 to 11 must be the ONU's serial number
     - O3
     - ONU-ID and default Alloc-ID; O4.
   * - Ranging_Time (4)
     - The ONU-ID
     - O4, O5
     - Equalization delay; from O4, O5.
   * - Deactivate_ONU-ID (5)
     - The ONU-ID or all ONUs
     - O4, O5, O6
     - O2.
   * - Disable_Serial_Number (6)
     - All ONUs; octets 4 to 11 must be the ONU's serial number, unless octet 3
       enables every ONU
     - Disable: any state but O7. Enable: O7
     - Disable: O7. Enable: O2.
   * - POPUP (12)
     - The ONU-ID or all ONUs
     - O6
     - Directed: O5. Broadcast: O4.
   * - Swift_POPUP (22)
     - All ONUs
     - O6
     - O5.
   * - BER_Interval (18)
     - The ONU-ID or all ONUs
     - O5
     - The interval of the REI reports, from now (see "PLOAM transmit"); 0
       stops them. Acknowledged.
   * - Ranging_Adjustment (23)
     - The ONU-ID or all ONUs
     - O5
     - Adds the change to the equalization delay, or takes it away with
       octet 3 bit 0 set; a delay below 0, or of 7 upstream frames or more,
       is not applied and is logged.
   * - Request_Key (13)
     - The ONU-ID
     - O5
     - A new key, sent in Encryption_Key messages and written as the next key
       (see "FEC and encryption").
   * - Key_Switching_Time (19)
     - The ONU-ID or all ONUs
     - O5
     - The superframe from which the next key applies; acknowledged.
   * - Encrypted_Port-ID (8)
     - The ONU-ID
     - O4, O5
     - Decryption on or off for the Port-ID's flow; acknowledged.
   * - Physical_Equipment_Error (15)
     - All ONUs
     - Any
     - Raises the PEE alarm, which clears 3 s after the last one (see
       "Reporting: the PON class and devlink").
   * - PST (17)
     - The ONU-ID or all ONUs
     - O5
     - Answered with the ONU's own PST (7) on the normal queue, carrying the
       same line number, K1 and K2: the ONU has one line and no protection
       switching, and the same octets keep the OLT from finding a link
       mismatch (G.984.3 clause 9.2.3.17, clause 9.2.4.7 and clause 11.1.2).
       The first PST in O5, and each change of its octets, is logged. An
       injected PST with line 1, K1 ``aa`` and K2 ``bb`` was answered with
       ``00 07 01 aa bb`` on the normal queue (the ``ploamlog`` ring).
   * - Change_Power_Level (16)
     - The ONU-ID or all ONUs
     - O4, O5
     - One transmit power level mode up ('10') or down ('01'), within modes 0
       to 2 (``rtl9602c_gpon_power_cpl()``), which the optics then set,
       measuring it (see "Transmit power level", and "Transmit power
       levelling" in Documentation/networking/device_drivers/pon/gn25l95.rst);
       '00', '11' and a step past either end change nothing. Each is logged;
       none is acknowledged, as G.984.3 asks for no acknowledgment
       (clause 9.2.3.16 and clause 10.5.2). Injected in O5, down twice and up
       twice, they took Serial_Number_ONU's flags octet through ``05``,
       ``04``, ``05`` and ``06`` with the power.
   * - Sleep_Allow (24)
     - The ONU-ID or all ONUs
     - None
     - Not acted on and not acknowledged; logged once, the first time one
       comes. It is ignored as G.984.3 has an ONU do whose power management
       OMCI has not enabled (Annex E.2.1): no OMCI entity of this ONU offers
       any.
   * - Request_Password (9)
     - The ONU-ID
     - O5
     - Three Password messages (2) on the normal queue, with the password of
       the devlink parameter ``gpon_password``; none, and a rate-limited
       warning, while no password is set. The ``ploamlog`` ring records them
       with the password zeroed. The request counts in ``password-requests``
       of the PON class's report, and an answered one in
       ``password-replies`` too.
   * - Assign_Alloc-ID (10)
     - The ONU-ID
     - O4, O5
     - Assigns an Alloc-ID to the ONU or takes it back, which binds or
       unbinds it where the T-CONT mappings say; acknowledged. A T-CONT that
       loses its Alloc-ID stops the upstream of its GEM ports
       (Documentation/networking/device_drivers/pon/rtl9602c.rst).
   * - Configure_Port-ID (14)
     - The ONU-ID
     - O4, O5
     - Binds or unbinds the OMCC; acknowledged.
   * - PON-ID (21)
     - All ONUs
     - None
     - Not recorded and not acted on.

The octets of Upstream_Overhead, with the offset macros that name them (they
count from 0, so the offset is the octet number less one):

.. list-table::
   :header-rows: 1
   :widths: 10 52 38

   * - Octet
     - Content
     - Offset macros
   * - 3
     - The number of guard bits.
     - ``RTL9602C_PLOAM_OVERHEAD_GUARD_BITS`` (2)
   * - 4
     - The number of type 1 preamble bits (all ones).
     - ``RTL9602C_PLOAM_OVERHEAD_TYPE1_BITS`` (3)
   * - 5
     - The number of type 2 preamble bits (all zeros).
     - ``RTL9602C_PLOAM_OVERHEAD_TYPE2_BITS`` (4)
   * - 6
     - The type 3 preamble pattern.
     - ``RTL9602C_PLOAM_OVERHEAD_TYPE3_PATTERN`` (5)
   * - 7 to 9
     - The delimiter.
     - ``RTL9602C_PLOAM_OVERHEAD_DELIMITER`` (6) and
       ``RTL9602C_PLOAM_OVERHEAD_DELIMITER_LEN`` (3)
   * - 10
     - The flags ``xxemsspp``: e (bit 5), the pre-assigned delay applies; m (bit
       4), deprecated; ss (bits 3:2), the number of extra serial-number
       transmissions, deprecated, 00; pp (bits 1:0), the default transmit power
       level mode, 0 the highest.
     - ``RTL9602C_PLOAM_OVERHEAD_FLAGS`` (9),
       ``RTL9602C_PLOAM_OVERHEAD_PRE_EQD_EN`` (bit 5),
       ``RTL9602C_PLOAM_OVERHEAD_EXTRA_SN_SHIFT`` (2) with
       ``RTL9602C_PLOAM_OVERHEAD_EXTRA_SN_MASK`` (3)
   * - 11 and 12
     - The pre-assigned delay, in units of 32 bytes.
     - ``RTL9602C_PLOAM_OVERHEAD_PRE_EQD_HI`` (10) and
       ``RTL9602C_PLOAM_OVERHEAD_PRE_EQD_LO`` (11);
       ``RTL9602C_PLOAM_OVERHEAD_PRE_EQD_SHIFT`` (8) turns the units into bits

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A PLOAM message is 13 octets (``RTL9602C_GPON_PLOAM_BYTES``): ONU-ID (0
       to 253 once assigned, 255 broadcast), message identifier, 10 data octets,
       and a CRC over x^8 + x^2 + x + 1. The OLT sends each downstream message
       three times; the ONU acts on one valid copy.
     - P
     - ITU-T G.984.3 clause 9.1 and clause 10.2.5.1.
   * - The receive queue: in ``RTL9602C_PLOAM_RX_CTRL`` (``0x701080``),
       ``RTL9602C_PLOAM_RX_CTRL_EMPTY`` (bit 5) is 1 while no PLOAM message
       waits, and setting ``RTL9602C_PLOAM_RX_CTRL_POP`` (bit 0) removes the
       message at the head. The head message is in ``RTL9602C_PLOAM_RX_DATA(i)``
       (``0x7010a0`` + 4i), each word's ``RTL9602C_PLOAM_RX_DATA_WORD`` (bits
       15:0) holding two octets, the earlier in bits 15:8; word 0 holds the
       ONU-ID and the message identifier. ``rtl9602c_ploam_pop()`` reads
       ``RTL9602C_GPON_PLOAM_WORDS`` (7) words into 14 bytes, then pops. The MAC
       checks the CRC and counts failures (see "Counters").
     - T
     - The ONU reaches O5 on an OLT: every activation decodes the OLT's
       messages.
   * - ``RTL9602C_GPON_IRQ_EN`` enables the two sub-block bits of
       ``RTL9602C_GPON_IRQ_STAT``, ``RTL9602C_GPON_IRQ_GTC_US`` (bit 5) and
       ``RTL9602C_GPON_IRQ_GTC_DS`` (bit 1); it is 0 at reset. The interrupt
       path above runs from the switch interrupt's GPON source, with the line
       masked while the thread runs.
     - M
     - Every activation reaches O5 from this path; dispatched from a poll,
       ranging does not complete (see "The activation state machine").
   * - The worker (``rtl9602c_gpon_work()``, every ``RTL9602C_GPON_POLL_MS``, 50
       ms) pops and dispatches up to 32 PLOAM messages, which is a safety net
       since the interrupt normally leaves none; records changes of the upstream
       queues in the ``ploamlog`` ring once the PON MAC is built; runs TO1;
       takes the counters; and sends the PON class the ONU's status if it
       changed. ``gpon_lock`` keeps it and the interrupt thread from dispatching
       at the same time.
     - T
     - The ONU reaches O5 on an OLT.
   * - A message is acted on only when its ONU-ID is 255 or the ONU's own; each
       type also needs a state (see the table above). Types the driver does not
       handle are recorded and dropped.
     - P
     - ITU-T G.984.3 clause 9.1.1 and clause 10.2.5.1.
   * - Upstream_Overhead (identifier 1, broadcast) carries, in octets 3 to 12,
       the guard bits, the type 1, type 2 and type 3 preamble parameters, the
       delimiter, a flags octet and the pre-assigned delay in units of 32 bytes,
       laid out as in the table above.
     - P
     - ITU-T G.984.3 clause 9.2.3.1.
   * - On Upstream_Overhead in O2 (``rtl9602c_gpon_set_overhead()``) the driver
       writes the burst overhead bytes and the pre-ranged length (see "The burst
       overhead"); writes the extra serial-number count into
       ``RTL9602C_GTC_DS_CTRL_EXTRA_SN`` (see "The bandwidth map"); sets the
       equalization delay to the pre-assigned delay when e is set and to 0
       otherwise, so that a zero delay named by the OLT is used as zero (see
       "Upstream timing"); and keeps the message for the length computation and
       zeroes both stored type 3 lengths. It asks for the default transmit
       power level mode the flags octet names (see "Transmit power level"). The
       Serial_Number_ONU template is then armed again and the state becomes
       O3. Every field of the message is logged.
     - T
     - The ONU reaches O5 on an OLT; the test OLT's flags octet is ``0x20``: e
       set with a zero delay.
   * - Extended_Burst_Length (identifier 20, broadcast): octet 3 the type 3
       preamble bytes in the pre-ranged states O3 and O4, octet 4 those used
       from O5 on (``RTL9602C_PLOAM_EXT_BURST_LEN_PRERANGED`` 2,
       ``RTL9602C_PLOAM_EXT_BURST_LEN_RANGED`` 3); the whole burst mode overhead
       is at most 128 bytes. The ONU acts on it only in O3 and only before any
       serial-number request.
     - P
     - ITU-T G.984.3 clause 9.2.3.20 and clause 10.2.5.1 b).
   * - The driver acts on it when it is broadcast, the state is O3 and no
       serial-number request has come in O3 (SN_REQ in
       ``RTL9602C_GTC_DS_IRQ_LATCH``, noted by the interrupt thread): a copy
       with the values already stored is ignored; otherwise both lengths are
       stored, the pre-ranged overhead length is applied at once (see "The burst
       overhead"), the ranged one is kept for Ranging_Time, and the change is
       logged. Upstream_Overhead, which starts O3, clears the note.
     - T
     - The ONU reaches O5 on an OLT; on the test OLT the log line reads ``burst
       length: 96 B pre-ranging, 56 after; overhead 103/63``.
   * - The test OLT sends Extended_Burst_Length 32 ms after the
       Upstream_Overhead that starts O3, 6 ms into O3, and repeats it; its
       first serial-number request comes about a second later, followed at
       once by Assign_ONU-ID. So the message
       is acted on before the first request, and O3 holds no time after one in
       which a changed message could be ignored.
     - M
     - The ``ploamlog`` ring of an activation: Upstream_Overhead at 232.552 s,
       O3 at 232.578 s, Extended_Burst_Length at 232.584 s and five times more,
       the latch ``00008100`` (SN_REQ) at 233.5786 s, O4 at 233.5787 s.
   * - Assign_ONU-ID (identifier 3, broadcast): octet 3 the ONU-ID, octets 4 to
       11 the serial number of the ONU it is for
       (``RTL9602C_PLOAM_ASSIGN_ONUID_ONUID`` 2,
       ``RTL9602C_PLOAM_ASSIGN_ONUID_SERIAL`` 3). The default Alloc-ID, equal to
       the ONU-ID, is assigned with it.
     - P
     - ITU-T G.984.3 clause 9.2.3.3 and clause 5.5.3.
   * - In O3, when octets 4 to 11 equal the ONU's serial number, the driver
       writes the ONU-ID into ``RTL9602C_ONU_US_ONUID`` (at the field position
       of ``RTL9602C_ONU_CTRL_ONUID``, bits 15:8) and into
       ``RTL9602C_ONU_CTRL_ONUID``; binds the default Alloc-ID into T-CONT 16
       (see "Tables: Alloc-ID, Port-ID and multicast"); writes the bandwidth-map
       filter bits (see "The bandwidth map"); sets O4; arms the bandwidth-map
       capture for 16 allocations; and arms the Serial_Number_ONU template
       again. The kernel log then states how long after the interrupt the
       writes before O4 were done.
     - M
     - ``ONU-ID 0 assigned; written 38 us after the interrupt`` at boot, then
       29 and 33 us in two registrations after ``ip link set pon down`` and up:
       within the 750 us, timed with the 10 ns clock source
       (Documentation/arch/mips/rtl9602c.rst).
   * - Ranging_Time (identifier 4, directed): octet 3 bit 0, 0 for the main path
       and 1 for the protection path; octets 4 to 7 the equalization delay in
       bits, most significant first (``RTL9602C_PLOAM_RANGING_TIME_PATH`` 2 with
       ``RTL9602C_PLOAM_RANGING_TIME_PATH_PROTECT`` bit 0,
       ``RTL9602C_PLOAM_RANGING_TIME_EQD`` 3,
       ``RTL9602C_PLOAM_RANGING_TIME_EQD_LEN`` 4). The OLT may send it at any
       time to correct the delay.
     - P
     - ITU-T G.984.3 clause 9.2.3.4 and clause 10.3.5.
   * - Directed to the ONU's ONU-ID, in O4 or O5: a protection-path delay is
       logged and not applied. In O4 the capture armed at Assign_ONU-ID is read,
       the delay is applied (see "Upstream timing"), the ranged overhead length
       is applied (see "The burst overhead"), the upstream PLOAM queues are
       flushed, the state becomes O5, and the message is logged raw. In O5 the
       delay is written only when it differs from the last one written, and the
       change is logged.
     - T
     - The ONU reaches O5 on an OLT.
   * - Deactivate_ONU-ID (identifier 5), directed or broadcast: the ONU switches
       off its laser, discards its ONU-ID, OMCI Port-ID and Alloc-IDs, and goes
       to O2. The driver also records the upstream queue state in the
       ``ploamlog`` ring.
     - P
     - ITU-T G.984.3 clause 9.2.3.5 and clause 9.2.1.
   * - Assign_Alloc-ID (identifier 10, directed): octet 3 the Alloc-ID's bits 11
       to 4, bits 7 to 4 of octet 4 its bits 3 to 0, octet 5 the type: 1 GEM
       payload (``RTL9602C_PLOAM_ASSIGN_ALLOCID_GEM``), 255 de-allocate
       (``RTL9602C_PLOAM_ASSIGN_ALLOCID_DEALLOC``), the rest reserved. Every
       correctly received copy is acknowledged.
     - P
     - ITU-T G.984.3 clause 9.2.3.10 and clause 9.2.1.
   * - Directed to the ONU's ONU-ID: type 1 binds the Alloc-ID into a T-CONT
       (see "Tables: Alloc-ID, Port-ID and multicast"), type 255 unbinds it,
       other types change nothing, and every copy is acknowledged (see "PLOAM
       transmit"). The driver tests only that octet 1 equals the ONU-ID it
       holds, which is 255 until O4.
     - M
     - Unacknowledged, the OLT sent Assign_Alloc-ID and Configure_Port-ID three
       times each and then Deactivate_ONU-ID; acknowledged, the activation
       stays; the PON class report lists Alloc-ID 257.
   * - Configure_Port-ID (identifier 14, directed): octet 3 bit 0, 1 activates
       and 0 deactivates; octet 4 the Port-ID's bits 11 to 4 and bits 7 to 4 of
       octet 5 its bits 3 to 0. An ONU has at most one OMCI connection, and a
       second replaces the first. Every correctly received copy is acknowledged.
     - P
     - ITU-T G.984.3 clause 9.2.3.14 and clause 9.2.1.
   * - Directed to the ONU's ONU-ID, with the same test as for Assign_Alloc-ID:
       activate binds the OMCC to that Port-ID (see "Tables: Alloc-ID, Port-ID
       and multicast"), replacing an earlier binding; deactivate unbinds it if
       it is bound; every copy is acknowledged.
     - T
     - OMCI messages pass both ways over ``/dev/pon0omci0``.
   * - PON-ID (identifier 21) is broadcast about once a second; it is not
       recorded in the ``ploamlog`` ring and not acted on. Other identifiers are
       recorded by name and dropped: the downstream names are those of G.984.3
       clause 9.2.3 and its Annexes C to E (2 and 7 deprecated), the upstream
       ones those of clause 9.2.4 and its Annex E.
     - P
     - ITU-T G.984.3 Annexes C, D and E.

PLOAM transmit
==============

The ONU sends an upstream PLOAM message (PLOAMu, 13 octets) in an allocation
whose Flags request it. The MAC has five slots for upstream messages, selected
in ``RTL9602C_PLOAM_TX_CTRL``: two queues and three templates. The driver writes
a message of twelve octets into a slot and the MAC adds the CRC. The driver
sends acknowledgments through the urgent queue, Password, Encryption_Key and REI
messages through the normal one, and arms the three templates.

Writing a message (``rtl9602c_ploam_enqueue()``), under ``ploam_lock``:

1. For the normal or urgent queue, check the queue's FULL bit and refuse the
   message with ``-EBUSY`` if it is set. (I)
2. Select the slot in ``RTL9602C_PLOAM_TX_CTRL_SLOT`` with
   ``RTL9602C_PLOAM_TX_CTRL_PUSH`` clear. Purpose: the data window addresses the
   slot selected last. (M)
3. Write ``RTL9602C_GPON_PLOAM_PAYLOAD_WORDS`` (6) words into
   ``RTL9602C_PLOAM_TX_DATA(i)``: octets 1 to 12, two to a word with the earlier
   in bits 15:8. (M)
4. Set ``RTL9602C_PLOAM_TX_CTRL_PUSH`` (bit 0). Purpose: hand the message to the
   slot. (M)
5. Record the write in the ``ploamlog`` ring with the slot and the queue flags.
   (M)

.. list-table::
   :header-rows: 1
   :widths: 36 8 30 26

   * - Slot
     - Number
     - What the MAC does with it
     - What the driver writes
   * - ``RTL9602C_PLOAM_TX_CTRL_SLOT_NORMAL``
     - 0
     - A queue: its messages go out in the ONU's allocations.
     - Password, Encryption_Key and REI messages.
   * - ``RTL9602C_PLOAM_TX_CTRL_SLOT_URGENT``
     - 1
     - A queue, as the normal one.
     - Acknowledge messages, and Dying_Gasp before a restart.
   * - ``RTL9602C_PLOAM_TX_CTRL_SLOT_DYING_GASP``
     - 5
     - A template that the MAC sends by itself.
     - Dying_Gasp, at bring-up.
   * - ``RTL9602C_PLOAM_TX_CTRL_SLOT_SN_ONU``
     - 6
     - A template that answers every discovery grant, without software.
     - Serial_Number_ONU, while a serial number is set.
   * - ``RTL9602C_PLOAM_TX_CTRL_SLOT_NO_MESSAGE``
     - 7
     - A template that the MAC sends by itself.
     - No message, at bring-up.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - An upstream PLOAM message (PLOAMu, 13 octets) is sent in an allocation
       whose Flags request it (bit 10), which the OLT should do only for the
       default Alloc-ID. In discovery and ranging the answer to the OLT's
       request is a burst of PLOu and a PLOAMu carrying Serial_Number_ONU.
     - P
     - ITU-T G.984.3 clause 8.2.3, clause 8.1.3.6.2 and clause 10.2.5.2.
   * - ``RTL9602C_PLOAM_TX_CTRL`` (``0x7050c0``):
       ``RTL9602C_PLOAM_TX_CTRL_SLOT`` (bits 10:8) selects what the data window
       ``RTL9602C_PLOAM_TX_DATA(i)`` (``0x7050e0`` + 4i) addresses.
       ``RTL9602C_PLOAM_TX_CTRL_SLOT_NORMAL`` (0) and
       ``RTL9602C_PLOAM_TX_CTRL_SLOT_URGENT`` (1) are queues of messages sent in
       the ONU's allocations; ``RTL9602C_PLOAM_TX_CTRL_SLOT_DYING_GASP`` (5),
       ``RTL9602C_PLOAM_TX_CTRL_SLOT_SN_ONU`` (6) and
       ``RTL9602C_PLOAM_TX_CTRL_SLOT_NO_MESSAGE`` (7) hold templates that the
       MAC sends by itself. The serial-number template answers every discovery
       grant without software.
     - M
     - The ``uscntr`` view: 979 transmissions from the templates against 957
       serial-number requests, and none from the queues.
   * - REI (identifier 8, G.984.3 clause 9.2.4.8): octets 3 to 6 the BIP
       errors of the last BER interval, most significant first, and the low 4
       bits of octet 7 a sequence number that counts the reports modulo 16.
       Once BER_Interval has set an interval, the worker sends one on the
       normal queue each time the interval has passed, in O5, with the BIP
       errors counted since the last one, in bits as G.984.3 clause 11.2.2
       counts them (``rtl9602c_gpon_rei_tick()``); it keeps the cadence to
       within its 50 ms period. The interval goes with the ONU-ID, and the OLT
       sets it again after the next ranging.
     - M, P
     - M: BER_Interval of 8000 frames injected through ``ploaminject``: the
       Acknowledge followed, then an REI with 0 errors at 25.82, 26.83, 27.84,
       28.79 and 29.80 s, sequence 0 to 4; an interval of 0 stopped them. With
       BIP errors present the reports have not been observed: the board's
       fibre gives none in O5. P: the count in bits, ITU-T G.984.3 clause
       11.2.2.
   * - The data window addresses the slot that was selected last, so data
       written before the slot is selected lands in whichever slot was selected
       before. ``ploam_lock`` serialises the writers.
     - M
     - The No message template read back ``ff 04 aa aa aa aa``, data meant for
       another slot.
   * - The MAC generates octet 13, the CRC; the driver writes twelve octets.
     - T
     - The OLT accepts the ONU's PLOAM messages.
   * - ``RTL9602C_PLOAM_TX_CTRL_NORMAL_EMPTY`` (bit 7) and
       ``RTL9602C_PLOAM_TX_CTRL_URGENT_EMPTY`` (bit 5) are 1 while their queue
       holds nothing: accepting a message clears the bit, sending it sets it
       again.
     - M
     - The register read ``0x000000a0`` before a message was pushed to the
       normal queue and ``0x00000020`` after, with the normal queue empty again
       10 ms later.
   * - Before writing to the normal or urgent queue the driver checks
       ``RTL9602C_PLOAM_TX_CTRL_NORMAL_FULL`` (bit 6) or
       ``RTL9602C_PLOAM_TX_CTRL_URGENT_FULL`` (bit 4). When it is set the driver
       refuses the message with ``-EBUSY``, records the refusal in the
       ``ploamlog`` ring and warns, rate-limited, because the data window is
       taken to be the queue's tail and a write would overwrite the message
       waiting there.
     - I
     - That a write into a full queue overwrites its tail is unverified; the
       refusal has never fired.
   * - Before an orderly restart, power-off or halt, while the ONU is in O4 or
       O5, a reboot notifier sends Dying_Gasp (the ONU-ID, 3, zero) on the
       urgent queue, at most ``RTL9602C_GPON_DG_COPIES`` (3) times, each copy
       given ``RTL9602C_GPON_DG_WAIT_MS`` (50 ms) to leave the queue, and stops
       at the first that does not; it logs ``Dying_Gasp sent <n> of 3 times
       before the restart``. G.984.3 has the ONU send it at least three times
       when it is about to change its powering conditions, so that the OLT
       does not report the loss of the ONU that follows.
     - M
     - ITU-T G.984.3 clause 9.2.2 and clause 9.2.4.3. ``reboot`` in O5 logged
       ``sent 1 of 3``: the test OLT gives the ONU no allocation after the
       first copy. Sent without a restart, a copy left the queue in 0.56 ms,
       the next two waited 2 s in vain, and the OLT deactivated the ONU and
       activated it again about 4 s later.
   * - At bring-up the driver arms the No message template (octet 1 255, octet 2
       4, the rest zero) and then the Dying_Gasp template (255, 3, zero). Every
       allocation that asks for a PLOAMu carries a message, so the No message
       template must hold a well-formed one.
     - M
     - Before it was armed, the No message template held leftover bytes after a
       correct header; after arming it read back ``ff 04`` and zeros. ITU-T
       G.984.3 clause 9.2.4.3 and clause 9.2.4.4.
   * - The Serial_Number_ONU template (``rtl9602c_gpon_arm_sn()``): octet 1 255,
       octet 2 1, octets 3 to 10 the serial number
       (``RTL9602C_PLOAM_SN_ONU_SERIAL`` 2, ``RTL9602C_GPON_SN_BYTES`` 8), octet
       11 zero, and octet 12 (``RTL9602C_PLOAM_SN_ONU_FLAGS`` 11) ``0x06``: G,
       GEM transport supported (``RTL9602C_PLOAM_SN_ONU_GEM``, bit 2), and TT,
       the transmit power level the ONU sends at
       (``RTL9602C_PLOAM_SN_ONU_POWER_LEVEL``, bits 1:0):
       ``RTL9602C_PLOAM_SN_ONU_POWER_LEVEL_HIGH`` (2, high) less the mode the
       optics last said they are at, so 2 at mode 0. It is armed at bring-up,
       on Upstream_Overhead, on Assign_ONU-ID, when a serial number is set and
       whenever the optics have answered a request for a level, and not at all
       while no serial number is set.
     - P
     - ITU-T G.984.3 clause 9.2.4.1; the ONU reaches O5 on an OLT.
   * - The ONU waits a random delay before each serial-number response, 0 to
       48 us in units of 32 bytes and new for each response, and reports it in
       the message: twelve bits, the high eight in octet 11 and the low four
       in the high four bits of octet 12. In ranging the field is 0.
     - P
     - ITU-T G.984.3 clause 9.2.4.1, clause 10.4.2.1 and clause A.6.5.2.
   * - The template carries a random delay of 0, and the driver applies none.
       No control of a random delay of the MAC's own has been found yet; bit
       11 of ``RTL9602C_GTC_US_CTRL``, which the driver leaves at its reset
       value, taken to be 1, is the one candidate. The driver writes no random
       value into the template: the OLT takes the field as the delay the ONU
       applied, and a value the burst did not carry would place the response
       up to 48 us from where the OLT reckons it. Where one ONU answers a
       request the delay changes nothing; ONUs at one distance that answer
       the same request can collide without it.
     - I
     - The register read before the bring-up; a capture of the serial-number
       responses at the OLT, their arrival against the grant and their octets
       11 and 12, with bit 11 at 1 and at 0 (written under the key); or two
       ONUs at one distance, discovered together, with bit 11 at each value.
   * - TT says the level the ONU sends at, in the reverse of the coding of
       Upstream_Overhead's level modes: mode 0, normal, the highest, is TT 2,
       high. The ONU sends at mode 0, and the ONU reaches O5 on the test OLT,
       which gives mode 0, with TT 2.
     - T
     - ITU-T G.984.3 clause 9.2.3.1 note 4 and clause 9.2.4.1 note 2; the ONU
       reaching O5 with the template.
   * - The template is written again, unchanged, on entering O4; whether that is
       needed is unverified.
     - I
     - Required: unverified; the ONU reaches O5 on an OLT.
   * - Acknowledge (``rtl9602c_gpon_ploam_ack()``): octet 1 the ONU's ONU-ID,
       octet 2 9 (``RTL9602C_PLOAM_US_ACKNOWLEDGE``), octet 3 the downstream
       message's identifier, octets 4 to 12 octets 1 to 9 of the downstream
       message; on the urgent queue.
     - P
     - ITU-T G.984.3 clause 9.2.4.9; acknowledged Assign_Alloc-ID and
       Configure_Port-ID keep the activation.
   * - ``RTL9602C_PLOAM_TX_CFG`` (``0x705100``), ``RTL9602C_PLOAM_TX_CFG_FLUSH``
       (bit 4): a rising edge empties the upstream PLOAM queues. On Ranging_Time
       in O4 the driver writes it 0 then 1 and leaves it 1, so that nothing
       queued during discovery goes out ahead of the first acknowledgment.
     - I
     - Required: unverified; the ONU reaches O5 on an OLT and OMCI messages pass
       both ways.
   * - The identifiers the driver uses. Downstream: Upstream_Overhead 1
       (``RTL9602C_PLOAM_DS_UPSTREAM_OVERHEAD``), Assign_ONU-ID 3
       (``RTL9602C_PLOAM_DS_ASSIGN_ONUID``), Ranging_Time 4
       (``RTL9602C_PLOAM_DS_RANGING_TIME``), Deactivate_ONU-ID 5
       (``RTL9602C_PLOAM_DS_DEACTIVATE_ONUID``), Assign_Alloc-ID 10
       (``RTL9602C_PLOAM_DS_ASSIGN_ALLOCID``), Configure_Port-ID 14
       (``RTL9602C_PLOAM_DS_CONFIGURE_PORTID``), Extended_Burst_Length 20
       (``RTL9602C_PLOAM_DS_EXTENDED_BURST_LENGTH``), PON-ID 21
       (``RTL9602C_PLOAM_DS_PONID``). Upstream: Serial_Number_ONU 1
       (``RTL9602C_PLOAM_US_SERIAL_NUMBER_ONU``), Dying_Gasp 3
       (``RTL9602C_PLOAM_US_DYING_GASP``), No message 4
       (``RTL9602C_PLOAM_US_NO_MESSAGE``), Acknowledge 9. ONU-ID 255
       (``RTL9602C_ONU_ONUID_UNASSIGNED``) is the broadcast address downstream
       and the unassigned ONU upstream.
     - P
     - ITU-T G.984.3 clause 9.2.3, clause 9.2.4 and clause 5.5.2, and Annex C.

The burst overhead
==================

Every upstream burst opens with the physical layer overhead: the guard time, in
which the ONU sends no light, the preamble and the delimiter, and then the burst
header. The OLT dictates the preamble and the delimiter in Upstream_Overhead and
Extended_Burst_Length, and the MAC sends them immediately before the byte that
StartTime points at. The driver programs the overhead in two places:
``RTL9602C_BURST_DATA(i)`` holds the overhead bytes, the preamble and then the
delimiter, and ``RTL9602C_BURST_CFG`` holds the overhead's length in bytes and a
repeat index, which is taken to name the byte that the MAC repeats to make up
the length. The length is the one thing that changes with the state.

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - When
     - Length written to ``RTL9602C_BURST_CFG``
   * - Upstream_Overhead, in O2
     - ``RTL9602C_BURST_CFG_PRERANGED`` (``0x080c``): repeat index 8 and 12
       bytes. The type 3 lengths are zeroed, so without Extended_Burst_Length
       this length stays in every state.
   * - Extended_Burst_Length, in O3
     - The length computed from the pre-ranged type 3 length.
   * - Ranging_Time, in O4
     - The length computed from the ranged type 3 length.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Every upstream burst opens with the physical layer overhead: the guard
       time (no light), the preamble and the delimiter, then the three-octet
       burst header. The ONU sends preamble and delimiter as the OLT dictates in
       Upstream_Overhead and Extended_Burst_Length, immediately before the byte
       StartTime points at. Type 3 preamble bytes carry octet 6's pattern; the
       delimiter field is 24 bits, and a 20-bit delimiter (``0xB5983`` proposed)
       is completed at its start by preamble bits.
     - P
     - ITU-T G.984.3 clause 5.7, clause 8.2.2 and clause 9.2.3.1 notes 1 to 3;
       the guard time without light: ITU-T G.984.2 Appendix I.
   * - At 1244.16 Mbit/s the total burst mode overhead is 96 bits (mandatory):
       guard at least 32, preamble 44 and delimiter 20 suggested; the
       transmitter's enable and disable times at most 16 bits each. With no
       Extended_Burst_Length the type 3 length is that total less the guard,
       type 1, type 2 and delimiter bits.
     - P
     - ITU-T G.984.2 Appendix I, Table I.2; ITU-T G.984.3 clause 9.2.3.20 note
       2.
   * - ``RTL9602C_BURST_DATA(i)`` (``0x705080`` + 4i, i from 0 to 11:
       ``RTL9602C_GPON_BOH_BYTES``) each hold one overhead byte in
       ``RTL9602C_BURST_DATA_BYTE`` (bits 7:0). The driver writes nine preamble
       bytes in the order of G.984.2, then the three delimiter octets: as many
       bytes as the guard counts in the length (at most four), which carry the
       type 3 pattern; the type 1 preamble's whole bytes,
       ``RTL9602C_PREAMBLE_TYPE1`` (``0xff``); the type 2 preamble's,
       ``RTL9602C_PREAMBLE_TYPE2`` (``0x00``); and the type 3 pattern for the
       rest. The last preamble byte, at the repeat index, always carries the
       pattern, so with a 32-bit guard at most four bytes are left for the type
       1 and type 2 preambles; what does not fit is cut, and the length still
       counts it.
     - M
     - Read back in the ``usburst`` view as nine ``aa`` then ``ab 59 83`` with
       the test OLT's values (type 1 and type 2 zero), and as ``aa aa aa aa ff
       ff 00 00 aa ab 59 83`` after an injected Upstream_Overhead with 16 bits
       of each.
   * - ``RTL9602C_BURST_CFG`` (``0x705054``): ``RTL9602C_BURST_CFG_LEN`` (bits
       7:0) is the overhead length in bytes; ``RTL9602C_BURST_CFG_REPEAT_IDX``
       (bits 11:8) is written ``RTL9602C_BURST_CFG_REPEAT_IDX_DEF`` (8), the
       index of the last pattern byte before the delimiter. The register reads
       ``0x0880`` at reset.
     - T
     - The ONU reaches O5 on an OLT with the lengths 12, 103 and 63; the
       register read at reset.
   * - That ``RTL9602C_BURST_CFG_REPEAT_IDX`` names the byte the MAC repeats to
       stretch the preamble to the length is inferred from its value.
     - I
     - Another index, with the OLT's delineation of the bursts as the witness.
   * - The length (``rtl9602c_gpon_boh_len()``): with no type 3 length,
       ``RTL9602C_BURST_CFG_LEN_DEF`` (12 bytes, the 96-bit total of G.984.2 in
       the row above); otherwise the guard bits (at most 32) divided by 8, plus
       the type 3 bytes, plus 3 for the delimiter, plus the type 1 and type 2
       bits divided by 8, clamped between 12 and ``RTL9602C_BURST_CFG_LEN_MAX``
       (128, the most G.984.3 allows). When Extended_Burst_Length asks for
       more, the driver warns: ``the OLT asks for more burst overhead than the
       128 bytes G.984.3 allows, which are sent``.
     - T
     - The ONU reaches O5 on an OLT with 103 and 63 bytes, from the test OLT's
       values.
   * - When the length is written: on Upstream_Overhead the driver writes
       ``RTL9602C_BURST_CFG_PRERANGED`` (``0x080c``: index 8, 12 bytes); on
       Extended_Burst_Length in O3, the length from its pre-ranged value; on
       Ranging_Time in O4, the length from its ranged value. Upstream_Overhead
       zeroes both stored lengths, so without Extended_Burst_Length the overhead
       stays at 12 bytes in every state.
     - M
     - With ``0x080c`` the OLT decoded the serial-number burst and assigned an
       ONU-ID; the lengths of the previous row then took the ONU to O5. ITU-T
       G.984.3 clause 9.2.3.20.
   * - The test OLT's Upstream_Overhead: guard 32 bits, type 1 and type 2 zero,
       pattern ``0xaa``, delimiter ``0xab5983``, flags ``0x20`` (the
       pre-assigned delay applies and is zero; no extra serial numbers; power
       level mode 0). Its Extended_Burst_Length: 96 bytes pre-ranged, 56 ranged.
     - M
     - The kernel log lines ``burst: ...``, ``Upstream_Overhead raw: ...`` and
       ``burst length: ...``.
   * - The OLT decodes the bursts with type 1 and type 2 bytes in the
       preamble: after an injected Upstream_Overhead with 16 bits of each, the
       ONU reached O5 and OMCI passed both ways.
     - T
     - The ONU reaching O5 after the injection on the test OLT, which ignores
       the preamble's content as long as the delimiter is found.
   * - That the MAC sends the bytes before the repeat index once, at the start
       of the overhead, so that the type 1 and type 2 bytes open the preamble
       as G.984.2 orders it, is inferred from the index's value.
     - I
     - An OLT that checks the type 1 and type 2 preambles, or a capture of the
       upstream light.
   * - The guard is counted into the length only up to 32 bits; that the length
       counts the guard at all rests on the lengths of the rows above reaching
       O5.
     - I
     - ITU-T G.984.3 clause 9.2.3.20 note 2; an OLT with a guard longer than 32
       bits.

Upstream timing
===============

The ONU sends each burst at a fixed distance from the downstream frame that
carried its allocation. The distance is the ONU's response time plus the
equalization delay that the OLT assigns in ranging, so that the bursts of all
ONUs arrive at the OLT as if they were equally far away. The driver writes the
response time once at bring-up (``RTL9602C_GTC_US_RESP_DELAY``) and the
equalization delay whenever Upstream_Overhead or Ranging_Time gives one
(``RTL9602C_RANGING_EQD``). The rest of this section is the laser timing, the
upstream interrupt latch and the upstream control, transmit mode, power save,
signal-detect and burst-merge registers that the bring-up writes.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The ONU keeps an upstream frame clock locked to the downstream frame and
       offset by its response time (35 us +/- 1 us) plus a requisite delay: in
       O3 the pre-assigned delay and a random delay, in O4 the pre-assigned
       delay, in O5 the equalization delay of Ranging_Time. An upstream frame is
       19440 bytes at 1.24416 Gbit/s. Delays reach about 250 us over 20 km,
       optionally 625 us.
     - P
     - ITU-T G.984.3 clause 10.4.1, clause 10.4.2.1, clause 10.4.3.1,
       clause 10.4.4, clause 10.4.3.4 and clause 8.2.
   * - ``RTL9602C_GTC_US_RESP_DELAY`` (``0x705040``, written under the key):
       ``RTL9602C_GTC_US_RESP_DELAY_COARSE`` (bits 15:7) =
       ``RTL9602C_GTC_US_RESP_DELAY_COARSE_DEF`` (290) and
       ``RTL9602C_GTC_US_RESP_DELAY_FINE`` (bits 6:0) =
       ``RTL9602C_GTC_US_RESP_DELAY_FINE_DEF`` (50), both in units of 16 bytes
       (``RTL9602C_GTC_US_RESP_DELAY_COARSE_BITS``, 128 bits). Together they
       delay the ONU's response.
     - M
     - Raising the fine part from 50 to 114 moved the delay the OLT assigned by
       -8192 bits (64 times 128); the ONU reaches O5 on an OLT with 290 and 50.
   * - (290 + 50) times 128 bits is 43520 bits, 34.98 us at 1.24416 Gbit/s,
       which is the response time of G.984.3. That the register realises the ONU
       response time is inferred from that equality.
     - I
     - Ranging with other values and comparing the delay the OLT assigns.
   * - ``RTL9602C_RANGING_EQD`` (``0x705044``): ``RTL9602C_RANGING_EQD_FRAMES``
       (bits 26:24) whole upstream frames and ``RTL9602C_RANGING_EQD_BITS``
       (bits 17:0) the remainder in bits. ``rtl9602c_gpon_set_eqd(delay)`` reads
       the coarse response delay back from ``RTL9602C_GTC_US_RESP_DELAY``, adds
       it times 128 bits to the delay, and splits the sum at
       ``RTL9602C_GPON_EQD_FRAME_BITS`` (155520, one upstream frame in bits)
       into the two fields, written as one word.
     - T
     - The ONU reaches O5 on an OLT: the bursts land where the OLT listens, in
       ranging and in O5.
   * - The delay given to ``rtl9602c_gpon_set_eqd()``: on Upstream_Overhead the
       pre-assigned delay in bits when e is set, else 0; on Ranging_Time the
       equalization delay. The driver keeps the delay last written, compares
       against it in O5, and logs each write (``EQD: ...``).
     - P
     - ITU-T G.984.3 clause 10.4.2.1, clause 10.4.3.1 and clause 10.4.4.
   * - On the test OLT the assigned equalization delay is about 97280 bits. It
       is the same on every activation within a boot and moves by a few bits
       from one boot to the next; it does not step between activations.
     - M
     - The ``EQD:`` line of the kernel log: 43 activations in one boot, each
       started by an injected broadcast Deactivate_ONU-ID and back in O5 within
       1.0 to 1.6 s, all at 97286; five more restarts at 97287, 97281, 97280,
       97279 and 97282, each over five activations; and four boots after the
       module was taken out of its cage and put back, at 97279, 97284, 97283
       and 97279, each over four activations.
   * - ``RTL9602C_GTC_US_LASER`` (``0x70504c``):
       ``RTL9602C_GTC_US_LASER_ON_LEAD`` (bits 13:8, how early the laser is
       keyed before a burst) = ``RTL9602C_GTC_US_LASER_ON_LEAD_DEF`` (``0x20``);
       ``RTL9602C_GTC_US_LASER_OFF_LAG`` (bits 5:0, how long it stays keyed
       after) = ``RTL9602C_GTC_US_LASER_OFF_LAG_DEF`` (``0x28``); the lag resets
       to 24.
     - M
     - Lags below 40 broke discovery; leads of 32 and 0 made no measurable
       difference; the lag read 24 before the bring-up wrote it.
   * - The unit of the lead and the lag is not measured, nor whether a lead of
       32 is needed.
     - I
     - ITU-T G.984.2 Table I.2 bounds the transmitter's enable and disable times
       at 16 bits; the ONU reaches O5 on an OLT with other values.
   * - On the test PON, a second unit of this model, whose bursts follow this
       ONU's in every upstream frame, lost no upstream frame while this ONU
       sent, idle or flat out.
     - M
     - Frames of 1514 B from each unit's host side, counted where they left
       the OLT: 120000 at 2000 a second from each unit at once, all arrived
       from both; 120000 from the second unit with this ONU disabled (O7), all
       arrived; 60000 from the second unit at 2000 a second while this ONU was
       offered more than its upstream carries, all arrived. The second unit
       stayed in O5 with no alarm.
   * - ``RTL9602C_GTC_US_IRQ_LATCH`` (``0x705000``) latches upstream GTC events
       and clears when read: ``RTL9602C_GTC_US_IRQ_SD_MISMATCH`` (bit 9), taken
       to mean that the optics' signal detect disagreed with the keyed burst;
       ``RTL9602C_GTC_US_IRQ_SD_TOO_LONG`` (bit 8), that it stayed asserted too
       long. ``RTL9602C_GTC_US_IRQ_EN`` (``0x705004``) is written
       ``RTL9602C_GTC_US_IRQ_EN_DEF`` (``0xa4``); it is 0 at reset. The
       interrupt thread reads the latch whenever ``RTL9602C_GPON_IRQ_GTC_US`` is
       set, records a non-zero value in the ``ploamlog`` ring and warns,
       rate-limited, on bit 8 or 9.
     - M
     - In O5 a latch left unread kept the source asserted, the interrupt thread
       re-entered without end and the watchdog reset the board; bits 8 and 9
       were never set under this driver.
   * - Bits 8 and 9 stay clear with the laser on outside the bursts: a test
       build forced the laser driver on through the module's own configuration
       table for 50 ms, 500 ms and 2 s in O5, and neither bit was set in reads
       of the latch every 10 ms, though the module showed the light; bit 10 was
       set at every read, before and after as well. So on this board the MAC's
       check of the optics' signal detect against the burst reports nothing,
       and the driver cannot tell a laser stuck on by itself.
     - M
     - The forced runs above; the module's indication of light, table 2
       ``0xb7`` bits 7 and 5, read every 10 ms: set throughout each forced
       run, and in normal O5, under the test OLT's allocation of 15417 of the
       19440 bytes of every upstream frame, set in runs of up to 24 reads.
   * - In O5, with both upstream PLOAM queues idle and no upstream FEC,
       ``RTL9602C_GTC_US_IRQ_STAT`` (``0x705008``) reads ``0x000000a0``: bits 7
       and 5 set, as bits 7 and 5 of ``RTL9602C_PLOAM_TX_CTRL`` read while the
       normal and the urgent queue are empty (see "PLOAM transmit"), and bit 2
       clear.
     - M
     - The ``us`` line of the ``gpon`` view, ``us    000000a0  fec 0``, on the
       test OLT, which sets Use FEC in no allocation.
   * - What bits 8 and 9 detect where the optics' signal detect reaches the
       MAC, what bit 10 reports, and which events bits 5 and 7 of the enable
       select, are not established. Bits 7 and 5 are taken to follow the normal
       and the urgent PLOAM queue being empty, from their idle value in the row
       above; bit 2 a change of the upstream FEC state (see "FEC and
       encryption").
     - I
     - A burst keyed with the laser disabled (bit 9); the status read while a
       message waits in each PLOAM queue (bits 7 and 5); an OLT that sets Use
       FEC (bit 2).
   * - ``RTL9602C_GTC_US_CTRL`` (``0x705014``, locked): the driver sets
       ``RTL9602C_GTC_US_CTRL_IND_NORMAL_PLOAM`` (bit 10: a queued normal PLOAM
       message is flagged as waiting in the burst header's Ind field) and
       ``RTL9602C_GTC_US_CTRL_BEN_POL`` (bit 3, the polarity of the burst-enable
       output), and clears ``RTL9602C_GTC_US_CTRL_PLOAM_DIS`` (bit 9, which
       would stop the PLOAMu field) and ``RTL9602C_GTC_US_CTRL_SCRAMBLE_DIS``
       (bit 0, which would switch the upstream scrambler off).
     - T
     - The ONU reaches O5 on an OLT; scrambling is ITU-T G.984.3 clause 8.2.1,
       the Ind field clause 8.2.2.3.
   * - Which level ``RTL9602C_GTC_US_CTRL_BEN_POL`` selects, and whether bit 10
       does what its use suggests, are not established.
     - I
     - Clearing each under the key and checking that the ONU reaches O5 on an
       OLT.
   * - ``RTL9602C_GTC_US_TX_MODE`` (``0x705200``, written under the key):
       ``RTL9602C_GTC_US_TX_MODE_SMALL_SSTART`` (bit 0) is set, so that a burst
       whose StartTime is smaller than its overhead starts in the previous
       upstream frame; ``RTL9602C_GTC_US_TX_MODE_SUPPRESS_DIS`` (bit 1) is
       clear, so that the automatic laser suppression stays enabled. Bit 0
       resets to 1 and the write changes nothing; it is written because the
       driver depends on it.
     - M
     - Bit 0 read 1 before bring-up; the test OLT grants StartTime 16 in every
       frame (the ``bwmap`` view), so every burst begins in the previous frame.
   * - The meanings of bits 0 and 1 are inferred from their use; what the laser
       suppression suppresses is not known.
     - I
     - Clearing bit 0 under the key with StartTime 16 grants, and checking that
       the ONU reaches O5 on an OLT.
   * - Power save around bursts: under the key the driver sets
       ``RTL9602C_GTC_US_POWERSAVE_EN`` (bit 0 of ``RTL9602C_GTC_US_POWERSAVE``,
       ``0x70526c``, 0 at reset), keeping the transmitter enabled only around
       bursts; in ``RTL9602C_GEM_US_POWERSAVE`` (``0x706024``) it writes
       ``RTL9602C_GEM_US_POWERSAVE_LAG`` (bits 20:16) = ``0x10`` and
       ``RTL9602C_GEM_US_POWERSAVE_LEAD`` (bits 9:0) = ``0x100``, the window
       around a burst in cycles; that register reads ``0x30`` at reset. The
       ``usburst`` view also shows bit 31 of it.
     - I
     - Required: unverified; the registers read as stated at reset.
   * - ``RTL9602C_GTC_US_SD_THR`` (``0x705188``) holds limits on the optics'
       signal detect against the keyed burst, ``RTL9602C_GTC_US_SD_THR_MISMATCH``
       (bits 30:16) among them. The driver sets the mismatch field to
       ``RTL9602C_GTC_US_SD_THR_MISMATCH_DEF`` (``0x50``) and leaves the rest of
       the word. It reads ``0x00a04bfa`` at reset, and so ``0x00504bfa``
       after.
     - I
     - Required: unverified; the register read at reset.
   * - ``RTL9602C_GEM_US_BURST_MERGE_THR`` (bits 6:0 of
       ``RTL9602C_GEM_US_BURST_MERGE``, ``0x706260``) =
       ``RTL9602C_GEM_US_BURST_MERGE_THR_DEF`` (40): the threshold below which
       adjacent burst ends are merged; its unit is not measured.
     - I
     - Required: unverified; the ONU reaches O5 on an OLT and upstream GEM
       frames reach the OLT with the ONU's state held.

The time of day
===============

The OLT tells the ONU the time of day through the OMCC (ITU-T G.984.3
Amendment 2 clause 10.4.6): it names a downstream frame N by its superframe
counter and gives TstampN, the time at which the frame's first bit reaches a
hypothetical ONU with an equalization delay and a response time of zero. The
OMCI daemon passes the pair, the Time of day information of the OLT-G managed
entity (ITU-T G.988 clause 9.12.2), to the PON class's ``tod-set``, and
``rtl9602c_gpon_tod_set()`` sets the switch's PTP clock at frame N to the time
at which the frame reaches this ONU:

    TrecvN = TstampN - (EqD + RspTime) x n1490 / (n1310 + n1490)

with the equalization delay last written (``gpon_eqd_delay``), the response
time of ``RTL9602C_GTC_US_RESP_DELAY`` (both parts, in units of 128 bits), and
the group indices that clause 10.4.6.3.2 gives for typical fibre, a factor of
0.500085 (``rtl9602c_gpon_tod_delta_ns()``). The clock takes the switch's PON
time as the superframe counter passes from N to N + 1, a frame after frame N
began, so the driver gives the switch TrecvN + 125 us (the host operation
``tod_arm``, which writes the PON time and its enable; see "The PON time of
day" in Documentation/networking/dsa/rtl9602c-ptp.rst), then arms N with an
edge of ``RTL9602C_GTC_DS_TOD_SUPERFRAME_REQ``. A Ranging_Time or
Ranging_Adjustment that changes the delay before frame N gives the switch the
time again (clause 10.4.6.2, step 7); a new ``tod-set`` replaces one not yet
taken. The ONU's internal delays, from the optical connector to the
superframe counter, are not compensated.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_GTC_DS_TOD_SUPERFRAME`` (``0x70104c``): a 0-to-1 edge of
       bit 31 (``_REQ``) arms the frame in bits 29:0 (``_CNT``), and the PTP
       clock takes the PON time when the superframe counter passes from that
       frame to the next, while the PON time's enable is set; the hardware
       then clears the enable. With the enable clear, or with bit 31 written
       while it reads 1, the clock does not take the time. Bit 30 reads 1
       before and after, whatever is written.
     - M
     - The clock read with lab-only register writes: a load with the edge,
       none without the enable, none without the edge, none with bit 31
       written again; the enable read 0 after the load.
   * - The clock takes the time 124 us after the counter turns to N, one
       frame within the measurement: 131.0, 131.2, 121.9 and 112.4 us in four
       loads, each fitted over 130 reads of the clock latched beside the
       counter in 22 s, the latch taken as uniform within a frame (residuals
       +/-65 us). The clock ran 13.6 to 15.6 ppm fast of the OLT's frames.
     - M
     - Lab-only debugfs reads of the latched clock and the counter, back to
       back with the interrupts off.
   * - Through ``tod-set`` the clock at the start of frame N read TrecvN less
       2.5 us, within the measurement: frame N 7917 frames ahead, TstampN
       1000000000.0 s, EqD 97328 bits and the response time 43520 bits, a
       Delta of 56613 ns; the PON time read 1000000000.000068384 and its
       enable 0 after the frame.
     - M
     - The ``tod`` line of the ``gpon`` view, ``ptpstat``, and the fit above
       over 130 reads.
   * - From the OLT through the OMCI daemon: a Set of the OLT-G's Time of day
       information, put into the OMCI port as if the OLT had sent it, reached
       ``tod-set`` with its frame and TstampN, and the clock at the start of
       frame N read TrecvN less 3 to 5 us, as through ``tod-set`` alone; the
       clock then ran 14.7 and 15.0 ppm fast of the OLT's frames. A Set naming
       a frame 1000 frames past was refused with ``-ERANGE``, the daemon
       logged that the clock was not set, and the clock ran on.
     - M
     - Two Sets 15172 and 31399 frames ahead, EqD and response time 140851
       bits, a Delta of 56614 ns; the daemon's log, the kernel's, the ``tod``
       line of the ``gpon`` view, and the fit above over 130 reads each. The
       messages went in through the lab-only ``CONFIG_PON_OMCI_INJECT``: the
       lab OLT sends no time of day.
   * - ``tod-set`` is refused with ``-ERANGE`` for a frame passed, fewer than
       ``RTL9602C_GPON_TOD_MIN_AHEAD`` (16) frames ahead, or half the
       counter's span or more ahead, and with ``-EAGAIN`` outside O5.
     - M, T
     - M: the refusals on the board for a frame 100 frames past, 5 frames
       ahead and half the span plus 20000 frames ahead, and an acceptance just
       under half the span. T: ``rtl9602c_test_tod_delta`` (KUnit); outside
       O5 is not exercised.
   * - A change of the equalization delay before frame N is followed.
     - I
     - Not exercised: an OLT that sends Ranging_Time in O5 with a new delay
       while a time of day is pending.

The bandwidth map
=================

The OLT tells the ONU when to send in the bandwidth map (BWmap), an array of
allocation structures at the start of every downstream frame. The MAC reads it
and sends a burst, or part of one, for each allocation to one of the ONU's
Alloc-IDs. The driver sets a filter by ONU-ID in ``RTL9602C_GTC_DS_CTRL`` and
uses a capture facility that records allocations, for the ``bwmap`` view and for
the grants that the ONU was ranged on.

Arming a capture (``rtl9602c_gpon_bwm_arm()``):

1. Write 0 to ``RTL9602C_BWMAP_CAPTURE_CTRL`` (``0x70200c``). (M)
2. Write ``RTL9602C_BWMAP_CAPTURE_CTRL_CLEAR`` (bit 14) with the count in
   ``RTL9602C_BWMAP_CAPTURE_CTRL_COUNT`` (bits 7:0). (M)
3. Write ``RTL9602C_BWMAP_CAPTURE_CTRL_START`` (bit 15) with the count. Purpose:
   the MAC records that many allocations. (M)

A captured allocation is two words:

.. list-table::
   :header-rows: 1
   :widths: 10 70 20

   * - Word
     - Field
     - Bits
   * - 0
     - ``RTL9602C_BWMAP_ALLOC_TCONT``, the T-CONT matched
     - 4:0
   * - 0
     - ``RTL9602C_BWMAP_ALLOC_MULTIFRAME``
     - 14:12
   * - 0
     - ``RTL9602C_BWMAP_ALLOC_DBRU``, the DBRu mode
     - 17:16
   * - 0
     - ``RTL9602C_BWMAP_ALLOC_FEC``, use FEC
     - 18
   * - 0
     - ``RTL9602C_BWMAP_ALLOC_PLOAMU``
     - 19
   * - 0
     - ``RTL9602C_BWMAP_ALLOC_BURST_START`` and
       ``RTL9602C_BWMAP_ALLOC_BURST_END``: the allocation opens or closes a
       burst
     - 20 and 21
   * - 0
     - ``RTL9602C_BWMAP_ALLOC_LAST``, last of its map
     - 22
   * - 0
     - ``RTL9602C_BWMAP_ALLOC_VALID``
     - 23
   * - 0
     - ``RTL9602C_BWMAP_ALLOC_FRAME``, the downstream frame count modulo 16
     - 27:24
   * - 1
     - ``RTL9602C_BWMAP_ALLOC_SSTART``, the StartTime
     - 15:0
   * - 1
     - ``RTL9602C_BWMAP_ALLOC_SSTOP``, the StopTime
     - 31:16

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The BWmap is an array of 8-byte allocation structures: Alloc-ID (12
       bits), Flags (12 bits: bit 10 send PLOAMu, bit 9 use FEC, bits 8 and 7
       the DBRu mode), StartTime and StopTime (16 bits each, bytes from the
       start of the upstream frame, StopTime the last byte), CRC. Alloc-ID 254
       addresses every ONU in O3 (a serial-number request: PLOAMu set, StopTime
       = StartTime + 12); a ranging request is addressed to the ONU's default
       Alloc-ID, its ONU-ID.
     - P
     - ITU-T G.984.3 clause 8.1.3.6, clause 8.1.3.6.1, clause 8.1.3.6.2 and
       clause 10.2.5.2.
   * - ``RTL9602C_GTC_DS_CTRL`` (``0x701014``):
       ``RTL9602C_GTC_DS_CTRL_BWMAP_MATCH_ONUID`` (bit 10) keeps only the
       allocations for this ONU as programmed;
       ``RTL9602C_GTC_DS_CTRL_BWMAP_ACCEPT_ALL`` (bit 11) treats every
       allocation as this ONU's. The driver sets bit 10 and clears bit 11 in
       every state, including while the ONU-ID is 255: the discovery grants are
       matched then as well.
     - M
     - In O3 over 6 s: with bit 10 set, 6 maps were accepted and 6 serial-number
       requests counted; with bit 11 set, 48086 maps were accepted, and a
       message queued for sending left within 10 ms, long before the next
       discovery grant.
   * - ``RTL9602C_GTC_DS_CTRL_EXTRA_SN`` (bits 7:6) receives Upstream_Overhead's
       ss bits, the number of extra serial-number transmissions per request.
       G.984.3 deprecates them (ss = 00), and the test OLT sends 00.
     - P
     - ITU-T G.984.3 clause 9.2.3.1 note 6.
   * - Once started, the MAC records that many allocations it matched to this
       ONU's T-CONTs in ``RTL9602C_BWMAP_CAPTURE_DATA(i)`` (``0x702400`` + 4i),
       two words each. ``RTL9602C_BWMAP_CAPTURE_STAT`` (``0x702010``) is printed
       raw; its fields are not measured.
     - M
     - The ``bwmap`` view.
   * - Entry word 0: ``RTL9602C_BWMAP_ALLOC_TCONT`` (bits 4:0, the T-CONT
       matched), ``RTL9602C_BWMAP_ALLOC_PLOAMU`` (bit 19),
       ``RTL9602C_BWMAP_ALLOC_VALID`` (bit 23). Word 1:
       ``RTL9602C_BWMAP_ALLOC_SSTART`` (bits 15:0) and
       ``RTL9602C_BWMAP_ALLOC_SSTOP`` (bits 31:16), the allocation's StartTime
       and StopTime.
     - M
     - In O4 and O5: T-CONT 16 with PLOAMu, StartTime 16 and StopTime 15432 in
       every frame (the ``bwmap`` view). A check with traffic on the board reads
       the view's T-CONT column to see that the bandwidth map grants the GEM
       port's T-CONT.
   * - The other fields of word 0 are displayed but not measured:
       ``RTL9602C_BWMAP_ALLOC_DBRU`` (bits 17:16, DBRu mode),
       ``RTL9602C_BWMAP_ALLOC_FEC`` (bit 18, use FEC),
       ``RTL9602C_BWMAP_ALLOC_BURST_START`` (bit 20) and
       ``RTL9602C_BWMAP_ALLOC_BURST_END`` (bit 21) (the allocation opens or
       closes a burst), ``RTL9602C_BWMAP_ALLOC_LAST`` (bit 22, last of its map),
       ``RTL9602C_BWMAP_ALLOC_MULTIFRAME`` (bits 14:12),
       ``RTL9602C_BWMAP_ALLOC_FRAME`` (bits 27:24, the downstream frame count
       modulo 16).
     - I
     - An OLT granting FEC or DBRu, or several allocations in one burst. The
       test OLT asks for no FEC: a capture of 16 allocations read FEC 0 in
       each.
   * - On Assign_ONU-ID the driver arms a capture of
       ``RTL9602C_GPON_BWM_O4_ENTRIES`` (16) allocations and reads it back when
       Ranging_Time arrives: the grants the ONU was ranged on, kept for the
       ``bwmap`` view.
     - T
     - The ``bwmap`` view, its "captured on entering O4" part.

Tables: Alloc-ID, Port-ID and multicast
=======================================

Three tables in the GPON MAC map identifiers to the driver's numbers. The
Alloc-ID table has one entry for each of 32 T-CONTs
(``RTL9602C_GPON_NUM_TCONT``) and holds the Alloc-ID whose allocations that
T-CONT takes. The driver uses entries 0 to 3 and 8 to 11, the eight T-CONTs
of the GEM ports (``rtl9602c_gem_tcont()``), for the Alloc-IDs that
Assign_Alloc-ID gives the ONU, and entry 16
(``RTL9602C_GPON_DEFAULT_TCONT_IDX``) for the default Alloc-ID, which carries
the upstream PLOAM messages and the OMCC. The Port-ID table has one entry for each of 128 flows
(``RTL9602C_GPON_NUM_FLOW``) and holds the GEM Port-ID that the MAC takes from
the downstream into that flow: the ONU's GEM port filter. Flow 64
(``RTL9602C_GPON_OMCC_FLOW``) is the OMCC's, and the GEM port code never gives
it out. The multicast filter table has 256 entries (``RTL9602C_GPON_NUM_MC``);
the driver only reads it and clears it. All three are RAM that neither the MAC
reset nor the chip reset clears.

.. list-table::
   :header-rows: 1
   :widths: 12 32 28 28

   * - Table
     - Command register
     - Write data
     - Read data
   * - Alloc-ID
     - ``RTL9602C_ALLOCID_TBL_CMD`` (``0x7010c0``)
     - ``RTL9602C_ALLOCID_TBL_WDATA`` (``0x7010c4``)
     - ``RTL9602C_ALLOCID_TBL_RDATA`` (``0x7010cc``)
   * - Port-ID
     - ``RTL9602C_GEM_DS_PORTID_TBL_CMD`` (``0x701100``)
     - ``RTL9602C_GEM_DS_PORTID_TBL_WDATA`` (``0x701104``)
     - ``RTL9602C_GEM_DS_PORTID_TBL_RDATA`` (``0x70110c``)
   * - Multicast
     - ``RTL9602C_GEM_DS_MC_TBL_CMD`` (``0x704084``)
     - ``0x704088``; the driver writes no entry.
     - ``0x704090``; the command register's VALID bit says whether the entry is
       in use.

Reading and writing an entry
----------------------------

Writing or clearing an entry is ``rtl9602c_gpon_tbl_op()``:

1. Write the command word with the operation and the index and EXEC clear.
   Purpose: data written before the index goes to another entry. (M)
2. Write the data word, the identifier in bits 11:0. (M)
3. Write the command word again with EXEC set. Purpose: start the operation. (M)
4. Poll DONE up to 100 times, 1 us apart. Purpose: the operation has completed
   when DONE is set; ``-ETIMEDOUT`` otherwise. (M)

Reading an entry is ``rtl9602c_gpon_tbl_read()``:

1. Write the command word with the read operation and the index and EXEC clear.
   (M)
2. Write the command word again with EXEC set. (M)
3. Poll DONE, with the same budget. (M)
4. Read VALID (bit 13) from the command register, which says whether the entry
   is in use, and the identifier from the read-data register. (M)

The multicast table is reached the same way, with an 8-bit index. The driver
only reads and clears its entries (``rtl9602c_gpon_mc_op()``), without the data
step.

Binding the OMCC
----------------

On Configure_Port-ID the driver binds the OMCC to its Port-ID
(``rtl9602c_gpon_omcc_bind()``), in flow ``RTL9602C_GPON_OMCC_FLOW`` (64):

1. Write the Port-ID into flow 64's entry of the Port-ID table. Purpose: the MAC
   takes the OMCC's downstream frames into flow 64. (T)
2. Type flow 64's downstream configuration (``RTL9602C_GEM_DS_FLOW_CFG(64)``) as
   OMCI. (T)
3. Give flow 64's upstream configuration (``RTL9602C_GEM_US_FLOW_CFG(64)``) the
   Port-ID. Purpose: the upstream frames of the flow carry it. (T)
4. Admit the flow's stream into the PON MAC (``rtl9602c_ponmac_sid_valid()``,
   see Documentation/networking/device_drivers/pon/rtl9602c.rst). (T)
5. Record the OMCC as bound, and report the OMCI port's link up. (T)

``rtl9602c_gpon_omcc_unbind()`` undoes all of it. It reports the link down
first, which drops the messages still waiting for the reader, and then clears
the Port-ID entry, the downstream type, the upstream Port-ID and the stream's
admission. OMCI messages pass both ways over ``/dev/pon0omci0`` with the OMCC
bound.

Facts about the tables
----------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The three tables are reached through a command register and data
       registers. Command: ``RTL9602C_GPON_TBL_CMD_EXEC`` (bit 15, start),
       ``RTL9602C_GPON_TBL_CMD_DONE`` (bit 14, set by the MAC when done),
       ``RTL9602C_GPON_TBL_CMD_VALID`` (bit 13, the entry read is in use),
       ``RTL9602C_GPON_TBL_CMD_OP`` (bits 9:8:
       ``RTL9602C_GPON_TBL_CMD_OP_WRITE`` 1, ``RTL9602C_GPON_TBL_CMD_OP_READ``
       2, ``RTL9602C_GPON_TBL_CMD_OP_CLEAR`` 3) and
       ``RTL9602C_GPON_TBL_CMD_IDX`` (bits 6:0); data:
       ``RTL9602C_GPON_TBL_DATA_ID`` (bits 11:0).
     - M
     - With the data written before the index, the default Alloc-ID went to
       another entry and in O4 the MAC sent nothing and classified nothing; the
       ``gpontbl`` view reads the entries back.
   * - Alloc-ID table: ``RTL9602C_ALLOCID_TBL_CMD`` (``0x7010c0``),
       ``RTL9602C_ALLOCID_TBL_WDATA`` (``0x7010c4``),
       ``RTL9602C_ALLOCID_TBL_RDATA`` (``0x7010cc``);
       ``RTL9602C_GPON_NUM_TCONT`` (32) entries indexed by T-CONT, each the
       Alloc-ID whose allocations that T-CONT takes.
     - M
     - Entry 16 read back from ``0x7010cc`` the ONU-ID written into it, while
       ``0x7010c8`` read 0.
   * - Port-ID table: ``RTL9602C_GEM_DS_PORTID_TBL_CMD`` (``0x701100``),
       ``RTL9602C_GEM_DS_PORTID_TBL_WDATA`` (``0x701104``),
       ``RTL9602C_GEM_DS_PORTID_TBL_RDATA`` (``0x70110c``);
       ``RTL9602C_GPON_NUM_FLOW`` (128) entries indexed by flow, each the GEM
       Port-ID the MAC takes from the downstream into that flow: the ONU's GEM
       port filter.
     - M
     - The ``gpontbl`` view; ITU-T G.984.3 clause 5.5.1.
   * - Multicast filter table: ``RTL9602C_GEM_DS_MC_TBL_CMD`` (``0x704084``),
       with the command bits of the first row and an 8-bit index
       ``RTL9602C_GEM_DS_MC_TBL_CMD_IDX`` (bits 7:0); ``RTL9602C_GPON_NUM_MC``
       (256) entries. The driver only reads VALID and clears entries
       (``rtl9602c_gpon_mc_op()``).
     - M
     - All 256 entries read in use after a reset.
   * - A multicast entry is the low 32 bits of a destination MAC address, its
       octets 2 to 5, written at ``0x704088`` before the write command and read
       at ``0x704090`` after a read. ``0x70409c`` reads ``0x01005e`` and
       ``0x7040a0`` reads ``0x3333`` at reset, the leading octets of IPv4 and
       IPv6 multicast addresses; that they tell the filter which frames are IPv4
       and IPv6 multicast is not verified.
     - M
     - Entry ``0x5e020201`` let frames to 239.2.2.1 (01:00:5e:02:02:01) through
       an including filter and stopped them at an excluding one, and did the
       opposite for 239.2.2.2.
   * - The three tables are RAM that neither the watchdog's chip reset nor the
       MAC reset clears. Right after the MAC reset, before anything is written
       into them, ``rtl9602c_gpon_tables_clear()`` clears every entry of each
       (the Port-ID entries with their flow type, see below), counts those it
       found in use, logs ``tables emptied: ...`` when there were any, forgets
       its record of T-CONTs 0 to 15, and marks the OMCC unbound.
     - M
     - After a watchdog reset and a fresh bring-up all 32 Alloc-ID, 128 Port-ID
       and 256 multicast entries were in use; with that residue the OMCC's entry
       at flow 64 matched no OMCI frame and every upstream grant got an empty
       burst; emptied, both worked.
   * - The default Alloc-ID, equal to the ONU-ID, goes into T-CONT
       ``RTL9602C_GPON_DEFAULT_TCONT_IDX`` (16): bound on Assign_ONU-ID, cleared
       when the ONU-ID is given back. The upstream PLOAM messages and the OMCC
       travel in it.
     - T
     - The ONU reaches O5 on an OLT and OMCI messages pass both ways; ITU-T
       G.984.3 clause 5.5.3 and clause 5.5.4.
   * - An Alloc-ID of Assign_Alloc-ID type 1 joins the assigned ones
       (``gpon_assigned``), type 255 leaves them, and the bindings follow
       (``rtl9602c_gpon_tcont_sync()``): a GEM T-CONT that management mapped to
       an assigned Alloc-ID through the OMCC (tcont-set) holds it, and an
       assigned Alloc-ID that no mapping names goes to an unmapped GEM T-CONT,
       the one holding it already, else the lowest free one, with a warning
       when there is none. The repeated copies of the message (the OLT sends
       three) change nothing. The GEM port code is told of each T-CONT that
       gains an Alloc-ID (``rtl9602c_gem_tcont_ready()``, see
       Documentation/networking/device_drivers/pon/rtl9602c.rst). The driver's
       own record, not a table read, answers what each T-CONT holds (see
       "Reporting: the PON class and devlink"). The mappings outlive a
       deactivation, as the OMCI MIB does; the assignments do not.
     - M, P
     - M: the PON class report lists Alloc-ID 257; a GEM port on the T-CONT
       holding it sends upstream (a routed flow leaves by that GEM port). With
       omcid mapping T-CONT 0 to the OLT's Alloc-ID 256 and Assign_Alloc-ID for
       300 injected: 300 went to T-CONT 1; mapped to T-CONT 3, it moved there;
       unmapped, it stayed on T-CONT 3; mapped to T-CONT 5, it moved there;
       taken back and assigned again, it came back to T-CONT 5; T-CONT 0 held
       256 throughout, and a mapping of another T-CONT to 256 was refused. P:
       ITU-T G.984.3 clause 5.5.4, the T-CONTs mapped to Alloc-IDs through the
       OMCC.
   * - ``RTL9602C_GEM_DS_FLOW_CFG(f)`` (``0x701400`` + 4f):
       ``RTL9602C_GEM_DS_FLOW_CFG_TYPE`` (bits 4:0) says what a downstream flow
       carries, ``RTL9602C_GEM_DS_FLOW_CFG_TYPE_MC`` (bit 0) multicast,
       ``RTL9602C_GEM_DS_FLOW_CFG_TYPE_ETH`` (bit 1) Ethernet,
       ``RTL9602C_GEM_DS_FLOW_CFG_TYPE_OMCI`` (bit 2) OMCI.
       ``RTL9602C_GEM_US_FLOW_CFG(f)`` (``0x706400`` + 4f):
       ``RTL9602C_GEM_US_FLOW_CFG_PORTID`` (bits 11:0), the Port-ID put on the
       flow's upstream frames. ``RTL9602C_GEM_US_FLOW_BYTES(f)`` (``0x706800`` +
       8f) counts the flow's upstream bytes. The GEM port code uses the
       multicast and Ethernet types and the byte counter (see
       Documentation/networking/device_drivers/pon/rtl9602c.rst).
     - T
     - OMCI messages pass both ways (the OMCI type); downstream GEM frames reach
       the SFP port and upstream GEM frames reach the OLT, checked with traffic
       on the board.
   * - Why stale entries elsewhere in the Port-ID table kept flow 64 from
       matching is not understood; emptying every entry does not depend on it.
     - I
     - Refilling the table with stale entries and binding the OMCC.

GEM framing
===========

Downstream the MAC takes the GEM frames whose Port-ID is in the Port-ID table
into their flows, and each flow has a type: multicast, Ethernet or OMCI.
Upstream it puts the flow's Port-ID and a PTI code in each GEM header. The
driver writes three settings, the PTI match that tells the last fragment of an
OMCI message, the PTI codes of the upstream framer and the downstream filter,
and reads the per-flow downstream counters.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A GEM header carries PLI (12 bits), Port-ID (12), PTI (3) and HEC (13).
       PTI 000 marks a user-data fragment that does not end a frame, 001 one
       that does, 100 and 101 GEM OAM.
     - P
     - ITU-T G.984.3 clause 8.3.1.
   * - ``RTL9602C_GEM_DS_OMCI_PTI`` (``0x701204``):
       ``RTL9602C_GEM_DS_OMCI_PTI_MASK`` (bits 6:4, the PTI bits compared) and
       ``RTL9602C_GEM_DS_OMCI_PTI_END`` (bits 2:0, the PTI of an OMCI message's
       last fragment) are both set to ``RTL9602C_GEM_DS_OMCI_PTI_EOF`` (1, user
       data, end of frame). They reset to 5 and 5.
     - T
     - OMCI messages arrive on the OMCI port; the register read ``0x55`` at
       reset.
   * - ``RTL9602C_GEM_US_PTI_MAP`` (``0x706020``): four 3-bit PTI codes,
       ``RTL9602C_GEM_US_PTI_MAP_0`` (bits 2:0), ``RTL9602C_GEM_US_PTI_MAP_1``
       (bits 6:4), ``RTL9602C_GEM_US_PTI_MAP_2`` (bits 10:8) and
       ``RTL9602C_GEM_US_PTI_MAP_3`` (bits 14:12), written 0, 1, 0, 1: the codes
       the upstream GEM framer puts in its headers. Which kind of fragment uses
       which slot is not established.
     - T
     - OMCI messages pass both ways, and the OLT takes the upstream OMCI and
       data (upstream GEM frames reach the OLT with the ONU's state held).
   * - ``RTL9602C_GEM_DS_MC_FILTER`` (``0x704080``): the driver sets
       ``RTL9602C_GEM_DS_MC_FILTER_BCAST_PASS`` (bit 6),
       ``RTL9602C_GEM_DS_MC_FILTER_UCAST_PASS`` (bit 4, frames that are not
       multicast), ``RTL9602C_GEM_DS_MC_FILTER_FCS_CHECK`` (bit 3) and
       ``RTL9602C_GEM_DS_MC_FILTER_EXCLUDE`` (bit 0: the table lists multicast
       to drop, and it is empty), and clears the IPv4 and IPv6 overrides
       ``RTL9602C_GEM_DS_MC_FILTER_IPV4_PASS`` (bit 2),
       ``RTL9602C_GEM_DS_MC_FILTER_IPV4_DROP`` (bit 1),
       ``RTL9602C_GEM_DS_MC_FILTER_IPV6_PASS`` (bit 10) and
       ``RTL9602C_GEM_DS_MC_FILTER_IPV6_DROP`` (bit 9). At reset it reads
       ``0x08``: an including filter with nothing in it, which refuses broadcast
       and unicast.
     - T
     - Downstream GEM frames reach the SFP port, checked with traffic on the
       board; the register read at reset.
   * - The filter judges only the frames of a flow whose downstream type has
       ``RTL9602C_GEM_DS_FLOW_CFG_TYPE_MC``. On such a flow an including filter
       with an empty table drops every IPv4 and IPv6 multicast frame, the
       link-local groups 224.0.0.1 and ff02::1 among them, and passes broadcast,
       with ``RTL9602C_GEM_DS_MC_FILTER_BCAST_PASS``, and multicast of other
       types. The driver keeps the filter excluding with an empty table and
       leaves multicast to the switch, whose snooping keeps the link-local
       groups flowing (see the *bridging* page of the switch, "Multicast
       snooping").
     - M
     - Flow 0 (Port-ID 4095) typed Ethernet alone: 20 of 20 frames each to
       239.2.2.1, 224.0.0.1 and ff02::1 passed an including filter with an
       empty table. Typed multicast as well: 0 of 20 of each passed, while 20 of
       20 broadcasts and 20 of 20 frames to 03:00:00:00:00:01 did.
   * - Per-flow downstream counters (``rtl9602c_gpon_flow_cntr()``): write the
       flow into ``RTL9602C_GEM_DS_CNT_CMD_FLOW`` (bits 6:0 of
       ``RTL9602C_GEM_DS_CNT_CMD``, ``0x701140``), with
       ``RTL9602C_GEM_DS_CNT_CMD_BYTES`` (bit 8) for bytes rather than frames;
       poll ``RTL9602C_GEM_DS_CNT_CMD_DONE`` (bit 15) for up to 1 ms; read
       ``RTL9602C_GEM_DS_CNT_DATA`` (``0x701144``); the result is all ones when
       it never completes. They count the downstream only.
     - M
     - A flow carrying 268896 frames upstream showed nothing in the ``gpontbl``
       view.
   * - These counters clear when read.
     - M
     - Flow 0 showed 865 frames at a first read and exactly the 100 sent since
       at the second.
   * - The frame counter counts GEM frames, a fragment as one, and the byte
       counter the payload, the Ethernet frame with its FCS, without the GEM
       header.
     - M
     - 20000 frames of 1514 bytes from the network side to a GEM port's flow:
       20692 frames, as the OLT split 692 of them, and 30361345 bytes, 1518 for
       each frame and 1345 of other traffic, the same 1345 as in a run of 10000
       frames that counted 10552 frames and 15181345 bytes.
   * - After power-up the counters of the flows hold noise; the first read
       empties them. The driver reads a GEM port's flow when it creates the
       port and starts the port's totals at zero.
     - M
     - The first read after a boot showed counts on many unused flows, up to
       3775172030, and the next read none.
   * - The driver adds a GEM port's counters into the port's totals
       (``rtl9602c_gem_cntr_sync()``, in the PON MAC page's "The GEM port
       counts") and is their only reader for GEM ports: the ``gpontbl`` view
       shows the totals. The ``ponpath`` view reads the OMCC flow's, which no
       GEM port has.
     - T
     - The view's totals followed the frames sent, read after read.

Counters
========

The GTC counters are registers from ``0x701180`` to ``0x7011fc`` that clear when
read. ``rtl9602c_gpon_cntr_sync()`` reads them, from the worker every 50 ms and
before every status report and every read of the ``gpon`` and ``ponpath``
views, and adds them into running totals (``struct pon_dev_stats``) that start
at zero at each bring-up. The PON class report and both views read the totals,
so they agree. The table gives the register behind each total. A second reader
of one of the registers, such as a tool that reads them by address, would take
counts out of the totals.

.. list-table::
   :header-rows: 1
   :widths: 22 42 36

   * - Total
     - Register
     - Field
   * - ``sn_requests``
     - ``RTL9602C_GPON_CNT_ACTIVATION`` (``0x7011ac``)
     - ``RTL9602C_GPON_CNT_ACTIVATION_SN`` (bits 15:0)
   * - ``ranging_requests``
     - ``RTL9602C_GPON_CNT_ACTIVATION`` (``0x7011ac``)
     - ``RTL9602C_GPON_CNT_ACTIVATION_RANGING`` (bits 31:16)
   * - ``ploam_rx``
     - ``RTL9602C_GPON_CNT_PLOAM_RX`` (``0x70119c``)
     - The whole word.
   * - ``ploam_crc_errors``
     - ``RTL9602C_GPON_CNT_PLOAM_ERR`` (``0x7011a0``)
     - ``RTL9602C_GPON_CNT_PLOAM_ERR_CRC`` (bits 15:0)
   * - ``ploam_overflows``
     - ``RTL9602C_GPON_CNT_PLOAM_ERR`` (``0x7011a0``)
     - ``RTL9602C_GPON_CNT_PLOAM_ERR_OVERFLOW`` (bits 31:16)
   * - ``bwmap_rx``
     - ``RTL9602C_GPON_CNT_BWMAP_RX`` (``0x7011b0``)
     - The whole word.
   * - ``bwmap_crc_errors``
     - ``RTL9602C_GPON_CNT_BWMAP_ERR`` (``0x7011a4``)
     - ``RTL9602C_GPON_CNT_BWMAP_ERR_CRC`` (bits 15:0)
   * - ``bwmap_overflows``
     - ``RTL9602C_GPON_CNT_BWMAP_ERR`` (``0x7011a4``)
     - ``RTL9602C_GPON_CNT_BWMAP_ERR_OVERFLOW`` (bits 31:16)
   * - ``gem_frames``
     - ``RTL9602C_GPON_CNT_GEM_FRAMES`` (``0x7011c4``)
     - The whole word.
   * - ``bip_errors``
     - ``RTL9602C_GPON_CNT_BIP_BITS`` (``0x701184``)
     - The whole word.
   * - ``fec_corrected_bytes``
     - ``RTL9602C_GPON_CNT_FEC_BYTES`` (``0x70118c``)
     - The whole word.
   * - ``fec_corrected_codewords``
     - ``RTL9602C_GPON_CNT_FEC_CODEWORDS`` (``0x701190``)
     - The whole word.
   * - ``fec_uncorrectable_codewords``
     - ``RTL9602C_GPON_CNT_FEC_UNCORRECTABLE`` (``0x701194``)
     - The whole word.
   * - ``fec_codewords``
     - None: derived from ``RTL9602C_GTC_DS_SUPERFRAME`` (``0x701048``) and
       ``RTL9602C_GTC_DS_IRQ_STAT`` (``0x701008``).
     - ``RTL9602C_GTC_DS_SUPERFRAME_CNT`` (bits 29:0) and
       ``RTL9602C_GTC_DS_IRQ_FEC`` (bit 2).

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Every counter from ``0x701180`` to ``0x7011fc`` clears when read.
     - M
     - Three reads of ``RTL9602C_GPON_CNT_PLOAM_RX`` two seconds apart gave
       ``0x270``, ``0x0c`` and ``0x06`` on an unchanging link, while the
       superframe counter rose 16046.
   * - ``RTL9602C_GPON_CNT_ACTIVATION`` (``0x7011ac``):
       ``RTL9602C_GPON_CNT_ACTIVATION_SN`` (bits 15:0) counts grants classified
       as serial-number requests. ``RTL9602C_GPON_CNT_PLOAM_RX`` (``0x70119c``)
       counts downstream PLOAM messages accepted, ``RTL9602C_GPON_CNT_BWMAP_RX``
       (``0x7011b0``) bandwidth maps accepted.
     - M
     - One serial-number request per discovery grant in O3; 8094 maps a second
       against 8000 downstream frames, and PLOAM messages counted, in the
       ``gpon`` view.
   * - In O5 the bandwidth-map count can rise at half the frame rate while the
       ONU is granted in every frame: on the lab OLT it rose 3998 a second
       (40,060 in 10.02 s), the ``bwmap`` view's capture held the ONU's grant
       of 15,417 bytes in every other frame only, and the upstream carried
       962 Mb/s, which takes that grant in every 125 us frame. What the MAC
       accepts in O5 is therefore not every map that grants the ONU, and is
       not known; the count is reported as the MAC keeps it.
     - M
     - ``pon`` read twice ten seconds apart, the ``bwmap`` view, and the
       upstream burst of ``tests/device`` (156,466 frames of 1514 bytes in
       1.97 s), all in one boot.
   * - Not measured field by field: ``RTL9602C_GPON_CNT_ACTIVATION_RANGING``
       (bits 31:16, ranging requests); ``RTL9602C_GPON_CNT_PLOAM_ERR``
       (``0x7011a0``) with ``RTL9602C_GPON_CNT_PLOAM_ERR_CRC`` (bits 15:0) and
       ``RTL9602C_GPON_CNT_PLOAM_ERR_OVERFLOW`` (bits 31:16);
       ``RTL9602C_GPON_CNT_BWMAP_ERR`` (``0x7011a4``) with
       ``RTL9602C_GPON_CNT_BWMAP_ERR_CRC`` and
       ``RTL9602C_GPON_CNT_BWMAP_ERR_OVERFLOW`` at the same positions;
       ``RTL9602C_GPON_CNT_GEM_FRAMES`` (``0x7011c4``), non-idle downstream GEM
       frames.
     - I
     - The ranging count reading 1 after each activation; a fibre attenuated
       towards errors for the CRC counts.
   * - The downstream BIP (G.984.3 clause 8.1.3.4) is counted twice:
       ``RTL9602C_GPON_CNT_BIP_BLOCKS`` (``0x701180``) counts the frames whose
       BIP disagreed and ``RTL9602C_GPON_CNT_BIP_BITS`` (``0x701184``) the
       parity bits that differed, which is G.984.3's ERR (clause 11.2.2).
       ``bip_errors`` is the bits; the frames are kept for the ``gpon`` view.
     - M
     - A boot and eight cycles of the PON down and up: after each return to O5
       the ``gpon`` view showed one frame and 2 or 4 bits (2 twice, 4 seven
       times), and the netlink report the same bits; 30 s more of O5 added
       none.
   * - The downstream FEC counts (G.984.3 clause 13.1.3):
       ``RTL9602C_GPON_CNT_FEC_BYTES`` (``0x70118c``) the bytes FEC corrected,
       ``RTL9602C_GPON_CNT_FEC_CODEWORDS`` (``0x701190``) the codewords it
       corrected, ``RTL9602C_GPON_CNT_FEC_UNCORRECTABLE`` (``0x701194``) the
       codewords it could not correct.
     - I
     - Assumption, not verified: they read 0 on the lab OLT, which sends no
       FEC. An OLT with downstream FEC on and a fibre attenuated towards errors
       would settle it.
   * - ``fec_codewords``, the codewords received, has no register of its own.
       A downstream frame that carries FEC holds 153 codewords (152 of 255
       bytes and a shortened last one, G.984.3 clause 13.2.1), so each sync
       adds 153 for every frame the superframe count advanced since the last
       one, if ``RTL9602C_GTC_DS_IRQ_FEC`` is set and neither LOS nor LOF is;
       an advance of more than 8000 frames (one second), as when the
       downstream is found again, adds nothing. The FEC state is taken at the
       sync, so frames around a change of it count by the state that follows.
     - I
     - Design choice. An OLT with downstream FEC on would show the count rising
       1,224,000 a second; on the lab OLT it stays 0.
   * - ``rtl9602c_gpon_cntr_sync()`` reads these registers: from the worker
       every 50 ms and before every report, only while the PON is up, adding
       into running totals (``struct pon_dev_stats``) that start at zero at each
       bring-up. The ``gpon`` view and the PON class report's statistics both
       read the totals.
     - M
     - Two reads five seconds apart gave 711266 then 751736 maps and 685 then
       715 PLOAM messages and one PLOAM CRC error since bring-up, the same in
       the ``gpon`` view and in the netlink report; the BIP errors likewise,
       4 bits in both.
   * - The bandwidth-map counter is 16 bits wide, so at 8000 maps a second it
       must be read within 8 s; the 50 ms period leaves that margin.
     - I
     - A read after more than 8 s without a sync.
   * - Upstream counters: write an index to ``RTL9602C_GPON_CNT_US_SEL``
       (``0x705140``) and read ``RTL9602C_GPON_CNT_US_DATA`` (``0x705148``);
       ``RTL9602C_GPON_US_CNTR_COUNT`` (8) indexes. Index 0 (``ploam-overhead``
       in the ``uscntr`` view) counts burst overheads sent, index 2
       (``ploam-cpu``) PLOAM messages sent from the normal and urgent queues,
       index 3 (``ploam-auto``) PLOAM messages sent from the templates.
     - M
     - The ``uscntr`` view: index 2 counted 8 for 8 messages pushed to a queue;
       indexes 0 and 3 counted 979 each for 957 serial-number requests.
   * - Reading an index clears it. Index 4 (``gem-bytes``) counts the GEM
       bytes of every allocation, idle GEM frames included, and indexes 5 to
       7 read the same as index 4.
     - M
     - Three readings 1 s apart in O5, the OLT granting the T-CONT 15417
       bytes in every other frame and no upstream traffic: each reading held
       one second's counts (index 3 about 4085), and index 4 advanced by
       62929557, about 15405 bytes for each of the 4085 allocations.
   * - The upstream counters seem to be pairs of 16-bit counts: in O5 index 0
       read the same count in both halves (``0x0ff60ff6``), index 1 in the upper
       half (``0x0ff50000``) and index 3 in the lower (``0x00000ff5``). What the
       upper halves count, and whether index 1 (``dbru``) counts DBRu sent, are
       not established; the view prints the whole words.
     - I
     - Readings while the OLT asks for DBRu, and during serial-number
       acquisition.

FEC and encryption
==================

G.984.3 lets the OLT switch on forward error correction in both directions, and
provides encryption of the downstream GEM payload. The driver sets one FEC
detection threshold, follows the upstream FEC state that the MAC reports, and
carries the ONU's side of the key exchange: on Request_Key it makes a new key,
sends it to the OLT and writes it into the MAC as the next key, holding back a
switch set up for the key before; on Key_Switching_Time it writes the
superframe from which that key applies; on Encrypted_Port-ID it marks the flow
bound to that Port-ID for decryption. It zeroes both keys at bring-up and when
the ONU gives up its ONU-ID. What it leaves out is under "Known limitations",
and the rows here state what the driver does and what is assumed.

Upstream FEC is asked for in the bandwidth map: the OLT sets the Use FEC flag of
the ONU's allocations, and the ONU then adds the parity of RS(255,239) to its
bursts and says so with the FEC bit of the burst header's Ind field. The driver
sets nothing for it, as no setting that switches it has been found yet; the MAC
is taken to answer the flag by itself. ``rtl9602c_gpon_us_fec_sync()`` reads
the state the MAC reports, ``RTL9602C_GTC_US_IRQ_FEC`` in
``RTL9602C_GTC_US_IRQ_STAT``, under ``gpon_lock``: from the interrupt thread
when the upstream latch holds that bit, and from the worker every
``RTL9602C_GPON_POLL_MS``. It logs each change once, as
``<GPON> upstream FEC on`` or ``<GPON> upstream FEC off``, and the ``us`` line
of the ``gpon`` view shows the word and the bit. The PON class has no attribute
for the upstream FEC state (Documentation/networking/pon.rst), so it is not
reported there.

The MAC holds two keys, the active one and the next one. A key is written a
16-bit word at a time (``rtl9602c_gpon_key_write()``), under ``gpon_lock``:

1. Write ``RTL9602C_GPON_AES_KEY_CFG_ACTIVE`` (bit 14), set for the active key
   and clear for the next one, then the same with
   ``RTL9602C_GPON_AES_KEY_CFG_REQ``, to ``RTL9602C_GPON_AES_KEY_CFG``.
   Purpose: address the key. (M)
2. For each word i from 0 to 7: write octets 2i and 2i + 1 of the key, the
   first in bits 15:8, into ``RTL9602C_GPON_AES_KEY_DATA``; write i into
   ``RTL9602C_GPON_AES_KEY_WORD``, then i with
   ``RTL9602C_GPON_AES_KEY_WORD_REQ``; poll
   ``RTL9602C_GPON_AES_KEY_WORD_DONE`` up to 100 times, 1 us apart. (M)

The MAC makes the next key the active one when the superframe counter equals
``RTL9602C_GPON_AES_SWITCH_TIME``. Before a new key is written,
``rtl9602c_gpon_key_hold()`` writes there the superframe just passed, which the
30-bit counter reaches again only after about 37 hours, so that the new key is
not switched to at a superframe that the OLT chose for the key before it.
``rtl9602c_gpon_key_clear()`` holds the switch the same way and writes zero
into the next key, then into the active key.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Downstream FEC is signalled by the FEC bit of the Ident field, with a
       four-frame hysteresis at the ONU. Upstream FEC is requested by the Use
       FEC flag (bit 9 of an allocation's Flags), the same for every Alloc-ID
       of an ONU; the ONU acts on it at once, adds 16 parity bytes to every 239
       data bytes from the BIP byte on, and confirms with the FEC bit (bit 6)
       of the burst header's Ind field. An ONU that does not support upstream
       FEC ignores the flag and reports FEC off. No upstream FEC is used
       outside O5. Encryption is 128-bit AES in counter mode over GEM
       payloads, with keys the ONU generates on Request_Key and switches to at
       Key_Switching_Time.
     - P
     - ITU-T G.984.3 clause 13.2.3, clause 8.1.3.6.2, clause 8.2.2.3,
       clause 13.1.2.2, clause 13.3.1.1, clause 13.3.3.1 and clause 13.4;
       clause 12.2, clause 12.3 and clause 12.4.
   * - Downstream FEC: ``RTL9602C_GTC_DS_IRQ_FEC`` shows whether the line
       carries FEC (the ``ds`` line of the ``gpon`` view). The driver writes
       ``RTL9602C_GTC_DS_CTRL_FEC_DET_THR`` (bits 3:1 of
       ``RTL9602C_GTC_DS_CTRL``) = ``RTL9602C_GTC_DS_CTRL_FEC_DET_THR_DEF`` (0),
       a detection threshold that reads 1 at reset.
     - I
     - Required: unverified; the test OLT's downstream FEC setting is not
       recorded. An OLT with downstream FEC on would settle it.
   * - Upstream FEC: the MAC codes a burst with FEC, and sets the FEC bit of
       its Ind field, when the ONU's allocations carry the Use FEC flag, with
       nothing for the driver to set. A setting that switches upstream FEC has
       not been found yet: the upstream GTC control word
       (``RTL9602C_GTC_US_CTRL``), the transmit mode
       (``RTL9602C_GTC_US_TX_MODE``) and the upstream GEM registers were
       looked at. The driver writes none, and shows a grant's flag in the
       ``bwmap`` view (``RTL9602C_BWMAP_ALLOC_FEC``).
     - I
     - An OLT that sets Use FEC, which the test OLT does not (the row below):
       the ``us`` line of the ``gpon`` view reads ``fec 1`` while the
       ``bwmap`` view shows the flag, the kernel log says ``upstream FEC on``
       once, and the OLT receives the ONU's bursts as FEC-coded, with the FEC
       bit set, and keeps the ONU in O5.
   * - ``RTL9602C_GTC_US_IRQ_STAT`` (``0x705008``) holds the upstream
       conditions present now: ``RTL9602C_GTC_US_IRQ_FEC`` (bit 2) is set while
       the bursts carry FEC, and clear while the OLT asks for none. Bit 2 of
       ``RTL9602C_GTC_US_IRQ_LATCH`` latches its changes, and
       ``RTL9602C_GTC_US_IRQ_EN_DEF`` (``0xa4``) enables them. The driver reads
       bit 2 of the status as a level, from the interrupt thread when the latch
       holds bit 2 and from the worker, logs each change once, starting from
       off at each bring-up, and prints the word and the bit on the ``us`` line
       of the ``gpon`` view.
     - M, I
     - M for FEC off: on the test OLT, which sets Use FEC in no allocation (a
       ``bwmap`` capture of 16 read FEC 0 in each), the ``us`` line read
       ``000000a0  fec 0`` in O5, and no ``upstream FEC`` line was logged in a
       session that held about ten re-activations. I for FEC on: an OLT that
       sets Use FEC, with the ``us`` line and the ``bwmap`` view's FEC column
       read together, and the ``ploamlog`` ring's upstream latch events at
       each change of the flag.
   * - Request_Key (identifier 13, directed, in O5) is answered with a new
       random key under the next key index (1 for the first): Encryption_Key
       messages (identifier 5) on the normal queue, octet 3 the index, octet 4
       the fragment, 0 for octets 0 to 7 of the key and 1 for octets 8 to 15,
       then the eight octets, both fragments three times over. The key is then
       written as the next key and not kept; the ``ploamlog`` ring records the
       fragments zeroed, and the kernel log ``key N sent``.
     - M, P
     - M: a Request_Key injected through ``ploaminject``: six Encryption_Key
       messages with index 1 and fragments 0, 1, 0, 1, 0, 1, and every word
       of the key taken by the MAC (no timeout). P: ITU-T G.984.3
       clause 9.2.3.13 and clause 9.2.4.5.
   * - Key_Switching_Time (identifier 19, in O5): octets 3 to 6 the superframe
       counter value, 30 bits, from which the new key applies. The driver
       writes it into ``RTL9602C_GPON_AES_SWITCH_TIME`` (``0x703014``,
       ``RTL9602C_GPON_AES_SWITCH_TIME_SF``, bits 29:0) and acknowledges every
       copy.
     - M
     - An injected Key_Switching_Time for superframe ``0x012345`` was
       acknowledged and the register read back ``00012345`` (the ``aes`` line
       of the ``gpon`` view).
   * - The MAC makes the next key the active one when the superframe counter
       (the ``frame`` line of the ``gpon`` view) equals
       ``RTL9602C_GPON_AES_SWITCH_TIME``, and only then: a superframe already
       passed waits for the counter to come round.
     - M
     - With a next key written by a test build, the switched bit read 0; for
       a Key_Switching_Time injected for superframe 78263320 it read 0 at
       superframe 78263096 and 1 at 78263340, read back to back; for one
       injected for 8000 superframes in the past it read 0 for 3 s.
   * - ``RTL9602C_GPON_AES_KEY_WORD_SWITCHED`` (bit 7 of
       ``RTL9602C_GPON_AES_KEY_WORD``) reads 0 from the write of a next key
       until the MAC has switched to it, and 1 after the switch and after a
       write of the active key, even over a next key that waits. The ``aes``
       line of the ``gpon`` view shows ``waiting`` while it reads 0.
     - M
     - Read after each write of a test build and around switches (the row
       above): 1 at reset, 1 after the active key, 0 after a next key, 1
       after the switch, and 1 after the active key over a waiting next key.
   * - ``RTL9602C_GPON_AES_KEY_DATA`` reads back the last word written into a
       key, and a write of 0 there afterwards does not change what it reads;
       the driver writes nothing there after a key.
     - M
     - After a test key ending in ``4f 3c`` it read ``0x4f3c``, after one
       ending in ``0e 0f`` ``0x0e0f`` (each followed by a write of 0), and 0
       after the zero keys; before any key, ``0x69a0``.
   * - Request_Key after an acknowledged Key_Switching_Time and before the
       switch: the new key replaces the next key, and the superframe chosen
       for the old one is dropped with it (``rtl9602c_gpon_key_hold()`` before
       the new key is written).
     - P, M
     - P: ITU-T G.984.3 clause 12.4. M: Request_Key injected after a
       Key_Switching_Time for superframe 82056983 and before it: the switch
       time read the superframe just passed, and 5 s later, past 82056983,
       the new key still waited; a Key_Switching_Time for it then switched it.
   * - Both keys are zeroed, the next one first, with the switch held, when
       the MAC is brought up and when the ONU gives up its ONU-ID; the
       superframe of the last Key_Switching_Time reads 0 after that.
     - M
     - At bring-up and with ``pon`` taken down (O1, ONU-ID 255): no warning,
       the ``aes`` line without ``waiting``, the switch time the superframe
       just passed and the superframe of the last Key_Switching_Time 0; O5
       again 2 s after ``pon`` came up. That the words are zero: the data
       register read 0 after zero keys written by a test build.
   * - Encrypted_Port-ID (identifier 8, directed, in O4 and O5): octet 3 bit 0
       set encrypts the Port-ID, clear stops it; the Port-ID as in
       Configure_Port-ID. The driver records the Port-ID and sets or clears
       ``RTL9602C_GEM_DS_FLOW_CFG_AES`` (bit 4 of the flow's
       ``RTL9602C_GEM_DS_FLOW_CFG``) of the flow bound to it, which then
       passes its downstream through the active key; a GEM port or OMCC bound
       later takes the bit with its type. Every copy is acknowledged. The
       record goes with the ONU-ID.
     - M
     - Port-ID 256 marked encrypted by an injected message while the OLT sent
       it in the clear: 0 of 243,287 downstream frames arrived, against
       242,150 of 242,150 before; unmarked again, 243,266 of 243,264 (the
       ``aes`` line counted 1 Port-ID, then 0).
   * - The MAC decrypts what an OLT encrypts with the key the driver sent: the
       octet order in which the driver writes the key, the earlier octet of
       each pair in bits 15:8, is the one the MAC and the OLT share, and so is
       the counter's start. Frames that a Port-ID marked encrypted carries in
       the clear cannot be read on the board: the decryption leaves their FCS
       wrong, and the switch drops them.
     - M
     - On an operator's OLT that encrypts the OMCC and every data Port-ID and
       asks for a new key once a day, the traffic was decrypted throughout:
       for 34 hours the ``aes`` line of the ``gpon`` view, read without
       sending anything, counted the encrypted Port-IDs while the traffic
       flowed. With Port-ID 4095 marked encrypted while the OLT sent it in the
       clear, 20 of 20 test frames were counted in ``rx_crc_errors`` of the
       ``pon`` port, with ``RTL9602C_GEM_DS_MC_FILTER_FCS_CHECK`` set and, in
       a test build, clear; none reached the CPU.

Reporting: the PON class and devlink
====================================

The GPON code reports the ONU to the PON class
(Documentation/networking/pon.rst). The PON device is registered by the GEM port
code (see Documentation/networking/device_drivers/pon/rtl9602c.rst), and its
``dev_status`` operation calls ``rtl9602c_gpon_fill_status()`` under
``gpon_lock``, so that a report is one moment of the activation.
``rtl9602c_gpon_notify()`` sends the class the report when it changes. The
serial number that the ONU presents to the OLT is the devlink parameter
``gpon_serial``. Whether the OLT has disabled the ONU is ``gpon_disabled``.
User space learns of a disable and of an enable from the state the class
reports and announces: while the PON is up the ONU is in O7 exactly while
``gpon_disabled`` is set, and it leaves O7 for O2 only when it is enabled. A
take-down reports O1, then no state, and keeps the disable.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The PON device, registered by the GEM port code (see
       Documentation/networking/device_drivers/pon/rtl9602c.rst) with 128 flows
       and 8 T-CONTs of 8 queues each, takes its status from
       ``rtl9602c_gpon_fill_status()`` under ``gpon_lock``: type GPON; the
       serial number when set; the ifindex of the PON port
       (``RTL9602C_PORT_PON``); and, only while the PON is up, the state (the
       driver's numbers 1 to 5 equal the class's O1 to O5), the ONU-ID unless it
       is 255, the OMCC Port-ID when bound, the Alloc-IDs of T-CONTs 0 to 15 in
       use, the totals of the counters, the transmit power level (high less the
       mode the optics are at, as Serial_Number_ONU carries it), the time since
       the state was entered (from ``jiffies_64``, so that a session of years
       does not wrap it), in O5 the equalization delay that Ranging_Time set,
       LOS, LOF and LOM from ``RTL9602C_GTC_DS_IRQ_STAT``, and the driver's PEE
       alarm.
       With the PON down there is no state, no level, no alarm and no counter:
       an absent value, not a zero.
     - M
     - The PON class report in O5 on an OLT: state O5, ONU-ID 1, Alloc-ID 257,
       OMCC Port-ID 1, no alarms, agreeing with the ``gpon`` view; ``eqd``
       97328, the value of the OLT's Ranging_Time in the log, and the time in
       the state from 0 again after the ``pon`` interface went down and up,
       with ``after 85.164 s in O5`` logged on leaving O5. The OLT at hand
       never sends Request_Password, so one was injected through
       ``ploaminject``: ``password-requests`` and ``password-replies`` went
       from 0 to 1, and ``ploamlog`` showed the three Password messages.
   * - ``rtl9602c_gpon_notify()`` compares the report, counters and the time
       in the state left out, with the last one sent and sends the full report
       when they differ; it runs after every transition and from the worker
       every 50 ms.
     - M
     - Across a down and up a listener on the class's ``monitor`` group received
       O1; a report with no state and no alarms; O2; O3; O4 with the ONU-ID; O5;
       O5 with the Alloc-ID and the OMCC.
   * - The PON class's T-CONT query is answered from the driver's record of
       T-CONTs 0 to 15: the Alloc-ID, ``-ENOENT`` when idle, ``-ERANGE`` from 16
       on. The class asks only below the 8 T-CONTs it was registered with, so
       ``-ERANGE`` is not reached through it.
     - M
     - The ``pon`` tool's dump of ``tcont-get`` in O5: T-CONT 0 with Alloc-ID
       256, the T-CONT that the ``gpon`` view showed bound to it
       (``0=256/256``), and T-CONTs 1 to 7 idle.
   * - The devlink parameter ``gpon_serial`` (string, runtime): it reads empty
       until set, then four characters and eight upper-case hexadecimal digits.
       Setting takes at least 12 characters, the first four printable ASCII
       (``0x20`` to ``0x7e``) and the next eight hexadecimal digits as four
       bytes; characters after the twelfth are ignored; anything else is
       ``-EINVAL``. It is refused with ``-EBUSY`` while the PON is up. A valid
       value is kept, logged as ``serial number set`` (the log line does not
       carry the value), and at once written into the Serial_Number_ONU
       template. The format is G.984.3's serial number: the 4 characters of
       Vendor_ID and a 4-byte serial number (VSSN).
     - M
     - The parameter read empty before it was set; ``ENXIO`` on ``up`` without
       it, ``EBUSY`` while up, ``EINVAL`` for a malformed value, and O5 with a
       valid one. ITU-T G.984.3 clause 9.2.4.1.

The debugfs views
=================

Seven views are created by ``rtl9602c_gpon_debugfs_init()`` in the switch's
debugfs directory, ``/sys/kernel/debug/rtl9602c/``. All are mode 0400 but
``ploamlog``, which is 0600 because a write to it empties the log. They exist
only with ``CONFIG_DEBUG_FS``. A test image built with
``CONFIG_PON_RTL9602C_PLOAM_INJECT`` also has ``ploaminject``, mode 0200:
a write of the twelve octets of a downstream PLOAM message before its CRC, in
hex, spaces allowed, hands the message to the dispatcher as if the OLT had sent
it, and is refused with ``-ENETDOWN`` while the PON is down. An ONU in service
must not have it, since a write can stop the ONU or take it off the PON.
Disabling and enabling an ONU with the serial number ``ABCD0000A001``, for
instance::

    echo ff06ff41424344 0000a001 00 > /sys/kernel/debug/rtl9602c/ploaminject
    echo ff060041424344 0000a001 00 > /sys/kernel/debug/rtl9602c/ploaminject

Times in the ``ploamlog`` view come from the kernel's clock, which advances in
steps of 10 ns on this board (Documentation/arch/mips/rtl9602c.rst).

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``gpon`` (mode 0400): ``top`` is ``RTL9602C_GPON_IRQ_STAT``; ``ds`` is
       ``RTL9602C_GTC_DS_IRQ_STAT`` with its LOS, LOF, LOM and FEC bits; ``us``
       is ``RTL9602C_GTC_US_IRQ_STAT`` with its FEC bit (see "FEC and
       encryption"); ``onu`` is ``RTL9602C_ONU_CTRL`` with the ONU-ID and the
       state; ``los`` is ``RTL9602C_GTC_DS_LOS`` with its ``cdr`` and
       ``optical`` loss bits (1 is loss); ``sig`` is
       ``RTL9602C_SERDES_PON_RX_STAT`` with its signal bit (``signok``, 1 is
       present) and its sync bit (``lecsync``); ``frame`` is the superframe
       counter; ``tod`` is the time of day the OLT gave last,
       its frame and TstampN, whether the clock is still to take it, and
       ``RTL9602C_GTC_DS_TOD_SUPERFRAME`` (see "The time of day"); then come
       the totals of the counters (``cntr``:
       ``sn-requests``, ``ranging-requests``, ``password-requests`` and
       ``password-replies``; PLOAM messages and bandwidth
       maps accepted, CRC errors, overflows, non-idle GEM frames, the BIP
       errors in bits and the frames they were in); ``rei``, the BER interval
       in ms (0 while none is set) and the REI messages sent since the ONU
       entered O5; ``power``, the transmit power level mode the optics are at,
       the one asked for, and the serial-number requests answered at it in O3
       of the ten that make the ONU step; ``aes``, the index of the last key
       sent (0 for none), ``waiting`` while the MAC has not switched to the
       next key, the superframe of the last Key_Switching_Time with
       ``RTL9602C_GPON_AES_SWITCH_TIME``, and the Port-IDs encrypted; last
       ``tcont``, each GEM T-CONT as ``n=B/M``, ``B`` the Alloc-ID it holds and
       ``M`` the one management mapped it to, ``-`` for none. No key is shown.
       A read takes the counters and writes no register.
     - T
     - A check with traffic on the board reads the ``onu`` line before and after
       a flood; every debugfs view reads.
   * - ``gpontbl`` (0400): the Alloc-ID entries in use (T-CONT and Alloc-ID),
       the Port-ID entries in use with the flow's type and upstream Port-ID, and
       each GEM port's totals of received frames and octets and of sent frames
       and octets, its counters added in first. It reads through the table
       protocol under ``gpon_lock``.
     - M
     - The residue after a reset (see "Tables: Alloc-ID, Port-ID and multicast")
       and the per-flow counters (see "GEM framing") were read with it.
   * - ``ploamlog`` (0600; a write clears it): a ring of
       ``RTL9602C_PLOAM_LOG_LEN`` (256) events, oldest first, each with its time
       since boot and the state then. ``RTL9602C_EV_DS``: a downstream message
       (PON-ID never; Upstream_Overhead only in O2; Extended_Burst_Length only
       in O3). ``RTL9602C_EV_US``: an upstream message written, with its slot
       and queue flags. ``RTL9602C_EV_US_REFUSED``: one refused because its
       queue was full. ``RTL9602C_EV_STATE``: a transition. ``RTL9602C_EV_USQ``:
       the queues' empty flags changed, and at Deactivate_ONU-ID.
       ``RTL9602C_EV_US_INTR``: the upstream latch. ``RTL9602C_EV_DS_INTR``: the
       downstream latch when it holds more than PLOAM_RX. An event equal to the
       previous one counts as its repeat, up to 65535, and refreshes its time.
     - M
     - The view showed each re-ranging across activations.
   * - ``bwmap`` (0400): arms a capture of 16 allocations, waits 5 ms plus 125
       us per allocation, prints each valid one under the header
       ``RTL9602C_BWM_HEADER`` (frame, MF, T-CONT, PLOAMu, FEC, DBRu, StartTime,
       StopTime, their length, burst start and end, last), marks one that does
       not fit in a 19440-byte frame, then prints the capture kept from O4.
     - M
     - The view read in O4 and O5 (see "The bandwidth map"); a check with
       traffic on the board reads its T-CONT column.
   * - ``usburst`` (0400): ``RTL9602C_GTC_US_LASER`` (lead, lag),
       ``RTL9602C_GTC_US_CTRL``, ``RTL9602C_GTC_US_POWERSAVE`` (on or off),
       ``RTL9602C_GEM_US_POWERSAVE`` (bit 31, lag, lead), ``RTL9602C_BURST_CFG``
       (index, length), the twelve bytes of ``RTL9602C_BURST_DATA(i)`` and
       ``RTL9602C_GTC_US_TX_MODE`` (its two bits).
     - T
     - Every debugfs view reads.
   * - ``uscntr`` (0400): writes each index 0 to 7 into
       ``RTL9602C_GPON_CNT_US_SEL`` and prints ``RTL9602C_GPON_CNT_US_DATA``
       beside the names of the upstream counters.
     - M
     - The readings of the upstream counters (see "Counters").
   * - ``gemusintr`` (0400): ``RTL9602C_GEM_US_IRQ_LATCH`` (``0x706000``,
       printed as ``latch``: events of the upstream GEM framer since the last
       read) and ``RTL9602C_GEM_US_IRQ_STAT`` (``0x706008``, printed as
       ``status``: the conditions now), with names for bits 0 to 9:
       ``underflow``, ``overflow``, ``residue``, ``excess``,
       ``burst-over-grant``, ``pli-error``, ``short-grant``, ``late-request``,
       ``sd-mismatch``, ``sd-too-long``. These two addresses are defined in
       ``rtl9602c-gpon.c`` and not in the header.
     - M
     - In O5 activations bit 0 (``underflow``) appeared now and then and bit 8
       (``sd-mismatch``) was always set.
   * - What the ten bits of the ``gemusintr`` view detect beyond their names,
       and that reading the latch clears it, are not established.
     - I
     - Two reads in a row under upstream traffic.

Known limitations
=================

- The steps that a loss of the downstream takes, to O1 and to O6, and the
  POPUP and TO2 that leave O6, have not been exercised on the board: they need
  the fibre pulled with the ONU in O5.
- The type 1 and type 2 preambles are sent in whole bytes, and with a 32-bit
  guard at most four bytes of them; a longer count is cut short and its
  remainder sent as the type 3 pattern. The test OLT asks for neither. That
  Extended_Burst_Length is ignored after the first serial-number request has
  not been seen on the board, since the test OLT assigns the ONU-ID at once.
- Sleep_Allow is recorded, logged once and not acted on. Swift_POPUP, like
  POPUP, has not been exercised on the board, since it needs the ONU in O6. The test OLT sends no Physical_Equipment_Error and no
  PST, so the PEE alarm and the PST answer have been exercised only with
  injected messages.
- Upstream FEC is left to the MAC, which is taken to answer the OLT's Use FEC
  flag by itself: no setting that switches it has been found yet, and no OLT
  tried has been seen to set the flag, so only the state with FEC off has been
  read. The driver logs and shows the state the MAC reports, and the PON class
  does not carry it. Downstream FEC has only its detection threshold and its
  counters.
- The driver raises no signal-fail or signal-degrade alarm: it reports the BIP
  errors in bits, and the OMCI daemon judges both from them.
- Before O4 the optics set a lower transmit power level only at a set point
  kept from a measurement since probe, and they measure a level only when
  Change_Power_Level asks for it in O4 or O5: until then the default mode of
  Upstream_Overhead and the ONU's own steps after unanswered serial-number
  requests (G.984.3 clause 10.5.1) leave the ONU at mode 0, and
  Serial_Number_ONU says so. The test OLT names mode 0, sends no
  Change_Power_Level and assigns the ONU-ID at the first answered request, so
  only injected messages have set a lower level, and the ONU's own steps have
  run only in KUnit. With TO1 at 10 s, an OLT that asks for serial numbers
  less often than once a second never makes the ONU step.
- The serial-number response reports a random delay of 0, and whether the MAC
  delays it by itself is not known: no control of such a delay has been found
  yet (see "PLOAM transmit"). ONUs at one distance that answer the same
  discovery request can collide, and the OLT then asks again.
- A laser that stays on by itself is not detected: the MAC's check of the
  optics' signal detect against the burst reports nothing on this board, and
  the module's own indication of light also rises in normal operation, for up
  to about 240 ms at a time under an allocation of most of each upstream frame,
  so it cannot tell a stuck laser from a busy one (see "Upstream timing"). An
  ONU that sends out of turn is left to the OLT, which disables it; in O7
  TX_DISABLE keeps the laser dark even with its driver forced on.
- A disabled ONU stays in O7 across a restart only through user space: the
  driver keeps the disable across a take-down and a bring-up of the PON, and
  user space has to keep it across a restart and set ``gpon_disabled`` again
  before the PON comes up. The bring-up in O7 has not been run on the board.
- Dying_Gasp is sent before an orderly restart, and on the test OLT only its
  first copy leaves (see "PLOAM transmit"). What makes the MAC send the
  Dying_Gasp template it holds armed, and whether the supply-loss interrupt
  needs handling, are not known: the driver does not enable that interrupt,
  whose bit reads clear in operation, and testing either needs a controlled
  drop of the supply.

Open questions
==============

These are the facts tagged **I** in the tables above, each with what would
settle it. The first list holds the assumptions the driver depends on, because
it writes the value or relies on the behaviour, and those behind the known
limitations: the driver is correct on this board only if they hold, and a board
test must settle them. The second list holds the questions to which the code is
indifferent: it works whichever way they resolve, and they only label what a
view or a log line shows.

Assumptions the driver depends on
---------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Assumption
     - What would settle it
   * - That the PON MAC has to be built before the SerDes is put in GPON mode
       ("Bring-up and take-down").
     - Reversing the two and checking that OMCI messages pass both ways.
   * - That activation needs the optics' modulation held ("Bring-up and
       take-down").
     - The ONU reaching O5 and OMCI messages passing both ways with the hold off
       (``CONFIG_RTL9602C_OPTICS_RO``).
   * - That parking the SerDes and clearing its reset register is needed ("PON
       SerDes and analog front end").
     - Removing the writes and checking that the ONU reaches O5 on an OLT.
   * - That the reset of the PON SerDes is needed, and whether it resets the
       GPON MAC as well ("PON SerDes and analog front end").
     - Writing a MAC register before the step and reading it after it.
   * - That the burst-enable output driver, the cleared burst delay and the
       cleared data source are needed ("PON SerDes and analog front end").
     - Removing the writes and checking that the OLT hears the bursts.
   * - That the override of the SerDes's bit-error indication is needed ("PON
       SerDes and analog front end").
     - Removing the write and checking that the ONU reaches O5 on an OLT.
   * - That each of the five analog tuning words is needed ("PON SerDes and
       analog front end").
     - Removing each and checking that the ONU reaches O5 on an OLT.
   * - That the clock-recovery loop setting is needed, and how it decodes ("PON
       SerDes and analog front end").
     - Removing it and checking that the ONU reaches O5 on an OLT.
   * - That the enable of the clock recovery's analog front end is needed ("PON
       SerDes and analog front end").
     - Clearing it and checking that the ONU reaches O5 on an OLT.
   * - That forcing the analog line-rate profile to GPON's is needed ("PON
       SerDes and analog front end").
     - Removing it and checking that the ONU reaches O5 on an OLT.
   * - That the burst-enable output at TTL level is needed ("PON SerDes and
       analog front end").
     - Clearing it and checking that the OLT hears the bursts.
   * - That transmit amplitude 5 rather than the reset value 7 is needed ("PON
       SerDes and analog front end").
     - Amplitude 7, and checking that the ONU reaches O5 on an OLT.
   * - That clearing the receiver filter configuration is needed ("PON SerDes
       and analog front end").
     - Removing it and checking that the ONU reaches O5 on an OLT.
   * - That the manual controls of the clock recovery are needed, and which
       level of the reset bit holds it in reset ("PON SerDes and analog front
       end").
     - Removing them and checking that the ONU reaches O5 on an OLT.
   * - That cycling the receiver-enable override is needed ("PON SerDes and
       analog front end").
     - Removing it and checking that the ONU reaches O5 on an OLT.
   * - That taking the burst-enable path out of power-down is needed ("PON
       SerDes and analog front end").
     - Removing it and checking that the OLT hears the bursts.
   * - That the optical module's LOS polarity bit is set rightly, and whether
       both LOS configuration bits are still needed ("Downstream lock, LOS and
       alarms").
     - Clearing each and reading the ``los`` line and the superframe counter,
       with the ONU reaching O5 on an OLT.
   * - That the frame-derived pulse and its offset are needed ("Downstream lock,
       LOS and alarms").
     - Finding what consumes the pulse, or removing the write.
   * - Which upstream registers the write key locks ("The upstream write key").
     - A write to each without the key, read back.
   * - That the PON port is dormant below O5 and up in O5 ("The activation state
       machine").
     - ``ip link show pon`` in each state.
   * - That a bring-up straight into O7 keeps the laser dark and still acts on
       the Disable_Serial_Number that enables the ONU ("The activation state
       machine").
     - ``gpon_disabled`` set before ``ip link set pon up``: bias and transmitted
       power 0 in O7, then an injected enable and the ONU in O5 again.
   * - That a write into a full upstream PLOAM queue overwrites its tail ("PLOAM
       transmit").
     - Filling a queue and reading its slot.
   * - That the serial-number response needs no random delay where several
       ONUs are discovered together, and what bit 11 of
       ``RTL9602C_GTC_US_CTRL`` does to the delay ("PLOAM transmit").
     - The responses captured at the OLT with the bit at 1 and at 0.
   * - That arming the Serial_Number_ONU template again on entering O4 is needed
       ("PLOAM transmit").
     - Removing it and checking that the ONU reaches O5 on an OLT.
   * - That flushing the upstream PLOAM queues on entering O5 is needed ("PLOAM
       transmit").
     - Removing it and checking that the ONU reaches O5 and OMCI messages pass
       both ways.
   * - That the repeat index of the burst overhead names the byte that is
       stretched into the preamble ("The burst overhead").
     - Another index, with the OLT's delineation of the bursts as the witness.
   * - That the MAC sends the bytes before the repeat index once, at the start
       of the overhead, so that the type 1 and type 2 preambles come first
       ("The burst overhead").
     - An OLT that checks them, or a capture of the upstream light.
   * - That the overhead length counts the guard, up to 32 bits ("The burst
       overhead").
     - An OLT with a guard longer than 32 bits.
   * - The unit of the laser lead and lag, and that the lead is needed
       ("Upstream timing").
     - Checking that the ONU reaches O5 on an OLT with other values (ITU-T
       G.984.2 Table I.2 bounds the transmitter's enable and disable times at 16
       bits).
   * - Which level the burst-enable polarity selects, and that the Ind bit does
       what its use suggests ("Upstream timing").
     - Clearing each under the key and checking that the ONU reaches O5 on an
       OLT.
   * - That the small-StartTime and laser-suppression bits do what their use
       suggests ("Upstream timing").
     - Clearing bit 0 under the key with StartTime 16 grants and checking that
       the ONU reaches O5 on an OLT.
   * - That the burst power save, and its window, are needed ("Upstream
       timing").
     - Removing the writes and checking that the ONU reaches O5 on an OLT.
   * - That the signal-detect limits are needed ("Upstream timing").
     - Removing the write and checking that the ONU reaches O5 on an OLT and the
       upstream latch stays quiet.
   * - That the burst merge threshold is needed ("Upstream timing").
     - Removing the write and checking that the ONU reaches O5 on an OLT and
       upstream GEM frames reach the OLT.
   * - That the bandwidth-map counter is 16 bits wide, so that the 50 ms period
       of the worker reads it in time ("Counters").
     - A read after more than 8 s without a sync.
   * - That a downstream FEC detection threshold of 0 is right ("FEC and
       encryption").
     - An OLT with downstream FEC on.
   * - That ``0x70118c``, ``0x701190`` and ``0x701194`` count the bytes and
       codewords FEC corrected and the codewords it could not ("Counters").
     - An OLT with downstream FEC on, and a fibre attenuated towards errors.
   * - That the MAC codes upstream FEC, and sets the FEC bit of the Ind field,
       by itself when the OLT's allocations ask for it ("FEC and encryption",
       "Known limitations").
     - An OLT that sets Use FEC: the ``us`` line of the ``gpon`` view, the
       kernel log, and the OLT's view of the ONU's bursts.
   * - That the serial-number requests the MAC counts in O3 are those it
       answered ("Transmit power level").
     - The ``ploam-auto`` count of the ``uscntr`` view rising by as many as
       ``sn-requests`` while the ONU answers in O3 without an ONU-ID.
   * - That the ONU's own levelling counts the modes of Upstream_Overhead, 0,
       1, 2, 0 ("Transmit power level").
     - An OLT that relies on the ONU's own levelling, or a later edition of
       G.984.3 clause 10.5.1.
   * - That a lower level set before O4 from a kept set point reaches the
       mode's power ("Transmit power level", and
       Documentation/networking/device_drivers/pon/gn25l95.rst).
     - The transmitted power in O5 after an injected Upstream_Overhead naming
       a mode measured earlier, against that measurement.

Questions the code does not depend on
-------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Question
     - What would settle it
   * - What the bits of the memory self-test mean ("The register block").
     - A part with a failing MAC memory.
   * - What bit 15 of ``RTL9602C_SERDES_ANA_RX_CFG`` selects, given that the
       downstream has also framed with it set ("Downstream lock, LOS and
       alarms").
     - Repeating the change of the bit with the optics' signal-detect
       configuration changed.
   * - What the receiver's sync bit reports ("Downstream lock, LOS and alarms").
     - A reading where it is 1.
   * - What the MAC's LOM bit detects ("Downstream lock, LOS and alarms").
     - A downstream with frames and a corrupted PLOAM channel.
   * - Which upstream field takes the upstream ONU-ID register ("The activation
       state machine").
     - A capture of the upstream at the OLT with the register at 255 and at an
       assigned value.
   * - That the response delay realises the ONU response time ("Upstream
       timing").
     - Ranging with other values and comparing the delay the OLT assigns.
   * - What the upstream signal-detect bits detect where the optics' signal
       detect reaches the MAC, what bit 10 reports, and what enable bits 5 and
       7 select; and whether status bits 7 and 5, which read 1 while both
       upstream PLOAM queues are idle, follow the queues ("Upstream timing").
     - A board that brings the signal detect to the MAC, and a burst keyed with
       the laser disabled; the ``us`` line read while a message waits in each
       PLOAM queue.
   * - That bit 2 of ``RTL9602C_GTC_US_IRQ_STAT`` is the upstream FEC state
       and bit 2 of the upstream latch a change of it ("FEC and encryption").
       It reads 0 while the OLT asks for no FEC.
     - An OLT that sets Use FEC, with the ``bwmap`` view's FEC column read
       beside the ``us`` line.
   * - What the capture fields other than the T-CONT, PLOAMu, valid, StartTime
       and StopTime hold ("The bandwidth map").
     - An OLT granting FEC or DBRu, or several allocations in one burst.
   * - Why stale Port-ID entries kept flow 64 from matching ("Tables: Alloc-ID,
       Port-ID and multicast").
     - Refilling the table with stale entries and binding the OMCC.
   * - What the error, ranging and GEM frame counter fields count
       ("Counters").
     - A fibre attenuated towards errors, and one ranging count per activation.
   * - What the unmeasured upstream counters count, and whether reading clears
       them ("Counters").
     - Readings in O5 under upstream traffic.
   * - Whether the PON class's T-CONT query follows the driver's record
       ("Reporting: the PON class and devlink").
     - The class's ``tcont-get`` in O5.
   * - What the ten bits of the upstream GEM framer's latch detect, and whether
       reading it clears it ("The debugfs views").
     - Two reads in a row under upstream traffic.
   * - What makes the MAC send the Dying_Gasp template it holds, and whether
       it puts the ONU-ID in place of the template's 255 ("PLOAM transmit",
       "Known limitations"). Bit 13 of ``RTL9602C_IRQ_PENDING``, taken to be
       the supply-loss interrupt, read clear in operation with
       ``RTL9602C_DGASP_CFG`` at ``0x000d000c``.
     - A controlled drop of the supply with the ONU in O5 (see
       Documentation/networking/device_drivers/pon/rtl9602c.rst).
