.. SPDX-License-Identifier: GPL-2.0-only

=================================
Realtek RTL9602C optics interface
=================================

Overview
========

The transmitter and the receiver of the stick are driven by a laser-driver IC
that sits between the SoC and the fibre. The SoC reaches it over its I2C
master 1. The IC serves the SFF-8472 management interface of an SFP module, the
page A0h at the 7-bit address ``0x50`` and the page A2h at ``0x51``, and it
keeps its configuration and calibration in an EEPROM of its own. Two more
connections run from the SoC to the optics: the hard transmit-disable input,
TX_DISABLE, taken to be SoC GPIO 13, and the burst enable, which the GPON MAC
keys for each upstream burst.

The optics have no driver of their own. The code is part of the switch driver,
``rtl9602c``, in ``rtl9602c-optics.c``, and it does five things:

- It moves bytes over the I2C master, one per transfer or the two of a
  diagnostic word together, through the SMBus adapter ``rtl9602c i2c`` that the
  master's own driver, ``i2c-rtl9602c``, registers.
- It publishes the five measurements of the SFF-8472 diagnostics, which are the
  temperature, the supply voltage, the transmitter bias, the transmitted power
  and the received power, as the hwmon device ``rtl9602c_optics``.
- It drives TX_DISABLE for the GPON state machine: asserted below O3, released
  from O3 on.
- It turns the laser driver's automatic extinction-ratio loop off at every PON
  bring-up, so that the modulation of the laser runs from the value the module
  loads at power-up. This is the modulation hold.
- It offers two debugfs views that only read, ``a2`` and ``eeblocks``, a
  third, ``tables``, that selects each table of the upper page to read it, and,
  with ``CONFIG_NET_DSA_RTL9602C_OPTICS_STORE``, an entry, ``t2write``, that
  stores a byte of the configuration table in the module's EEPROM, or resets
  the laser driver, when root writes to it.

