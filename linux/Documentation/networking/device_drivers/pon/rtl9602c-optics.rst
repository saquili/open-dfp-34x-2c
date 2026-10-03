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

- It moves single bytes over the I2C master and registers the master as the
  SMBus adapter ``rtl9602c i2c``.
- It publishes the five measurements of the SFF-8472 diagnostics, which are the
  temperature, the supply voltage, the transmitter bias, the transmitted power
  and the received power, as the hwmon device ``rtl9602c_optics``.
- It drives TX_DISABLE for the GPON state machine: asserted below O3, released
  from O3 on.
- It turns the laser driver's automatic extinction-ratio loop off at every PON
  bring-up, so that the modulation of the laser runs from the value the module
  loads at power-up. This is the modulation hold.
- It offers two debugfs views that only read, ``a2`` and ``eeblocks``.

The driver writes nothing to the laser driver at probe or at setup. Its only
writes are those of the modulation hold, at PON bring-up, and
``CONFIG_RTL9602C_OPTICS_RO`` removes them. The connections::

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
     - The debugfs views ``a2`` and ``eeblocks`` exist only with it.
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

There is no node for the I2C master or for the laser driver. The driver
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
     - The byte transfers over the I2C master and the SMBus adapter
       (``rtl9602c_i2c_read_byte()``, ``rtl9602c_i2c_register()``), the
       numbering of the part's registers (``rtl9602c_l_read()``), the TX_DISABLE
       drive (``rtl9602c_l_txdis_gpio()``), the hwmon device
       (``rtl9602c_optics_hwmon_register()``), the modulation hold
       (``rtl9602c_optics_hold_modulation()``) with the
       ``RTL9602C_OPTICS_TBL2_*`` macros of the configuration table it writes,
       and the debugfs views (``rtl9602c_optics_debugfs_init()``).
   * - ``drivers/net/dsa/realtek/rtl9602c.h``
     - The registers of the I2C master and of the pad functions
       (``RTL9602C_I2C_*``, ``RTL9602C_PADMUX_*``), the byte numbers of the
       diagnostics (``RTL9602C_OPTICS_A2_*``), the members ``tx_disable`` and
       ``i2c`` of ``struct rtl9602c_priv``, and the prototypes of the calls
       above.
   * - ``drivers/net/dsa/realtek/rtl9602c-main.c``
     - ``rtl9602c_probe()`` takes the ``tx-disable`` line. ``rtl9602c_setup()``
       creates the debugfs directory with the optics views, registers the I2C
       adapter and then the hwmon device (see "Setup, in order").
   * - ``drivers/net/dsa/realtek/rtl9602c-gpon.c``
     - ``rtl9602c_gpon_apply_state()`` drives TX_DISABLE at every change of the
       ONU state, and ``rtl9602c_gpon_bringup()`` calls the modulation hold
       after the MAC reset.
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
  ``power1_input`` and ``power2_input``, each with a ``_label`` file, all
  read-only (mode ``0444``). Their units are those of the hwmon interface:
  millidegree Celsius, millivolt, milliampere and microwatt (see
  "Diagnostics"). Reading a value takes two one-byte transfers on the I2C
  master; reading a label takes none.
- The I2C adapter ``rtl9602c i2c``, whose name is in
  ``/sys/class/i2c-adapter/i2c-N/name``. It offers SMBus byte-data transfers
  only (see "The I2C adapter"). With ``CONFIG_I2C_CHARDEV`` it has a file
  ``/dev/i2c-N``, and a write through it can change the module for good (see
  "The management interface").
- ``/sys/kernel/debug/rtl9602c/a2`` and ``/sys/kernel/debug/rtl9602c/eeblocks``,
  two views that read the laser driver one byte at a time and print it (see
  "The debugfs views"). Both have mode ``0400`` and exist only with
  ``CONFIG_DEBUG_FS``.
- The TX_DISABLE line. It has no file of its own: it is line 13 of the GPIO
  bank, requested under the consumer name ``tx-disable``, and
  ``/sys/kernel/debug/gpio`` shows its direction and level (high while
  TX_DISABLE is asserted). Its level follows the ONU state, so
  ``ip link set pon up`` and ``ip link set pon down`` are the commands that
  move it (see "TX_DISABLE").
- ``ip link set pon up`` also runs the modulation hold, unless
  ``CONFIG_RTL9602C_OPTICS_RO`` is set (see "The modulation hold").
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

