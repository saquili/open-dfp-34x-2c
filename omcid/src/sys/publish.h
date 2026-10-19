/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * What the daemon publishes of what it holds (ADR 0011): the instances of
 * the MIB, every attribute in hexadecimal and a table's rows end to end,
 * and the counts of the frames. The ONU-G's logical password is never
 * written, only whether one is set. It is written through a writer: on the
 * board straight into the messages of ubus (blobmsg, target/bus_ubus.c),
 * with no text in between; in the tests into text.
 */

#ifndef OMCI_SYS_PUBLISH_H
#define OMCI_SYS_PUBLISH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct omci_mib;
struct omci_ctl_counters;

/**
 * struct omci_publish_ops - A writer of tables, arrays and values.
 * @open: begin a table, or an array when @array; @name is NULL inside an
 *	array
 * @close: end the table, or the array, last begun
 * @str: a string
 * @num: a number
 * @flag: true or false
 */
struct omci_publish_ops {
	void (*open)(void *ctx, const char *name, bool array);
	void (*close)(void *ctx, bool array);
	void (*str)(void *ctx, const char *name, const char *value);
	void (*num)(void *ctx, const char *name, uint32_t value);
	void (*flag)(void *ctx, const char *name, bool value);
};

/**
 * omci_publish_mib() - Write the MIB.
 * @mib: the MIB
 * @counters: the counts of the frames
 * @classes: the classes to write, in that order, or NULL for every
 *	instance in the order of a walk
 * @n: the number of @classes
 * @max: the most bytes the reader takes as JSON text, or 0 for no limit
 * @ops: the writer
 * @ctx: passed to @ops
 *
 * What is written is, as JSON, {"mes":[{"class":C,"instance":I,"attrs":
 * {"1":"hex",...},"tables":[A,...]},...],"truncated":B,
 * "logical_password_set":B,"frames":{"handled":N,"rejected":N,
 * "answered":N}}. An instance whose text would go past @max is left out,
 * with the rest, and "truncated" is true.
 *
 * Return: the length of the JSON text of what was written.
 */
size_t omci_publish_mib(const struct omci_mib *mib,
			const struct omci_ctl_counters *counters,
			const uint16_t *classes, size_t n, size_t max,
			const struct omci_publish_ops *ops, void *ctx);

/**
 * omci_publish_status() - Write the daemon's state: {"instances":N,
 *	"frames":{"handled":N,"rejected":N,"answered":N}}.
 * @mib: the MIB
 * @counters: the counts of the frames
 * @ops: the writer
 * @ctx: passed to @ops
 */
void omci_publish_status(const struct omci_mib *mib,
			 const struct omci_ctl_counters *counters,
			 const struct omci_publish_ops *ops, void *ctx);

#endif
