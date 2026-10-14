# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# timed, the clock of the stick: it sets the clock from the host by NTP or PTP.
#
#   make          build/timed
#   make check    the tests of the protocols, on any host
#
# CC, CFLAGS and LDFLAGS come from the caller for a build for the board.

CC      ?= cc
CFLAGS  ?= -O2 -g
BUILD   ?= build

override CPPFLAGS += -Isrc
override WARN := -std=gnu11 -Wall -Wextra -Wmissing-prototypes -Wstrict-prototypes \
                 -Wshadow -Wpointer-arith -Wwrite-strings

PROTO := $(addprefix $(BUILD)/src/,ntp.o olt.o phc.o pon.o ptp.o select.o servo.o stamp.o)

.PHONY: all check clean

all: $(BUILD)/timed

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(BUILD)/timed: $(BUILD)/src/main.o $(PROTO)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_timed: $(BUILD)/tests/test_timed.o $(PROTO)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

check: $(BUILD)/test_timed
	$(BUILD)/test_timed

clean:
	rm -rf $(BUILD)
