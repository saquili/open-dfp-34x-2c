/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The messages of the kernel's "pon" generic netlink family, read into plain
 * structures and printed for a person: a PON device with its ONU, and a
 * T-CONT. The family is described in the kernel's
 * Documentation/netlink/specs/pon.yaml.
 */

#ifndef ONU_UTILS_PONDEV_H
#define ONU_UTILS_PONDEV_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <linux/netlink.h>
#include <linux/pon.h>

#define PON_SERIAL_LEN 8

/**
 * struct pon_dev - A PON device as one message reports it.
 * @have: bit N set when attribute N of the message was there
 * @id: the device's number
 * @ifindex: its network interface
 * @type: an &enum pon_type
 * @state: an &enum pon_onu_state
 * @onu_id: the ONU-ID the OLT assigned
 * @alloc_id: the default Alloc-ID
 * @omcc: the Port-ID of the OMCI channel
 * @alarms: an &enum pon_alarm mask
 * @gem_ports: the GEM ports set up
 * @tconts: the T-CONTs
 * @tcont_queues: the upstream queues of each T-CONT
 * @power_level: the transmit power level, an &enum pon_power_level
 * @serial: the serial number
 * @stats_have: bit N set when counter N was there
 * @stats: the counters, by attribute number
 */
struct pon_dev {
	uint32_t have;
	uint32_t id, ifindex, type, state, onu_id, alloc_id, omcc, alarms;
	uint32_t gem_ports, tconts, tcont_queues, power_level;
	uint8_t serial[PON_SERIAL_LEN];
	uint32_t stats_have;
	uint64_t stats[PON_A_DEV_STATS_MAX + 1];
};

/**
 * struct pon_tcont - A T-CONT as one message reports it.
 * @have: bit N set when attribute N of the message was there
 * @dev: the device's number
 * @index: the T-CONT's index on the device
 * @alloc_id: the Alloc-ID the OLT bound to it
 */
struct pon_tcont {
	uint32_t have;
	uint32_t dev, index, alloc_id;
};

/* Read a device message; -EINVAL when it is malformed. */
int pon_dev_parse(const struct nlmsghdr *nlh, struct pon_dev *d);
/* Read a T-CONT message; -EINVAL when it is malformed. */
int pon_tcont_parse(const struct nlmsghdr *nlh, struct pon_tcont *t);

/* The name of an ONU state, "O5 operation"; "unknown" for another value. */
const char *pon_state_name(uint32_t state);

/*
 * Print a device for a person. The serial number, which the OLT takes as
 * the credential of the line, is printed only when @serial is true.
 */
void pon_dev_print(FILE *f, const struct pon_dev *d, const char *ifname,
		   bool serial);
void pon_tcont_print(FILE *f, const struct pon_tcont *t);

#endif