The SoC has two I2C masters with the same set of registers. The laser driver
answers on master 1 only, and the driver uses master 1 only
(``RTL9602C_I2C_OPTICS_MASTER``): the macros below are master 1's. A master
moves one byte per transfer. The driver describes the transfer in the transfer
configuration register (the device address, the widths of the register address
and of the data, and the clock divider), writes the register address and, for a
write, the byte, starts the transfer with a command, and polls the command
register until the master is no longer busy. No interrupt is used. A master
ignores its commands until its pad function is enabled, in a register that
other pad functions share.

Registers
---------

Offsets are the values of the macros for master 1.

.. list-table::
   :header-rows: 1
   :widths: 14 36 50

   * - Offset
     - Register
     - What the driver does
   * - ``+0x0000b4``
     - ``RTL9602C_I2C_TX_DATA``
     - Writes the byte that a write transfer sends.
   * - ``+0x0000bc``
     - ``RTL9602C_I2C_SUBADDR``
     - Writes the register address inside the device, before every transfer.
   * - ``+0x0000c4``
     - ``RTL9602C_I2C_CMD``
     - Writes START, with WRITE for a write; reads it until BUSY clears, then
       tests NACK.
   * - ``+0x0000cc``
     - ``RTL9602C_I2C_RX_DATA``
     - Reads the byte that a read transfer received, in the low 8 bits.
   * - ``+0x023008``
     - ``RTL9602C_I2C_XFER_CFG``
     - Read-modify-write of bits 21:0 before every transfer.
   * - ``+0x023018``
     - ``RTL9602C_PADMUX_FUNC_EN``
     - Sets bit 14 (``RTL9602C_PADMUX_FUNC_EN_I2C``) once, when the adapter is
       registered.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The two masters have the same registers at a 4-byte stride. For master
       m, ``RTL9602C_I2C_TX_DATA`` is at ``0x0000b0 + 4m``,
       ``RTL9602C_I2C_SUBADDR`` at ``0x0000b8 + 4m``, ``RTL9602C_I2C_CMD`` at
       ``0x0000c0 + 4m``, ``RTL9602C_I2C_RX_DATA`` at ``0x0000c8 + 4m``, and the
       transfer configuration register ``RTL9602C_I2C_XFER_CFG`` at
       ``0x023004 + 4m``. The driver uses m = ``RTL9602C_I2C_OPTICS_MASTER`` =
       1 only, and its macros already include the 4m of master 1.
     - T
     - The optics diagnostics read: every read of the laser driver goes through
       these registers of master 1.
   * - The laser driver is on master 1: through master 1 it answers at ``0x50``
       and ``0x51``, while the same reads through master 0 return zeros.
     - M
     - A0h bytes 40-55 read through each master in turn.
   * - ``RTL9602C_PADMUX_FUNC_EN`` (``0x023018``), bit 13 + m, enables master m
       (``RTL9602C_PADMUX_FUNC_EN_I2C``, bit 14 for master 1). With the bit
       clear the master ignores the start command: the transfer ends at once,
       BUSY is never set, there is no NACK, and the data byte is zero. The
       driver sets the bit once, when it registers the adapter.
     - M
     - With bit 14 clear no transfer ran; setting it made master 1 run at once.
   * - The same register holds the enables of other pad functions, so the
       driver sets its bit by read-modify-write.
     - I
     - What the other bits enable is not verified; the read-modify-write is
       right either way. Not load-bearing.
   * - ``RTL9602C_I2C_XFER_CFG_BUS_EN`` (bit 21 of ``RTL9602C_I2C_XFER_CFG``)
       must be set, or a transfer runs without driving the bus: it ends without
       a NACK and every read returns the same byte (``0xff`` at a clock divider
       of 30).
     - M
     - With bit 21 clear every byte of ``0x50`` and ``0x51`` read the same
       constant; with it set they read the module's contents.
   * - ``RTL9602C_I2C_XFER_CFG_DEV_ADDR`` (bits 20:14) is the 7-bit device
       address. ``RTL9602C_I2C_XFER_CFG_SUBADDR_WIDTH`` (bits 13:12) and
       ``RTL9602C_I2C_XFER_CFG_DATA_WIDTH`` (bits 11:10) are written
       ``RTL9602C_I2C_XFER_CFG_WIDTH_8BIT`` (0): one register-address byte and
       one data byte per transfer.
     - T
     - The optics diagnostics read: the identifier byte and the diagnostic words
       read back at their SFF-8472 offsets.
   * - The driver writes bits 21:0 of ``RTL9602C_I2C_XFER_CFG`` by
       read-modify-write before every transfer and leaves bits 31:22 as it
       found them.
     - T
     - The optics diagnostics read.
   * - ``RTL9602C_I2C_XFER_CFG_CLK_DIV`` (bits 9:0) sets SCL to 31.25 MHz /
       (``CLK_DIV`` + 1). The driver writes
       ``RTL9602C_I2C_XFER_CFG_CLK_DIV_100KHZ`` (311): 100 kHz, I2C's standard
       mode. Until it did, it wrote 30, about 1 MHz.
     - M
     - 256 one-byte reads of A0h at dividers 30, 77, 311 and 479 took 12.0,
       29.5, 117.8 and 180.9 ms: 1.47 us for each step of ``CLK_DIV`` + 1, so a
       read lasts about 46 periods of 31.25 MHz / (``CLK_DIV`` + 1). In 2560
       reads of A0h no transfer went unacknowledged at 30 or at 311.
   * - ``RTL9602C_I2C_CMD``: ``RTL9602C_I2C_CMD_START`` (bit 0) starts the
       transfer the other registers describe and clears itself;
       ``RTL9602C_I2C_CMD_WRITE`` (bit 1) makes it a write, clear a read;
       ``RTL9602C_I2C_CMD_BUSY`` (bit 2) is set while it runs;
       ``RTL9602C_I2C_CMD_NACK`` (bit 3) says the device did not acknowledge.
     - M
     - START and BUSY polled; the ``eeblocks`` view counts the unacknowledged
       reads per address.
   * - ``RTL9602C_I2C_SUBADDR`` takes the register address inside the device and
       ``RTL9602C_I2C_TX_DATA`` the byte a write sends; ``RTL9602C_I2C_RX_DATA``
       returns the byte a read received, in its low 8 bits.
     - T
     - The optics diagnostics read, for reads; for writes, the modulation hold
       reads back what it wrote.
   * - Every user of the master holds the switch's ``i2c_lock`` for its whole
       sequence of transfers: a hwmon read, the ``a2`` and ``eeblocks`` views,
       the modulation hold, and each transfer of a client of the adapter. The
       hwmon core and the I2C core serialise only their own callers, and the
       modulation hold switches the page of A2h 128-255 and puts it back, which
       no other user may see.
     - I
     - Read from the code; in a kernel with lockdep, the transfer functions
       assert that the lock is held. Reading the hwmon attributes in a loop
       while reading the ``a2`` view would show no mixed bytes; not run.

