/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Requests to a generic netlink family with libmnl: build one, send it and
 * read its answer to the end, and find a family's number, and the number of
 * one of its multicast groups, by name.
 */

#ifndef ONU_UTILS_GENL_H
#define ONU_UTILS_GENL_H

#include <stdint.h>

#include <libmnl/libmnl.h>

/*
 * A request in @buf, of MNL_SOCKET_BUFFER_SIZE bytes. One that is not a dump
 * asks for the kernel's acknowledgement, so that its answer has an end.
 */
struct nlmsghdr *genl_request(char *buf, uint16_t type, uint8_t cmd,
			      uint16_t flags, uint32_t seq);

/* Send a request and run @cb on every message of the answer, to its end. */
int genl_talk(struct mnl_socket *nl, struct nlmsghdr *nlh, mnl_cb_t cb,
	      void *data);

/*
 * The number of the family @name and, unless @group is NULL, of its multicast
 * group @group in @group_id (0 when the family has no such group); -ENOENT
 * when the kernel has no such family.
 */
int genl_resolve(struct mnl_socket *nl, const char *name, const char *group,
		 uint16_t *id, uint32_t *group_id);

#endif
