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

all: $(BUILD)/ponid $(BUILD)/pon $(BUILD)/onu-config $(BUILD)/onu-update $(BUILD)/onu-svc \
	$(BUILD)/onu-act $(BUILD)/onu-nl $(BUILD)/onu-drop $(BUILD)/onu-call $(BUILD)/onu-fw

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(BUILD)/ponid: $(BUILD)/src/ponid.o $(BUILD)/src/factory.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lz

$(BUILD)/pon: $(BUILD)/src/pon.o $(BUILD)/src/pondev.o $(BUILD)/src/genl.o \
	       $(BUILD)/src/gemlink.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lmnl

# onu-nl sets traffic control and devlink parameters in place of tc and
# devlink: libmnl.
$(BUILD)/onu-nl: $(BUILD)/src/onu-nl.o $(BUILD)/src/tcrule.o $(BUILD)/src/dlparam.o \
		$(BUILD)/src/linkmsg.o $(BUILD)/src/genl.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lmnl

# onu-fw puts the firewall's defaults into nf_tables, and its rules into the
# switch's ACL as tc flower filters: libmnl.
$(BUILD)/onu-fw: $(BUILD)/src/onu-fw.o $(BUILD)/src/fwmsg.o $(BUILD)/src/fwacl.o \
		$(BUILD)/src/aclmsg.o $(BUILD)/src/tcrule.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lmnl

$(BUILD)/test_fwacl: $(BUILD)/tests/test_fwacl.o $(BUILD)/src/fwacl.o $(BUILD)/src/aclmsg.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lmnl

$(BUILD)/test_fwmsg: $(BUILD)/tests/test_fwmsg.o $(BUILD)/src/fwmsg.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lmnl

$(BUILD)/test_tcrule: $(BUILD)/tests/test_tcrule.o $(BUILD)/src/tcrule.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lmnl

$(BUILD)/test_linkmsg: $(BUILD)/tests/test_linkmsg.o $(BUILD)/src/linkmsg.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lmnl

$(BUILD)/test_dlparam: $(BUILD)/tests/test_dlparam.o $(BUILD)/src/dlparam.o \
		      $(BUILD)/src/genl.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lmnl

$(BUILD)/onu-config: $(BUILD)/src/onu-config.o $(BUILD)/src/cfgstore.o $(BUILD)/src/settings.o \
		    $(BUILD)/src/password.o $(BUILD)/src/sha256.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lz -lcrypt

$(BUILD)/test_password: $(BUILD)/tests/test_password.o $(BUILD)/src/password.o \
		       $(BUILD)/src/sha256.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lcrypt

$(BUILD)/onu-update: $(BUILD)/src/onu-update.o $(BUILD)/src/bootenv.o \
		     $(BUILD)/src/update.o $(BUILD)/src/sha256.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lz

# onu-call asks onu-act for a method over ubus: libubus and libubox.
$(BUILD)/onu-call: $(BUILD)/src/onu-call.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lubus -lubox

# onu-drop starts a program as an account of its own, with the powers it needs.
$(BUILD)/onu-drop: $(BUILD)/src/onu-drop.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# onu-svc talks to procd over ubus: libubus and libubox.
$(BUILD)/onu-svc: $(BUILD)/src/onu-svc.o $(BUILD)/src/svcmsg.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lubus -lubox

# onu-act serves its table (act.c) on ubus: libubus and libubox.
$(BUILD)/onu-act: $(BUILD)/src/onu-act.o $(BUILD)/src/act.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lubus -lubox

$(BUILD)/test_act: $(BUILD)/tests/test_act.o $(BUILD)/src/act.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

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

# mkslots writes a store of before step 9 for test_onu_config.sh.
$(BUILD)/mkslots: $(BUILD)/tests/mkslots.o $(BUILD)/src/cfgstore.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lz

$(BUILD)/test_cfgstore: $(BUILD)/tests/test_cfgstore.o $(BUILD)/src/cfgstore.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lz

$(BUILD)/test_settings: $(BUILD)/tests/test_settings.o $(BUILD)/src/settings.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# test_genl asks the kernel it runs on, through a generic netlink socket;
# test_onu_nl.sh sets filters on it, and test_onu_fw.sh puts a table of
# nf_tables in place; both skip without the right to.
check: $(BUILD)/onu-drop $(BUILD)/test_factory $(BUILD)/test_pondev $(BUILD)/test_genl \
	$(BUILD)/test_tcrule $(BUILD)/test_linkmsg $(BUILD)/test_dlparam $(BUILD)/onu-nl \
	$(BUILD)/test_cfgstore $(BUILD)/onu-config $(BUILD)/mkslots $(BUILD)/test_update \
	$(BUILD)/onu-update \
	$(BUILD)/test_svcmsg $(BUILD)/test_settings $(BUILD)/test_act $(BUILD)/test_password \
	$(BUILD)/test_fwmsg $(BUILD)/test_fwacl $(BUILD)/onu-fw
	$(BUILD)/test_factory
	$(BUILD)/test_pondev
	$(BUILD)/test_genl
	$(BUILD)/test_tcrule
	$(BUILD)/test_linkmsg
	$(BUILD)/test_dlparam
	$(BUILD)/test_cfgstore
	$(BUILD)/test_update
	$(BUILD)/test_svcmsg
	$(BUILD)/test_settings
	$(BUILD)/test_act
	$(BUILD)/test_password
	$(BUILD)/test_fwmsg
	$(BUILD)/test_fwacl
	BUILD=$(abspath $(BUILD)) sh tests/test_onu_config_settings.sh
	BUILD=$(abspath $(BUILD)) sh tests/test_onu_update.sh
	BUILD=$(abspath $(BUILD)) sh tests/test_onu_drop.sh
	python3 -I tests/test_acl.py $(abspath ..)
	sh tests/test_onu_trial.sh
	sh tests/test_onu_lane.sh
	sh tests/test_onu_change.sh
	sh tests/test_onu_mgmt.sh
	sh tests/test_onu_net.sh
	sh tests/test_onu_run.sh
	sh tests/test_onu_apply.sh
	BUILD=$(abspath $(BUILD)) sh tests/test_onu_config.sh
	BUILD=$(abspath $(BUILD)) sh tests/test_onu_nl.sh
	BUILD=$(abspath $(BUILD)) sh tests/test_onu_fw.sh
	BUILD=$(abspath $(BUILD)) sh tests/test_onu_fw_acl.sh

clean:
	rm -rf $(BUILD)
