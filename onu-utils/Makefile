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
LDLIBS  += -lz

.PHONY: all check clean

all: $(BUILD)/ponid

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(BUILD)/ponid: $(BUILD)/src/ponid.o $(BUILD)/src/factory.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_factory: $(BUILD)/tests/test_factory.o $(BUILD)/src/factory.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

check: $(BUILD)/test_factory
	$(BUILD)/test_factory

clean:
	rm -rf $(BUILD)
