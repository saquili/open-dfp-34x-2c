.. SPDX-License-Identifier: GPL-2.0-only

======================================
Realtek RTL9602C switch: the PTP clock
======================================

Overview
========

The switch of the RTL9602C has an IEEE 1588 clock of its own and a few blocks
that work with it. This page describes them as the driver uses them: the
clock, which the driver registers as a PTP hardware clock (PHC); the source of
the switch interrupt that fires once a second; the receive timestamps, which
the switch writes into the PTP messages it sends to the CPU; the transmit
timestamps, which the switch reports for the PTP messages the CPU sends; the
capture of the last frame descriptor that left by a port, which shows the PTP
fields of a frame; the per-port and per-message PTP settings, of which the
driver writes only those that transmit timestamps need; the PON's time-of-day
registers, which the driver only reads; and the time-of-day transmitter, which
the driver also only reads. The switch core, its interrupt and the order of
setup are on the *main* page.

The path of a PTP message that arrives from the host::

    host
      |  PTP message, for example a Sync with EtherType 0x88f7
    port 0 (sfp)
    +--------------+  stamps the frame from its clock and inserts the
    | switch core  |  16-byte timestamp tag
    +--------------+
    port 3 (CPU)
      |
    +--------------+  takes the tag off and keeps its time as the frame's
    | conduit      |  hardware timestamp (all 48 bits of the seconds)
    +--------------+
      |
    +--------------+  ptp_classify_raw(), then rtl9602c_port_rxtstamp(),
    | DSA          |  which applies the port's receive filter
    +--------------+
      |
    user port sfp: SO_TIMESTAMPING

The path of a PTP event message that a socket sends with transmit stamping
on::

    user port sfp: SO_TIMESTAMPING
      |
    +--------------+  rtl9602c_port_txtstamp() keeps a clone and marks
    | DSA          |  the message SKBTX_IN_PROGRESS
    +--------------+
      |
    +--------------+  sends the marked message with two descriptor bits
    | conduit      |  clear, so that the switch applies its egress action
    +--------------+
      |
    port 3 (CPU)
    +--------------+  latches the time the message leaves and sends the
    | switch core  |  CPU a departure report with it
    +--------------+
      |                         |
    port 0 (sfp): the message   port 3: the report
                                  |
                                conduit: rtl9602c_ptp_departure() completes
                                the seconds and the clone
                                  |
                                the socket's error queue

The other pages of the port are named by short names on this page:

- *main*, Documentation/networking/dsa/rtl9602c.rst: the ports, the DSA
  integration, the switch interrupt, the order of setup and a code map of the
  whole driver.
- *acl-qos*, Documentation/networking/dsa/rtl9602c-acl-qos.rst: the header
  capture and, from the side of frame capture, the egress descriptor capture
  that this page uses to read the PTP fields of a frame.
- *CPU port*,
  Documentation/networking/device_drivers/ethernet/realtek/rtl9602c.rst: the
  CPU port's DMA engine, the conduit that takes the timestamp tag off a
  received frame.

What the driver offers
----------------------

- A PTP hardware clock, ``rtl9602c-ptp``, on the switch's clock. It reads,
  steps and slews the clock, reports the once-per-second pulse as a PPS event,
  and the seconds of the OLT's downstream frames as external timestamps of
  channel 0 (see "The OLT's seconds"). The clock has no periodic output and no
  pins. All ports share it.
- Hardware receive timestamps, in the form the switch provides them: a PTP
  message that the switch sends to the CPU port arrives on the user port it
  came in by, with the time of the clock at the frame as its hardware
  timestamp. The receive filters are the layer-2 PTP version 2 ones, all
  answered as ``HWTSTAMP_FILTER_PTP_V2_L2_EVENT``. A Sync over Ethernet is the
  only message seen stamped.
- Hardware transmit timestamps, two-step (``HWTSTAMP_TX_ON``): the time at
  which each Sync, Delay_Req, Pdelay_Req and Pdelay_Resp over Ethernet that a
  port sends leaves the switch, one message at a time.
- Debugfs views of registers that the driver does not use for timestamping:
  the per-port and per-message settings, the PON's time-of-day value and the
  time-of-day transmitter (``ptpstat`` and ``todstat``), and the egress
  capture (``egress_capture``).

The blocks
----------

.. list-table::
   :header-rows: 1
   :widths: 20 34 26 20

   * - Block
     - What it does
     - Reached through
     - Described under
   * - The clock
     - Counts 48-bit seconds and 8 ns ticks from power-on. It is read by
       latching it, stepped by adding a signed offset and slewed by a
       frequency word.
     - The PHC ``/dev/ptp<N>``.
     - "The clock"
   * - The once-per-second interrupt
     - A source of the switch interrupt that fires once a second.
     - ``PTP_ENABLE_PPS`` on the PHC; the ``ptpstat`` view.
     - "The once-per-second interrupt"
   * - Receive timestamps
     - A 16-byte tag with the time of the frame, inserted into each PTP
       message the switch sends to the CPU.
     - ``SO_TIMESTAMPING`` on ``sfp`` or ``pon``; ``SIOCSHWTSTAMP``.
     - "Receive timestamps"
   * - Transmit timestamps
     - A departure report, which the switch sends the CPU when a PTP message
       the CPU sent leaves under the egress action that asks for it.
     - ``SO_TIMESTAMPING`` on ``sfp`` or ``pon``; ``SIOCSHWTSTAMP``; the
       ``tx stamps`` line of ``ptpstat``.
     - "Transmit timestamps"
   * - Egress descriptor capture
     - The descriptor of the last frame that left by a port, with the PTP
       fields of the frame.
     - The ``egress_capture`` view.
     - "The egress capture"
   * - Per-port and per-message settings
     - A PTP enable per port, a transparent-clock bit per port and an action
       per message type, of which the driver writes the egress actions of the
       event messages and the CPU port's enable while transmit stamping is on.
     - The ``ptpstat`` view.
     - "Per-port configuration and message actions"
   * - The PON time of day
     - A second time value beside the clock and its enable: the clock takes
       it at the GPON MAC's time-of-day frame.
     - The ``pon tod`` line of ``ptpstat``.
     - "The PON time of day"
   * - The time-of-day transmitter
     - A pulse once a second and, after a delay, a serial time message, in the
       state the board comes up in.
     - The ``todstat`` view.
     - "The time-of-day transmitter"

Operations and files
--------------------

The driver implements these operations of ``struct dsa_switch_ops``; every
name is prefixed ``rtl9602c_``. DSA calls them for the user ports ``sfp`` and
``pon``.

.. list-table::
   :header-rows: 1
   :widths: 26 52 22

   * - Operation
     - What it does
     - Described under
   * - ``get_ts_info``
     - Reports the timestamping capabilities of a port: software transmit and
       receive stamping, hardware receive stamping, raw hardware stamps, the
       PHC index (-1 when no clock is registered), the transmit type off and
       the receive filters none, ``HWTSTAMP_FILTER_PTP_V2_L2_EVENT``,
       ``_L2_SYNC`` and ``_L2_DELAY_REQ``; and, while the conduit hands the
       departure reports on, hardware transmit stamping and the transmit types
       on, one-step Sync and one-step peer-to-peer.
     - "Transmit timestamps"
   * - ``port_hwtstamp_get``
     - Returns the configuration last set on the port. It is all zero, no
       transmit stamping and no receive filter, until something sets it.
     - "Receive timestamps"
   * - ``port_hwtstamp_set``
     - Records a configuration after checking it, and turns the departure
       reports on while any port's transmit type is not off and off when none
       is. A transmit type other than off, on, one-step Sync and one-step
       peer-to-peer, a type other than off while the conduit hands no reports
       on, one-step peer-to-peer beside a port with two-step Pdelay_Resp, and
       a receive filter the port cannot provide are refused with ``-ERANGE``.
     - "Transmit timestamps"; "Receive timestamps"
   * - ``port_rxtstamp``
     - Completes the seconds of the hardware timestamp of a received PTP
       message; on a port that answers one-step, keeps the receive time of a
       Pdelay_Req.
     - "Receive timestamps"
   * - ``port_txtstamp``
     - Keeps a clone of a PTP event message to be stamped and marks the
       message for the conduit, one message at a time; writes the time into
       a one-step Sync and the turnaround into a one-step Pdelay_Resp.
     - "Transmit timestamps"

The PHC is a ``struct ptp_clock_info`` with the name ``rtl9602c-ptp``,
``max_adj`` 32,767,000 (parts per billion), ``n_ext_ts`` 0 and ``pps`` 1.
Its callbacks are these functions of ``rtl9602c-ptp.c``.

.. list-table::
   :header-rows: 1
   :widths: 26 52 22

   * - Callback
     - Function
     - Described under
   * - ``gettime64``
     - ``rtl9602c_ptp_gettime()``
     - "The clock"
   * - ``settime64``
     - ``rtl9602c_ptp_settime()``
     - "The clock"
   * - ``adjtime``
     - ``rtl9602c_ptp_adjtime()``, through ``rtl9602c_ptp_offset()``
     - "The clock"
   * - ``adjfine``
     - ``rtl9602c_ptp_adjfine()``
     - "The clock"
   * - ``enable``
     - ``rtl9602c_ptp_enable()``; the only request it accepts is
       ``PTP_CLK_REQ_PPS``
     - "The once-per-second interrupt"

``rtl9602c_ptp_register()`` runs from ``rtl9602c_setup()`` after the optics
have been registered, and ``rtl9602c_ptp_unregister()`` from
``rtl9602c_teardown()`` (see the *main* page, "Setup, in order" and
"Teardown"). ``rtl9602c_ptp_debugfs_init()`` creates the two views of
``rtl9602c-ptp.c``.

The files that hold this page:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - File
     - What of this page it holds
   * - ``drivers/net/dsa/realtek/rtl9602c-ptp.c``
     - The PHC callbacks, ``rtl9602c_ptp_rd_sec()``, the five DSA operations,
       the PON time (``rtl9602c_ptp_pon_tod()``),
       the handler of departure reports and the views ``ptpstat`` and
       ``todstat``.
   * - ``drivers/net/dsa/realtek/rtl9602c.h``
     - The registers (``RTL9602C_PTP_*``, ``RTL9602C_TOD_*``, the egress
       capture ``RTL9602C_PKTDBG_DATA``, ``RTL9602C_PKTDBG_CFG`` and
       ``RTL9602C_PKTDBG_PTP_*``, ``RTL9602C_PTP_ACTION_LATCH_REPORT``,
       ``RTL9602C_PTP_ACTION_CORRECT``, the message slots
       ``RTL9602C_PTP_SLOT_*``, and ``RTL9602C_IRQ_SRC_PTP_PPS``) and the
       driver's state for them: ``ptp_info``, ``ptp_clock``, ``ptp_lock``,
       ``ptp_pps_count``, ``ptp_sec_seen``, ``hwtstamp``, ``ptp_onestep_at``,
       ``ptp_pdelay`` and the ``ptp_tx_*`` fields.
   * - ``drivers/net/dsa/realtek/rtl9602c-main.c``
     - Registration of the PHC at setup and its removal at teardown, and the
       branch of the interrupt handler for the once-per-second source.
   * - ``drivers/net/dsa/realtek/rtl9602c-pon-host.c``
     - The host operation ``tod_arm``, through which the GPON MAC's driver
       gives the PON time.
   * - ``drivers/net/dsa/realtek/rtl9602c-offload.c``
     - The ``egress_capture`` view, which reads the egress capture and the clock.
   * - ``drivers/net/ethernet/realtek/rtl9602c-eth.c``
     - The conduit's side of the timestamp tag: the ``RTL9602C_ETH_P_REALTEK``
       and ``RTL9602C_PTP_TAG_*`` macros and the code that takes the tag off;
       and of the transmit stamps: the descriptor bits it clears for a marked
       message and the departure reports it hands on.
   * - ``include/linux/dsa/rtl9602c.h``
     - The contract between the conduit and the switch driver:
       ``rtl9602c_conduit_tstamp_attach()``, ``rtl9602c_tstamp_rx_t`` and the
       mark of a message awaiting its departure time.
   * - ``drivers/net/dsa/realtek/Kconfig``, ``Makefile``
     - ``rtl9602c-ptp.o`` is one of the objects of ``rtl9602c.o``.