One byte
--------

Every read and write of the laser driver is one byte, in these steps:

1. Read-modify-write bits 21:0 of ``RTL9602C_I2C_XFER_CFG``:
   ``RTL9602C_I2C_XFER_CFG_BUS_EN`` set, ``RTL9602C_I2C_XFER_CFG_DEV_ADDR`` the
   device, both widths 8-bit, ``RTL9602C_I2C_XFER_CFG_CLK_DIV`` 30. Purpose:
   describe the transfer; without BUS_EN the master does not drive the bus. (M
   for BUS_EN and the divider, T for the other fields)
2. Write the register address to ``RTL9602C_I2C_SUBADDR``. Purpose: name the
   byte inside the device. (T)
3. For a write, write the byte to ``RTL9602C_I2C_TX_DATA``. Purpose: the byte
   the transfer sends. (T)
4. Write ``RTL9602C_I2C_CMD_START`` to ``RTL9602C_I2C_CMD`` for a read, and
   ``RTL9602C_I2C_CMD_START`` with ``RTL9602C_I2C_CMD_WRITE`` for a write.
   Purpose: start the transfer. (M)
5. Poll ``RTL9602C_I2C_CMD`` until ``RTL9602C_I2C_CMD_BUSY`` is clear: at most
   2000 polls 1 us apart, else fail with ``-ETIMEDOUT``. Purpose: wait for the
   transfer. The bound is the driver's; a one-byte read at 100 kHz takes about
   460 us. (T)
6. If ``RTL9602C_I2C_CMD_NACK`` is set, fail with ``-ENXIO``. Purpose: the
   device did not acknowledge. (M)
7. For a read, the byte is the low 8 bits of ``RTL9602C_I2C_RX_DATA``. Purpose:
   the result. (T)

