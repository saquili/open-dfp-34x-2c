.. SPDX-License-Identifier: GPL-2.0-only

==========================
Kernel driver i2c-rtl9602c
==========================

Supported adapters:
  * The second I2C master of the Realtek RTL9602C, the SoC of GPON ONUs such
    as SFP sticks, which the optics of the stick hang on.

Author: Paweł Phan <szop.hektar_1w@icloud.com>

Description
-----------

The RTL9602C has two I2C masters with the same set of registers, in the
register window of its switch (``0x1b000000`` in the device tree of the
board). The laser driver IC of the stick's optics, which serves the SFF-8472
management interface (A0h at ``0x50``, A2h at ``0x51``), answers on master 1
only. The driver runs master 1 and registers it as an SMBus adapter named
``rtl9602c i2c``.

The registers lie in the switch's window, which the switch driver
(``rtl9602c``, drivers/net/dsa/realtek) maps, so the master has no node of its
own with registers. The devices on its bus are the children of the switch
node's ``i2c`` node (Documentation/devicetree/bindings/net/dsa/
realtek,rtl9602c-switch.yaml), which the driver gives the adapter, so that the
I2C core registers them with it; without the node the adapter has no devices.
The switch driver makes the auxiliary device
``rtl9602c.i2c`` and gives it the window as platform data
(include/linux/platform_data/i2c-rtl9602c.h); this driver binds to that
device. It may bind before or after the switch is set up, and the switch
driver is told when the adapter appears or is about to go. Without this
driver the switch has no optics: it cannot identify the module, publish its
diagnostics through hwmon, or hold the laser's modulation.

The adapter offers SMBus byte data, read and write, and word data, read only.
A word is read in one transfer of two bytes, so that the module does not
update it between the bytes. The first byte read is the low byte of the SMBus
word; ``i2c_smbus_read_word_swapped()`` gives the word as SFF-8472 has it,
most significant byte first. Every other transaction is refused with
``-EOPNOTSUPP``.

A write from a client is passed on only if the switch driver allows it, which
it does for a module that it has identified as one it was measured on, and not
with ``CONFIG_RTL9602C_OPTICS_RO``. Otherwise it is refused with ``-EPERM``:
the A0h and A2h bytes 0-127 of the module take writes without a password and
keep them across a power cycle, so that a write is a permanent change to the
module (Documentation/networking/device_drivers/pon/rtl9602c-optics.rst).

There is no interrupt. The master is polled until it is no longer busy, which
takes up to about two milliseconds for a byte, and the driver relies on the
I2C core to serialize the transfers of the adapter.

Configuration
-------------

``CONFIG_I2C_RTL9602C`` builds the driver, as a module ``i2c-rtl9602c`` if
``m``. ``CONFIG_NET_DSA_RTL9602C`` implies it and builds the switch driver
that makes the device it binds to.

Registers
---------

Offsets are the values of the macros of the driver for master 1.

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
     - Sets bit 14 (``RTL9602C_PADMUX_FUNC_EN_I2C``) once, when the driver
       binds.

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
       ``0x023004 + 4m``. The driver uses m = ``RTL9602C_I2C_MASTER`` = 1
       only, and its macros already include the 4m of master 1.
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
       driver sets the bit once, when it binds.
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
       address. ``RTL9602C_I2C_XFER_CFG_SUBADDR_WIDTH`` (bits 13:12) is written
       ``RTL9602C_I2C_XFER_CFG_WIDTH_8BIT`` (0): one register-address byte.
       ``RTL9602C_I2C_XFER_CFG_DATA_WIDTH`` (bits 11:10) is the number of data
       bytes: 0 one, ``RTL9602C_I2C_XFER_CFG_WIDTH_16BIT`` (1) two, 2 and 3
       four. A read leaves them in ``RTL9602C_I2C_RX_DATA`` in the order they
       came, the first in bits 7:0. The driver moves two bytes for an
       SMBus word and one for a byte.
     - M, T
     - M: A0h bytes 0 to 7 read at each width. At 1, ``RX_DATA`` held
       ``0x0403`` at byte 0, whose bytes are ``03 04``, and a read lasted 9 SCL
       periods more than at 0; at 2 and at 3, ``0x00010403`` and 27 periods
       more. The temperature word read in one transfer matched its two bytes
       read one by one. T: the optics diagnostics read.
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

One transfer
------------

Every read and write of the laser driver is one byte, or two for a word that
is read, in these steps:

1. Read-modify-write bits 21:0 of ``RTL9602C_I2C_XFER_CFG``:
   ``RTL9602C_I2C_XFER_CFG_BUS_EN`` set, ``RTL9602C_I2C_XFER_CFG_DEV_ADDR`` the
   device, the register-address width 8-bit, the data width 8-bit (16-bit for a word),
   ``RTL9602C_I2C_XFER_CFG_CLK_DIV`` 311. Purpose:
   describe the transfer; without BUS_EN the master does not drive the bus. (M
   for BUS_EN and the divider, T for the other fields)
2. Write the register address to ``RTL9602C_I2C_SUBADDR``. Purpose: name the
   byte inside the device. (T)
3. For a write, write the byte to ``RTL9602C_I2C_TX_DATA``. Purpose: the byte
   the transfer sends. (T)
4. Write ``RTL9602C_I2C_CMD_START`` to ``RTL9602C_I2C_CMD`` for a read, and
   ``RTL9602C_I2C_CMD_START`` with ``RTL9602C_I2C_CMD_WRITE`` for a write.
   Purpose: start the transfer. (M)
5. Poll ``RTL9602C_I2C_CMD`` until ``RTL9602C_I2C_CMD_BUSY`` is clear, 1 us
   apart for up to 2 ms (``read_poll_timeout_atomic()``), else fail with
   ``-ETIMEDOUT``. Purpose: wait for the transfer. The bound is the driver's; a
   one-byte read at 100 kHz takes about 460 us. The wait does not sleep: the
   board's kernel has no high resolution timers, and a sleep would last at
   least a tick, 4 ms at its 250 Hz. (T)
6. If ``RTL9602C_I2C_CMD_NACK`` is set, fail with ``-ENXIO``. Purpose: the
   device did not acknowledge. (M)
7. For a read, the byte is the low 8 bits of ``RTL9602C_I2C_RX_DATA``; for a
   word, bits 7:0 are the first byte read and bits 15:8 the second, which is
   the SMBus word (its low byte is the first on the wire). Purpose: the
   result. (T, M for the word)

The adapter
-----------

.. list-table::
   :header-rows: 1
   :widths: 58 6 36

   * - Fact
     - Tag
     - Checked by
   * - The adapter ``rtl9602c i2c`` offers SMBus byte-data transfers, read and
       write, and word-data reads (``I2C_FUNC_SMBUS_BYTE_DATA`` and
       ``I2C_FUNC_SMBUS_READ_WORD_DATA``), which are the transactions the
       controller performs. A word read is one transfer at the 16-bit data
       width; the word has the first byte read in its low byte.
     - T, M
     - The optics diagnostics read: the switch driver reads every diagnostic
       word through the adapter and swaps it, and the temperature matched its
       two bytes read one by one.
   * - A write from a client is refused with ``-EPERM`` unless the switch
       driver allows it, and a write of a word with ``-EOPNOTSUPP``.
     - T
     - The modulation hold: it writes through the adapter on the module the
       driver knows, and is refused on one it does not.
   * - The adapter is registered when the driver binds and goes before the
       driver does; the switch driver is told of both, and a sequence of
       transfers in progress holds the adapter until it ends.
     - I
     - Read from the code. Unbinding the driver while the optics are read in a
       loop would settle it; not run.
