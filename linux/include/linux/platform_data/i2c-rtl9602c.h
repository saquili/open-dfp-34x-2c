/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The I2C master of the Realtek RTL9602C that the optics hang on, as the
 * switch driver hands it over to the driver i2c-rtl9602c: the master's
 * registers lie in the switch's register window, which the switch driver
 * maps, so it makes an auxiliary device named "rtl9602c.i2c" and gives it
 * this as its platform data.
 */

#ifndef _LINUX_PLATFORM_DATA_I2C_RTL9602C_H
#define _LINUX_PLATFORM_DATA_I2C_RTL9602C_H

#include <linux/types.h>

struct i2c_adapter;

/**
 * struct rtl9602c_i2c_data - what the switch driver gives the master's driver
 * @reg: the switch's register window; the master's registers are in it
 * @priv: the argument of @writable and @adapter
 * @writable: whether the adapter may pass on a write of a client, which the
 *	switch driver allows only for a module it knows; called for every write
 * @adapter: called with the adapter once it is registered, so that the
 *	switch driver can reach the optics through it, and with NULL before it
 *	goes; may be NULL
 */
struct rtl9602c_i2c_data {
	void __iomem *reg;
	void *priv;
	bool (*writable)(void *priv);
	void (*adapter)(void *priv, struct i2c_adapter *adap);
};

#endif /* _LINUX_PLATFORM_DATA_I2C_RTL9602C_H */
