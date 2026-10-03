# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# pppoed, the PPPoE client of the ONU.
#
#   make          build/pppoed
#   make check    the tests
#
# CC, CFLAGS and LDFLAGS come from the caller for a build for the board.

CC      ?= cc
CFLAGS  ?= -O2 -g
BUILD   ?= build

override CPPFLAGS += -Isrc
override WARN := -std=gnu11 -Wall -Wextra -Wmissing-prototypes -Wstrict-prototypes \
                 -Wshadow -Wpointer-arith -Wwrite-strings

OBJS := $(addprefix $(BUILD)/src/,main.o disc.o ppp.o session.o md5.o)

.PHONY: all check clean

all: $(BUILD)/pppoed

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(BUILD)/pppoed: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_md5: $(BUILD)/tests/test_md5.o $(BUILD)/src/md5.o $(BUILD)/tests/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_ncp: $(BUILD)/tests/test_ncp.o $(addprefix $(BUILD)/src/,session.o ppp.o disc.o md5.o) \
		   $(BUILD)/tests/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_disc: $(BUILD)/tests/test_disc.o $(BUILD)/tests/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

check: $(BUILD)/test_md5 $(BUILD)/test_ncp $(BUILD)/test_disc
	$(BUILD)/test_md5
	$(BUILD)/test_ncp
	$(BUILD)/test_disc

clean:
	rm -rf $(BUILD)
