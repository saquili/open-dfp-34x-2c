.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

====================================
Optical safety and rogue ONUs
====================================

The stick is a laser on a fibre that other people's equipment shares. This page
is about two harms that follow from that, what the system does against them,
what it does not, and what a person with a stick on a bench does. The facts are
tagged as the provenance policy asks (``docs/policy/provenance.rst``): M,
measured on the board; T, tested by function; P, a public standard, with its
clause; I, inferred and not verified. The kernel pages hold the detail:
``linux/Documentation/networking/device_drivers/pon/gn25l95.rst`` for
the laser driver and its line, ``rtl9602c-gpon.rst`` for the activation states.

The two harms
=============

**The eye.** The transmitter is invisible infrared light, and a fibre end or
the open cage of a stick that is powered and connected can emit it. The
stick's own measurement of one burst in operation read a bias of 7 mA to 8 mA
and a transmitted power of 1672 uW to 1682 uW, about +2.2 dBm (M: the hwmon
device ``gn25l95``, ``curr1_input`` and ``power1_input``, on the
DFP-34X-2C3 in O5 on a lab OLT, 2026-10-12). Never look into the cage, a
connector or a fibre. Keep the caps on whatever is not connected. The
DFP-34X-2C2 and the DFP-34X-2C3 are one stick, the 2C2 with a UPC optical
connector and the 2C3 with an APC one; use the cord of the kind the stick has.

**The other subscribers.** The upstream of a PON is shared by time: the OLT
grants each ONU its allocations, and an ONU may emit only inside them. An ONU
that emits at other times, a rogue ONU, corrupts the bursts of the others, and
on the OLT's tree it is not one subscriber's fault alone. The standard keeps
every ONU dark until it is told it may transmit: no upstream transmission
before the Serial-Number state, O3 (P: ITU-T G.984.3 clause 10.2.2), and the
laser switched off on a deactivation and on loss of signal (P: ITU-T G.984.3
clause 9.2.1 and clause 11.1.2).

What the system does to stay dark
=================================

Three things hold the light back. Below O3 and in O7 the hard line below is
the only block on the light besides the MAC not keying bursts; in O3 to O5 the
burst enable confines it to the allocations the OLT grants; and the OLT can
silence an ONU for good with the emergency stop.

1. **The hard disable line.** The laser has an input, TX_DISABLE, that stops
   the transmitter while it is asserted: the SoC's GPIO 13, high for dark. The
   driver requests the line at probe already asserted, so that nothing lases
   before the PON path allows it (T: the ONU reaches O5 on an OLT with the line
   released from O3 on). It follows the activation state: asserted in O1 and
   O2, released in O3 to O5, asserted again in O6 (the downstream lost in O5)
   and in O7, on a deactivation and when the interface goes down (M: with the
   ONU in O5 the line driven high by hand took the module's bias from 7 mA to 0
   and its transmitted power from 1677 uW to 0 within a second;
   ``gn25l95.rst``, "TX_DISABLE").
2. **The burst enable.** With the line released the laser still does not emit
   continuously: the GPON MAC keys each burst, and only inside the allocations
   the OLT grants (M: ``rtl9602c-gpon.rst``, the TX_DISABLE row of the state
   machine).
3. **The emergency stop.** An OLT that wants an ONU silent sends
   Disable_Serial_Number with the ONU's serial number: the ONU goes to O7 from
   any state, asserts TX_DISABLE and gives back its ONU-ID, and stays there
   until an enable for its serial number or for every ONU (P: ITU-T G.984.3
   clause 9.2.3.6 and clause 10.2.5.1), across a restart as well (below,
   "What it cannot do"). A test build forced the laser driver on
   through the module's own configuration table: in O7, after an injected
   Disable_Serial_Number, the module's indication of light stayed clear in all
   130 reads, 10 ms apart, with the driver forced on (M: ``rtl9602c-gpon.rst``,
   "Disable_Serial_Number"; the messages were injected with ``ploaminject``,
   an entry that a test build has and this image does not: its debugfs
   directory lists none, 2026-10-12).

Setting the PON interface down is the same hard stop by hand: bias and
transmitted power read 0 within a second and still 4 s later, and after the
interface came up again the ONU was back in O5 within a second (M: ``ip link
set pon down`` and ``up`` on the DFP-34X-2C3 on a lab OLT, bias 8 mA and 1672 uW
before, 0 and 0 after, 2026-10-12).

What the system does not write
==============================

The laser driver of the module has controls of its own for faults, alarms and
rogue ONUs in a configuration table that lives in its EEPROM, set by the
module's maker. A change to it survives every power cycle, so a wrong byte
there outlives the image that wrote it, and the controls it could remove
protect the other ONUs of the PON. The system therefore writes the module as
little as it can:

