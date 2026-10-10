# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# pond, the HTTP status daemon of the ONU.
#
#   make          build/pond
#   make check    the tests, on the files of tests/fixtures
#
# CC, CFLAGS and LDFLAGS come from the caller for a build for the board. The
# HTTP layer is Mongoose, in third_party/mongoose, built as it comes; pond's
# own sources are in src/. On Linux, pond reads netlink with libmnl and the
# kernel's headers, among them linux/pon.h of this repository's kernel, and
# checks root's password with crypt() of libcrypt.

CC      ?= cc
CFLAGS  ?= -O2 -g
BUILD   ?= build

override CPPFLAGS += -Isrc -Ithird_party/mongoose
override WARN := -std=gnu11 -Wall -Wextra -Wmissing-prototypes -Wstrict-prototypes \
                 -Wshadow -Wpointer-arith -Wwrite-strings
MG_FLAGS := -DMG_TLS=0 -DMG_ENABLE_PACKED_FS=0 -DMG_MAX_RECV_SIZE=262144
LDLIBS  += -lmnl -lcrypt

PROV := prov.o prov_file.o prov_net.o prov_mtd.o prov_netlink.o prov_settings.o \
        prov_identity.o prov_firmware.o prov_omci.o prov_system.o table.o
OBJS := $(addprefix $(BUILD)/src/,pond.o auth.o changes.o firmware.o mgmt.o $(PROV)) $(BUILD)/mongoose.o

.PHONY: all check clean

all: $(BUILD)/pond

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(BUILD)/mongoose.o: third_party/mongoose/mongoose.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MG_FLAGS) -w -c -o $@ $<

$(BUILD)/pond: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/tests/test_prov.o: tests/test_prov.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(BUILD)/test_prov: $(BUILD)/tests/test_prov.o $(addprefix $(BUILD)/src/,$(PROV))
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/tests/test_auth.o: tests/test_auth.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(BUILD)/test_auth: $(BUILD)/tests/test_auth.o $(addprefix $(BUILD)/src/,auth.o $(PROV)) \
		    $(BUILD)/mongoose.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# The settings' rules are onu-config's, of onu-utils (ADR 0014): the tests of the
# settings run it, built here from its sources.
ONU_UTILS ?= ../onu-utils
ONU_CONFIG_SRC := $(addprefix $(ONU_UTILS)/src/,onu-config.c cfgstore.c settings.c)

$(BUILD)/onu-config: $(ONU_CONFIG_SRC) $(wildcard $(ONU_UTILS)/src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -I$(ONU_UTILS)/src $(CFLAGS) $(WARN) -o $@ $(ONU_CONFIG_SRC) -lz

check: all $(BUILD)/test_prov $(BUILD)/test_auth $(BUILD)/onu-config
	rm -rf $(BUILD)/fixtures && cp -R tests/fixtures $(BUILD)/fixtures
	python3 tests/make-env.py $(BUILD)/fixtures
	POND_ROOT=$(abspath $(BUILD))/fixtures POND_ONU_CONFIG=$(abspath $(BUILD))/onu-config \
		$(BUILD)/test_prov
	rm -rf $(BUILD)/auth && mkdir -p $(BUILD)/auth
	POND_ROOT=$(abspath $(BUILD))/auth $(BUILD)/test_auth

clean:
	rm -rf $(BUILD)