The I2C adapter
---------------

The master is also registered as an I2C adapter, so that the I2C core and its
clients can reach the laser driver. The driver's own reads do not go through
it. The adapter passes a read or a write on to the 7-bit address and the
register it is given, without restricting either, and refuses every transfer
size other than an SMBus byte-data read or write with ``-EOPNOTSUPP``.

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The master is registered at switch setup as the I2C adapter
       ``rtl9602c i2c``, offering SMBus byte-data transfers only
       (``I2C_FUNC_SMBUS_BYTE_DATA``), which is the one transaction the
       controller performs. A registration failure is logged and the switch
       carries on. The driver's own reads do not go through the adapter, and no
       client of it has been run.
     - I
     - Reading ``0x50`` through the adapter (i2c-dev, where
       ``CONFIG_I2C_CHARDEV`` is set) and comparing with the ``eeblocks`` view
       would settle it.

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
       permanent change to the module. The driver writes none of it except A2h
       bytes 123-127, in the modulation hold.
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
   * - The modulation hold writes A2h bytes 123-126 and byte 127 on every PON
       bring-up, and restores byte 127 to the value it found. Those are writes
       to bytes that the module keeps across a power cycle; whether the part
       skips rewriting a byte that already holds the value is not known (see
       "Known limitations").
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
     - The ``eeblocks`` view.
     - Nothing in the driver.
   * - ``0x51`` (A2h), bytes 0-127
     - The ``a2`` view; the hwmon device reads bytes 96-105 only.
     - The modulation hold: ``0xff`` to bytes 123-126, ``02h`` to byte 127, and
       byte 127 again to restore it.
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
module, and the driver applies no calibration. The driver reads each value as
two one-byte transfers, the most significant byte first. The hwmon device
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

The device has no limit or alarm attributes: the driver does not interpret the
threshold and flag bytes of SFF-8472, and they are visible only in the hex dump
of the ``a2`` view. The device is registered once, at setup: if the temperature
word cannot be read then, there is no hwmon device until the switch is set up
again.

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
     - The ``a2`` view, repeated: ``00 00 00 00`` after bring-ups that had
       written ``0xff`` to each.
   * - The driver writes ``0xff`` to each of A2h bytes 123-126 before it writes
       the table; whether the table takes the writes without that is not
       tested.
     - I
     - Skipping the step and checking that ER_CTRL still reads back with bit 0
       clear would settle it. Not load-bearing while the step stays.

The hold runs at ``ip link set pon up``, after the GPON MAC reset and before the
first O3. With ``CONFIG_RTL9602C_OPTICS_RO`` it returns at once and writes
nothing. Its steps:

1. Write ``0xff`` to A2h bytes 123, 124, 125 and 126. Purpose: prepare the table
   for writing. (I)
2. Read A2h byte 127 and keep the value; write ``02h`` to it. Purpose: show the
   configuration table at A2h 128-255, and remember the page to restore. (T)
3. Read byte ``0xcc``; if bit 3 or bit 2 is set, write it back with both clear
   and wait 400 ms. Purpose: the table takes writes. (I)
4. Read PERSIST, MOD_LIMIT and ER_CTRL. Purpose: the three values the next steps
   decide on. (T)
5. If bits 1:0 of PERSIST are not both set, log ``EEPROM updates are enabled``
   and give up with ``-EPERM``. Purpose: a register write now would be copied
   into the EEPROM. (M) By then steps 1 to 3 have written A2h bytes 123 to 127,
   which the module keeps, and possibly ``0xcc``, which is copied when PERSIST
   allows it.
6. Write ER_CTRL with bit 0 clear, read it back, and read MOD_DAC. Purpose: turn
   the loop off, and see the value the modulation now runs from. (M)
7. If bit 0 still reads set, or MOD_DAC is zero, or MOD_LIMIT is not ``0xff``
   and the high byte of MOD_DAC exceeds it: write ER_CTRL back as it was, so
   that the loop keeps running, log ``modulation DAC seed ... is unusable`` and
   give up with ``-EINVAL``. Purpose: do not leave the modulation at a value
   that cannot be right. (I)
8. Log the held modulation. Purpose: show the value the modulation runs from.
   (I)
9. Write the value kept in step 2 back to byte 127. Purpose: leave the page
   selection as it was found. This follows steps 4 to 8 whatever their outcome;
   a transfer that fails in steps 1 to 3 returns before it and can leave
   ``02h`` in byte 127. (T)