* The module is written only when its header reads ``ODI`` and
  ``DFP-34X-2C``, the module the driver was measured on: the switch driver
  identifies it, and the module's bus passes writes on only then. For any other
  it behaves as the read-only build: no write to the module, and no transmit
  power level
  (``gn25l95.rst``, "The modules the driver writes"). Whether the part
  of an unlisted module is the same laser driver is not known (I: a module of
  the same name with another laser driver would be written as this one).
* At each bring-up it writes registers of the part, not its EEPROM. It reads
  the persistence control first and, if register changes would reach the
  EEPROM, logs ``EEPROM updates are enabled`` and writes no table byte (M:
  ``gn25l95.rst``, "The modulation hold"). The one byte the module
  would keep is the page select, which holds the page the driver wants on this
  board, so a bring-up writes nothing that stays.
* The entry that stores bytes in the EEPROM, ``t2write``, exists only in a
  kernel built with ``CONFIG_PON_GN25L95_STORE`` (until 2026-10-19
  ``CONFIG_NET_DSA_RTL9602C_OPTICS_STORE``), which is off by default and meant
  for test images, and it takes only the bytes a repair has needed, none known
  to hold a rogue ONU control. This image has no such entry (M:
  ``/sys/kernel/debug/rtl9602c``, where the entry was then, listed no
  ``t2write`` on the image of 2026-10-12; it is now in the laser driver's
  directory, ``/sys/kernel/debug/i2c/i2c-0/0-0050``).
* The module's bus is not open to user space: the image has no ``/dev/i2c-N``
  (M: ``ls /dev/i2c*`` on the same image), because ``CONFIG_I2C_CHARDEV`` is not
  set. Where it is set, a client can write the module for good unless the
  kernel is built with ``CONFIG_RTL9602C_OPTICS_RO``, which makes the bus
  refuse every write (``gn25l95.rst``, "The read-only
  build", "Known limitations").

What it cannot do
=================

* **It does not notice its own laser stuck on.** The MAC's check of the
  module's signal detect against the burst reports nothing on this board, and
  the module's indication of light also rises in normal operation, for up to
  about 240 ms at a time under a large allocation, so it cannot tell a stuck
  laser from a busy one. An ONU that sends out of turn is left to the OLT, which
  disables it (``rtl9602c-gpon.rst``, "Known limitations"). In O7 TX_DISABLE
  keeps the laser dark even with its driver forced on (M, above).
* **A disabled ONU stays disabled across a restart only through the store.**
  The standard has the disabled state survive a power-off (P: ITU-T G.984.3
  clause 10.2.4 and clause 11.1.2). ``pon keep`` keeps the OLT's disable in
  the configuration store, and the boot gives it to the PON driver before the
  PON comes up, which then comes up in O7 with the laser off. Without a store
  that can be written a restarted stick is enabled again, and a kernel that
  cannot take the disable leaves the PON down; ``pon enable`` forgets the
  disable by hand (I: not run on the board; ``onu-utils/README.rst``,
  ``pon``, and ``rtl9602c-gpon.rst``, "Known limitations").
* **Before the kernel's probe.** Between power-up and the driver's request of
  the line, the transmitter is as the loader and the module leave it; nothing
  here measured it (I: a trace of the line from power-up would settle it).
* **After a watchdog reset at a lower power level.** The module stays at the
  lower level until it loses power: the one the OLT named or set, or, before
  the ONU was ranged, the one it took itself after serial-number requests went
  unanswered. The ONU sends at a lower level only from O3 to O6; below O3 and
  in O7 the driver asks for the normal level again (I: not run on the board;
  ``gn25l95.rst``, "Known limitations").
* **Dying_Gasp** is sent before an orderly restart; whether the loss of supply
  makes the MAC send it is not known.

What a person does
==================

* **Try a stick on a lab OLT first.** On a production tree the stick shares the
  fibre with every other ONU (``README.rst``).
* **Keep experiments off a live tree.** A kernel that changes the GPON
  bring-up, the burst parameters or the SerDes is an experiment, and so is any
  image with the options above switched on.
* **Check that the laser is dark** before touching a fibre::

      for h in /sys/class/hwmon/hwmon*; do
          [ "$(cat $h/name)" = gn25l95 ] || continue
          echo "bias $(cat $h/curr1_input) mA, tx $(cat $h/power1_input) uW"
      done

  Below O3 both read 0, in O5 they read the burst values. They are measured
  only in bursts, so a stick that is dark and one with nothing allocated read
  alike; the received power (``power2_input``) says whether light arrives.
* **Stop a stick that you suspect of emitting out of turn.** Take the PON
  interface down at once, over ssh or the console (``ip link set pon down``),
  then take the stick out of the cage, which takes its power; or unplug the
  fibre. Leave the stick as it is for a look at the kernel log and the
  counters (``pon`` and ``ip -s link show pon``) before it is restarted.
* **Do not write the module** from user space, in a bench image or any other,
  and do not carry a ``t2write`` build onto a PON that has subscribers on it.
