# SPDX-License-Identifier: GPL-2.0

# The tests of the PON class over the stand-in MAC, drivers/net/pon/pon_sim.c.
# Not in the selftests' TARGETS: run them with
#
#   make -C tools/testing/selftests TARGETS=drivers/net/pon run_tests
#
# on a kernel with the options of "config", as root.

TEST_PROGS := pon.py

TEST_INCLUDES := $(wildcard ../../../net/lib/py/*.py)

include ../../../lib.mk