A failed hold is logged by the GPON bring-up (``modulation not held``), and the
bring-up carries on.

The read-only build
===================

``CONFIG_RTL9602C_OPTICS_RO`` makes ``rtl9602c_optics_hold_modulation()`` return
0 at once, before it writes anything. The driver itself then writes nothing to
the laser driver: the page select, the bytes 123 to 126 and the configuration
table stay as the module has them, the automatic loop keeps running, and the
hold logs nothing. The I2C adapter refuses every write with ``-EPERM``, so a
client of it, user space through ``i2c-dev`` included, cannot change the
module either. The hwmon device and the ``a2`` and ``eeblocks`` views only
read, and they are unchanged.

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
   * - Setup, the adapter cannot be registered
     - ``i2c master not registered: <error>``
   * - Setup, no hwmon device
     - ``no optical diagnostics: <error>``
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

Setup, in order
===============

The steps are in the order the driver takes them. The tag in parentheses at the
end of a step is the tag of the fact the step rests on (see the tables above).

1. At probe, request the ``tx-disable`` line as an output driven to its asserted
   level. Purpose: nothing lases before the PON path allows it. (I)
2. During switch setup, create the debugfs directory and with it the views
   ``a2`` and ``eeblocks``. Purpose: the laser driver can be read back without
   the hwmon device. (M)
3. Set ``RTL9602C_PADMUX_FUNC_EN_I2C`` in ``RTL9602C_PADMUX_FUNC_EN`` by
   read-modify-write, and register the master as the adapter ``rtl9602c i2c``.
   Purpose: without the enable the master ignores every command. (M for the
   enable, I for the adapter)
4. Read the temperature word once (A2h bytes 96 and 97). If it reads, register
   the hwmon device ``rtl9602c_optics``; if not, log ``no optical diagnostics``
   and go on. Purpose: no device appears when nothing answers at A2h. (M)
5. At the first ONU state change after ``ip link set pon up``, which is O1, and
   at every later one, drive TX_DISABLE (see "TX_DISABLE"). Purpose: follow the
   activation state. (T)
6. In the GPON bring-up, after the MAC reset and before the first O3, run the
   modulation hold (see "The modulation hold"). Purpose: the modulation runs
   from the value the module loads at power-up. (M)

The adapter and the hwmon device are device-managed
(``devm_i2c_add_adapter()`` and ``devm_hwmon_device_register_with_info()``) and
go with the switch's platform device. The debugfs views go with the debugfs
directory, which switch teardown removes.

Known limitations
=================

- A diagnostic word is read as three one-byte transfers. SFF-8472 (Rev 12.4
  clause 9.1) has a multi-byte diagnostic field read in one two-byte sequence,
  during which the module does not update it, and the master moves one byte per
  transfer. The driver reads the most significant byte, the least significant
  one and the most significant one again, and takes the word when the two
  readings of the high byte agree, up to three times; otherwise the read fails
  with ``-EAGAIN``. The low byte read between two equal high bytes belongs to a
  sample with that high byte, so the word is one the module held. How often the
  module updates a word during the three transfers is not measured.
- Without ``CONFIG_RTL9602C_OPTICS_RO`` the I2C adapter passes every write on,
  and A0h and A2h bytes 0-127 take writes without a password and keep them
  across a power cycle of the module, so a client of the adapter can change the
  module for good. User space reaches the adapter through ``i2c-dev`` where
  ``CONFIG_I2C_CHARDEV`` is set.
- Each PON bring-up writes bytes that the module keeps. Before it has read
  PERSIST, the modulation hold writes ``0xff`` to A2h bytes 123 to 126 and
  ``02h`` to byte 127, and at the end it writes the old value back to byte 127:
  six byte writes. Whether the part skips rewriting a byte that already holds
  the value is not known, so the wear of the EEPROM is unmeasured.

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
       ("The I2C adapter").
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
   * - Whether the table write needs byte ``0xcc`` cleared, and the ``0xff``
       written to A2h bytes 123-126 ("The modulation hold").
     - Skipping each step and checking that ER_CTRL still reads back with bit 0
       clear.
   * - Whether activation needs the modulation hold ("The modulation hold").
     - Repeated activations from a module power-up with the hold (the default)
       and without it (``CONFIG_RTL9602C_OPTICS_RO``), counting those that the
       OLT provisions.