Configuration
-------------

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Symbol
     - Effect
   * - ``CONFIG_NET_DSA_RTL9602C``
     - Builds ``rtl9602c-ptp.o`` into the switch driver, as described on the
       *main* page.
   * - ``CONFIG_PTP_1588_CLOCK``
     - The PTP hardware clock exists only with it: without it
       ``ptp_clock_register()`` returns NULL, no clock is registered and
       ``get_ts_info`` reports the PHC index -1. The symbol is a tristate that
       depends on ``NET`` and ``POSIX_TIMERS``, defaults to the value of
       ``CONFIG_ETHERNET``, and selects ``CONFIG_PPS`` and
       ``CONFIG_NET_PTP_CLASSIFY``, the kernel's PTP classification, without
       which DSA never calls ``port_rxtstamp`` (see "Known limitations"). The
       driver's own entry neither selects it nor depends on it.
   * - ``CONFIG_RTL9602C_ETH``
     - The conduit, which takes the timestamp tag off a received PTP message
       and keeps its time as the frame's hardware timestamp, and hands the
       departure reports on. Without it ``get_ts_info`` offers no transmit
       stamping.
   * - ``CONFIG_DEBUG_FS``
     - The views ``ptpstat`` and ``todstat``, and ``egress_capture`` of the egress
       capture.

``rtl9602c_defconfig`` (``arch/mips/configs/``) does not name
``CONFIG_PTP_1588_CLOCK``. The symbol follows ``CONFIG_ETHERNET``, which is on
by default, so a kernel configured from the defconfig has the PTP hardware
clock, ``CONFIG_PPS`` and ``CONFIG_NET_PTP_CLASSIFY``. The defconfig sets
``CONFIG_DEBUG_FS=y`` and builds no modules.

Device tree
-----------

No property configures the clock, the tag or the captures, and no node
describes them: they belong to the ``switch`` node, compatible
``realtek,rtl9602c-switch``, and to its register window (see the *main* page).
What this page uses from the node is its ``interrupts``, the switch interrupt,
on which the once-per-second source arrives: source 8 of the SoC interrupt
controller, Linux interrupt 16, named ``rtl9602c-switch`` in
``/proc/interrupts``. Without that interrupt in the device tree the driver sets
up no handler, and no pulse is counted.

Interfaces to user space
------------------------

- The PTP hardware clock. ``/sys/class/ptp/ptp<N>/clock_name`` reads
  ``rtl9602c-ptp``, ``max_adjustment`` reads 32767000 (parts per billion),
  ``pps_available`` and ``n_external_timestamps`` read 1, and ``n_alarms``,
  ``n_periodic_outputs`` and ``n_pins`` read 0. The character device
  ``/dev/ptp<N>`` serves ``clock_gettime()``, ``clock_settime()`` and
  ``clock_adjtime()`` on the clock (see "The clock"); ``N`` is the index that
  ``ethtool -T`` reports. The image's C library, built with 64-bit time, makes
  these calls through their 64-bit system calls, so the board's kernel needs
  none of the 32-bit-time calls (``CONFIG_COMPAT_32BIT_TIME`` is off): its
  ``clock_adjtime()`` set the clock 50 ppm fast and back.
