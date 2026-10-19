/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The parameters of a devlink instance, as the "devlink" generic netlink
 * family carries them (ADR 0017): the stick's one instance is its
 * switch's, whose parameters Documentation/networking/devlink/rtl9602c.rst
 * lists. A parameter's value is read and set in the runtime configuration
 * mode, the only one the switch driver has, as text: a number in decimal,
 * "true" or "false", or the string itself. A value to set is written in
 * the type the kernel names for the parameter, so it is read first.
 */

#ifndef ONU_UTILS_DLPARAM_H
#define ONU_UTILS_DLPARAM_H

#include <stdbool.h>
#include <stdint.h>

#include <linux/netlink.h>

/* The longest bus and device names, and the longest value as text. */
#define DL_NAME_MAX 64
#define DL_VALUE_MAX 64

/**
 * struct dl_dev - A devlink instance: its handle is "@bus/@dev".
 */
struct dl_dev {
	char bus[DL_NAME_MAX];
	char dev[DL_NAME_MAX];
};

/**
 * struct dl_param - A parameter, from the kernel's answer.
 * @type: its DEVLINK_VAR_ATTR_TYPE_*
 * @have: the answer gave its value in the runtime mode
 * @value: that value as text
 */
struct dl_param {
	uint8_t type;
	bool have;
	char value[DL_VALUE_MAX];
};

/*
 * Requests to the family @family in @buf, of MNL_SOCKET_BUFFER_SIZE bytes:
 * a dump of the instances, and the parameter @name of the instance @d.
 */
struct nlmsghdr *dl_dev_dump_msg(char *buf, uint16_t family, uint32_t seq);
struct nlmsghdr *dl_param_get_msg(char *buf, uint16_t family, uint32_t seq,
				  const struct dl_dev *d, const char *name);
/*
 * Set @name of @d, of type @type, to the value @text: -EINVAL when @text is
 * no value of that type, -ERANGE when it is a number too large for it,
 * -EOPNOTSUPP for a type this does not write.
 */
int dl_param_set_msg(char *buf, uint16_t family, uint32_t seq,
		     const struct dl_dev *d, const char *name, uint8_t type,
		     const char *text, struct nlmsghdr **out);

/* Read an instance of the dump: 0, or -EINVAL when it names none. */
int dl_dev_parse(const struct nlmsghdr *nlh, struct dl_dev *d);
/* Read the answer about a parameter: 0, or -EINVAL when malformed. */
int dl_param_parse(const struct nlmsghdr *nlh, struct dl_param *p);

#endif
