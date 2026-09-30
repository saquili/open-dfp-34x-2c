# SPDX-License-Identifier: GPL-2.0
#
# On MIPS, BUILTIN_DTB links every blob built here into the kernel, and the
# RTL9602C platform boots with the first one: every line is guarded by its
# platform, so that another board's tree is never the one it takes.
#
dtb-$(CONFIG_MACH_REALTEK_RTL)	+= cisco_sg220-26.dtb
dtb-$(CONFIG_MACH_REALTEK_RTL)	+= cameo-rtl9302c-2x-rtl8224-2xge.dtb
dtb-$(CONFIG_RTL9602C)		+= dfp-34x-2c3.dtb
