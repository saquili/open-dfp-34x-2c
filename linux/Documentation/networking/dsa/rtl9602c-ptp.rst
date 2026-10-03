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
the switch writes into the PTP messages it sends to the CPU; the lack of
transmit timestamps; the capture of the last frame descriptor that left by a
port, which shows the PTP fields of a frame; the per-port and per-message PTP
settings and the PON's time-of-day registers, which the driver only reads; and
the time-of-day transmitter, which the driver also only reads. The switch core,
its interrupt and the order of setup are on the *main* page.

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
    | conduit      |  hardware timestamp (the low 16 bits of the seconds)
    +--------------+
      |
    +--------------+  ptp_classify_raw(), then rtl9602c_port_rxtstamp(),
    | DSA          |  which completes the seconds from the last clock read
    +--------------+
      |
    user port sfp: SO_TIMESTAMPING

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
  steps and slews the clock and reports the once-per-second pulse as a PPS
  event. The clock has no external timestamp input, no periodic output and no
  pins. All ports share it.
- Hardware receive timestamps, in the form the switch provides them: a PTP
  message that the switch sends to the CPU port arrives on the user port it
  came in by, with the time of the clock at the frame as its hardware
  timestamp. The receive filters are the layer-2 PTP version 2 ones, all
  answered as ``HWTSTAMP_FILTER_PTP_V2_L2_EVENT``. A Sync over Ethernet is the
  only message seen stamped.
- No hardware transmit timestamps. The switch produces no departure time for a
  PTP message that the CPU sends.
- Debugfs views of registers that the driver does not use for timestamping:
  the per-port and per-message settings, the PON's time-of-day value and the
  time-of-day transmitter (``ptpstat`` and ``todstat``), and the egress
  capture (``hsd``).

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
     - None: a per-port indication that a PTP message left, without a time.
     - The ``tx_latched`` column of ``ptpstat``.
     - "Transmit timestamps"
   * - Egress descriptor capture
     - The descriptor of the last frame that left by a port, with the PTP
       fields of the frame.
     - The ``hsd`` view.
     - "The egress capture"
   * - Per-port and per-message settings
     - A PTP enable per port, a transparent-clock bit per port and an action
       per message type, which the driver never writes.
     - The ``ptpstat`` view.
     - "Per-port configuration and message actions"
   * - The PON time of day
     - A second time value beside the clock and its enable, never written by
       the driver.
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
       ``_L2_SYNC`` and ``_L2_DELAY_REQ``.
     - "Transmit timestamps"
   * - ``port_hwtstamp_get``
     - Returns the configuration last set on the port. It is all zero, no
       transmit stamping and no receive filter, until something sets it.
     - "Receive timestamps"
   * - ``port_hwtstamp_set``
     - Records a configuration after checking it; it writes no register. A
       transmit type other than off, and a receive filter the port cannot
       provide, are refused with ``-ERANGE``.
     - "Transmit timestamps"; "Receive timestamps"
   * - ``port_rxtstamp``
     - Completes the seconds of the hardware timestamp of a received PTP
       message.
     - "Receive timestamps"