- The pulse per second. ``PTP_ENABLE_PPS`` on ``/dev/ptp<N>``, or writing 1 to
  ``/sys/class/ptp/ptp<N>/pps_enable``, enables the source. Each pulse is an
  event of the PPS source ``ptp<N>`` of the kernel's PPS subsystem, which
  stamps it with the system clock when the interrupt handler runs (see "The
  once-per-second interrupt").
- The OLT's seconds. ``PTP_EXTTS_REQUEST`` or ``PTP_EXTTS_REQUEST2`` for
  channel 0, on ``/dev/ptp<N>`` open for writing, turns them on and off; each
  second is then a ``struct ptp_extts_event`` to ``read()`` from the device
  (see "The OLT's seconds").
- ``ethtool -T sfp``, or ``pon``, lists the capabilities that
  ``rtl9602c_get_ts_info()`` reports: software transmit and receive stamping,
  software system clock, hardware transmit and receive stamping, raw hardware
  clock, the PHC index, the transmit types off, on, one-step Sync and
  one-step peer-to-peer and the receive filters none and PTP v2 event. The
  ``ethtool`` of the image prints the first three types only; one-step
  peer-to-peer, not printed, is taken by ``SIOCSHWTSTAMP``.
- ``SIOCSHWTSTAMP`` and ``SIOCGHWTSTAMP``, or the ``tsconfig`` request of the
  ethtool netlink interface, on ``sfp`` or ``pon``, set and read the
  timestamping configuration (see "Receive timestamps" and "Transmit
  timestamps").
- ``SO_TIMESTAMPING`` with ``SOF_TIMESTAMPING_RX_HARDWARE`` and
  ``SOF_TIMESTAMPING_RAW_HARDWARE`` on a socket of ``sfp`` or ``pon`` returns
  the hardware timestamp of a received PTP message; with
  ``SOF_TIMESTAMPING_TX_HARDWARE``, and the port's transmit type on, the
  departure time of each event message the socket sends comes back on its
  error queue (``MSG_ERRQUEUE``). A VLAN interface on the port passes both
  on.
- Three debugfs views in ``/sys/kernel/debug/rtl9602c/``. ``ptpstat`` and
  ``todstat`` (mode 0400) are created by ``rtl9602c-ptp.c``; ``egress_capture`` (mode
  0600, because a write changes the capture's setting) is created by
  ``rtl9602c-offload.c``. Reading ``ptpstat`` clears the per-port transmit
  indications; its line ``tx stamps`` counts the transmit stamps (see
  "Transmit timestamps").
- The kernel log. At setup ``rtl9602c-switch: no PTP clock: <error>`` when the
  registration of the clock fails, and ``rtl9602c-switch: no transmit
  timestamps: <error>`` when the conduit cannot hand departure reports on.
  The conduit logs ``ptp tag reason <reason>, protocol <byte>: <tag>``, the
  16 bytes of the tag in hex, the first time it sees each reason value in a
  timestamp tag, and ``ptp departure report: <tag>`` for the first report.

How to read the fact tables
===========================

Each block below has a short account of what the hardware does and what the
driver does with it, followed by fact tables with three columns: *Fact*, *Tag*
and *Checked by*. The tag says how the fact is established. **M**, measured on
the board (the DFP-34X-2C3): *Checked by* gives the command or observation,
through the driver's own interfaces (the PHC, ``SO_TIMESTAMPING``, the kernel
log, the debugfs views ``ptpstat``, ``todstat`` and ``egress_capture``), that shows it.
**T**, tested by function: the driver depends on the fact, and the named
behaviour fails if it is wrong. **P**, public standard: *Checked by* names the
document and the clause. **I**, inferred, not verified: the fact is an
assumption, and *Checked by* says what would confirm or refute it. The last
chapter collects these facts and says which of them the driver depends on.

A row with two tags is established in two ways, or in two parts; *Checked by*
says which part carries which tag. Register offsets are from the base of the
switch's register window, physical ``0x1b000000``. Values are 32-bit words as
``readl()`` and ``writel()`` see them on this big-endian CPU. Times are given
as the PHC returns them, in seconds and nanoseconds. Port numbers are the
hardware's (0 ``sfp``, 1 not wired, 2 ``pon``, 3 the CPU port; see the *main*
page), and "the host" is the equipment at the far end of the SFP lane, the
source of the PTP messages in the measurements. The behaviour that a tested
fact depends on is named in words: the PTP hardware clock reads and steers.

The clock
=========

The clock counts seconds in 48 bits and nanoseconds in ticks of 8 ns. It runs
from power-on, and nothing in the driver starts, resets or sets it, so the PHC
registered on it is independent of the kernel's clocks: a program that needs
the two related has to measure the offset between them itself. The driver
registers the PHC at the end of setup and unregisters it at teardown. A
registration that fails is logged (``no PTP clock``) and the switch carries on
without a clock.

The registers do not show the running time. ``RTL9602C_PTP_CLK_SEC`` and
``RTL9602C_PTP_CLK_NSEC`` hold what the last latch command captured, so the
driver reads the clock by writing the latch bit and then reading both, under a
mutex. No register takes an absolute time either: the driver steps the clock
by writing a signed offset into the step registers and then the step bit,
which adds the offset to the running clock, and setting the clock is a read
followed by a step of the difference. The frequency is one 28-bit word; at its
middle value, 2^27, the clock advances one second per second.

.. list-table::
   :header-rows: 1
   :widths: 38 18 44

   * - Register
     - Offset
     - What it holds
   * - ``RTL9602C_PTP_CLK_SEC``
     - ``0x01b000``, two words
     - The seconds of the last latch, the high part in the first word.
   * - ``RTL9602C_PTP_CLK_NSEC``
     - ``0x01b008``
     - The nanoseconds of the last latch, in 8 ns ticks (bits 29:3).
   * - ``RTL9602C_PTP_CLK_STEP_SEC``
     - ``0x01b00c``, two words
     - The seconds of the offset a step adds, in two's complement.
   * - ``RTL9602C_PTP_CLK_STEP_NSEC``
     - ``0x01b014``
     - The nanoseconds of that offset, in 8 ns ticks (bits 29:3).
   * - ``RTL9602C_PTP_CLK_FREQ``
     - ``0x01b018``
     - The frequency word, ``RTL9602C_PTP_CLK_FREQ_WORD`` (bits 27:0).
   * - ``RTL9602C_PTP_CLK_CTRL``
     - ``0x01b01c``
     - Bit 0 ``RTL9602C_PTP_CLK_CTRL_STEP``, bit 1
       ``RTL9602C_PTP_CLK_CTRL_LATCH``, bit 2
       ``RTL9602C_PTP_CLK_CTRL_PON_TIME_EN`` (see "The PON time of day").

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The switch has a PTP clock of its own. It runs from power-on, a warm
       reboot does not reset it, and the driver neither starts, resets nor sets
       it at setup. The PHC registered on it, ``rtl9602c-ptp``, is therefore
       independent of the kernel's clocks. A PHC that fails to register is
       logged (``no PTP clock``) and the switch carries on; without
       ``CONFIG_PTP_1588_CLOCK`` none is registered.
     - M
     - ``clock_gettime()`` on the PTP device 82 s after a RAM boot returned
       23,853 s, while ``CLOCK_MONOTONIC`` and ``CLOCK_REALTIME`` returned
       82.8 s.
   * - ``RTL9602C_PTP_CLK_SEC`` (``0x01b000``, two words) and
       ``RTL9602C_PTP_CLK_NSEC`` (``0x01b008``) hold a capture of the clock,
       not the running time: they keep what the last latch command captured,
       and read zero on a board where nothing has latched.
     - M
     - Three reads 43 s apart without a latch returned 0 s each time; with a
       latch before each read the seconds advance by one per second.
   * - ``RTL9602C_PTP_CLK_CTRL_LATCH``, bit 1 of ``RTL9602C_PTP_CLK_CTRL``
       (``0x01b01c``): writing 1 captures the running clock into
       ``RTL9602C_PTP_CLK_SEC`` and ``RTL9602C_PTP_CLK_NSEC`` as one instant.
     - T
     - The PTP hardware clock reads: without the latch every read returns the
       same capture (see the row above).
   * - Seconds are 48 bits over two words, the high part at the lower
       address: bits 47:32 are ``RTL9602C_PTP_SEC_UPPER`` (bits 15:0 of the
       first word), bits 31:0 the whole second word.
       ``RTL9602C_PTP_CLK_STEP_SEC`` has the same layout.
     - M
     - Assembled with the low word first, the seconds advanced by 2^32 per
       second; with the high word first, by one per second (23,092 s, 23,099 s
       and 23,106 s, 7 s apart).
   * - Nanoseconds count 8 ns ticks: ``RTL9602C_PTP_NSEC_TICKS`` is bits 29:3
       (27 bits, for 0 to 124,999,999 ticks) and
       ``RTL9602C_PTP_NSEC_TICK_NS`` is 8. ``RTL9602C_PTP_CLK_NSEC`` and
       ``RTL9602C_PTP_CLK_STEP_NSEC`` use this field.
     - M
     - An adjtime of -1.5 s through the PHC, written as -2 s and 62,500,000
       ticks, stepped the clock by -1.501 s, measured to the millisecond
       against ``CLOCK_MONOTONIC``.
   * - No register takes an absolute time. ``RTL9602C_PTP_CLK_STEP_SEC``
       (``0x01b00c``, two words) and ``RTL9602C_PTP_CLK_STEP_NSEC``
       (``0x01b014``) hold a signed offset, and
       ``RTL9602C_PTP_CLK_CTRL_STEP`` (bit 0 of
       ``RTL9602C_PTP_CLK_CTRL``) adds it to the running clock.
     - M
     - An adjtime of +2 s through the PHC stepped the clock by +2.000 s,
       measured against ``CLOCK_MONOTONIC``.
   * - A negative offset is written with its seconds in two's complement over
       the 48 bits and its nanoseconds as the non-negative remainder within
       the second: -1.5 s is -2 s and 500,000,000 ns.
     - M
     - The -1.5 s step of the nanoseconds row above.
   * - Each write of 1 to ``LATCH`` or ``STEP`` acts once. The driver only
       sets these bits, by read-modify-write of ``RTL9602C_PTP_CLK_CTRL``
       (which keeps bit 2, the PON time-of-day enable), and never clears them;
       a settime, two steps and the reads between them each acted exactly
       once.
     - M
     - The -1.5 s step, the +2 s step and the settime of the neighbouring
       rows, read against ``CLOCK_MONOTONIC``.
   * - settime is a latch of the clock followed by a step of the difference,
       so the time that passes between the two passes on the clock too: what
       the clock lacks after a settime is only the time from the caller's read
       of its own clock to the latch. Set to ``CLOCK_REALTIME``, the PHC stayed
       about 20 microseconds behind it.
     - M
     - Three settimes from ``CLOCK_REALTIME``, each call taking about 30
       microseconds, then ``PTP_SYS_OFFSET``: the PHC read 18.6 to 20.5
       microseconds behind the system clock, and the same 10 s later. With the
       clock of jiffies, before the system had a finer one, a read straight
       after a settime had taken the 122 microseconds that elapsed until it for
       an error of settime.
   * - ``RTL9602C_PTP_CLK_FREQ`` (``0x01b018``), field
       ``RTL9602C_PTP_CLK_FREQ_WORD`` (bits 27:0), reads ``0x8000000`` = 2^27,
       the middle of the field, on a board nothing has adjusted; the driver
       names that value ``RTL9602C_PTP_CLK_FREQ_WORD_NOMINAL``. At that value
       the clock advances one second per second (see the seconds row above).
     - M
     - The register read on a freshly powered board.
   * - The clock's rate follows the frequency word in proportion: a larger
       word runs it faster, and a word of ``2^27 * (1 + x)`` runs it at
       ``1 + x`` times its rate. ``adjfine`` writes
       ``adjust_by_scaled_ppm(2^27, scaled_ppm)``, where ``scaled_ppm`` is the
       requested offset in parts per million with a 16-bit fraction, computed
       in 64 bits and clamped to 1 ... ``0x0fffffff``.
     - M
     - The PHC timed against ``CLOCK_MONOTONIC`` for 20 s at each offset:
       asked 100, 1000, 10,000 and 32,000 ppm either way, it ran within 0.15
       ppm of each; at 0 within 0.07 ppm. Before the driver computed the word
       in 64 bits, a 32-bit overflow made every word 58.96 times as far off
       nominal as asked, and those words, up to 94 % of the rate off, fitted
       the same proportion.
   * - Steps and the frequency word act as asked, to the resolution of the
       reads. Steps of +1.5 s, -1.5 s, -250 ms, +250 ms, +100 us, -100 us,
       +8 ns and -1 ns each landed within 200 ns of the step asked for, less
       than the 6.7 to 7.4 us that the two reads around a step take. The
       free-running clock gained 0.12 ppm on ``CLOCK_MONOTONIC_RAW`` over
       10 s; with ``ADJ_FREQUENCY`` at +100 ppm and at -100 ppm it gained
       +100.23 ppm and -99.70 ppm, and +0.30 ppm again at 0.
     - M
     - ``clock_adjtime64`` (the only clock_adjtime this kernel has: the
       32-bit-time one answers ENOSYS) with ``ADJ_SETOFFSET`` and
       ``ADJ_FREQUENCY`` on ``/dev/ptp0``, each result against
       ``CLOCK_MONOTONIC_RAW`` read before and after.
   * - ``max_adj`` is 32,767,000 ppb, the largest offset whose ``scaled_ppm``
       fits the ``long`` that ``adjfine`` takes on this 32-bit architecture.
       The PTP core refuses a larger request with ``-ERANGE``.
     - M
     - Asked 32,000 ppm, the clock ran 31,999.97 ppm fast; asked 40,000 ppm
       either way, ``clock_adjtime()`` failed with "Numerical result out of
       range". Before the core checked that the request fits a ``long``,
       +40,000 ppm was cut to 32 bits and set the clock 25,536 ppm slow.
   * - Reading the time is a latch command followed by three reads, and a step
       is three writes followed by a command. ``ptp_lock`` serialises
       ``gettime64``, ``adjtime``, ``adjfine`` and the clock read of the
       ``egress_capture`` view, so that two callers cannot hand each other's capture
       back; ``settime64`` takes it once for its read and once for its step.
     - T
     - The PTP hardware clock reads and steers.

Reading the time, ``rtl9602c_ptp_now()``, which ``rtl9602c_ptp_gettime()`` and
the ``egress_capture`` view call, in order:

1. Take ``ptp_lock``. Purpose: the latch and the reads are one operation. (T)
2. Set ``RTL9602C_PTP_CLK_CTRL_LATCH`` by read-modify-write. Purpose: capture
   the running clock into both time registers as one instant. (T)
3. Read both words of ``RTL9602C_PTP_CLK_SEC`` and assemble 48 bits. Purpose:
   the seconds. (M)
4. Read ``RTL9602C_PTP_CLK_NSEC``; the nanoseconds are the tick field times 8.
   Purpose: the nanoseconds. (M)
5. Release the lock and keep the seconds in ``ptp_sec_seen``. Purpose: they
   complete the receive stamps (see "Receive timestamps"). (I)

``ptp_lock`` is initialised at probe, before setup creates any view that reads
the clock.

Stepping, ``rtl9602c_ptp_adjtime()`` (``rtl9602c_ptp_settime()`` is a read,
then a step of the difference), in order:

1. Split the offset into seconds and nanoseconds with 0 <= ns < 10^9, negative
   seconds in two's complement. Purpose: the nanoseconds of the register have
   no sign. (M)
2. Take ``ptp_lock``. Purpose: the three writes and the command are one
   operation. (T)
3. Write seconds 47:32 into bits 15:0 of the first word of
   ``RTL9602C_PTP_CLK_STEP_SEC`` and seconds 31:0 into the second word.
   Purpose: the offset's seconds. (M)
4. Write ns / 8 into the tick field of ``RTL9602C_PTP_CLK_STEP_NSEC``; the
   remainder of up to 7 ns is dropped. Purpose: the offset's nanoseconds. (M)
5. Set ``RTL9602C_PTP_CLK_CTRL_STEP`` by read-modify-write. Purpose: add the
   offset once to the running clock. (M)
6. Release the lock. The seconds kept in ``ptp_sec_seen`` are left as they
   were.

Frequency, ``rtl9602c_ptp_adjfine()``, in order:

1. Compute the word from the model in the table. Purpose: the requested
   offset becomes a value of the frequency field. (I)
2. Under ``ptp_lock``, read-modify-write the field
   ``RTL9602C_PTP_CLK_FREQ_WORD``. Purpose: the other bits of the register
   stay as they were. (T)

The steps tagged M and T are checked by the reads and steps in the table.

The once-per-second interrupt
=============================

Bit 12 of the switch's interrupt registers is a source that fires once a
second. The driver enables it only while the PTP core has PPS enabled for the
clock, because an enabled source with nobody listening is one interrupt a
second for nothing. The clock's ``enable`` callback accepts one kind of
request, ``PTP_CLK_REQ_PPS``. The branch of the switch interrupt handler for
the source counts the pulse, hands a ``PTP_CLOCK_PPS`` event to the PTP core
and acknowledges the source. The PTP core reports the event to the PPS source
``ptp<N>`` and stamps it with the system clock at the time of the call. The
clock declares one PPS source and no external-timestamp or periodic-output
channel, and the driver drives no pulse out of the chip. The handler is one
branch of the switch interrupt that the *main* page describes ("The switch
interrupt").

To watch the pulses count (``ptp0`` stands for the index that
``ethtool -T sfp`` reports)::

    echo 1 > /sys/class/ptp/ptp0/pps_enable
    grep pulses /sys/kernel/debug/rtl9602c/ptpstat
    sleep 10
    grep pulses /sys/kernel/debug/rtl9602c/ptpstat
    echo 0 > /sys/class/ptp/ptp0/pps_enable

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Bit 12 of the switch interrupt registers,
       ``RTL9602C_IRQ_SRC_PTP_PPS``, is a source that fires once per second.
     - M
     - With the source enabled for five seconds, the ``pulses`` count in
       ``ptpstat`` rose by six. Repeat: enable PPS on the PTP device
       (``PTP_ENABLE_PPS``) and read ``ptpstat`` twice, a timed interval apart.
   * - The source is latched in ``RTL9602C_IRQ_PENDING`` whether it is enabled
       or not: it was set in the first pending word the driver read, before
       anything had enabled it. The handler therefore acts only on sources
       that are both pending and enabled.
     - M
     - The first pending word read, with nothing enabled, was ``0x00001109``:
       bit 12 set.
   * - The driver enables the source only while the PTP core has PPS enabled
       (request ``PTP_CLK_REQ_PPS``; any other request returns
       ``-EOPNOTSUPP``), since an enabled source with no consumer is one
       interrupt a second for nothing. It clears the source's latch before it
       enables it, so that the first event is the next pulse and not one from
       while the source was off. Each pulse is counted (``ptpstat``:
       ``pulses``; ``armed`` shows the enable bit), reported with
       ``ptp_clock_event(PTP_CLOCK_PPS)`` and acknowledged by writing its bit
       back to ``RTL9602C_IRQ_PENDING``. The clock declares one PPS source and
       no external-timestamp or periodic-output channel; the driver drives no
       pulse out of the chip.
     - M
     - Six counts in five seconds (the first row), each pulse acknowledged
       rather than a continuous stream. With PPS enabled after three seconds
       off, ``pulses`` did not move at once in five tries of five; without the
       clearing it rose at once in five of five, from the latched pulse.
   * - The pulse falls on the PTP clock's second boundary and moves with its
       steps and frequency changes; this is what a PPS event means to its
       consumers.
     - M
     - The PPS source's system timestamps, turned into PHC time with
       ``PTP_SYS_OFFSET``: each pulse fell 7 to 16 us after a second of the
       PHC, the time to the handler. After a step of +0.3 s the next pulse
       came 0.7 s after the last, after one of -0.65 s 0.65 s after it, each
       again on a second of the PHC; with the clock 3.2 % fast the pulses came
       every 0.969 s.
   * - The enable goes through ``rtl9602c_irq_enable()``, which changes
       ``RTL9602C_IRQ_ENABLE`` under ``irq_enable_lock``, as the interrupt
       handler, its re-arm work and the other users of the register do (see
       the *main* page, "The switch interrupt"), so no change is lost.
     - T
     - The PPS measurements above enable and disable the source this way; the
       lock is read from the code.

Receive timestamps
==================

The switch stamps a PTP message with its own clock as the message goes up to
the CPU port, and writes the time into a 16-byte tag in the frame. This tag is
separate from the CPU tag, which travels in the descriptors of the CPU port's
DMA engine. The conduit takes the tag off before DSA sees the frame and keeps
its time as the frame's hardware timestamp (``skb_hwtstamps()``); the
conduit's side is described in
Documentation/networking/device_drivers/ethernet/realtek/rtl9602c.rst. The tag
holds all 48 bits of the seconds. The half tag of a message to this device's
own address holds the low 32, and the DSA operation ``port_rxtstamp``,
``rtl9602c_port_rxtstamp()``, completes the seconds from the last read of the
clock.

The driver has no setting to change for this: the tag is there whatever the
per-port enable and the message actions say (see "The tag").

The tag
-------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The switch inserts a 16-byte tag into a PTP message it sends to the CPU
       port, between the source address and the message's own EtherType, which
       follows the tag.
     - M
     - A 60-byte Sync with EtherType ``0x88F7`` from the host arrived at the
       CPU as 76 bytes, with ``0x8899`` at offset 12 and ``0x88F7`` at offset
       28.
   * - The tag opens with EtherType ``0x8899`` (``RTL9602C_ETH_P_REALTEK``).
     - P
     - IEEE Registration Authority EtherType assignment 8899 (Realtek);
       ``ETH_P_REALTEK`` in ``include/uapi/linux/if_ether.h``.
   * - Tag fields the driver uses, as offsets from the start of the frame: 14,
       a protocol byte (``RTL9602C_PTP_TAG_PROTO_OFFSET``; 4 in every tagged
       frame seen); 15, a reason byte (``RTL9602C_PTP_TAG_REASON_OFFSET``);
       18 to 23, the PTP clock's seconds, all 48 bits, big-endian
       (``RTL9602C_PTP_TAG_SEC48_OFFSET``); 24 to 27, the nanoseconds,
       big-endian, in units of 1 ns rather than the 8 ns ticks of the clock
       registers (``RTL9602C_PTP_TAG_NSEC_OFFSET``). The tag length is
       ``RTL9602C_PTP_TAG_LEN``, 16.
     - M
     - ``SO_TIMESTAMPING`` on a packet socket stamped Syncs sent 250 ms apart
       250.221 ms and 250.177 ms apart; the seconds and nanoseconds equal the
       egress capture of the same frame (see the next row). With the clock on
       the TAI time scale, offsets 18 to 23 read ``0000 6acf ddbb``, the
       clock's seconds then.
   * - The stamp is the switch's PTP clock at the frame: the egress capture of
       the same frame on its way to the CPU (see "The egress capture") holds
       the same instant to the nanosecond, with all 48 bits of seconds, while
       the frame's software stamp read 201.8 s.
     - M
     - ``SO_TIMESTAMPING`` gave 20,323.098509680 s, and the ``egress_capture`` view the
       same time.
   * - The tag is added on egress to the CPU port, whatever the per-port
       enable (``RTL9602C_PTP_PORT_EN``) and whatever the ingress action for
       Sync: with the enable off, and with each of the four ingress action
       values, Syncs still arrived tagged. There is nothing to switch on, so
       ``port_hwtstamp_set`` records the requested configuration and writes no
       register.
     - M
     - Syncs from the host read at the CPU under each setting, the enable
       written per port and the action per message type, both read back in
       ``ptpstat``.
   * - The reason byte follows the ingress action: 240 with the ingress action
       for Sync at 0, 241 with it at 3. The driver does not interpret the
       byte; the conduit logs each value the first time it sees it
       (``ptp tag reason``).
     - M
     - The kernel log, with the ingress action for Sync at 0 and at 3.
   * - A well-formed PTP message is what draws the tag: a frame with EtherType
       ``0x88F7`` and an all-zero body arrived untagged, as did frames with
       ``0x88F8`` and ``0x0800``. Sync over IEEE 802.3 (EtherType ``0x88F7``,
       IEEE 1588-2008 Annex F) is tagged.
     - M
     - Frames of each kind sent from the host; their lengths and EtherTypes
       read at the CPU.

Taking the tag off and completing the seconds
---------------------------------------------

A received PTP message, from the conduit's receive path to DSA, in order:

1. A frame that came up with a CPU tag and the trap reason 240 or 241, longer
   than 16 bytes, with ``0x8899`` at offset 12 and the protocol byte 4 at
   offset 14: read the seconds (offset 18, 48 bits) and the nanoseconds
   (offset 24, 32 bits), and log the reason byte once per value. A frame that
   came up with the trap reason 245, with the PTP EtherType or a VLAN tag at
   offset 20, carries the half tag of a message to this device's own address:
   read the seconds at offset 12 (32 bits) and the nanoseconds at offset 16.
   Purpose: the tag holds the time of the frame. (M)
2. Set the frame's hardware timestamp to those seconds and nanoseconds.
   Purpose: the time travels with the frame. (M)
3. Move the 12 address bytes 16 bytes forward and pull 16 bytes, or 8 bytes
   for a half tag. Purpose: the frame reaches DSA as it was sent, with its own
   EtherType at offset 12. (M)
4. DSA classifies the frame as PTP and calls ``port_rxtstamp``, which
   completes the seconds of a half tag. Purpose: a stamp with all its
   seconds. (I)

The steps are checked by the rows below and by those of "The tag".

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The conduit takes the tag off before DSA sees the frame: it stores the
       tag's seconds and nanoseconds as the frame's hardware timestamp, moves
       the two addresses 16 bytes forward and pulls the tag. With the tag left
       in place ``ptp_classify_raw()`` does not recognise the frame and
       ``port_rxtstamp`` is never called.
     - M
     - After the strip the Sync reached a packet socket as 60 bytes with
       EtherType ``0x88F7``.
   * - ``rtl9602c_port_rxtstamp()`` keeps the low 32 bits of the stamp's
       seconds and takes the upper 16 from the seconds of the last clock read
       (``ptp_sec_seen``, set by every read through ``rtl9602c_ptp_now()``),
       choosing the window of 2^32 seconds that puts the stamp nearest to
       them, and delivers the frame at once (it returns false). A stamp from a
       whole tag keeps its own seconds; one from a half tag is right while the
       PHC has been read within 68 years of the frame. The PHC's auxiliary
       work (``do_aux_work``) reads the clock at registration and then once a
       minute, whether or not a PTP daemon reads the clock.
     - M
     - With the PHC set to 65,530 s, 24 Syncs from the host half a second
       apart were stamped 65,532.76 to 65,544.27 s, without a jump where the
       16 bits of seconds the driver then took from the tag went from 65,535
       to 0; the auxiliary work is read from the code.
   * - The conduit takes the tag off only a frame that came up with a CPU tag
       and one of the two PTP trap reasons, 240 or 241, and that carries
       ``0x8899`` at offset 12 and the protocol byte 4 at offset 14. Other
       frames use that EtherType too, the switch's own loop-detection probes
       among them, and a host can send a frame shaped like a tag; they go up
       as they came.
     - M
     - 100 Syncs from the host came up with trap reason 240 and their stamps.
       100 frames shaped like a tag with reason byte 119, sent by the host,
       came up with trap reason 0, flooded like any frame; before the conduit
       checked the reason, each lost 16 bytes and went up with the forged
       time as its hardware stamp. A ``0x8899`` frame with protocol byte
       ``0x23`` from the host did not reach the CPU.
   * - A PTP message addressed to this device's own address comes up with the
       trap reason 245 and a half tag of 8 bytes between the source address
       and the message's EtherType, or its VLAN tag: the low 32 bits of the
       seconds and the nanoseconds. The conduit takes it off as it takes a
       whole tag, so the message reaches the stack as sent and stamped. A
       message to a group address, the PTP one or broadcast, or to another
       unicast address, comes up with the whole tag.
     - M
     - Five untagged Syncs from the host to the stick's address: each came up
       with reason 245, 68 bytes long with the half tag at offset 12; once the
       conduit took it off, 60 bytes with EtherType ``0x88F7`` and a hardware
       stamp. Five Syncs in VLAN 4094: the half tag before the VLAN tag, reason
       245, and once taken off, stamped. Before, the frames reached the stack
       8 bytes longer, with EtherType 0, and no PTP stack saw them. The half
       tag opened with two zero bytes while the PHC was below 65,536 s and
       with ``6acf``, the seconds' bits 31 to 16, once it held TAI; the
       conduit had taken the zeros for the half tag's mark, and with TAI the
       messages again reached the stack 8 bytes longer, until it looked for the
       EtherType after the half tag instead. Syncs to a unicast address of a
       VLAN interface other than the stick's own came up with the whole
       tag.

The filter
----------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The driver answers a request for the layer-2 event, Sync or Delay_Req
       filter (``HWTSTAMP_FILTER_PTP_V2_L2_EVENT``, ``_L2_SYNC`` and
       ``_L2_DELAY_REQ``) with ``HWTSTAMP_FILTER_PTP_V2_L2_EVENT``, takes
       ``HWTSTAMP_FILTER_NONE``, and refuses any other request, every frame
       and UDP included, with ``-ERANGE``: a filter may be widened, not
       narrowed. The filter promises every event message over Ethernet (IEEE
       1588-2008 6.4: Sync, Delay_Req, Pdelay_Req, Pdelay_Resp; Annex F). The
       switch stamps the general messages over Ethernet as well, and no PTP
       message over UDP.
     - M
     - Three of each message type over Ethernet from the host, Sync,
       Delay_Req, Pdelay_Req, Pdelay_Resp, Follow_Up, Delay_Resp,
       Pdelay_Resp_Follow_Up and Announce, all reached a packet socket on
       ``sfp`` with a hardware stamp, 0.2 s apart as sent; three Syncs over
       UDP to port 319 reached it without one. Load-bearing for the
       filter the driver advertises.
   * - The switch tags its PTP messages whatever the filter, so under
       ``HWTSTAMP_FILTER_NONE`` ``rtl9602c_port_rxtstamp()`` takes the stamp
       off the frame, and a port that nothing has configured delivers none.
     - T
     - Syncs from the host to the PTP group address, received on ``sfp``
       through a packet socket with ``SO_TIMESTAMPING``, carried no hardware
       timestamp under the filter none and one under the layer-2 filter.

Transmit timestamps
===================

The switch reports when a PTP message that the CPU sends leaves it. Each of
the ten message types has an egress action, and the action that latches the
departure time and mirrors it to the CPU (``RTL9602C_PTP_ACTION_LATCH_REPORT``,
2) makes the switch send the CPU a departure report: a frame of its own with
the time, which the conduit hands to ``rtl9602c_ptp_departure()`` (the report
is described on the *CPU port* page, "Departure reports"). The switch applies
an egress action to a message from the CPU only when the conduit sends it with
two descriptor bits clear that it sets on every other frame, and only while
the CPU port's PTP enable is set.

The driver offers two-step stamps, ``HWTSTAMP_TX_ON``; one-step Syncs,
``HWTSTAMP_TX_ONESTEP_SYNC`` (see "One-step Sync and the reference time"),
with which the other event messages stay two-step; and one-step Syncs and
Pdelay_Resp, ``HWTSTAMP_TX_ONESTEP_P2P`` (see "One-step Pdelay_Resp"). While
any port asks for them, ``rtl9602c_ptp_tx_reports()`` sets the egress action
of the event messages' six slots to 2, that of the one-step Sync to 3 while a
port asks for one-step messages, that of Pdelay_Resp to 3 while a port asks
for one-step peer-to-peer, and the CPU port's enable; when none does, it
clears them. ``rtl9602c_port_txtstamp()`` takes a Sync, Delay_Req, Pdelay_Req or
Pdelay_Resp over Ethernet, with or without a VLAN tag, keeps a clone of it and
marks the message ``SKBTX_IN_PROGRESS``, which tells the conduit to clear the
two bits. The report carries the time and nothing of the message, so one
message at a time awaits its report (``ptp_tx_skb``): a message sent while
another waits goes unstamped and is counted as busy, unless the other has
waited longer than ``RTL9602C_PTP_TX_TIMEOUT`` (100 ms), when it is given up
and counted as lost. The report completes the clone's seconds as a receive
stamp's are completed and delivers it with ``skb_complete_tx_timestamp()``. A
report with no message awaited is counted as an orphan.

The order, for one message:

1. DSA calls ``port_txtstamp`` for a frame whose socket asked for a hardware
   transmit stamp. On a port with ``HWTSTAMP_TX_ON``, an event message over
   Ethernet is cloned (``skb_clone_sk()``) and, if no other message awaits a
   report, kept with the time it was taken and marked
   ``SKBTX_IN_PROGRESS``. Purpose: the report will not say which message
   left. (T)
2. The conduit sends the marked message without ``RTL9602C_NIC_TXD_NO_REWRITE``
   and ``RTL9602C_NIC_TXD_L34_BYPASS``. Purpose: with either set the switch
   applies no egress action. (M)
3. The message leaves; the switch latches the time and sends the report to
   the CPU port. (M)
4. The conduit hands the report's time to ``rtl9602c_ptp_departure()``, which
   takes the clone, completes the seconds from the last clock read and
   delivers the stamp to the socket's error queue. Purpose: the stamp in the
   form ``SO_TIMESTAMPING`` returns. (T)

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A message that the CPU sends gets the egress action of its type only
       when the descriptor bits ``RTL9602C_NIC_TXD_NO_REWRITE`` and
       ``RTL9602C_NIC_TXD_L34_BYPASS`` are both clear. With either set, as on
       every other frame the conduit sends, the egress capture of the frame
       read action 0 under each of the four egress actions, with the per-port
       enable on all four ports, the destination given or left to the switch,
       and the PTP multicast address; no report came and nothing was written
       into the message.
     - M
     - ``egress_capture`` (filter 0x18), ``ptpstat`` and the receive reasons,
       with the actions and the descriptor bits set by lab-only debugfs files,
       a message at a time from a packet socket on ``sfp``.
   * - The action needs the PTP enable (``RTL9602C_PTP_PORT_EN``) of the CPU
       port, the port the message enters the switch by; the enable of the
       port it leaves by makes no difference.
     - M
     - Delay_Req with the bits clear and action 2: no report with every enable
       clear or with only port 0's set; a report with only port 3's set, and
       with ports 0 and 3.
   * - Under ``RTL9602C_PTP_ACTION_LATCH_REPORT`` (2) the switch sends the CPU
       one departure report per message, holding the time the message left;
       under action 1 no report comes, and where that action keeps the time is
       not established; action 3 writes into the message (see "One-step
       Sync and the reference time"). The egress capture of the frame reads the action
       applied.
     - M
     - The egress capture read action 1, 2 or 3 as set, and the ``rxreason``
       view one report per message under action 2 only.
   * - Message slots of the egress actions: a Sync with the twoStepFlag set
       goes by slot 0 (``RTL9602C_PTP_SLOT_SYNC``) and one without it by
       slot 8 (``RTL9602C_PTP_SLOT_SYNC_1STEP``); a Pdelay_Req with the flag
       by slot 2 (``RTL9602C_PTP_SLOT_PDELAY_REQ_2STEP``) and one without it,
       the usual one, by slot 9 (``RTL9602C_PTP_SLOT_PDELAY_REQ``); Delay_Req
       by slot 1 (``RTL9602C_PTP_SLOT_DELAY_REQ``) and Pdelay_Resp, with the
       flag or without it, by slot 3 (``RTL9602C_PTP_SLOT_PDELAY_RESP``).
       The general messages go by slots 4 to 7: Follow_Up, Delay_Resp,
       Pdelay_Resp_Follow_Up and Announce.
     - M
     - Each of eleven kinds of message, the eight types with Sync, Pdelay_Req
       and Pdelay_Resp each sent with and without the flag, sent with only one
       slot's action at 1, for each of the ten slots, and the egress capture
       read: each took the action of its slot alone. Three Pdelay_Req of each
       kind sent with transmit stamping on got their stamps.
   * - The stamp is the time the message left the switch: it lies between a
       read of the clock before the message was sent and one after, and the
       report comes 58 us to 9.2 ms after the send returns, most within
       170 us. The nanoseconds lie on a 128 ns grid, their low seven bits one
       value through a boot (see the *CPU port* page).
     - M
     - 21 Delay_Req and Sync through ``SO_TIMESTAMPING`` on ``sfp`` and on its
       VLAN 4094 interface, timed by the sender against the PHC.
   * - A message sent with the two bits clear leaves byte for byte as one sent
       with them set, VLAN tag included.
     - M
     - Three Delay_Req each way captured at the far end of the SFP lane, the
       same 60 bytes; three stamped Delay_Req on the VLAN 4094 interface
       arrived in the VLAN, as sent.
   * - One message awaits its report at a time; another sent meanwhile goes
       unstamped. Messages sent three at a time got one stamp, or two when the
       first report came before the third message was sent; every stamp was
       its own message's.
     - M
     - ``ptpstat`` counted the busy messages; the sender matched each stamp
       to its message by the sequence ID of the message on the error queue.
   * - A report never comes later than ``RTL9602C_PTP_TX_TIMEOUT`` (100 ms)
       after its message; one that did would be handed to the next message
       awaited.
     - I
     - The slowest report seen took 9.2 ms. The ``lost`` count of ``ptpstat``
       would show messages given up; a report that matched no message, the
       ``orphan reports`` count.
   * - A message over UDP is not stamped: the switch recognises no PTP message
       over UDP on receive or on transmit, so ``rtl9602c_port_txtstamp()``
       takes only layer-2 messages.
     - M
     - Three Syncs over UDP to port 319 reached the CPU without a tag (see
       "The filter"); Syncs and Delay_Req sent over UDP with the descriptor
       bits clear, under action 2 on every slot, read no PTP message in the
       egress capture, and no report came.
   * - With the departure time a port can time its own Delay_Req, which the
       delay request-response mechanism needs to measure the path delay, and,
       as a master, send the precise origin of its Syncs in a Follow_Up.
     - P
     - IEEE 1588-2008, 11.3 and 9.5.10.
   * - A PTP message arriving from the host goes to the CPU port and to no
       other port. A message that the switch forwards between the PON and SFP
       ports leaves as it came too: under each egress action for Sync, with
       the per-port enable on every port, and with the transparent-clock bit
       of every port set or clear, its correctionField and originTimestamp
       were unchanged, and no copy came back to the CPU.
     - M
     - The egress capture narrowed to each port in turn caught the Sync only on
       port 3, the CPU port; destination 01-1B-19-00-00-00 (IEEE 1588-2008,
       Annex F). Syncs sent at the OLT side, three or five per setting, and
       at the subscriber side, captured at the other: the fields read 0, the
       transmit indication of the egress port set, and the CPU's receive
       reasons counted only the copies that the bridge sends it anyway.
   * - ``RTL9602C_PTP_TX_STATUS(p)`` = ``0x020024`` + ``0x400`` x p lies in
       the per-port register window that repeats every ``0x400`` bytes; it is
       not a 4-byte array. Bit 0, ``RTL9602C_PTP_TX_STATUS_LATCHED``, sets when
       a PTP message leaves port p, under every egress action including none,
       stays clear for other frames, and is cleared by reading it. It says that
       a PTP message left, not that a timestamp is ready.
     - M
     - ``ptpstat``, column ``tx_latched``: after a Sync out of port 0 it read 1
       under each of the four egress actions and 0 on the next read; after an
       ordinary frame, 0; after a Sync forwarded to the CPU, only port 3 read
       1.

One-step Sync and the reference time
------------------------------------

Under egress action 1 (``RTL9602C_PTP_ACTION_LATCH``) the switch writes the
time a one-step Sync leaves into its originTimestamp, as Realtek's own PTP
master sets it for one-step Syncs. For a one-step Sync,
``rtl9602c_ptp_onestep()`` only marks the message for the conduit, like a
message awaiting its report, and counts it (``one-step syncs`` in
``ptpstat``); the correctionField stays as the sender wrote it.

Under egress action 3 (``RTL9602C_PTP_ACTION_CORRECT``) the switch writes the
time a message spends until it leaves into the message itself: it adds the
departure time less a reference time to the correctionField, modulo 8 s, and
adds ``RTL9602C_PTP_PATH_DELAY`` (``0x01b020``) as nanoseconds. The reference
is one register for the switch, ``RTL9602C_PTP_RX_TSTAMP`` (``0x0111e0``, two
words); the driver uses the action for the one-step Pdelay_Resp (see
"One-step Pdelay_Resp"), and keeps the path delay 0
(``rtl9602c_ptp_block_reset()``). As the register is shared, a write waits
until ``RTL9602C_PTP_ONESTEP_GAP_US`` (250 us) have passed since the last,
with the interrupts off; ``ptpstat`` shows the reference (``reference``).

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Under action 1 a one-step Sync from the CPU leaves with its departure
       time in its originTimestamp: an origin of zero, sent, lay between two
       reads of the clock before and after the send, and the correction
       stayed 0. Without the action, or with the descriptor bits set, the
       origin stayed zero.
     - M
     - Three Syncs each with action 1, with action 0, and with the bits set,
       from a packet socket on the VLAN 4094 interface, captured at the far
       end against the clock read by the sender.
   * - Through ``HWTSTAMP_TX_ONESTEP_SYNC`` the origin is the time the Sync
       left: it lay between the two reads of the clock, the correction 0.
     - M
     - Three one-step Syncs through the driver, captured likewise, with the
       receive trap of "Per-port configuration and message actions" on.
   * - ``RTL9602C_PTP_RX_TSTAMP`` holds the seconds' low three bits in bits
       2:0 of the first word (``RTL9602C_PTP_RX_TSTAMP_SEC``) and the
       nanoseconds in 8 ns ticks in bits 29:3 of the second
       (``RTL9602C_PTP_RX_TSTAMP_NSEC``); the other bits read 0. The switch
       does not write it: it stayed zero with Syncs arriving under every
       setting.
     - M
     - All ones written to both words read back 7 and ``0x3ffffff8``
       (lab-only debugfs write); ``ptpstat`` with Syncs arriving.
   * - Under action 3 the correctionField of a message from the CPU grows by
       its departure time less the reference, modulo 8 s, the reach of the
       reference's three bits of seconds.
     - M
     - One-step Syncs captured at the far end: with the reference zero, a
       Sync left at 74.864 s carried 2.864012376 s; with the reference at
       3.5 s, one that left at 75.8776 s carried 0.377608664 s.
   * - Under action 3 the correction grows by ``RTL9602C_PTP_PATH_DELAY`` as
       nanoseconds as well: with 100000 written, the corrections of three
       Syncs lay 89 us past the later read of the clock instead of 11 us
       before it. Under action 1 the word does not change the origin.
     - M
     - Three Syncs each way with the reference zero, captured at the far
       end; lab-only debugfs writes of the word.

One-step Pdelay_Resp
--------------------

A peer that answers a Pdelay_Req one-step (IEEE 1588-2008 11.4.3) sends a
Pdelay_Resp without the twoStepFlag, its requestReceiptTimestamp zero and its
correctionField grown by the turnaround: the time from the Pdelay_Req's
arrival to the Pdelay_Resp's departure. Action 3 does that when the reference
holds the arrival. On a port with ``HWTSTAMP_TX_ONESTEP_P2P``,
``rtl9602c_port_rxtstamp()`` keeps the sequence ID, the source port identity
and the completed receive time of the last Pdelay_Req (``ptp_pdelay``), and
``rtl9602c_ptp_onestep_p2p()`` takes a Pdelay_Resp whose sequence ID and
requestingPortIdentity are that Pdelay_Req's: it writes the receive time into
the reference, cut to its 8 ns ticks, at least 250 us after the last write
(``rtl9602c_ptp_reference()``), and marks the message for the conduit. A
Pdelay_Resp that answers no Pdelay_Req known leaves as sent, without the
action, and is counted. ``ptpstat`` counts both (``pdelay_resp``, ``without
their pdelay_req``). A Sync without the twoStepFlag is one-step on such a port
as under ``HWTSTAMP_TX_ONESTEP_SYNC``.

Pdelay_Resp has one slot, whatever its flag, so its action holds for the whole
switch: ``rtl9602c_port_hwtstamp_set()`` refuses one-step peer-to-peer on a
port while another port stamps Pdelay_Resp two-step (``HWTSTAMP_TX_ON`` or
``HWTSTAMP_TX_ONESTEP_SYNC``), and the reverse.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Through ``HWTSTAMP_TX_ONESTEP_P2P`` the receive time of the Pdelay_Req
       plus the correction of its Pdelay_Resp is the time the Pdelay_Resp
       left: it lay between two reads of the clock, before and after the send.
       The turnaround was 476 to 510 us, the requester's round trip by its
       software stamps 152 to 181 us longer.
     - M
     - Six Pdelay_Req from the far end of the SFP lane on its VLAN 4094
       interface, each answered from a packet socket on the stick; the
       requester read the correction, the twoStepFlag clear,
       requestReceiptTimestamp zero and its own sequence ID and identity.
   * - One-step peer-to-peer on one port is refused while another stamps
       two-step, in either order.
     - T
     - ``SIOCSHWTSTAMP`` on ``pon`` beside ``sfp``, each way.

The transparent clock
---------------------

The switch is a PTP transparent clock between its wire ports, the SFP port and
the PON port, when the devlink parameter ``ptp_transparent_clock`` says so:
``off`` (the default), ``e2e`` or ``p2p`` (IEEE 1588-2008 11.5.2 and 11.5.4;
``enum rtl9602c_ptp_tc``). ``rtl9602c_ptp_tc_apply()`` then sets the PTP
enable and the transparent-clock bit of the user ports, the ingress action
that forwards a message through the clock (2) for the event messages, and the
egress action that adds the time from the message's arrival to its departure
to its correctionField (3). End-to-end, every event message is corrected;
peer-to-peer, only the Syncs, which also get the path delay of the parameter
``ptp_path_delay_ns`` (``RTL9602C_PTP_PATH_DELAY``), and the peer delay
messages, Pdelay_Req, Pdelay_Resp and Pdelay_Resp_Follow_Up, go to the CPU
only, which answers them per link. While a port stamps what
it receives, every message is forwarded and copied to the CPU (ingress action
3). The egress actions are the clock's, so the clock and transmit stamping
exclude each other: the parameter is refused with ``-EBUSY`` while a port
stamps what it sends, and ``SIOCSHWTSTAMP`` with a transmit type other than
off while the clock is on. ``ptpstat`` shows the mode and the path delay.

::

    devlink dev param set platform/1b000000.switch \
            name ptp_transparent_clock value e2e cmode runtime

The residence time ends where the message leaves the switch. Upstream that is
the GPON MAC, where a message then waits for its allocation before it reaches
the fibre; that wait is not in the correction. Downstream it is the SFP port,
so the wait behind other frames there is.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - End-to-end, a Sync forwarded from the SFP port to the PON port leaves
       with its residence time in its correctionField: 1808 to 1912 ns,
       against 0 with the clock off. A two-step Sync, a Delay_Req and a
       Pdelay_Req are corrected alike; a Follow_Up is not.
     - M
     - Messages from a host on the SFP side to one on the lab OLT's side, in
       a VLAN the stick bridged between the two ports, through the lab OLT;
       the router at both ends: 5 to 8 of each, all received.
   * - The correction follows the queue in the switch: with about 1 Gbit/s
       sent the same way, four Syncs carried 19 to 404 us; after it, 1.8 us
       again. The transit through the lab chain grew to 8.3 to 9.2 ms, most
       of it the wait in the GPON MAC, which the correction does not hold.
     - M
     - The same, with two senders of 1500-byte frames for 3 s; the transit
       from the router's send to its receive, one clock at both ends.
   * - Peer-to-peer with a path delay of 100000 ns, a Sync carried 101800 to
       101904 ns, the delay and its residence time; a Delay_Req carried 0,
       and a Pdelay_Req reached no other port. End-to-end, the Pdelay_Req was
       forwarded and corrected.
     - M
     - As above, through the devlink parameters.
   * - Peer-to-peer, a Pdelay_Resp_Follow_Up goes to the CPU only, as the
       other peer delay messages: slot 6 (``RTL9602C_PTP_SLOT_PDELAY_RESP_FUP``)
       is among ``RTL9602C_PTP_PDELAY_SLOTS``. A Follow_Up and a Delay_Req are
       forwarded as other frames.
     - M
     - Upstream, three of each: no Pdelay_Resp_Follow_Up or Pdelay_Req reached
       the lab OLT's side and three of each came up with reason 240, while
       all Follow_Ups did. With slot 6's ingress action cleared by hand, the
       three Pdelay_Resp_Follow_Ups reached it and none came up.
   * - With the clock off, what a port stamps changes no forwarding: the
       ingress actions and the enables of the wire ports are off. Syncs,
       Follow_Ups and Announces from the SFP side reached the lab OLT's side,
       unicast and to 01-1B-19-00-00-00, each once, and the CPU took a stamped
       copy of each that the switch sent it as well (reason 240). With the
       clock on and a port stamping, each message reached the far side once,
       corrected, and its copy came up with reason 241.
     - M
     - As above, with timed's filter on ``sfp``: 3 to 5 of each, all
       received once. Before, every ingress action trapped while a port
       stamped, and none reached the far side. Letting the bridge forward the
       trapped messages could not serve: DSA hands the driver the event
       messages only (``ptp_classify_raw()``), and the Follow_Ups and
       Announces stayed at the CPU.
   * - A peer delay message to the nearest-bridge address 01-80-C2-00-00-0E
       stays at the CPU, stamped, whether or not the clock is on: the
       reserved-multicast control of that address takes it there (see the
       *bridging* page).
     - M
     - Three Pdelay_Reqs from the SFP side: none reached the lab OLT's side,
       and a packet socket on ``sfp`` received all three with hardware stamps.
   * - Downstream, from the PON port to the SFP port, the clock corrects as
       upstream. End-to-end, Syncs of both kinds, Delay_Reqs and Pdelay_Reqs
       carried 1680 to 1792 ns, Follow_Ups 0, and every one 0 with the clock
       off. Peer-to-peer with a path delay of 100000 ns, Syncs carried 101688
       to 101792 ns, Delay_Reqs 0, and no Pdelay_Req reached the SFP side.
     - M
     - Messages from a host on the lab OLT's side to one on the SFP side, in
       the bridged VLAN, through the lab OLT: 3 to 8 of each, all received.
   * - Downstream the correction holds the wait at the SFP port: with about
       1 Gbit/s sent the same way, Syncs carried 6424 to 6504 ns, about the
       residence time and one 1500-byte frame on the 2.5 Gbit/s SFP link
       (4.9 us).
     - M
     - The same, with one sender of 1500-byte frames for 4 s: 8 of 12 Syncs
       received. The other 4 were lost before the stick, which passed on
       every frame it received: the sender offered 1.56 Gbit/s to the lab
       OLT's 1 Gbit/s link.
   * - The lab OLT sends every frame it receives upstream back downstream. A
       learned address of the SFP side then moves to the PON port with the
       first frame sent back, and the switch drops what is sent to it from
       the PON side; a static entry of the SFP port does not move.
     - M
     - 20 frames from the SFP-side host to the lab OLT's side: all 20 reached
       the router and 20 more arrived on the stick's GEM port. After a ping,
       the host's address stood on the PON port and pings failed both ways;
       with ``bridge fdb replace ... static sticky`` on the SFP port, they
       all passed and the downstream tests above ran.

The egress capture
==================

A second instrument holds the descriptor of the last frame that left by the
ports its configuration names. The descriptor of a PTP frame carries the PTP
fields of the frame: a flag that the frame is a PTP message, and the time the
switch stamped it with. For a frame on its way to the CPU that time is the
receive stamp, so the capture is the means of checking the tag's time. The
instrument is read and armed through the debugfs view ``egress_capture`` of
``rtl9602c-offload.c``, which also reads the switch clock at the same moment.
The same instrument is described from the side of frame capture in
Documentation/networking/dsa/rtl9602c-acl-qos.rst ("Egress descriptor
capture").

The view prints four lines::

    raw <15 words>
    cfg <word>  valid <0|1>  reason <n>  mode <n>  igr <port>  egr <port>
    ptp <flag>  act <n>  id <n>  stamp <seconds>.<nanoseconds>
    clock    <seconds>.<nanoseconds>

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_PKTDBG_DATA(i)`` = ``0x02a070`` + 4 x i, i = 0 ... 14
       (``RTL9602C_PKTDBG_DATA_WORDS`` = 15), hold the descriptor of the last
       frame that left and matched the filter in ``RTL9602C_PKTDBG_CFG``
       (``0x02a0b4``): one frame, the most recent, not a queue. The ``egress_capture``
       view prints the words, the filter, the decoded PTP fields and the
       clock; a write to it stores the written value in
       ``RTL9602C_PKTDBG_CFG``.
     - M
     - ``cat /sys/kernel/debug/rtl9602c/egress_capture`` after PTP frames; then
       ``echo <word> > /sys/kernel/debug/rtl9602c/egress_capture`` and a second read.
   * - Filter fields: ``RTL9602C_PKTDBG_CFG_EGRESS_PORT`` (bits 2:0) selects
       the frames leaving one port, ``RTL9602C_PKTDBG_CFG_INGRESS_PORT``
       (bits 5:3) those arriving on one.
     - M
     - Narrowing by egress port caught the frame leaving that port (see
       "Transmit timestamps"); a value that put 6, a port that does not exist,
       into bits 5:3 matched nothing and the capture kept its old contents.
   * - Capture bits are numbered from bit 0 of the first word (lowest address)
       upward across the 15 words; a field may cross into the next word.
     - M
     - Numbered so, the capture of a Sync on its way to the CPU gave
       20,323.098509680 s, the same frame's receive stamp to the nanosecond
       (see "Receive timestamps"); with the words taken in reverse order it
       gave 0.000004096 s.
   * - PTP fields: ``RTL9602C_PKTDBG_PTP_FLAG_LSB`` (bit 181) is set for a PTP
       message; seconds are bits 139:92 (``RTL9602C_PKTDBG_PTP_SEC_LSB``,
       ``RTL9602C_PKTDBG_PTP_SEC_BITS`` = 48); nanoseconds are bits 171:140,
       in units of 1 ns (``RTL9602C_PKTDBG_PTP_NSEC_LSB``,
       ``RTL9602C_PKTDBG_PTP_NSEC_BITS`` = 32). With the flag clear the time
       fields hold stale data.
     - M
     - As the row above; with the flag clear, captures on three ports printed
       one and the same stale time.
   * - ``RTL9602C_PKTDBG_CFG_VALID`` (bit 16 of ``RTL9602C_PKTDBG_CFG``) reads
       0 straight after the configuration is written and 1 once a frame has
       been captured.
     - M
     - ``egress_capture``: a configuration written through the view read back with the bit
       clear, and with it set once traffic had left by the selected port.
   * - The 2-bit action at bits 180:179 (``RTL9602C_PKTDBG_PTP_ACTION_LSB``,
       ``RTL9602C_PKTDBG_PTP_ACTION_BITS``) is the egress action the switch
       applied to the frame: 0 for a message sent with the conduit's usual
       descriptor bits, the configured action for one sent with them clear.
     - M
     - Delay_Req and Sync under each egress action, with the bits set and
       clear (see "Transmit timestamps").
   * - Printed by ``egress_capture`` but not interpreted: a 7-bit identifier
       at bits 178:172 (``RTL9602C_PKTDBG_PTP_ID_LSB``,
       ``RTL9602C_PKTDBG_PTP_ID_BITS``), and in ``RTL9602C_PKTDBG_CFG``
       ``RTL9602C_PKTDBG_CFG_MODE`` (bits 7:6) and
       ``RTL9602C_PKTDBG_CFG_REASON`` (bits 15:8). The identifier read 0 to 3
       and 22 in the captures seen.
     - I
     - The meanings are unverified; debugfs only.

Per-port configuration and message actions
==========================================

The switch has a PTP enable for each port, a transparent-clock bit for each
port, and an ingress and an egress action for each of ten message types. The
driver writes the egress actions of the event messages and the CPU port's
enable while transmit stamping is on (see "Transmit timestamps"), and the
enables of the wire ports, their transparent-clock bits and the ingress
actions only for the transparent clock (see "The transparent clock"). An
action acts on the messages arriving by an enabled port. Without the clock
``rtl9602c_ptp_apply()`` leaves them as ``rtl9602c_ptp_block_reset()`` does,
which clears every enable but the CPU port's, the ingress actions, the
transparent-clock bits and the path delay, also at registration and teardown:
the switch then forwards PTP messages as other frames, whatever the ports
stamp. The receive stamping does not depend on them (see "Receive
timestamps"). The ``ptpstat`` view prints them all. The
message types are numbered in the order of the action registers, which is not
the ``messageType`` field of the PTP header (IEEE 1588-2008, clause 13.3.2.2).

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_PTP_PORT_EN`` = ``0x0111e8`` + 4 x port, bit 0: a per-port
       PTP enable. The driver sets it on the CPU port while transmit stamping
       is on, and a message from the CPU gets its egress action only then; and
       on the user ports while the transparent clock is on.
       ``ptpstat`` prints it (``enabled``). Receive stamping does not depend
       on it.
     - M
     - Set on every port and read back per port in ``ptpstat`` at the 4-byte
       stride; after boot, with timed's filter on ``sfp`` and the clock off,
       only port 3 reads 1, and Syncs from the SFP side come up stamped.
   * - With every ingress action at 1 (``RTL9602C_PTP_ACTION_TRAP``) and the
       enable of port 0 set, Syncs from the far end reached the CPU with
       their stamps, and the CPU's own event messages still left: two-step
       stamps (eight of Sync, Delay_Req and both Pdelay_Req), one-step Syncs
       and one-step Pdelay_Resp all as without it. A trapped message reaches
       no other port (see "The transparent clock").
     - M
     - Three Syncs received, eight two-step stamps, three one-step Syncs and
       six one-step Pdelay_Resp on the VLAN 4094 interface, with the
       registers read in ``ptpstat``.
   * - ``RTL9602C_PTP_ACTION_INGRESS`` (``0x0111d8``) and
       ``RTL9602C_PTP_ACTION_EGRESS`` (``0x0111dc``) hold a 2-bit action
       (``RTL9602C_PTP_ACTION_MASK``) per message type, type n at bits
       2n+1:2n, for ``RTL9602C_PTP_MSG_TYPES`` = 10 types. Type 0 is Sync: its
       ingress action at 3 changed the tag's reason byte from 240 to 241. The
       driver writes the egress actions of the event messages while transmit
       stamping is on, and the ingress actions while a port stamps what it
       receives; ``ptpstat`` prints both, per type.
     - M
     - ``ptpstat`` and the kernel log, with the ingress action for Sync at 0
       and at 3.
   * - Types 1 to 9 as ``ptpstat`` names them (``delay_req``, ``pdelay_req``,
       ``pdelay_resp``, ``follow_up``, ``delay_resp``, ``pdelay_resp_fup``,
       ``announce``, ``sync_1step``, ``pdelay_req_1step``): ``sync`` and
       ``pdelay_req`` are the messages with the twoStepFlag, the ``_1step``
       ones those without it. Every type is measured on the egress side, and
       what the egress actions 1 to 3 do (see "Transmit timestamps").
     - M, I
     - M: the egress capture and the reports, per type (see "Transmit
       timestamps"). I: the ingress actions but 3 on Sync; receiving each
       message under each action of its slot would settle them.
   * - ``RTL9602C_PTP_TRANSPARENT_PORTS`` (``0x0111d4``): one bit per port,
       read 0 on every port; ``ptpstat`` prints it (``transparent``). With
       the bits of ports 0 and 3, of either alone, or none, and the ingress
       action of one-step Sync at 0 to 3, a one-step Sync from the CPU left
       under action 3 with its departure time less the reference, as with no
       bit: the bits do not act on a message the CPU sends. A Sync from
       port 0 to the CPU was received alike under every setting.
     - M
     - Three Syncs each way per setting on the VLAN 4094 interface, the
       correction read at the far end; lab-only debugfs writes.
   * - ``ptpstat`` and ``todstat`` are mode 0400 and only read, except that
       reading ``ptpstat`` clears the transmit indications.
     - M
     - ``tx_latched`` read 1 after a Sync out of a port and 0 on the next read
       (see "Transmit timestamps").

The lines of ``ptpstat``, in order:

.. list-table::
   :header-rows: 1
   :widths: 30 44 26

   * - Lines
     - What they show
     - Register
   * - ``port enabled transparent tx_latched``, then one line for each of the
       four ports
     - The port's PTP enable, its transparent-clock bit, and whether a PTP
       message has left the port since the last read.
     - ``RTL9602C_PTP_PORT_EN``, ``RTL9602C_PTP_TRANSPARENT_PORTS``,
       ``RTL9602C_PTP_TX_STATUS(p)``
   * - ``message ingress egress``, then one line for each of the ten message
       types
     - The ingress and the egress action of each type.
     - ``RTL9602C_PTP_ACTION_INGRESS``, ``RTL9602C_PTP_ACTION_EGRESS``
   * - ``pulses <n>  armed <0|1>``
     - The once-per-second pulses counted since the driver was loaded, and
       whether the source is enabled.
     - ``ptp_pps_count``, ``RTL9602C_IRQ_ENABLE``
   * - ``pon tod <s>.<ns>  enabled <0|1>``
     - The PON time-of-day value and its enable.
     - ``RTL9602C_PTP_PON_TIME_SEC``, ``RTL9602C_PTP_PON_TIME_NSEC``,
       ``RTL9602C_PTP_CLK_CTRL``
   * - ``one-step syncs <n>  pdelay_resp <n>  without their pdelay_req <n>``
     - The one-step Syncs whose time the switch wrote, the one-step
       Pdelay_Resp whose turnaround it wrote, and those that answered no
       Pdelay_Req known and left as sent.
     - ``ptp_tx_onestep``, ``ptp_tx_p2p``, ``ptp_tx_p2p_missed``
   * - ``reference <s>.<ns>  (raw <word> <word>)``
     - The reference time of the last one-step Pdelay_Resp, the seconds' low
       three bits and the nanoseconds.
     - ``RTL9602C_PTP_RX_TSTAMP``

The PON time of day
===================

Beside the clock the switch has a second time value, the PON time, and an
enable for it: while the enable is set, the clock takes the PON time when the
GPON MAC's time-of-day superframe arrives, and the hardware then clears the
enable. That is how the ONU sets its clock from the time of day the OLT gives
(ITU-T G.984.3 Amendment 2 clause 10.4.6): the GPON MAC's driver computes the
time and names the frame (see "The time of day" in
Documentation/networking/device_drivers/pon/rtl9602c-gpon.rst), and calls the
host operation ``tod_arm``, for which ``rtl9602c_ptp_pon_tod()`` writes the
PON time and sets its enable under ``ptp_lock``. It also has the PHC's worker
read the clock 100 ms after the frame (``RTL9602C_PTP_TOD_SETTLE``), so that
receive stamps complete their seconds from the new time. The time replaces
one given before and not yet taken.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_PTP_PON_TIME_SEC`` (``0x01b024``, two words) and
       ``RTL9602C_PTP_PON_TIME_NSEC`` (``0x01b02c``) hold the PON time in the
       layout of the clock, the seconds over two words and the nanoseconds in
       8 ns ticks; ``RTL9602C_PTP_CLK_CTRL_PON_TIME_EN`` (bit 2 of
       ``RTL9602C_PTP_CLK_CTRL``) is its enable. They read zero, and the
       enable 0, until written.
     - M
     - ``ptpstat``: the line ``pon tod 0.000000000  enabled 0`` after boot,
       and the time written read back.
   * - The clock takes the PON time at the armed superframe only with the
       enable set, the hardware clears the enable when it has, and the clock
       runs on from that time: loaded with 1000.5 s, it read 1018.4536 s
       143509 frames (17.94 s) later.
     - M
     - Lab-only register writes and the egress capture's clock; the timing
       to a frame is on the page of the GPON MAC.
   * - The worker's read after the frame keeps the completion of receive
       stamps right across a load.
     - I
     - Not exercised: PTP messages received across a load that moves the
       clock by more than 32768 s.

The OLT's seconds
=================

The GPON MAC raises its interrupt once every 8000 downstream frames, a second
of the OLT's clock, while the switch driver asks it to (the PON operation
``second_enable``; see "Downstream lock, LOS and alarms" in
Documentation/networking/device_drivers/pon/rtl9602c-gpon.rst). The clock
reports those seconds as external timestamps of channel 0: the time of the
clock when a second began, as near as an interrupt reads it. They let a PTP
daemon keep the clock at the OLT's rate, and at the OLT's time once the time
of day the OLT gives has set it (see "The PON time of day"). Asking the clock
for channel 0 (``PTP_CLK_REQ_EXTTS``) asks the MAC for the interrupt
(``rtl9602c_ptp_pon_extts()``); a request without a MAC bound fails with
``-ENODEV``. The only edge is ``PTP_RISING_EDGE``. ``ptpstat`` counts the
seconds reported and those come late (``olt seconds``, ``late``), says whether
they are wanted, and gives the frame of the last one.

A second, in order:

1. While the seconds are wanted, the first half of every GPON interrupt
   (``rtl9602c_irq()``) reads the superframe counter,
   ``RTL9602C_GTC_DS_SUPERFRAME``, and then the clock
   (``rtl9602c_ptp_pon_irq()``). Purpose: both are read as near to the
   frame as the interrupt comes; the thread runs later. (M)
2. The interrupt's thread reads the MAC's latch, and on
   ``RTL9602C_GTC_DS_IRQ_SECOND`` calls the host operation ``second``.
   Purpose: the pulse is told from the other events of the same interrupt.
   (M)
3. ``rtl9602c_ptp_pon_second()`` reports the time of step 1 as the event when
   the counter then stood a whole number of seconds of frames after it stood
   at the second reported last, or at the second before
   (``rtl9602c_ptp_pon_take()``): the counter starts again when the OLT does,
   and two seconds that agree with each other and not with the one reported
   last count the frames from themselves. An interrupt that agrees with
   neither came a frame or more late, and is counted and not reported.
   Purpose: every second reported was read within the frame it began in, also
   after the counter started again. (M for the count; T for the rule, the
   KUnit suite ``rtl9602c-ptp``; I for a late interrupt: none was seen)

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The seconds come 8000 frames apart: 120 in a row, none late and none
       missed, 1000014406.5 ns apart on average by the clock, which ran
       14.4 ppm fast of the OLT's frames. An interval differed from the mean
       by 2.0 us (standard deviation), at most 5.0 us short and 5.8 us long,
       the time the interrupt took to be read.
     - M
     - ``testptp -e 120`` on ``/dev/ptp0`` (tools/testing/selftests/ptp), on
       the lab chain in O5, with nothing steering the clock.
   * - Steered by them, the clock keeps to the OLT's frames: over 8 minutes
       its time at the start of a frame, against the superframe counter, kept
       within 15 us (-112.9 to -98.0 us from where it started), a drift of
       -0.005 ppm, where unsteered it ran 14.4 ppm fast; 547 seconds came,
       none late.
     - M
     - timed steering the clock (its README, "The OLT's seconds"), and every
       34 s 40 lab-only reads of the clock latched beside the counter, each
       batch placing the start of a frame within 4 to 22 us. The test was cut
       short when the lab OLT was taken out.
   * - When the OLT starts again, so does the counter, and the seconds fall
       on other frames. Counted from the second reported last alone, every
       second after counted as late and none was reported: 3012 in a row,
       after the lab OLT was moved and powered again.
     - M
     - ``ptpstat`` (``olt seconds 2889  late 3012``) with the rule of before;
       the rule of step 3 takes the case in the KUnit suite.
   * - What the MAC's pulse offset (``RTL9602C_GTC_DS_PPS_OFFSET``) counts,
       and whether the pulse marks the start of a frame or a point inside one.
     - I
     - A constant either way, which a daemon that follows the seconds takes
       as part of the clock's phase.

The time-of-day transmitter
===========================

The block sends a pulse once a second and, after a programmable delay, a
serial time message. The driver reads it (``todstat``) and neither writes it
nor enables an output pad for it; the values below are those the board comes
up with.

.. list-table::
   :header-rows: 1
   :widths: 40 18 42

   * - Register
     - Offset
     - What it holds
   * - ``RTL9602C_TOD_VERSION``
     - ``0x01b030``
     - The version of the block.
   * - ``RTL9602C_TOD_PPS_CFG``
     - ``0x01b034``
     - The pulse width and the delay from the pulse to the message.
   * - ``RTL9602C_TOD_MSG_CFG``
     - ``0x01b038``
     - The message length and the source of the message.
   * - ``RTL9602C_TOD_GPS_WEEK``
     - ``0x01b03c``
     - The GPS week number.
   * - ``RTL9602C_TOD_UTC_SEC``
     - ``0x01b040``
     - The UTC seconds.
   * - ``RTL9602C_TOD_MSG_DATA(n)``
     - ``0x01b060`` - 4n
     - The bytes of a software message, n = 0 ... 7.
   * - ``RTL9602C_TOD_BAUD``
     - ``0x01b064``
     - The divisor of the serial output's baud rate.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_TOD_VERSION`` (``0x01b030``) reads ``0x16030301``.
     - M
     - ``todstat``, line ``version``.
   * - ``RTL9602C_TOD_GPS_WEEK`` (``0x01b03c``;
       ``RTL9602C_TOD_GPS_WEEK_NUM``, bits 15:0) reads 1877 and
       ``RTL9602C_TOD_UTC_SEC`` (``0x01b040``) reads 1,451,606,400. The two
       place one instant, which fixes both fields: GPS week 1877 begins at
       Unix time 1,451,174,400 (the GPS epoch, 6 January 1980, is
       315,964,800), and 1,451,606,400 is exactly five days later, 1 January
       2016, 00:00:00 UTC.
     - M
     - ``todstat``, line ``hardware``, and the arithmetic.
   * - ``RTL9602C_TOD_BAUD`` (``0x01b064``; ``RTL9602C_TOD_BAUD_DIVISOR``, bits
       15:0) reads 1085. As a divisor of a 125 MHz clock that is 115,207 baud,
       0.006 % from 115,200.
     - I
     - The value is measured (``todstat``, line ``baud div``); the 125 MHz
       clock and the baud rate are inferred. A capture of the serial output
       would settle them.
   * - ``RTL9602C_TOD_PPS_CFG`` (``0x01b034``): the pulse width
       ``RTL9602C_TOD_PPS_CFG_WIDTH`` (bits 5:0, in 10 ms) and the delay from
       the pulse to the message ``RTL9602C_TOD_PPS_CFG_MSG_DELAY`` (bits
       11:6, in 1 ms); both read 10.
     - I
     - The values are measured (``todstat``, line ``pulse``); the fields and
       units are inferred. An oscilloscope on the output would settle them.
   * - ``RTL9602C_TOD_MSG_CFG`` (``0x01b038``): the message length
       ``RTL9602C_TOD_MSG_CFG_LEN`` (bits 8:1, 0 to 32 bytes) and
       ``RTL9602C_TOD_MSG_CFG_SRC_SW`` (bit 0: set, the message is the bytes
       of the software message; clear, the hardware builds it from the GPS week
       and the UTC seconds); it reads 23 bytes, built by the hardware.
     - I
     - As the row above, for the line ``frame``.
   * - ``RTL9602C_TOD_MSG_DATA(n)`` = ``0x01b060`` - 4 x n, n = 0 ... 7, holds
       message bytes 4n to 4n+3, byte 4n+m at bits 8m+7:8m: word 0 is at the
       highest address. ``RTL9602C_TOD_MSG_BYTES`` = 32. ``todstat`` prints the
       bytes in message order.
     - I
     - Nothing has written a message or captured the output; writing a known
       message and capturing the serial line would settle it.

``todstat`` on the DFP-34X-2C3, without its last line (``software``, the 32
bytes of the software message in message order)::

    version   16030301
    pulse     width 10 (x10 ms)  delay 10 (ms)
    frame     23 bytes, hardware
    hardware  gps week 1877  utc sec 1451606400
    baud div  1085

Known limitations
=================

- Transmit stamps are two-step and one message at a time: a message sent
  while another awaits its departure report goes unstamped, and only event
  messages over Ethernet are stamped. One-step stamping is offered for Sync
  and Pdelay_Resp; a one-step Pdelay_Resp waits up to 250 us, with the
  interrupts off, for the last one to leave (see "One-step Sync and the
  reference time"), and one-step Pdelay_Resp holds for every port that stamps
  or none.
  The nanoseconds of a transmit stamp lie on a 128 ns grid (see "Transmit
  timestamps").
- Only the layer-2 PTP version 2 receive filters are taken, answered as
  ``HWTSTAMP_FILTER_PTP_V2_L2_EVENT``. A request for every frame, for UDP, for
  PTP version 1 or for NTP is refused with ``-ERANGE`` and leaves the port's
  configuration as it was. The switch tags no PTP message over UDP (see
  "Receive timestamps").
- The clock and the completion of receive stamps depend on the kernel
  configuration. Without ``CONFIG_PTP_1588_CLOCK`` no PHC is registered
  (``ethtool -T`` reports no PHC index) and nothing reads the clock. DSA hands
  a received frame to ``rtl9602c_port_rxtstamp()`` only when the kernel's PTP
  classification finds a PTP message in it, and that classification,
  ``CONFIG_NET_PTP_CLASSIFY``, is selected by ``CONFIG_PTP_1588_CLOCK``.
  Without it the function is never called, and the hardware timestamp of a
  received PTP message stays the tag's time as it stands, which for a half tag
  holds only the low 32 bits of the seconds. ``rtl9602c_defconfig`` does not
  name the clock, but the symbol follows ``CONFIG_ETHERNET``, so a kernel configured
  from the defconfig has the clock and the classification.

Open questions
==============

These are the facts tagged **I** in the tables above, each with what would
settle it. The first list holds the
assumptions that the driver depends on: the driver is correct on this board
only if they hold, and a board test must settle them. The second list holds
the questions to which the code is indifferent: it works whichever way they
resolve.

Assumptions the driver depends on
---------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Assumption
     - What would settle it
   * - A one-step Pdelay_Resp has left the switch 250 us after its reference
       was written ("One-step Sync and the reference time").
     - Two ports answering one-step together, under load.
   * - A departure report never comes more than 100 ms after its message; one
       that did would be handed to the next message awaited ("Transmit
       timestamps").
     - Watching the ``lost`` and ``orphan reports`` counts of ``ptpstat``
       under load.

Questions the code does not depend on
-------------------------------------

All but the last concern values that only the debugfs views print.

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Question
     - What would settle it
   * - What the identifier, mode and reason fields of the egress capture mean
       ("The egress capture").
     - A capture with a reason that only some of the traffic carries.
   * - Why the nanoseconds of a transmit stamp lie on a 128 ns grid: a coarse
       latch, or departures that fall on it ("Transmit timestamps").
     - A second stamped port, or a scope on the SFP lane.
   * - The baud rate of the time-of-day transmitter and the clock its divisor
       divides ("The time-of-day transmitter").
     - A capture of the serial output.
   * - The fields and units of the pulse width, the delay to the message, the
       message length and the message source ("The time-of-day transmitter").
     - An oscilloscope on the output.
   * - The layout of the software message bytes ("The time-of-day
       transmitter").
     - Writing a known message and capturing the serial line.
