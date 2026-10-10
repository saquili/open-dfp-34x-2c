# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# The small programs of the ONU.
#
#   make          the programs, in build/
#   make check    the tests
#
# CC, CFLAGS and LDFLAGS come from the caller for a build for the board.

CC      ?= cc
CFLAGS  ?= -O2 -g
BUILD   ?= build

override CPPFLAGS += -Isrc
override WARN := -std=gnu11 -Wall -Wextra -Wmissing-prototypes -Wstrict-prototypes \
                 -Wshadow -Wpointer-arith -Wwrite-strings

# pon includes linux/pon.h of this repository's kernel: for a build on the
# host, point CPPFLAGS at an installation of its headers.
.PHONY: all check clean

all: $(BUILD)/ponid $(BUILD)/pon $(BUILD)/onucfg $(BUILD)/onu-update $(BUILD)/onu-svc

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(BUILD)/ponid: $(BUILD)/src/ponid.o $(BUILD)/src/factory.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lz

$(BUILD)/pon: $(BUILD)/src/pon.o $(BUILD)/src/pondev.o $(BUILD)/src/genl.o \
	       $(BUILD)/src/gemlink.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lmnl

$(BUILD)/onucfg: $(BUILD)/src/onucfg.o $(BUILD)/src/cfgstore.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lz

$(BUILD)/onu-update: $(BUILD)/src/onu-update.o $(BUILD)/src/bootenv.o \
		     $(BUILD)/src/update.o $(BUILD)/src/sha256.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lz

# onu-svc talks to procd over ubus: libubus, libubox and libubox's JSON part.
$(BUILD)/onu-svc: $(BUILD)/src/onu-svc.o $(BUILD)/src/svcmsg.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lubus -lblobmsg_json -lubox

$(BUILD)/test_svcmsg: $(BUILD)/tests/test_svcmsg.o $(BUILD)/src/svcmsg.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_update: $(BUILD)/tests/test_update.o $(BUILD)/src/bootenv.o \
		      $(BUILD)/src/update.o $(BUILD)/src/sha256.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lz

$(BUILD)/test_factory: $(BUILD)/tests/test_factory.o $(BUILD)/src/factory.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lz

$(BUILD)/test_pondev: $(BUILD)/tests/test_pondev.o $(BUILD)/src/pondev.o \
		      $(BUILD)/src/gemlink.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lmnl

$(BUILD)/test_genl: $(BUILD)/tests/test_genl.o $(BUILD)/src/genl.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lmnl

$(BUILD)/test_cfgstore: $(BUILD)/tests/test_cfgstore.o $(BUILD)/src/cfgstore.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lz

# test_genl asks the kernel it runs on, through a generic netlink socket.
check: $(BUILD)/test_factory $(BUILD)/test_pondev $(BUILD)/test_genl \
	$(BUILD)/test_cfgstore $(BUILD)/onucfg $(BUILD)/test_update $(BUILD)/onu-update \
	$(BUILD)/test_svcmsg
	$(BUILD)/test_factory
	$(BUILD)/test_pondev
	$(BUILD)/test_genl
	$(BUILD)/test_cfgstore
	$(BUILD)/test_update
	$(BUILD)/test_svcmsg
	BUILD=$(abspath $(BUILD)) sh tests/test_onu_update.sh
	sh tests/test_onu_trial.sh
	sh tests/test_onu_lane.sh
	sh tests/test_onu_mgmt.sh
	sh tests/test_onu_net.sh
	sh tests/test_onu_run.sh
	sh tests/test_onu_mode.sh
	BUILD=$(abspath $(BUILD)) sh tests/test_onu_store.sh

clean:
	rm -rf $(BUILD)