The PHC is a ``struct ptp_clock_info`` with the name ``rtl9602c-ptp``,
``max_adj`` 500,000,000 (parts per billion), ``n_ext_ts`` 0 and ``pps`` 1.
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
     - The PHC callbacks, ``rtl9602c_ptp_rd_sec()``, the four DSA operations
       and the views ``ptpstat`` and ``todstat``.
   * - ``drivers/net/dsa/realtek/rtl9602c.h``
     - The registers (``RTL9602C_PTP_*``, ``RTL9602C_TOD_*``, the egress
       capture ``RTL9602C_PKTDBG_DATA``, ``RTL9602C_PKTDBG_CFG`` and
       ``RTL9602C_PKTDBG_PTP_*``, and ``RTL9602C_IRQ_SRC_PTP_PPS``) and the
       driver's state for them: ``ptp_info``, ``ptp_clock``, ``ptp_lock``,
       ``ptp_pps_count``, ``ptp_sec_seen`` and ``hwtstamp``.
   * - ``drivers/net/dsa/realtek/rtl9602c-main.c``
     - Registration of the PHC at setup and its removal at teardown, and the
       branch of the interrupt handler for the once-per-second source.
   * - ``drivers/net/dsa/realtek/rtl9602c-offload.c``
     - The ``hsd`` view, which reads the egress capture and the clock.
   * - ``drivers/net/ethernet/realtek/rtl9602c-eth.c``
     - The conduit's side of the timestamp tag: the ``RTL9602C_ETH_P_REALTEK``
       and ``RTL9602C_PTP_TAG_*`` macros and the code that takes the tag off.
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
       and keeps its time as the frame's hardware timestamp.
   * - ``CONFIG_DEBUG_FS``
     - The views ``ptpstat`` and ``todstat``, and ``hsd`` of the egress
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
  ``rtl9602c-ptp``, ``max_adjustment`` reads 500000000 (parts per billion),
  ``pps_available`` reads 1, and ``n_alarms``, ``n_external_timestamps``,
  ``n_periodic_outputs`` and ``n_pins`` read 0. The character device
  ``/dev/ptp<N>`` serves ``clock_gettime()``, ``clock_settime()`` and
  ``clock_adjtime()`` on the clock (see "The clock"); ``N`` is the index that
  ``ethtool -T`` reports.
