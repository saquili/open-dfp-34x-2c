# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the PON class. pon_nl.c holds the policies and the operation
# table of the pon generic netlink family, generated from
# Documentation/netlink/specs/pon.yaml.
#

obj-$(CONFIG_PON) += pon.o
pon-y := pon_core.o pon_gem.o pon_netlink.o pon_nl.o
obj-$(CONFIG_PON_RTL9602C) += rtl9602c/
