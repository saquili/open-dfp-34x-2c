# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the GPON MAC of the Realtek RTL9602C. The switch driver is in
# drivers/net/dsa/realtek/; it makes the auxiliary device this driver binds to.
#

obj-$(CONFIG_PON_RTL9602C) += rtl9602c-pon.o
rtl9602c-pon-y := rtl9602c-pon-main.o rtl9602c-pon-calc.o rtl9602c-gpon.o \
		  rtl9602c-ponmac.o rtl9602c-omci.o rtl9602c-gem.o
