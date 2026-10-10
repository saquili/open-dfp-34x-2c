# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# dhcp6d, the IPv6 of a PPPoE session handed on: DHCPv6 prefix delegation
# and router advertisements.
#
#   make          build/dhcp6d
#   make check    the tests, and dhcp6d-none for tests/netns.sh
#
# CC, CFLAGS and LDFLAGS come from the caller for a build for the board.

CC      ?= cc
CFLAGS  ?= -O2 -g
BUILD   ?= build

override CPPFLAGS += -Isrc
override WARN := -std=gnu11 -Wall -Wextra -Wmissing-prototypes -Wstrict-prototypes \
                 -Wshadow -Wpointer-arith -Wwrite-strings

PROTO := $(addprefix $(BUILD)/src/,msg.o client.o server.o ra.o)
DAEMON := $(BUILD)/src/main.o $(PROTO)

.PHONY: all check clean

all: $(BUILD)/dhcp6d

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

# dhcp6d publishes its state on ubus (bus.h): libubus and libubox.
# dhcp6d-none, which the tests build on a host without them, publishes
# nothing.
$(BUILD)/dhcp6d: $(DAEMON) $(BUILD)/src/bus-ubus.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lubus -lubox

$(BUILD)/dhcp6d-none: $(DAEMON) $(BUILD)/src/bus-none.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_dhcp6: $(BUILD)/tests/test_dhcp6.o $(PROTO) $(BUILD)/tests/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

check: $(BUILD)/dhcp6d-none $(BUILD)/test_dhcp6
	$(BUILD)/test_dhcp6

clean:
	rm -rf $(BUILD)
