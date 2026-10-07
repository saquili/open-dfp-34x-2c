# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# dhcp6d, the IPv6 of a PPPoE session handed on: DHCPv6 prefix delegation
# and router advertisements.
#
#   make          build/dhcp6d
#   make check    the program and the tests
#
# CC, CFLAGS and LDFLAGS come from the caller for a build for the board.

CC      ?= cc
CFLAGS  ?= -O2 -g
BUILD   ?= build

override CPPFLAGS += -Isrc
override WARN := -std=gnu11 -Wall -Wextra -Wmissing-prototypes -Wstrict-prototypes \
                 -Wshadow -Wpointer-arith -Wwrite-strings

PROTO := $(addprefix $(BUILD)/src/,msg.o client.o server.o ra.o)

.PHONY: all check clean

all: $(BUILD)/dhcp6d

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(BUILD)/dhcp6d: $(BUILD)/src/main.o $(PROTO)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_dhcp6: $(BUILD)/tests/test_dhcp6.o $(PROTO) $(BUILD)/tests/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

check: all $(BUILD)/test_dhcp6
	$(BUILD)/test_dhcp6

clean:
	rm -rf $(BUILD)