The driver writes nothing to the laser driver at probe or at setup. Its only
writes are those of the modulation hold, at PON bring-up, those a read of the
``tables`` view makes and those a write to ``t2write`` asks for, and
``CONFIG_RTL9602C_OPTICS_RO`` removes all three. So does a module that does not
declare itself as one the driver was measured on (see "The modules the driver
writes"). The connections::

    RTL9602C                               laser-driver IC
    +---------------+                      +------------------------+
    | I2C master 1  |==== SCL, SDA ========| A0h (0x50), A2h (0x51) |
    |               |                      | configuration EEPROM   |
    | GPIO 13       |--- TX_DISABLE ------>|                        |
    |               |                      |   drives the           |
    | GPON MAC      |--- burst enable ---->|   transmitter and      |
    |               |                      |   the receiver         |
    +---------------+                      +------------------------+

The GPON MAC is described in
Documentation/networking/device_drivers/pon/rtl9602c-gpon.rst and the PON MAC,
together with the overview of the set, in
Documentation/networking/device_drivers/pon/rtl9602c.rst. The switch is
described in Documentation/networking/dsa/rtl9602c.rst, the PON class in
Documentation/networking/pon.rst, and the GPIO controller and the pad control in
Documentation/arch/mips/rtl9602c.rst.

Configuration
-------------

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Symbol
     - Effect
   * - ``CONFIG_NET_DSA_RTL9602C``
     - The switch driver. ``rtl9602c-optics.o`` is one of the objects that make
       up ``rtl9602c.o``, so the optics are built with the switch driver and
       have no symbol of their own.
   * - ``CONFIG_RTL9602C_OPTICS_RO``
     - Bool, "RTL9602C: never write to the optics part"; depends on
       ``NET_DSA_RTL9602C``; off unless set. With it the modulation hold
       returns at once, the driver itself writes nothing to the laser driver,
       and the I2C adapter refuses writes (see "The read-only build").
   * - ``CONFIG_I2C``
     - The I2C core, which the optics code registers the master with. The
       Kconfig entry of the switch driver depends on it.
   * - ``CONFIG_I2C_CHARDEV``
     - The ``/dev/i2c-N`` device files, which give user space the adapter.
   * - ``CONFIG_HWMON``
     - Tristate, on by default. The hwmon device ``rtl9602c_optics`` is
       registered only with it; without it the registration is a stub that
       returns 0.
   * - ``CONFIG_DEBUG_FS``
     - The debugfs views ``a2``, ``eeblocks`` and ``tables`` and the entry
       ``t2write`` exist only with it, ``tables`` only without
       ``CONFIG_RTL9602C_OPTICS_RO``.
   * - ``CONFIG_NET_DSA_RTL9602C_OPTICS_STORE``
     - Boolean, off by default, and only without ``CONFIG_RTL9602C_OPTICS_RO``.
       The entry ``t2write`` exists only with it. The bytes it stores outlive
       every power cycle, among them the transmitter's rogue ONU controls,
       which protect the other ONUs of the PON: the option is for test images.
   * - ``CONFIG_GPIOLIB``, ``CONFIG_GPIO_REALTEK_OTTO``
     - The GPIO controller that serves the TX_DISABLE line
       (Documentation/arch/mips/rtl9602c.rst).

``rtl9602c_defconfig`` (``arch/mips/configs/``) sets ``CONFIG_I2C=y``,
``CONFIG_GPIOLIB=y``, ``CONFIG_GPIO_REALTEK_OTTO=y`` and ``CONFIG_DEBUG_FS=y``;
``CONFIG_HWMON`` is on by default, and the defconfig sets neither
``CONFIG_I2C_CHARDEV`` nor ``CONFIG_RTL9602C_OPTICS_RO``.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - ``CONFIG_NET_DSA_RTL9602C`` builds the optics code into the switch
       driver: the object ``rtl9602c-optics.o`` is linked into ``rtl9602c.o``.
     - T
     - The board boots to a shell with the switch driver probed and the optics
       diagnostics readable.

Device tree and binding
-----------------------

The optics have no device tree node of their own. Their one property,
``tx-disable-gpios``, is on the ``switch`` node (compatible
``realtek,rtl9602c-switch``) of ``arch/mips/boot/dts/realtek/rtl9602c.dtsi``,
and it names a line of the GPIO controller node ``gpio0``::

    switch: switch@1b000000 {
            compatible = "realtek,rtl9602c-switch";
            ...
            tx-disable-gpios = <&gpio0 13 GPIO_ACTIVE_HIGH>;
            ...
    };

The binding is
``Documentation/devicetree/bindings/net/dsa/realtek,rtl9602c-switch.yaml``. It
describes ``tx-disable-gpios`` as the hard transmit-disable input of the laser
driver, with one entry and not required: while the input is asserted the
transmitter emits no light. ``rtl9602c_probe()`` takes the line with
``devm_gpiod_get_optional()`` under the name ``tx-disable``, already driven to
its asserted level (``GPIOD_OUT_HIGH``), so that nothing lases before the PON
path allows it. Without the property every TX_DISABLE step is skipped. GPIO
13, active high, is the laser driver's disable input: driving it high stops the
light (measured, see "TX_DISABLE").

There is no node for the I2C master or for the laser driver. The master's driver
registers the adapter without a node, and the devices at ``0x50`` and ``0x51``
are not described. The GPIO controller, ``gpio@18003300`` (compatible
``realtek,rtl9602c-gpio`` and ``realtek,otto-gpio``), and the pad-function
registers are described in Documentation/arch/mips/rtl9602c.rst.

Where the code is
-----------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - File
     - Contents
   * - ``drivers/net/dsa/realtek/rtl9602c-optics.c``
     - The byte transfers through the SMBus adapter and their locks
       (``rtl9602c_i2c_read_byte()``, ``rtl9602c_i2c_lock()``), the hand-over of
       the master to its driver (``rtl9602c_optics_i2c_add()``), the numbering of the part's registers (``rtl9602c_l_read()``), the TX_DISABLE
       drive (``rtl9602c_l_txdis_gpio()``), the hwmon device
       (``rtl9602c_optics_hwmon_register()``), the modulation hold
       (``rtl9602c_optics_hold_modulation()``) with the
       ``RTL9602C_OPTICS_TBL2_*`` macros of the configuration table it writes,
       the transmit power levels (``rtl9602c_optics_level_register()``,
       ``rtl9602c_optics_level_work()``), and the debugfs views
       (``rtl9602c_optics_debugfs_init()``).
   * - ``drivers/net/dsa/realtek/rtl9602c.h``
     - The byte numbers of the diagnostics (``RTL9602C_OPTICS_A2_*``), the
       members ``tx_disable``, ``i2c`` and ``optics_lock`` of ``struct
       rtl9602c_priv``, and the prototypes of the calls above.
   * - ``drivers/i2c/busses/i2c-rtl9602c.c``
     - The I2C master: its registers (``RTL9602C_I2C_*``, ``RTL9602C_PADMUX_*``)
       and the SMBus adapter (Documentation/i2c/busses/i2c-rtl9602c.rst).
   * - ``include/linux/platform_data/i2c-rtl9602c.h``
     - What the switch driver gives the master's driver, and what it gets back.
   * - ``drivers/net/dsa/realtek/rtl9602c-main.c``
     - ``rtl9602c_probe()`` takes the ``tx-disable`` line and prepares the
       transmit power levels. ``rtl9602c_setup()`` creates the debugfs
       directory with the optics views, registers the I2C adapter and then the
       hwmon device (see "Setup, in order").
   * - ``drivers/net/pon/rtl9602c/rtl9602c-gpon.c``
     - ``rtl9602c_gpon_apply_state()`` drives TX_DISABLE at every change of the
       ONU state, ``rtl9602c_gpon_bringup()`` calls the modulation hold after
       the MAC reset, and Change_Power_Level and Upstream_Overhead ask for
       transmit power levels. It reaches the optics through the host operations
       ``tx_disable``, ``hold_modulation`` and ``power_level_changed``, and the
       optics ask which level it wants, and tell it what they set, through the
       PON operations ``power_mode`` and ``power_applied``.
   * - ``drivers/net/dsa/realtek/Kconfig``, ``Makefile``
     - ``CONFIG_RTL9602C_OPTICS_RO`` and the list of objects of ``rtl9602c.o``.
   * - ``arch/mips/boot/dts/realtek/rtl9602c.dtsi``
     - ``tx-disable-gpios`` on the ``switch`` node.
   * - ``realtek,rtl9602c-switch.yaml`` in
       ``Documentation/devicetree/bindings/net/dsa/``
     - The binding of that property.

Interfaces to user space
------------------------

- The hwmon device ``rtl9602c_optics``, with the five measurements of the
  diagnostics. Its directory is ``/sys/class/hwmon/hwmonN``, where N follows
  the order of registration, and it is found by its ``name`` file::

      # grep -l rtl9602c_optics /sys/class/hwmon/hwmon*/name
      /sys/class/hwmon/hwmon0/name

  The attributes are ``temp1_input``, ``in0_input``, ``curr1_input``,
  ``power1_input`` and ``power2_input``, each with a ``_label`` file and the
  four thresholds ``_lcrit``, ``_min``, ``_max`` and ``_crit``, all read-only
  (mode ``0444``). Their units are those of the hwmon interface: millidegree
  Celsius, millivolt, milliampere and microwatt (see "Diagnostics"). Reading
  a value or a threshold takes one two-byte transfer on the I2C master;
  reading a label takes none.
- The I2C adapter ``rtl9602c i2c``, whose name is in
  ``/sys/class/i2c-adapter/i2c-N/name``. It offers SMBus byte-data transfers
  and word-data reads (Documentation/i2c/busses/i2c-rtl9602c.rst). With ``CONFIG_I2C_CHARDEV`` it has a file
  ``/dev/i2c-N``, and a write through it can change the module for good (see
  "The management interface").
- ``/sys/kernel/debug/rtl9602c/a2``, ``/sys/kernel/debug/rtl9602c/eeblocks``
  and ``/sys/kernel/debug/rtl9602c/tables``, views that read the laser driver
  one byte at a time and print it (see "The debugfs views"). They have mode
  ``0400`` and exist only with ``CONFIG_DEBUG_FS``.
- ``/sys/kernel/debug/rtl9602c/t2write``, mode ``0200``, which stores a byte
  of table 2 in the module's EEPROM or resets the laser driver (see "Storing
  table 2 bytes"), only with ``CONFIG_NET_DSA_RTL9602C_OPTICS_STORE``.
- The TX_DISABLE line. It has no file of its own: it is line 13 of the GPIO
  bank, requested under the consumer name ``tx-disable``, and
  ``/sys/kernel/debug/gpio`` shows its direction and level (high while
  TX_DISABLE is asserted). Its level follows the ONU state, so
  ``ip link set pon up`` and ``ip link set pon down`` are the commands that
  move it (see "TX_DISABLE").
- ``ip link set pon up`` also runs the modulation hold, unless
  ``CONFIG_RTL9602C_OPTICS_RO`` is set or the module is not one the driver
  writes (see "The modulation hold" and "The modules the driver writes").
- The kernel log lines of the optics, which begin with ``rtl9602c-switch:``
  (see "Logging").
- The optics have no module parameters, devlink parameters or ethtool
  statistics.

First checks on a board
-----------------------

1. Read ``/sys/kernel/debug/rtl9602c/eeblocks``. The blocks for ``0x50`` and
   ``0x51`` report no unanswered read, and A0h byte 0 reads ``03``, the SFP
   identifier. The blocks for ``0x52`` and ``0x53`` read ``ff`` throughout, with
   at most one unanswered read in 256 (see "The debugfs views").
2. Look for the hwmon device. If it is missing, either ``CONFIG_HWMON`` is off
   or the kernel log carries ``rtl9602c-switch: no optical diagnostics`` (see
   "Logging"), and the ``eeblocks`` view shows whether anything answers.
3. Read the five measurements (see "The hwmon device"). Below O3, with
   TX_DISABLE asserted, the bias and the transmitted power read 0; in O5 they
   read the burst values. The received power shows whether light from the OLT
   reaches the receiver (see "Loss of signal and received power").

How to read the fact tables
===========================

Each block below has a short account of what the hardware does and what the
driver does with it, followed by fact tables with three columns: *Fact*, *Tag*
and *Checked by*. The tag says how the fact is established. **M**, measured on
the board (the DFP-34X-2C3): *Checked by* gives the command or observation,
through the driver's own interfaces (the kernel log, hwmon, debugfs), that shows
it. **T**, tested by function: the driver depends on the fact, and the named
behaviour fails if it is wrong. **P**, public standard: *Checked by* names the
document and the clause. **I**, inferred, not verified: the fact is an
assumption, and *Checked by* says what would confirm or refute it. The last
chapter collects these facts and says which of them the driver depends on.

A row with two tags is established in two ways, or in two parts; *Checked by*
says which part carries which tag. Register offsets are from the base of the
switch register window, physical ``0x1b000000``. The driver reaches the I2C
registers with 32-bit accesses, and a byte travels in the low 8 bits. Bytes of
the laser driver are named by their SFF-8472 page and number: A0h byte 40 is
byte 40 of the page at the 7-bit address ``0x50``, and A2h byte 96 is byte 96 of
the page at ``0x51``. The behaviours that a tested fact depends on are named in
words: the optics diagnostics read, and the ONU reaches O5 on an OLT.

The I2C master
==============

The SoC has two I2C masters with the same set of registers, and the laser
driver answers on master 1 only. The master has a driver of its own,
``i2c-rtl9602c`` (Documentation/i2c/busses/i2c-rtl9602c.rst), which owns its
registers, the pad function that enables it and the order of a transfer, and
registers the SMBus adapter ``rtl9602c i2c``. The switch driver makes the
auxiliary device that driver binds to, with the switch's register window
(``rtl9602c_optics_i2c_add()``), and is told when the adapter appears
(``rtl9602c_optics_adapter()``): that identifies the module and publishes the
diagnostics. The code of this page moves its bytes through the I2C core, with
``__i2c_smbus_xfer()``, in sequences under the locks below.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - Every user of the optics holds the switch's ``optics_lock`` for its
       whole sequence of transfers, and the adapter's bus lock inside it
       (``rtl9602c_i2c_lock()``): a hwmon read, the ``a2``, ``eeblocks`` and
       ``tables`` views, the modulation hold, the transmit power levelling and
       the store view. A transfer of another client of the adapter takes the
       bus lock alone, so none can come between two transfers of a sequence.
       The hwmon core and the I2C core serialise only their own callers, and
       the modulation hold switches the page of A2h 128-255 and puts it back,
       which no other user may see. Without the adapter, the sequence runs under
       ``optics_lock`` and every transfer fails with ``-ENODEV``.
     - I
     - Read from the code; in a kernel with lockdep, the transfer function
       asserts that ``optics_lock`` is held. Reading the hwmon attributes in a
       loop while reading the ``a2`` view would show no mixed bytes; not run.
   * - The adapter passes a write on only for a module the driver identified
       (``rtl9602c_optics_writable()``) and never with
       ``CONFIG_RTL9602C_OPTICS_RO``; otherwise it refuses it with ``-EPERM``.
       The driver's own writes check the same condition first.
     - T
     - The modulation hold writes on the module the driver knows, and on one
       it does not know the hold, the levelling and the store view return
       without a write.
   * - A client of the adapter has not been run: the I2C core can reach the
       laser driver through ``/dev/i2c-N`` with ``CONFIG_I2C_CHARDEV``.
     - I
     - Reading ``0x50`` through the adapter and comparing with the ``eeblocks``
       view would settle it.

The management interface
========================

The laser driver serves the two pages of the management interface of an SFP
module: A0h, the serial ID, at the 7-bit address ``0x50``, and A2h, the
diagnostics and the module's own controls, at ``0x51``. The part runs from the
configuration and calibration in its own EEPROM, and the driver writes nothing
to it at probe. The addresses ``0x52`` and ``0x53`` acknowledge as well, but
every byte reads ``0xff`` there, so the stored configuration cannot be read
through them.

The driver numbers the registers of the part from ``0x000`` to ``0x1ff``, the
way ``rtl9602c_l_read()`` takes them:

.. list-table::
   :header-rows: 1
   :widths: 20 20 60

   * - Number
     - Device
     - Byte
   * - ``0x000`` to ``0x0ff``
     - ``0x50``
     - A0h byte n.
   * - ``0x100`` to ``0x1ff``
     - ``0x51``
     - A2h byte n - ``0x100`` (``RTL9602C_OPTICS_A2_BASE`` = ``0x100``).
   * - ``0x200`` and above
     - none
     - Refused with ``-EINVAL``.

In the rest of this page a byte is named by its page and its byte number, as
SFF-8472 does. The header names two places in A2h:
``RTL9602C_OPTICS_A2_DDM`` (``0x60``, the first diagnostic word) and
``RTL9602C_OPTICS_A2_PASSWORD`` (``0x7b``, byte 123, the first of four bytes
inside the range 120-126 that SFF-8472 leaves to the module maker). The page
select, byte 127, is written as ``0x7f`` in the code.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The laser driver serves the SFF-8472 management interface: A0h at the
       7-bit address ``0x50``, A2h at ``0x51``.
     - P
     - SFF-8472 Rev 12.4 clause 1 and Figure 4-1 (A0h = 1010000x, A2h =
       1010001x).
   * - Both answer on master 1, all 256 bytes each, and A0h byte 0 reads
       ``03h``, the SFP identifier (SFF-8472 clause 5.1).
     - M
     - The ``eeblocks`` view.
   * - ``0x52`` and ``0x53`` acknowledge, but every byte reads ``0xff``: the
       module's stored configuration is not readable through them.
     - M
     - The ``eeblocks`` view: at most one of 256 reads unanswered per address,
       the data ``0xff``.
   * - The driver numbers the part's registers ``0x000`` to ``0x1ff``: ``0x000``
       to ``0x0ff`` is A0h byte n (device ``0x50``), ``0x100`` to ``0x1ff`` is
       A2h byte n - ``0x100`` (device ``0x51``). A higher number is refused with
       ``-EINVAL``.
     - T
     - The optics diagnostics read.
   * - The part runs from the configuration and calibration in its own EEPROM,
       and the driver writes nothing to it at probe: with the modulation hold
       as the only write, it transmits at +2.3 dBm, receives an OLT at -20 dBm,
       and the ONU reaches O5.
     - M
     - The hwmon attributes and the ``a2`` view in O5 on an OLT.
   * - A0h and A2h bytes 0-127 take writes without any password, and what is
       written there survives a power cycle of the module: a write there is a
       permanent change to the module. The driver writes none of it except the
       page select, A2h byte 127, which it writes only when the byte holds
       another value, and, from the ``t2write`` view only, the password bytes,
       123-126, which read 00 whatever is written.
     - M
     - Bytes written to A0h and A2h 0-127 over this bus were still there after
       the module had been without power.
   * - A2h byte 127 selects the page shown at A2h bytes 128-255, and pages
       ``80h`` to ``FFh`` are left to the module maker.
     - P
     - SFF-8472 Rev 12.4 clause 10.3, Table 10-3 and Figure 4-1.
   * - On this module, writing ``02h`` to A2h byte 127 shows the laser driver's
       configuration table at A2h 128-255 (the code's "table 2").
     - T
     - The modulation hold's log after a module power-up, with the text
       ``automatic ER e3 -> e2``, reads that table before and after its write.
   * - The modulation hold writes A2h byte 127 on a PON bring-up only when it
       holds another page than ``02h``, and restores it to the value it found.
       That byte is kept across a power cycle; whether the part skips
       rewriting a byte that already holds the value is not known (see "Known
       limitations").
     - I
     - Bears on EEPROM wear, not on function. The write cycles cannot be
       observed on this board; reading the ``a2`` view ("table select") after a
       module power cycle taken while byte 127 held ``02h`` would at least show
       whether byte 127 is stored.
   * - The laser driver IC is a GN25L95, the name the driver's log messages
       use.
     - I
     - Not verified from anything public; the package marking would settle it.
       Not load-bearing: the driver relies on the measured behaviour in this
       page, not on the name.

What the driver reads and writes of the part:

.. list-table::
   :header-rows: 1
   :widths: 24 38 38

   * - Device and bytes
     - Read by
     - Written by
   * - ``0x50`` (A0h), bytes 0-255
     - The ``eeblocks`` view; bytes 20-35 and 40-55 once at probe (see "The
       modules the driver writes").
     - Nothing in the driver.
   * - ``0x51`` (A2h), bytes 0-127
     - The ``a2`` view; the hwmon device reads bytes 0-39 and 96-105 only.
     - The modulation hold: ``02h`` to byte 127 when it holds another page, and
       byte 127 again to restore it. The ``t2write`` view: ``0xff`` to bytes
       123-126, and its reset key there.
   * - ``0x51`` (A2h), bytes 128-255
     - The ``eeblocks`` view, whichever page byte 127 selects at that moment;
       the modulation hold reads the configuration table there.
     - The modulation hold, in the configuration table with byte 127 at
       ``02h`` (see "The modulation hold").
   * - ``0x52`` and ``0x53``, bytes 0-255
     - The ``eeblocks`` view.
     - Nothing in the driver.

Diagnostics
===========

A2h bytes 96 to 105 hold five 16-bit measurements, most significant byte first.
The module reports them in the units SFF-8472 gives an internally calibrated
module, and the driver applies no calibration. The driver reads each value in
one transfer of two bytes, the most significant byte first, during which the
module does not update it, as SFF-8472 has a measurement read. The hwmon device
converts each value to the unit of the hwmon interface for its type
(``Documentation/ABI/testing/sysfs-class-hwmon``), as ``drivers/net/phy/sfp.c``
does, rounded to the nearest; the ``a2`` view prints the values in the SFF-8472
units.

.. list-table::
   :header-rows: 1
   :widths: 10 18 14 18 12 28

   * - A2h bytes
     - Measurement
     - SFF-8472 unit
     - hwmon attribute
     - Label
     - Conversion
   * - 96-97
     - Temperature, signed
     - 1/256 degC
     - ``temp1_input``
     - ``temperature``
     - Millidegrees = raw x 1000 / 256.
   * - 98-99
     - Supply voltage
     - 100 uV
     - ``in0_input``
     - ``VCC``
     - Millivolts = raw / 10.
   * - 100-101
     - Transmitter bias
     - 2 uA
     - ``curr1_input``
     - ``bias``
     - Milliamperes = raw / 500.
   * - 102-103
     - Transmitted power
     - 0.1 uW
     - ``power1_input``
     - ``TX_power``
     - Microwatts = raw / 10.
   * - 104-105
     - Received power
     - 0.1 uW
     - ``power2_input``
     - ``RX_power``
     - Microwatts = raw / 10.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A2h bytes 96-105 (``RTL9602C_OPTICS_A2_DDM`` = ``0x60`` onward) hold five
       16-bit measurements, most significant byte first: temperature (96),
       supply voltage (98), transmitter bias (100), transmitted power (102) and
       received power (104).
     - P
     - SFF-8472 Rev 12.4 clause 9.8, Table 9-16.
   * - Units, internally calibrated: temperature signed, in 1/256 degC; supply
       voltage in 100 uV; bias in 2 uA; both powers in 0.1 uW. The driver
       converts as ``drivers/net/phy/sfp.c`` does: millidegrees = raw x 1000 /
       256, millivolts = raw / 10, milliamperes = raw / 500, microwatts = raw /
       10, each rounded to the nearest.
     - P
     - SFF-8472 Rev 12.4 clause 9.2, items 1-5.
   * - No calibration is applied, and on this module none is needed: its
       external-calibration constants (A2h 56-91) hold exactly the values
       SFF-8472 gives for an internally calibrated module (Rx_PWR(1) = 1.0;
       Rx_PWR(4), (3), (2) and (0) = 0; every slope 1.0; every offset 0), so the
       internal and the external reading coincide. The driver does not read A0h
       byte 92.
     - M
     - The ``a2`` view, repeated, against SFF-8472 Rev 12.4 clause 9.5,
       Table 9-6.
   * - Bias and transmitted power read 0 while the transmitter is not emitting,
       as below O3, where TX_DISABLE is asserted and no burst is sent; in O5
       they read the burst values (7 mA and 1697 uW in the measurement of the
       hwmon device below). SFF-8472 declares the transmitted power not valid
       while the transmitter is disabled.
     - M
     - The ``a2`` view below O3: bias 0 uA, transmitted power 0.0 uW;
       SFF-8472 Rev 12.4 clause 9.2, item 4.

The hwmon device
----------------

The driver publishes the five measurements as a hwmon device whose attributes
and labels follow those of ``drivers/net/phy/sfp.c``, which does the same for
the SFP modules it manages.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The hwmon device ``rtl9602c_optics`` has ``temp1`` labelled
       ``temperature``, ``in0`` ``VCC``, ``curr1`` ``bias``, ``power1``
       ``TX_power`` and ``power2`` ``RX_power``, the labels of
       ``drivers/net/phy/sfp.c``. It is registered at switch setup if one read
       of the temperature word succeeds; otherwise the driver logs
       ``no optical diagnostics`` and the switch carries on. Without
       ``CONFIG_HWMON`` nothing is registered.
     - M
     - In O5 on an OLT: 46.8 degC, 3.217 V, 7 mA, 1697 uW (+2.3 dBm) and 10 uW,
       the same values the ``a2`` view showed.

To read the five measurements, in the directory of the device::

    # for c in temp1 in0 curr1 power1 power2; do
    >     echo $(cat ${c}_label) $(cat ${c}_input)
    > done

It prints one line per measurement: the label and the value in the hwmon unit
of the table in "Diagnostics".

A2h bytes 0 to 39 hold the module's thresholds for the five measurements, in
the same units: for each in the order of the measurements, four 16-bit words,
the high alarm, low alarm, high warning and low warning thresholds. The device
reports them as ``drivers/net/phy/sfp.c`` reports those of the modules it
manages: the alarm thresholds as ``_crit`` and ``_lcrit``, the warning
thresholds as ``_max`` and ``_min``, converted as the measurements are. It has
no ``_alarm`` attributes: the flag bytes 112 to 117, where a module says which
threshold a measurement has crossed, are not read, and they are visible only
in the hex dump of the ``a2`` view.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - A2h bytes 0-39 (``RTL9602C_OPTICS_A2_LIMIT``) hold four 16-bit
       thresholds per measurement, most significant byte first, in the order
       temperature, supply voltage, bias, transmitted power, received power:
       high alarm, low alarm, high warning, low warning.
     - P
     - SFF-8472 Rev 12.4, its table of the alarm and warning thresholds of
       A2h.
   * - The module fitted here holds: temperature 100, -40, 90 and -20 degC;
       supply voltage 3.6, 3.0, 3.5 and 3.1 V; bias 100, 1, 90 and 1 mA;
       transmitted power 3981, 891, 3548 and 1000 uW (+6.0, -0.5, +5.5 and
       0 dBm); received power 199.5, 1.0, 158.5 and 1.3 uW (-7.0, -30.0, -8.0
       and -28.9 dBm). Its measurements in O5 sit within the warning
       thresholds.
     - M
     - The ``a2`` view in O5 on an OLT: 46 degC, 3.209 V, 6.4 mA, 1671.5 uW
       and 3.0 uW.

The device is registered once, at setup: if the temperature word cannot be
read then, there is no hwmon device until the switch is set up again.

The debugfs views
-----------------

Both views read the part and never write it. They are created in the driver's
debugfs directory, ``/sys/kernel/debug/rtl9602c/``, with mode ``0400``::

    # cat /sys/kernel/debug/rtl9602c/a2
    # cat /sys/kernel/debug/rtl9602c/eeblocks

The ``a2`` view reads A2h bytes 0 to 127 one byte at a time, stopping at the
first failed read with the line ``A2h read failed at 0x<byte>: <error>``, and
prints them as eight rows of sixteen bytes. It then prints the five diagnostic
words in the SFF-8472 units, bytes 123 to 126 and byte 127::

    00: <16 bytes>
    ...
    70: <16 bytes>

    temperature <t> C
    vcc         <v> V
    tx bias     <b> uA
    tx power    <p> uW
    rx power    <r> uW   <- light on the fibre

    password 0x7b-0x7e  <byte 123> <byte 124> <byte 125> <byte 126>
    table select 0x7f = 0x<byte 127>

The ``eeblocks`` view reads all 256 bytes of each of the devices ``0x50`` to
``0x53`` and prints each as a block of sixteen rows, headed by the device, the
place of the block in a 1 KiB image and the number of reads that went
unanswered::

    === i2c 0x50 -- image 0x000 -- <n> of 256 did not answer
    00: <16 bytes>
    ...
    f0: <16 bytes>

The blocks follow for ``0x51`` (image ``0x100``), ``0x52`` (``0x200``) and
``0x53`` (``0x300``). A block in which every read went unanswered is replaced by
the word ``silent``. An unanswered read shows as ``00`` in the dump. The view
never writes byte 127, so A2h bytes 128 to 255 show the page that byte selects
at that moment, which is page ``02h`` while the modulation hold runs.

The ``tables`` view reads A2h bytes 128 to 255 for each table the page select
can show, 0, 1, 2, 4, 5 and 6, and prints each as eight rows of sixteen bytes
under a line ``=== table <n>``. It puts byte 127 back as it found it at the end, also when a table fails (``=== table <n>:
failed: <error>``). Table 2 holds the laser driver's configuration, among it
the receiver's loss-of-signal control (BBh) and the APD bias control (BDh);
table 6 the temperature-indexed APD DAC values at C0h to FFh. Byte 127 is kept
in the module's EEPROM, so a read of the view writes the EEPROM once for each
table it shows; a build with ``CONFIG_RTL9602C_OPTICS_RO`` has no such view::

    # cat /sys/kernel/debug/rtl9602c/tables
    === table 0
    80: <16 bytes>
    ...

Storing table 2 bytes
---------------------

The configuration table, table 2, is the laser driver's configuration as its
maker sets it: among it the receiver's loss-of-signal control (BBh), whose
``LOS_POLARITY`` bit decides how the receiver's signal detect has to be read
(``rtl9602c-gpon.rst``, "Downstream lock, LOS and alarms"), the APD bias
control (BDh) and the current limits (ADh-AFh). A module whose table 2 has gone
wrong is put right through ``t2write``, mode ``0200``, in a kernel built with
``CONFIG_NET_DSA_RTL9602C_OPTICS_STORE``, one command per write::

    # W=/sys/kernel/debug/rtl9602c/t2write
    # echo open > $W
    # echo "bb 1c" > $W
    # echo close > $W
    # echo reset > $W
    # cat /sys/kernel/debug/rtl9602c/tables

``open`` writes ``0xff`` to the password bytes 123 to 126, shows table 2 and
clears both bits of EEPROM_CTRL (C7h), so that the part copies the changes of
its registers into its EEPROM; it keeps C7h and the page select as found.
``REG VALUE``, both hexadecimal, writes the password bytes and shows table 2
again, so that a page switched in between cannot send the byte to another
table, then writes ``VALUE`` at table 2 byte ``REG``. The byte is not read
back: while the part copies into its EEPROM it does not acknowledge and its
reads return a constant, so a read then proves nothing. ``close`` puts C7h and
the page select back as found. The kernel log has a line for each step,
beginning ``GN25L95: table 2``.

Only the bytes that a repair of a module has needed are taken, the others fail
with ``EINVAL``, as does a byte before ``open``: the current limits ADh-AFh,
B6h-B8h, the receiver's loss-of-signal control BBh, its APD bias control BDh
and the ADC control C1h. Not the other bytes of B1h-BDh, among which are the
transmitter's fault, alarm and rogue ONU controls; which of B1h-BAh holds which
control is not recorded, so that B6h-B8h hold none of the rogue ONU controls is
not known. Not SAFE_MODE_STARTUP (A0h), whose wrong value turns the transmitter
off for good; not the calibrated DACs (A8h-ACh) and constants (D2h-E7h), which
belong to the one module; not the sleep control (BFh), the status (C0h), the
part's I2C address (C2h, C3h), the EEPROM controls (C4h-C8h), the passwords or
the monitors.

``reset``, with table 2 closed, restarts the part's state machine with its reset
key, ``5D 2C 6A C9`` at bytes 123 to 126 and ``0B`` at the page select, which
loads the tables from the EEPROM as at power-up; the entry then writes ``0xff``
to the password bytes, as ``open`` does, and puts the page select back. The laser
goes through its start-up again, so the PON drops for that time. A read of the
``tables`` view after ``reset`` shows what the EEPROM holds, which is how a
stored byte is checked without taking the module out.

.. list-table::
   :header-rows: 1
   :widths: 60 6 34

   * - Fact
     - Tag
     - Checked by
   * - Table 2 bytes written while EEPROM_CTRL is clear are kept in the
       module's EEPROM and load again at power-up.
     - M
     - ADh-AFh written so read the written values again through every power-up
       of the module for weeks after.
   * - The reset key loads the tables from the EEPROM.
     - M
     - Table 2 bytes changed with C7h at ``03`` read their stored values again
       after ``reset``.
   * - With C7h set again one second after a byte, the byte does not reach the
       EEPROM, and reads in that second return a constant.
     - M
     - Nine bytes so written, then ``reset``: the ``tables`` view showed the
       old values; every read-back had returned the value of CCh.
   * - With table 2 held open for a minute after the last byte, the bytes
       reach the EEPROM.
     - M
     - Nine bytes (ADh-AFh, B6h-B8h, BBh, BDh, C1h) stored, then ``close`` and
       ``reset``: the ``tables`` view showed all nine; with BBh back at its
       maker's value the ONU reached O5, and again after a restart into an
       image without the entry.

Worked example
--------------

Each word below is one raw value that converts to a reading measured in O5 on
an OLT (46.8 degC, 3.217 V, 7 mA, 1697 uW and 10 uW, as in the table of the
hwmon device); the raw values themselves are examples. The ``a2`` view prints
the value in the SFF-8472 unit, and hwmon the rounded integer of its own unit:

.. list-table::
   :header-rows: 1
   :widths: 22 18 30 30

   * - Bytes
     - Word
     - ``a2`` view
     - hwmon
   * - A2h 96-97
     - ``0x2ecd`` (11981)
     - ``temperature 46.80 C``
     - ``temp1_input`` 46801
   * - A2h 98-99
     - ``0x7daa`` (32170)
     - ``vcc 3.217 V``
     - ``in0_input`` 3217
   * - A2h 100-101
     - ``0x0dde`` (3550)
     - ``tx bias 7100 uA``
     - ``curr1_input`` 7
   * - A2h 102-103
     - ``0x424a`` (16970)
     - ``tx power 1697.0 uW``
     - ``power1_input`` 1697
   * - A2h 104-105
     - ``0x0064`` (100)
     - ``rx power 10.0 uW``
     - ``power2_input`` 10

The transmitted power of 1697 uW is +2.3 dBm, and the received power of 10 uW
is -20 dBm.

Loss of signal and received power
---------------------------------

The GPON MAC has its own loss-of-signal and signal-detect handling, which
Documentation/networking/device_drivers/pon/rtl9602c-gpon.rst describes. The
GPON MAC reports one alarm for two different conditions, and the received power
tells them apart.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The received power shows whether light reaches the receiver, which the
       GPON MAC's loss of signal cannot: that alarm is the same for no light and
       for light the receiver does not recover.
     - M
     - 9.3 uW (-20.3 dBm) read from an OLT while the GPON MAC still reported
       loss of signal; 10 uW in O5 (hwmon ``power2_input``).

TX_DISABLE
==========

The laser has a hard disable input, TX_DISABLE: while it is asserted the
transmitter emits no light. The device tree gives the line as SoC GPIO 13,
active high, and asserting it with the ONU in O5 stopped the light (see the
rows below). The GPON state machine drives the
line: asserted in every state below O3, released from O3 on, because an ONU
first transmits in the Serial-Number state, O3, answering a serial number
request. Releasing the line does not make the laser emit continuously: the GPON
MAC still keys each burst. Below O3 the line is the only hardware block on the
light besides the GPON MAC not keying bursts.

.. list-table::
   :header-rows: 1
   :widths: 30 30 40

   * - ONU state
     - TX_DISABLE
     - Laser
   * - O1 initial, O2 standby
     - Asserted (GPIO 13 high).
     - No light.
   * - O3 serial number, O4 ranging, O5 operation
     - Released.
     - The GPON MAC keys the bursts.

The state machine itself is described in
Documentation/networking/device_drivers/pon/rtl9602c-gpon.rst.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The laser's hard disable input is SoC GPIO 13, active high (high: no
       light). The device tree gives it as ``tx-disable-gpios`` with
       ``GPIO_ACTIVE_HIGH``, and the driver requests it at probe as an output
       already asserted, so that nothing lases before the PON path allows it.
       The line is optional: without the property every TX_DISABLE step is
       skipped. Below O3 it is the only hardware block on the light besides
       the GPON MAC not keying bursts.
     - M
     - With the ONU in O5 and its bursts keyed, the line driven high by hand (a
       test build): the module's bias fell from 7 mA to 0 and its transmitted
       power from 1677 uW to 0 within a second, and 3 s later the OLT
       deactivated the ONU; released, both came back and the ONU was in O5
       again 1.5 s later (hwmon ``curr1_input`` and ``power1_input``).
   * - With the line released the laser emits.
     - T
     - The ONU reaches O5 on an OLT with the line released from O3 on.
   * - Before driving the line the driver sets bit 13 of
       ``RTL9602C_PADMUX_GPIO_EN`` (``0x48``) by read-modify-write, which gives
       the pad to its GPIO function; ``RTL9602C_PADMUX_GPIO_EN_TX_DISABLE`` =
       13 is both the GPIO number and the bit. The GPIO controller does not own
       this register.
     - M, I
     - M: with the bit set, the line acts (the row above). I: whether the pad
       needs it; a run with the bit clear would show it.
   * - The line follows the GPON activation state: asserted in every state
       below O3 and released from O3 on, because an ONU first transmits in the
       Serial-Number state, O3, answering a serial number request. Releasing it
       does not make the laser emit continuously; the GPON MAC still keys each
       burst.
     - P
     - ITU-T G.984.3 clause 10.2, Figure 10-1 and Table 10-1.
   * - The soft TX disable of SFF-8472 (A2h byte 110 bit 6) is not used: on this
       module A2h bytes 0-127 are stored in its EEPROM, so every change would be
       an EEPROM write.
     - P
     - SFF-8472 Rev 12.4 clause 9.8, Table 9-16.

At each ONU state change the driver runs these steps
(``rtl9602c_l_txdis_gpio()``, called from ``rtl9602c_gpon_apply_state()``); the
request of the line at probe is the first step of "Setup, in order":

1. If the device tree gave no TX_DISABLE line, stop. Purpose: a board without
   the wiring skips all of this. (T)
2. Set bit 13 of ``RTL9602C_PADMUX_GPIO_EN`` by read-modify-write. Purpose: the
   pad is a GPIO; only bit 13 changes, and setting it again is harmless. (I)
3. Set the line with ``gpiod_set_value_cansleep()``: asserted for a state below
   O3, released from O3 on. The flags of the device tree turn "asserted" into a
   high level. Purpose: no light below O3, where an ONU does not transmit. (I
   for the line, P for the states)

The modulation hold
===================

The part runs an automatic extinction-ratio loop. On this board, in burst
operation, the loop lowers the modulation current, and the OLT then needs a
longer preamble to lock onto the bursts of the ONU (see the measurements
below). At every PON bring-up, before the first O3, the driver turns the loop
off, so that the modulation runs from the value the module loads at power-up.
The change is to registers only: the module's persistence control keeps it out
of the EEPROM, and a power cycle gives the module its own configuration back.
Whether activation needs the hold is not verified; the GPON bring-up logs a
failed hold and carries on.

The hold works in a configuration table that the module shows at A2h bytes
128-255 while A2h byte 127 holds ``02h``. The driver calls it table 2. The
registers it uses are listed by their A2h byte numbers (the driver's macros are
defined in ``rtl9602c-optics.c``):

.. list-table::
   :header-rows: 1
   :widths: 14 20 66

   * - A2h byte
     - Register
     - What the driver does with it
   * - ``0xa5``
     - ``RTL9602C_OPTICS_TBL2_ER_CTRL``
     - The extinction-ratio control. Reads it, clears bit 0
       (``RTL9602C_OPTICS_TBL2_ER_CTRL_AUTO``, the automatic loop) and reads it
       back. Bits 3:2 (``RTL9602C_OPTICS_TBL2_ER_CTRL_MOD_RANGE``) only decide
       how the log shows the modulation.
   * - ``0xab``, ``0xac``
     - ``RTL9602C_OPTICS_TBL2_MOD_DAC``
     - The modulation DAC, 16 bits, most significant byte first. Reads it.
   * - ``0xaf``
     - ``RTL9602C_OPTICS_TBL2_MOD_LIMIT``
     - The limit on the modulation DAC. Reads it; ``0xff`` means no limit.
   * - ``0xc7``
     - ``RTL9602C_OPTICS_TBL2_PERSIST``
     - The persistence control. Reads it; bits 1:0 both set
       (``RTL9602C_OPTICS_TBL2_PERSIST_OFF``, ``0x03``) keep register changes
       out of the EEPROM.
   * - ``0xcc``
     - none in the code
     - Clears bits 3:2 if either is set, and waits 400 ms.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The registers of the configuration table that the driver uses are, as A2h
       addresses with byte 127 at ``02h``: ``RTL9602C_OPTICS_TBL2_ER_CTRL``
       ``0xa5``, ``RTL9602C_OPTICS_TBL2_MOD_DAC`` ``0xab`` and ``0xac``,
       ``RTL9602C_OPTICS_TBL2_MOD_LIMIT`` ``0xaf``,
       ``RTL9602C_OPTICS_TBL2_PERSIST`` ``0xc7``, and ``0xcc``, which has no
       name in the code.
     - T
     - The hold's log after each module power-up, ``modulation held at 39.0 mA
       (modulation DAC 5f80, ...); automatic ER e3 -> e2``, reads back what it
       wrote.
   * - ``RTL9602C_OPTICS_TBL2_ER_CTRL_AUTO``, bit 0 of ER_CTRL, turns the
       automatic extinction-ratio loop on; ER_CTRL reads ``0xe3`` after a
       module power-up. Cleared (``0xe2``), the loop stays off until the module
       loses power: a warm reboot of the SoC does not restore it, and a power
       cycle does.
     - M
     - The hold's log: ``e3 -> e2`` at the first bring-up after each module
       power-up, ``e2 -> e2`` after a warm reboot.
   * - With the loop on, the modulation current falls from its power-up value:
       MOD_DAC read ``0x48c0`` against ``0x5f80``, about 24 % lower, while the
       transmitted power stayed at +2.3 dBm (the power-control loop
       compensates) and the bias rose (13.2 mA against 7.1 mA). The extinction
       ratio falls with the modulation, and the OLT needs a longer preamble to
       lock onto a burst: at 88 bytes of burst overhead, none of three fresh
       activations to O5 was provisioned by the OLT with the loop on, and both
       of two were with the modulation held.
     - M
     - On an OLT: the ``a2`` view, MOD_DAC read back, provisioned activations
       counted.
   * - ``RTL9602C_OPTICS_TBL2_MOD_DAC`` (``0xab`` and ``0xac``) is 16 bits, most
       significant byte first. With the loop off the modulation runs from it.
       After hours with the loop on, turning the loop off read back ``0x5f80``,
       the value the hold reads at every first bring-up after a module
       power-up.
     - M
     - MOD_DAC read back after hours of loop operation; the hold's log after
       power-ups (``modulation DAC 5f80``).
   * - ``RTL9602C_OPTICS_TBL2_PERSIST`` (``0xc7``): with bits 1:0 both set
       (``RTL9602C_OPTICS_TBL2_PERSIST_OFF``, ``0x03``) register changes are not
       copied into the module's EEPROM, so a power cycle restores the stored
       configuration; with ``0x00`` they are copied, the diagnostic words that
       change all the time included, and while a copy runs the part does not
       acknowledge on the bus. This module reads ``0x03``.
     - M
     - Table writes made at ``0x00`` were still there after a module reset, and
       reads went unanswered at a drifting address while the part copied; at
       ``0x03`` the hold's ER_CTRL write is gone after every module power cycle
       (the ``e3 -> e2`` line above).
   * - ``RTL9602C_OPTICS_TBL2_MOD_LIMIT`` (``0xaf``) is a limit on the
       modulation DAC, ``0xff`` meaning none; the driver refuses a MOD_DAC whose
       high byte exceeds a limit other than ``0xff``.
     - I
     - Unverified; the check only guards against an implausible value. Not
       load-bearing.
   * - ``RTL9602C_OPTICS_TBL2_ER_CTRL_MOD_RANGE`` (bits 3:2 of ER_CTRL): when it
       is 0 the driver logs the held modulation in mA as (DAC + 8) / 10, with
       DAC = bits 15:6 of MOD_DAC; otherwise it logs the raw register. This
       module reads 0 there.
     - I
     - The field, the DAC position and the conversion are unverified; they
       affect only the log line.
   * - Byte ``0xcc``: before writing the table the driver clears its bits 3:2 if
       either is set, then waits 400 ms; this module reads ``0xff`` there.
     - I
     - Whether the table write needs it is not tested; skipping the step and
       checking that ER_CTRL still reads back with bit 0 clear would settle it.
       Not load-bearing while the step stays.
   * - A2h bytes 120-126 are left by SFF-8472 to the module maker, for example
       for a password field.
     - P
     - SFF-8472 Rev 12.4 clause 10.2.
   * - On this module A2h bytes 123-126 (``RTL9602C_OPTICS_A2_PASSWORD``) read
       ``00`` whatever was written to them.
     - M
     - The ``a2`` view, repeated: ``00 00 00 00`` after writes of ``0xff`` to
       each.
   * - Table 2 needs no password. After the part's reset, which loads the
       tables from the EEPROM as at power-up and turns the automatic ER loop
       back on, and after a power cycle of the module, with nothing written to
       bytes 123-126 the table read as before (PERSIST ``03h``), and the
       hold's writes took (``automatic ER e3 -> e2``), so the driver writes no
       password outside the ``t2write`` view.
     - M
     - The hold's log line and table 2 as read before it: after ``echo reset``
       to ``t2write`` in a build that left out every write of the password
       bytes, and after the module was taken out of its cage for 34 s and put
       back (2026-10-13), the stick starting from its flash image, which
       writes no password byte. ER_CTRL read ``e3`` before the hold, which only
       a module that lost power gives, and the stick went on to O5 with the
       transmitted power at +2.2 dBm.

The hold runs at ``ip link set pon up``, after the GPON MAC reset and before the
first O3. With ``CONFIG_RTL9602C_OPTICS_RO``, or for a module the driver does
not write, it returns at once and writes nothing. Its steps:

1. Read A2h byte 127 and keep the value; write ``02h`` to it unless it holds
   ``02h`` already. Purpose: show the configuration table at A2h 128-255, and
   remember the page to restore. (T)
2. Read PERSIST, MOD_LIMIT and ER_CTRL. Purpose: the three values the next steps
   decide on. (T)
3. If bits 1:0 of PERSIST are not both set, log ``EEPROM updates are enabled``
   and give up with ``-EPERM``. Purpose: a table write now would be copied into
   the EEPROM, so none is made. (M) By then step 1 has written the page select,
   when it held another page, which the module keeps.
4. Read byte ``0xcc``; if bit 3 or bit 2 is set, write it back with both clear
   and wait 400 ms. Purpose: the table takes writes. (I)
5. If a lower transmit power level is set, put the module's own modulation and
   set point back (see "Transmit power levelling") and log ``transmit power
   back at level mode 0``. Purpose: every session starts at mode 0. (T)
6. Write ER_CTRL with bit 0 clear, read it back, and read MOD_DAC. Purpose: turn
   the loop off, and see the value the modulation now runs from. (M)
7. If bit 0 still reads set, or MOD_DAC is zero, or MOD_LIMIT is not ``0xff``
   and the high byte of MOD_DAC exceeds it: write ER_CTRL back as it was, so
   that the loop keeps running, log ``modulation DAC seed ... is unusable`` and
   give up with ``-EINVAL``. Purpose: do not leave the modulation at a value
   that cannot be right. (I)
8. At the first hold since probe, read the set point (``0xa8``) and keep it
   with MOD_DAC as the settings of transmit power level mode 0. Purpose: the
   lower levels are made from them, and mode 0 puts them back. (M)
9. Log the held modulation. Purpose: show the value the modulation runs from.
   (I)
10. Write the value kept in step 1 back to byte 127. Purpose: leave the page
    selection as it was found. This follows every step after the write of
    step 1, whatever its outcome; only a transfer that fails in the read of
    step 1 returns before it, when the page select has not been written. (T)

A failed hold is logged by the GPON bring-up (``modulation not held``), and the
bring-up carries on.

Transmit power levelling
========================

ITU-T G.984.2 gives the ONU's transmitter three power levels, the level modes:
0, normal, and 1 and 2, 3 dB and 6 dB below it. The OLT moves an ONU between
them with Change_Power_Level (see
Documentation/networking/device_drivers/pon/rtl9602c-gpon.rst). The driver
makes level mode 0 the power the module came with and sets the lower modes
from it, in the configuration table of "The modulation hold", shown and kept
out of the EEPROM in the same way.

The part's power control loop moves the bias until the monitor photodiode
reaches a set point, so the transmitted power follows the set point while the
bias has room. With the modulation held it has little: the bias reaches its
floor about 1.5 dB down. So a lower level takes a lower modulation too, scaled
by the same factor, and then a lower set point.

.. list-table::
   :header-rows: 1
   :widths: 18 22 60

   * - A2h byte
     - Register
     - What the driver does with it
   * - ``0xa8``
     - ``RTL9602C_OPTICS_TBL2_APCSET``
     - The power control's set point. Keeps the module's own value at the
       first hold since probe; writes lower values for modes 1 and 2, never a
       higher one, and the module's own back for mode 0.
   * - ``0xab``, ``0xac``
     - ``RTL9602C_OPTICS_TBL2_MOD_DAC``
     - The modulation DAC. Keeps the module's own value, read with the
       automatic loop off, at the first hold since probe; writes it with its
       DAC field, bits 15:6 (``RTL9602C_OPTICS_TBL2_MOD_DAC_FIELD``), halved
       for mode 1 and quartered for mode 2, and the module's own back for mode
       0.
   * - 100-101 and 102-103
     - (diagnostics)
     - The bias and the transmitted power, read
       ``RTL9602C_OPTICS_LEVEL_SETTLE_MS`` (20 ms) after each change.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The ONU's output power has three modes: mode 0, normal, within the
       mean launched power of the ODN class; mode 1, 3 dB below; mode 2, 6 dB
       below. The step need not be exactly 3 dB as long as the power lies
       within the new mode's range.
     - P
     - ITU-T G.984.2 clause 8.3.2 and Appendix II.
   * - Byte ``0xa8`` is the power control's set point. With the modulation
       held, the set point at ``ab`` (the module's own) gave 1667 uW at a bias
       of 7.1 mA; ``90`` gave 1217 uW with the bias at its floor, 0.77 mA;
       ``78`` and ``60`` gave 645 uW; at ``55`` the laser went out (bias and
       power 0) and the OLT lost the ONU, which was back in O5 once the set
       point was ``ab`` again (1697 uW). Bytes ``0xa9`` and ``0xaa`` change by
       themselves: ``0f 80``, then ``10 00`` and ``10 80`` with nothing
       written.
     - M
     - The table and the diagnostics read back after each write, on an OLT.
   * - With the set point at ``78`` and the modulation lowered, the loop holds
       the set point's power with more bias: MOD_DAC ``1fc0`` gave a bias of
       4.7 mA and 552 uW, ``17c0`` 9.65 mA and 549 uW.
     - M
     - The same reads.
   * - MOD_DAC takes its two bytes together when the less significant one,
       ``0xac``, is written: ``5f`` written to ``0xab`` alone read back as the
       old ``2f``, and the pair read ``5f 80`` once ``80`` was written to
       ``0xac``. The driver writes ``0xab`` first.
     - M
     - The table read back after each byte.
   * - Change_Power_Level injected in O5 took the transmitted power from 1672
       uW to 823 uW at mode 1 (3.1 dB down, bias 9 mA) and 423 uW at mode 2
       (6.0 dB down, bias 10 mA), and back up to 833 uW and 1697 uW, with the
       ONU in O5 throughout; a step past either end changed nothing, and the
       PON taken down and up from mode 2 came back at mode 0, at 1672 uW.
     - T
     - The hwmon readings after each injected message, on an OLT.
   * - That the extinction ratio stays as it is at modes 1 and 2: scaling the
       modulation with the power keeps it while the light grows linearly with
       the current above the laser's threshold, which is not verified.
     - I
     - The eye of the transmitted light at each mode, with an optical
       instrument.
   * - The module keeps its registers across a warm reboot of the SoC, and the
       next kernel takes the settings it finds for the module's own: booted
       with the optics at mode 2, a kernel logged ``modulation held at 19.8 mA
       (modulation DAC 2f80, ...)``. Before an orderly restart, power-off or
       halt the driver puts mode 0 back; a reset that runs no notifier, such
       as the watchdog's, leaves the lower level until the module loses power.
     - M
     - The hold's log after a warm reboot from mode 2 without the notifier;
       with it, ``reboot`` at mode 2 logged ``transmit power back at level
       mode 0 before the restart``, and the next kernel held the modulation at
       39.0 mA (``5f80``) and sent 1661 uW.

Setting a level (``rtl9602c_optics_level_work()``, queued by the GPON code):

1. Take the mode the GPON code asks for; if the optics are already at it,
   stop. Purpose: one work item serves every request. (T)
2. With ``CONFIG_RTL9602C_OPTICS_RO``, or before a hold has kept the module's
   own settings, give up with ``-EOPNOTSUPP``. Purpose: nothing is written
   that could not be put back. (I)
3. Show the table and check PERSIST as the hold does, giving up with
   ``-EPERM`` when register changes would reach the EEPROM; clear the bits of
   byte ``0xcc``. Purpose: the writes stay in registers. (M)
4. For mode 0, write the module's modulation and then its set point. (M)
5. For a lower mode, read the transmitted power while the optics are still at
   mode 0: the module measures the bursts, so the power read is that of mode 0
   (no power read gives ``-ENODATA``). The target is that power divided by 2
   for mode 1 and by 4 for mode 2. Purpose: the level is made from this
   module's own power. (T)
6. Write the scaled modulation. Purpose: going down, the loop then holds the
   power with more bias, which the set point lowers next; going up, the bias
   falls first. (M)
7. Move the set point one step at a time towards the target, reading the
   power and the bias after each, until the power crosses the target. A set
   point above the module's own, a power of 0 or a bias above
   ``RTL9602C_OPTICS_LEVEL_BIAS_MAX`` (40 mA) gives ``-ERANGE``. Purpose: the
   power lands at the level without the laser going out. (T)
8. On a failure, put mode 0 back; when even that fails, log it and count the
   optics at no known level, which the next request and the next hold try to
   leave. (I)
9. Write the page select back, log the level with the power and the bias, and
   tell the GPON code what was set, which declares it in Serial_Number_ONU.
   (T)

The modules the driver writes
=============================

At probe, once the master is enabled, ``rtl9602c_optics_identify()`` reads the
vendor name (A0h bytes 20-35) and the vendor part number (A0h bytes 40-55) the
module declares, the header the host reads through the SFP cage, and drops the
trailing spaces. The driver writes the laser driver only when the name is
``ODI`` and the part number begins with ``DFP-34X-2C``, the module this page
was measured on. For any other module, and when the header cannot be read, the
driver behaves as the read-only build below, without the build option: the
modulation hold returns at once, ``rtl9602c_l_write()`` and the I2C adapter
refuse writes with ``-EPERM``, no transmit power level is set, and the log has
one line at probe (see "Logging").

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Claim
     - Kind
     - Checked by
   * - The header is that of the module, not of its laser driver; no
       register that identifies the laser driver is known, so the module that
       was measured stands for the laser driver it carries.
     - I
     - A module of the same vendor name and part number with another laser
       driver would be written as this one.
   * - The header can be rewritten (A0h bytes 0-127 take writes without a
       password, see "Known limitations"). A module whose header was changed
       to another name is taken for an unknown one, and one changed to this
       module's name is written.
     - I
     - The driver reads the header once, at probe; a change takes effect at
       the next boot.
   * - The module this page was measured on reads ``ODI`` and
       ``DFP-34X-2C3``.
     - M
     - The ``eeblocks`` view, A0h bytes 20-35 and 40-55.

The read-only build
===================

``CONFIG_RTL9602C_OPTICS_RO`` makes ``rtl9602c_optics_hold_modulation()`` return
0 at once, before it writes anything. The driver itself then writes nothing to
the laser driver: the page select, the bytes 123 to 126 and the configuration
table stay as the module has them, the automatic loop keeps running, and the
hold logs nothing. The I2C adapter refuses every write with ``-EPERM``, so a
client of it, user space through ``i2c-dev`` included, cannot change the
module either. No transmit power level is set: a request for one gives
``-EOPNOTSUPP``, and the ONU stays at level mode 0. The hwmon device and the
``a2`` and ``eeblocks`` views only read, and they are unchanged.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - With ``CONFIG_RTL9602C_OPTICS_RO`` the modulation hold returns at once,
       so the driver itself writes nothing to the part, and the adapter refuses
       every write with ``-EPERM``; the hwmon reads and the ``a2`` and
       ``eeblocks`` views are unchanged.
     - I
     - Read from the code; a build with the option and ``i2cset`` on the
       adapter's ``/dev/i2c-N`` would show the refusal.
       Checking the image's configuration settles it without writing to the
       part. Bears on any image that must leave the module unchanged.

Logging
=======

The optics log through ``pr_warn()`` and ``pr_info()`` with the prefix
``rtl9602c-switch:``, except the probe line, which carries the name of the
device.

.. list-table::
   :header-rows: 1
   :widths: 24 76

   * - When
     - Line
   * - Probe, the ``tx-disable`` line is given and cannot be taken
     - ``cannot get the laser tx-disable gpio``, reported by
       ``dev_err_probe()``; the probe fails or is deferred.
   * - Setup, the master cannot be handed to its driver
     - ``i2c master not handed over: <error>``
   * - Setup, no hwmon device
     - ``no optical diagnostics: <error>``
   * - Setup, the module is not one the driver writes
     - ``optics: module "<vendor name>" "<part number>" is not one the driver
       was measured on; the laser driver is only read``
   * - Setup, the module's header cannot be read
     - ``optics: the module's header could not be read (<error>); the laser
       driver is only read``
   * - Bring-up, the hold succeeded
     - ``GN25L95: modulation held at <m>.<d> mA (modulation DAC <hhhh>, the
       module's calibration); automatic ER <hh> -> <hh>``. When bits 3:2 of
       ER_CTRL are not 0: ``GN25L95: modulation held at modulation DAC <hhhh>
       (the module's calibration); automatic ER <hh> -> <hh>``. The two ER
       values are ER_CTRL before and after the hold.
   * - Bring-up, PERSIST lets register changes reach the EEPROM
     - ``GN25L95: EEPROM updates are enabled (C7h <hh>); modulation left alone
       rather than written to the EEPROM``
   * - Bring-up, the hold cannot use MOD_DAC
     - ``GN25L95: modulation DAC seed <hhhh> is unusable (modulation max <hh>,
       ER control <hh>); modulation left to the module``
   * - Bring-up, the hold failed for any reason
     - ``GN25L95: modulation not held (<error>)``, from the GPON bring-up. When
       the hold gave up for a reason of its own, the two rows above, this line
       follows the line of that reason.
   * - Bring-up, a lower power level was set
     - ``GN25L95: transmit power back at level mode 0``
   * - A power level was set
     - ``GN25L95: transmit power level mode <n>: <uW>.<d> uW, bias <mA>.<dd>
       mA``
   * - Neither the level asked for nor mode 0 could be set
     - ``GN25L95: transmit power level not restored to mode 0``
   * - Before a restart, a lower level was set
     - ``GN25L95: transmit power back at level mode 0 before the restart``, or
       ``not put back at`` in place of ``back at`` when it failed.

Setup, in order
===============

The steps are in the order the driver takes them. The tag in parentheses at the
end of a step is the tag of the fact the step rests on (see the tables above).

1. At probe, request the ``tx-disable`` line as an output driven to its asserted
   level. Purpose: nothing lases before the PON path allows it. (I)
2. During switch setup, create the debugfs directory and with it the views
   ``a2`` and ``eeblocks``. Purpose: the laser driver can be read back without
   the hwmon device. (M)
3. Make the auxiliary device ``rtl9602c.i2c``, for the master's driver to set
   ``RTL9602C_PADMUX_FUNC_EN_I2C`` in ``RTL9602C_PADMUX_FUNC_EN`` by
   read-modify-write and register the master as the adapter ``rtl9602c i2c``.
   When the adapter is there, read the module's vendor name and part number, and with them whether
   the driver writes the laser driver (see "The modules the driver writes").
   Purpose: without the enable the master ignores every command; a module the
   driver was not measured on is only read. (M for the enable, I for the
   adapter and for the module standing for its laser driver)
4. Read the temperature word once (A2h bytes 96 and 97). If it reads, register
   the hwmon device ``rtl9602c_optics``; if not, log ``no optical diagnostics``
   and go on. Purpose: no device appears when nothing answers at A2h. (M)
5. At the first ONU state change after ``ip link set pon up``, which is O1, and
   at every later one, drive TX_DISABLE (see "TX_DISABLE"). Purpose: follow the
   activation state. (T)
6. In the GPON bring-up, after the MAC reset and before the first O3, run the
   modulation hold (see "The modulation hold"). Purpose: the modulation runs
   from the value the module loads at power-up. (M)

At probe the driver also sets up the work that sets power levels and registers
the notifier that puts level mode 0 back before a restart
(``rtl9602c_optics_level_register()``; a failure fails the probe).

The adapter and the hwmon device are device-managed
(``devm_i2c_add_adapter()`` and ``devm_hwmon_device_register_with_info()``) and
go with the switch's platform device. The debugfs views go with the debugfs
directory, which switch teardown removes.

Known limitations
=================

- Without ``CONFIG_RTL9602C_OPTICS_RO``, for a module the driver writes, the
  I2C adapter passes every write on, and A0h and A2h bytes 0-127 take writes without a password and keep them
  across a power cycle of the module, so a client of the adapter can change the
  module for good. User space reaches the adapter through ``i2c-dev`` where
  ``CONFIG_I2C_CHARDEV`` is set.
- A watchdog reset, or any reset that runs no restart notifier, with the
  transmit power at level mode 1 or 2 leaves the module there, and the next
  kernel takes that level for the module's own until the module loses power
  (see "Transmit power levelling"). The extinction ratio at the lower levels
  is not measured.
- A PON bring-up writes a byte that the module keeps only when A2h byte 127
  holds another page than ``02h``: before it has read PERSIST, the modulation
  hold writes ``02h`` there, and it puts the page back at the end. At rest the
  byte holds ``02h`` on this board, so a bring-up writes nothing the module
  keeps.

Open questions
==============

These are the facts tagged **I** in the tables above, each with what would
settle it. The first list holds the assumptions the driver depends on: the
driver is correct on this board only if they hold. The second list holds the
questions to which the code is indifferent: it works whichever way they
resolve.

Assumptions the driver depends on
---------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Assumption
     - What would settle it
   * - The level requested at probe reaches the pin before the driver first
       sets bit 13 of ``RTL9602C_PADMUX_GPIO_EN``, at the first ONU state
       change after ``ip link set pon up``: that holds only if the pad is a
       GPIO function at reset, or the bit is not needed ("TX_DISABLE").
     - A run with the bit clear shows whether it is needed at all.
   * - Nothing serialises a transfer between its users ("Known limitations").
     - Reading the hwmon attributes in a loop while reading the ``a2`` view: it
       would show mixed or misplaced bytes.
   * - Two one-byte reads of a diagnostic word can mix two samples ("Known
       limitations").
     - Reading the received-power word in a tight loop while the level changes,
       looking for jumps of about 256 counts.
   * - That the extinction ratio stays as it is at transmit power level modes
       1 and 2 ("Transmit power levelling").
     - The eye of the transmitted light at each mode, with an optical
       instrument.
   * - The read-only build still exposes a writable adapter ("The read-only
       build").
     - Checking the image's configuration for ``CONFIG_I2C_CHARDEV`` settles
       whether user space can reach the adapter, without writing to the part.

Questions the code does not depend on
-------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 58 42

   * - Question
     - What would settle it
   * - What the other bits of ``RTL9602C_PADMUX_FUNC_EN`` enable ("The I2C
       master").
     - Nothing the driver needs; the read-modify-write is right either way.
       Finding what each other set bit of the word enables would settle it.
   * - Whether a client can read and write the laser driver through the adapter
       ("The I2C master").
     - Reading ``0x50`` through the adapter (i2c-dev, where
       ``CONFIG_I2C_CHARDEV`` is set) and comparing with the ``eeblocks`` view.
   * - Whether the part skips rewriting a byte that already holds the value, and
       whether byte 127 is stored ("The management interface").
     - Reading the ``a2`` view ("table select") after a module power cycle taken
       while byte 127 held ``02h``; the write cycles themselves cannot be
       observed on this board.
   * - Whether the part is a GN25L95 ("The management interface").
     - The package marking.
   * - Whether ``RTL9602C_OPTICS_TBL2_MOD_LIMIT`` limits the modulation DAC, and
       what bits 3:2 of ER_CTRL and the conversion of the logged current mean
       ("The modulation hold").
     - Nothing the driver needs: the first guards against an implausible value
       and the others only change the log line.
   * - Whether the table write needs byte ``0xcc`` cleared ("The modulation
       hold").
     - Skipping the step and checking that ER_CTRL still reads back with bit 0
       clear.
   * - Whether activation needs the modulation hold ("The modulation hold").
     - Repeated activations from a module power-up with the hold (the default)
       and without it (``CONFIG_RTL9602C_OPTICS_RO``), counting those that the
       OLT provisions.