- The pulse per second. ``PTP_ENABLE_PPS`` on ``/dev/ptp<N>``, or writing 1 to
  ``/sys/class/ptp/ptp<N>/pps_enable``, enables the source. Each pulse is an
  event of the PPS source ``ptp<N>`` of the kernel's PPS subsystem, which
  stamps it with the system clock when the interrupt handler runs (see "The
  once-per-second interrupt").
- ``ethtool -T sfp``, or ``pon``, lists the capabilities that
  ``rtl9602c_get_ts_info()`` reports: software transmit and receive stamping,
  software system clock, hardware receive stamping, raw hardware clock, the
  PHC index, the transmit type off and the receive filters none and PTP v2
  event.
- ``SIOCSHWTSTAMP`` and ``SIOCGHWTSTAMP``, or the ``tsconfig`` request of the
  ethtool netlink interface, on ``sfp`` or ``pon``, set and read the
  timestamping configuration (see "Receive timestamps" and "Transmit
  timestamps").
- ``SO_TIMESTAMPING`` with ``SOF_TIMESTAMPING_RX_HARDWARE`` and
  ``SOF_TIMESTAMPING_RAW_HARDWARE`` on a socket of ``sfp`` or ``pon`` returns
  the hardware timestamp of a received PTP message.
- Three debugfs views in ``/sys/kernel/debug/rtl9602c/``. ``ptpstat`` and
  ``todstat`` (mode 0400) are created by ``rtl9602c-ptp.c``; ``hsd`` (mode
  0600, because a write changes the capture's setting) is created by
  ``rtl9602c-offload.c``. Reading ``ptpstat`` clears the per-port transmit
  indications.
- The kernel log. At setup ``rtl9602c-switch: no PTP clock: <error>`` when the
  registration of the clock fails. The conduit logs
  ``ptp tag reason <reason>, protocol <byte>: <tag>``, the 16 bytes of the tag
  in hex, the first time it sees each reason value in a timestamp tag.

How to read the fact tables
===========================

Each block below has a short account of what the hardware does and what the
driver does with it, followed by fact tables with three columns: *Fact*, *Tag*
and *Checked by*. The tag says how the fact is established. **M**, measured on
the board (the DFP-34X-2C3): *Checked by* gives the command or observation,
through the driver's own interfaces (the PHC, ``SO_TIMESTAMPING``, the kernel
log, the debugfs views ``ptpstat``, ``todstat`` and ``hsd``), that shows it.
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
   * - The frequency model is the driver's own: ``adjfine`` writes the word
       ``2^27 + 2^27 * scaled_ppm / (10^6 * 2^16)``, where ``scaled_ppm`` is
       the requested offset in parts per million with a 16-bit fraction,
       clamped to 1 ... ``0x0fffffff``. It takes a larger word to run the
       clock faster, in proportion. ``max_adj`` is 500,000,000 ppb, which keeps
       the word between 2^26 and 3 x 2^26, well inside the field.
     - I
     - Neither the direction nor the proportion is measured. Setting a known
       frequency offset through the PHC (``clock_adjtime()`` with
       ``ADJ_FREQUENCY``) and timing the PHC against ``CLOCK_MONOTONIC`` over
       minutes, at two offsets of each sign, would settle it. Load-bearing for
       any servo (``ptp4l``, ``phc2sys``) that steers this clock.
   * - Reading the time is a latch command followed by three reads, and a step
       is three writes followed by a command. ``ptp_lock`` serialises
       ``gettime64``, ``adjtime``, ``adjfine`` and the clock read of the
       ``hsd`` view, so that two callers cannot hand each other's capture
       back; ``settime64`` takes it once for its read and once for its step.
     - T
     - The PTP hardware clock reads and steers.

Reading the time, ``rtl9602c_ptp_gettime()``, in order:

1. Take ``ptp_lock``. Purpose: the latch and the reads are one operation. (T)
2. Set ``RTL9602C_PTP_CLK_CTRL_LATCH`` by read-modify-write. Purpose: capture
   the running clock into both time registers as one instant. (T)
3. Read both words of ``RTL9602C_PTP_CLK_SEC`` and assemble 48 bits. Purpose:
   the seconds. (M)
4. Read ``RTL9602C_PTP_CLK_NSEC``; the nanoseconds are the tick field times 8.
   Purpose: the nanoseconds. (M)
5. Release the lock and keep the seconds in ``ptp_sec_seen``. Purpose: they
   complete the receive stamps (see "Receive timestamps"). (I)

The ``hsd`` view reads the clock with steps 1 to 4 and does not keep the
seconds, so a read through it does not refresh the seconds that step 5 keeps.

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
     - I
     - Not measured. Reading the PHC in the handler across a step would settle
       it: the pulses should follow the step and fall at .000000000 of the
       PHC. Load-bearing for PPS consumers.
   * - The enable is a read-modify-write of ``RTL9602C_IRQ_ENABLE``, which the
       interrupt handler and its re-arm work also modify, with no lock shared
       between them; a change made by one of them at the same moment can be
       lost.
     - I
     - Not observed. Toggling PPS while a masked source is being re-armed,
       then reading ``armed`` in ``ptpstat``, would show it.

Receive timestamps
==================

The switch stamps a PTP message with its own clock as the message goes up to
the CPU port, and writes the time into a 16-byte tag in the frame. This tag is
separate from the CPU tag, which travels in the descriptors of the CPU port's
DMA engine. The conduit takes the tag off before DSA sees the frame and keeps
its time as the frame's hardware timestamp (``skb_hwtstamps()``); the
conduit's side is described in
Documentation/networking/device_drivers/ethernet/realtek/rtl9602c.rst. The tag
holds only the low 16 bits of the seconds, so the DSA operation
``port_rxtstamp``, ``rtl9602c_port_rxtstamp()``, completes the seconds from the
last read of the clock.

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
       22 and 23, the low 16 bits of the PTP clock's seconds, big-endian
       (``RTL9602C_PTP_TAG_SEC_OFFSET``); 24 to 27, the nanoseconds,
       big-endian, in units of 1 ns rather than the 8 ns ticks of the clock
       registers (``RTL9602C_PTP_TAG_NSEC_OFFSET``). The tag length is
       ``RTL9602C_PTP_TAG_LEN``, 16.
     - M
     - ``SO_TIMESTAMPING`` on a packet socket stamped Syncs sent 250 ms apart
       250.221 ms and 250.177 ms apart; the seconds and nanoseconds equal the
       egress capture of the same frame (see the next row).
   * - The stamp is the switch's PTP clock at the frame: the egress capture of
       the same frame on its way to the CPU (see "The egress capture") holds
       the same instant to the nanosecond, with all 48 bits of seconds, while
       the frame's software stamp read 201.8 s.
     - M
     - ``SO_TIMESTAMPING`` gave 20,323.098509680 s, and the ``hsd`` view the
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

1. A frame longer than 16 bytes with ``0x8899`` at offset 12: read the seconds
   (offset 22, 16 bits) and the nanoseconds (offset 24, 32 bits), and log the
   reason byte once per value. Purpose: the tag holds the time of the frame.
   (M)
2. Set the frame's hardware timestamp to those seconds and nanoseconds.
   Purpose: the time travels with the frame. (M)
3. Move the 12 address bytes 16 bytes forward and pull 16 bytes. Purpose: the
   frame reaches DSA as it was sent, with its own EtherType at offset 12. (M)
4. DSA classifies the frame as PTP and calls ``port_rxtstamp``, which
   completes the seconds. Purpose: a stamp with all its seconds. (I)

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
   * - ``rtl9602c_port_rxtstamp()`` completes the 16 bits of seconds from the
       seconds of the last clock read (``ptp_sec_seen``, set by every
       ``gettime64``), choosing the 65,536-second window that puts the stamp
       nearest to them (a difference over 32,768 s moves it one window), and
       delivers the frame at once (it returns false). The result is right
       while the PHC has been read within about nine hours of the frame. The
       PHC's auxiliary work (``do_aux_work``) reads the clock at registration
       and then once a minute, so the seconds are never older than a minute,
       whether or not a PTP daemon reads the clock.
     - I
     - The window arithmetic has run only with stamps below 65,536 s; the
       auxiliary work is read from the code. A frame received with the clock's
       low 16 bits of seconds above 32,768 and no other reader would show it.
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
       1588-2008 6.4: Sync, Delay_Req, Pdelay_Req, Pdelay_Resp; Annex F). Only
       Sync over Ethernet has been seen tagged.
     - I
     - Sending each event message over Ethernet from the host and checking
       ``SO_TIMESTAMPING`` for each would settle it. Load-bearing for the
       filter the driver advertises.
   * - The switch tags its PTP messages whatever the filter, so under
       ``HWTSTAMP_FILTER_NONE`` ``rtl9602c_port_rxtstamp()`` takes the stamp
       off the frame, and a port that nothing has configured delivers none.
     - T
     - Syncs from the host to the PTP group address, received on ``sfp``
       through a packet socket with ``SO_TIMESTAMPING``, carried no hardware
       timestamp under the filter none and one under the layer-2 filter.

The receive capture register
----------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_PTP_RX_TSTAMP`` (``0x0111e0``, two words) is a receive
       capture that the driver does not use for timestamps: it stayed zero
       with Syncs arriving, the port enable on or off and every ingress
       action. ``ptpstat`` prints it (``capture``).
     - M
     - ``ptpstat`` with Syncs arriving under each setting.
   * - As ``ptpstat`` decodes it: seconds bits 2:0 in the first word
       (``RTL9602C_PTP_RX_TSTAMP_SEC``), nanoseconds in 8 ns ticks at bits 29:3
       of the second (``RTL9602C_PTP_RX_TSTAMP_NSEC``).
     - I
     - Never seen non-zero; a capture that changes would settle it. Debugfs
       only.

Transmit timestamps
===================

The switch produces no departure time for a PTP message that the CPU sends.
It has a per-port indication that a PTP message left, and nothing that says
when. The driver therefore offers ``HWTSTAMP_TX_OFF`` only: ``get_ts_info``
lists no other transmit type, and ``rtl9602c_port_hwtstamp_set()`` refuses any
other with ``-ERANGE`` and the message "transmit timestamping is not
implemented" in the netlink extended ack.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - No departure time is produced for a PTP message the CPU sends: with the
       egress action for Sync at each of its four values, the per-port enable
       on all ports, the destination given or left to the switch's lookup,
       and the PTP multicast destination, no copy came back to the CPU and the
       egress capture of the frame had its PTP flag set and a zero time. The
       driver therefore offers only ``HWTSTAMP_TX_OFF`` and answers any other
       ``tx_type`` with ``-ERANGE`` and a message.
     - M
     - ``hsd``, ``ptpstat`` and the conduit's receive log, for each setting.
   * - Without a transmit stamp a port can timestamp the Syncs it receives but
       cannot time its own Delay_Req, which the delay request-response
       mechanism needs: it can listen, not synchronise. ``get_ts_info``
       reports software transmit and receive stamping, hardware receive and
       raw hardware stamps, and the PHC index (-1 when no PHC is registered).
     - P
     - IEEE 1588-2008, 11.3, for the mechanism; the capabilities are the
       driver's own report.
   * - A PTP message arriving from the host goes to the CPU port and to no
       other port. With the host on port 0 as the only PTP partner, no PTP
       message left by a wire port, so what the egress actions do to a message
       the switch forwards out of one is untested.
     - M
     - The egress capture narrowed to each port in turn caught the Sync only on
       port 3, the CPU port; destination 01-1B-19-00-00-00 (IEEE 1588-2008,
       Annex F).
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

The egress capture
==================

A second instrument holds the descriptor of the last frame that left by the
ports its configuration names. The descriptor of a PTP frame carries the PTP
fields of the frame: a flag that the frame is a PTP message, and the time the
switch stamped it with. For a frame on its way to the CPU that time is the
receive stamp, so the capture is the means of checking the tag's time. The
instrument is read and armed through the debugfs view ``hsd`` of
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
       (``0x02a0b4``): one frame, the most recent, not a queue. The ``hsd``
       view prints the words, the filter, the decoded PTP fields and the
       clock; a write to it stores the written value in
       ``RTL9602C_PKTDBG_CFG``.
     - M
     - ``cat /sys/kernel/debug/rtl9602c/hsd`` after PTP frames; then
       ``echo <word> > /sys/kernel/debug/rtl9602c/hsd`` and a second read.
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
     - ``hsd``: a configuration written through the view read back with the bit
       clear, and with it set once traffic had left by the selected port.
   * - Printed by ``hsd`` but not interpreted: a 2-bit action at bits 180:179
       (``RTL9602C_PKTDBG_PTP_ACTION_LSB``,
       ``RTL9602C_PKTDBG_PTP_ACTION_BITS``), a 7-bit identifier at bits
       178:172 (``RTL9602C_PKTDBG_PTP_ID_LSB``,
       ``RTL9602C_PKTDBG_PTP_ID_BITS``), and in ``RTL9602C_PKTDBG_CFG``
       ``RTL9602C_PKTDBG_CFG_MODE`` (bits 7:6) and
       ``RTL9602C_PKTDBG_CFG_REASON`` (bits 15:8). Every PTP capture seen had
       action 0 and identifier 1.
     - I
     - The meanings are unverified; debugfs only.

Per-port configuration and message actions
==========================================

The switch has a PTP enable for each port, a transparent-clock bit for each
port, and an ingress and an egress action for each of ten message types. The
driver never writes any of them, and the receive stamping does not depend on
them (see "Receive timestamps"); the ``ptpstat`` view prints them. The
message types are numbered in the order of the action registers, which is not
the ``messageType`` field of the PTP header (IEEE 1588-2008, clause 13.3.2.2).

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_PTP_PORT_EN`` = ``0x0111e8`` + 4 x port, bit 0: a per-port
       PTP enable. The driver never writes it; ``ptpstat`` prints it
       (``enabled``). Receive stamping does not depend on it.
     - M
     - Set on every port and read back per port in ``ptpstat`` at the 4-byte
       stride.
   * - ``RTL9602C_PTP_ACTION_INGRESS`` (``0x0111d8``) and
       ``RTL9602C_PTP_ACTION_EGRESS`` (``0x0111dc``) hold a 2-bit action
       (``RTL9602C_PTP_ACTION_MASK``) per message type, type n at bits
       2n+1:2n, for ``RTL9602C_PTP_MSG_TYPES`` = 10 types. Type 0 is Sync: its
       ingress action at 3 changed the tag's reason byte from 240 to 241. The
       driver never writes these registers; ``ptpstat`` prints both, per type.
     - M
     - ``ptpstat`` and the kernel log, with the ingress action for Sync at 0
       and at 3.
   * - Types 1 to 9 as ``ptpstat`` names them (``delay_req``, ``pdelay_req``,
       ``pdelay_resp``, ``follow_up``, ``delay_resp``, ``pdelay_resp_fup``,
       ``announce``, ``sync_1step``, ``pdelay_req_1step``), and what each
       action value does.
     - I
     - Debugfs only. Setting the ingress action of a type to 3 and sending that
       message would settle it per type: the reason byte should change as it
       did for Sync.
   * - ``RTL9602C_PTP_TRANSPARENT_PORTS`` (``0x0111d4``): one bit per port,
       read 0 on every port; ``ptpstat`` prints it (``transparent``). What a
       set bit does is not exercised.
     - I
     - Debugfs only. Setting a bit and checking the correction field of a Sync
       forwarded between two wire ports would settle it.
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
   * - ``capture <s>.<ns>  (raw <word> <word>)``
     - The receive capture register.
     - ``RTL9602C_PTP_RX_TSTAMP``

The PON time of day
===================

Beside the clock the switch has a second time value and an enable for it. The
driver writes neither. They are taken to hold the time of day that an ONU
learns from the OLT, in the layout of the clock.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``RTL9602C_PTP_PON_TIME_SEC`` (``0x01b024``) and
       ``RTL9602C_PTP_PON_TIME_NSEC`` (``0x01b02c``) are a second time value
       beside the clock, and ``RTL9602C_PTP_CLK_CTRL_PON_TIME_EN`` (bit 2 of
       ``RTL9602C_PTP_CLK_CTRL``) is its enable. Nothing in the driver writes
       them; they read zero and the enable reads 0.
     - M
     - ``ptpstat``: the line ``pon tod 0.000000000  enabled 0``.
   * - They are meant for the time of day an ONU learns from the OLT (ITU-T
       G.988 9.12.2, OLT-G "Time of day information": a GEM superframe number
       and an IEEE 1588-2008 5.3.3 timestamp), in the layouts of the clock
       (seconds over two words, nanoseconds in 8 ns ticks), which is how
       ``ptpstat`` decodes them.
     - I
     - Debugfs only. Writing a time into them and observing what the hardware
       does with it would settle it.

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

- A port cannot stamp what it sends. A request for any transmit type but
  ``HWTSTAMP_TX_OFF`` is refused with ``-ERANGE``. A port can stamp the Syncs
  it receives but cannot time its own Delay_Req, which the delay
  request-response mechanism needs: it can listen, not synchronise (see
  "Transmit timestamps").
- A PTP message addressed to this device's own MAC address arrives with only
  the second half of the tag: 8 bytes, two zero bytes, the low 16 bits of the
  seconds and the nanoseconds, between the source address and the message's
  EtherType. The conduit does not recognise that, so the frame reaches the
  stack 8 bytes longer, with EtherType 0, and no PTP stack sees it as PTP. A
  message to a group address, the PTP one or broadcast, arrives with the whole
  tag and is stamped. The same held for a message in a VLAN.
- Only the layer-2 PTP version 2 receive filters are taken, answered as
  ``HWTSTAMP_FILTER_PTP_V2_L2_EVENT``. A request for every frame, for UDP, for
  PTP version 1 or for NTP is refused with ``-ERANGE`` and leaves the port's
  configuration as it was. Only a Sync over Ethernet has been seen tagged (see
  "Receive timestamps").
- The clock and the completion of receive stamps depend on the kernel
  configuration. Without ``CONFIG_PTP_1588_CLOCK`` no PHC is registered
  (``ethtool -T`` reports no PHC index) and nothing reads the clock. DSA hands
  a received frame to ``rtl9602c_port_rxtstamp()`` only when the kernel's PTP
  classification finds a PTP message in it, and that classification,
  ``CONFIG_NET_PTP_CLASSIFY``, is selected by ``CONFIG_PTP_1588_CLOCK``.
  Without it the function is never called, and the hardware timestamp of a
  received PTP message stays the tag's time as it stands: the low 16 bits of
  the seconds and the nanoseconds. ``rtl9602c_defconfig`` does not name the
  clock, but the symbol follows ``CONFIG_ETHERNET``, so a kernel configured
  from the defconfig has the clock and the classification.
- The conduit takes every frame longer than 16 bytes that carries EtherType
  ``0x8899`` at offset 12 for a tagged PTP message, whatever its protocol byte.
  Other frames use that EtherType, among them the switch's loop-detection
  probes; one that reached the CPU would lose 16 bytes and gain a timestamp.
  Only protocol 4 has been seen at the CPU.

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
   * - A larger frequency word runs the clock faster, in proportion to the
       word ("The clock"). Every frequency adjustment, and so any servo that
       steers the clock, rests on it.
     - A known frequency offset set through the PHC and the PHC timed against
       ``CLOCK_MONOTONIC`` over minutes, at two offsets of each sign.
   * - The pulse of the once-per-second source falls on the PTP clock's second
       boundary and moves with its steps and frequency changes ("The
       once-per-second interrupt"). PPS consumers rely on it.
     - Reading the PHC in the handler across a step: the pulses should follow
       the step and fall at .000000000 of the PHC.
   * - The interrupt enable is changed by an unlocked read-modify-write that
       the interrupt handler also performs ("The once-per-second interrupt").
     - Toggling PPS while a masked source is being re-armed, then reading
       ``armed`` in ``ptpstat``.
   * - Completing the tag's 16 bits of seconds from the seconds of the last
       clock read gives the right stamp ("Taking the tag off and completing
       the seconds"). It is wrong when nothing has read the PHC for more than
       about nine hours.
     - A frame received more than 32,768 s after the last PHC read, and one
       received with the clock's low 16 bits of seconds above 32,768 and no
       reader.
   * - The advertised receive filter covers every event message and PTP over
       UDP ("The filter"). Only Sync over Ethernet has been seen tagged.
     - Each event message over Ethernet, and a Sync over UDP to port 319, from
       the host, with ``SO_TIMESTAMPING`` checked for each.

Questions the code does not depend on
-------------------------------------

All but the last concern values that only the debugfs views print.

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Question
     - What would settle it
   * - Whether the receive capture register has the layout ``ptpstat`` decodes
       ("The receive capture register").
     - A capture that changes.
   * - What the action, identifier, mode and reason fields of the egress
       capture mean ("The egress capture").
     - A PTP frame forwarded out of a wire port under a known action; a
       capture with a reason that only some of the traffic carries.
   * - The names of message types 1 to 9 and what each action value does
       ("Per-port configuration and message actions").
     - Setting the ingress action of a type to 3 and sending that message: the
       reason byte should change as it did for Sync.
   * - What a transparent-clock bit does ("Per-port configuration and message
       actions").
     - Setting a bit and checking the correction field of a Sync forwarded
       between two wire ports.
   * - Whether the PON time-of-day registers hold the time of day of the OLT
       ("The PON time of day").
     - Writing a time into them and observing what the hardware does with it.
   * - The baud rate of the time-of-day transmitter and the clock its divisor
       divides ("The time-of-day transmitter").
     - A capture of the serial output.
   * - The fields and units of the pulse width, the delay to the message, the
       message length and the message source ("The time-of-day transmitter").
     - An oscilloscope on the output.
   * - The layout of the software message bytes ("The time-of-day
       transmitter").
     - Writing a known message and capturing the serial line.
