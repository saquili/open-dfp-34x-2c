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
 * @none: no value, null in JSON
 */
struct omci_publish_ops {
	void (*open)(void *ctx, const char *name, bool array);
	void (*close)(void *ctx, bool array);
	void (*str)(void *ctx, const char *name, const char *value);
	void (*num)(void *ctx, const char *name, uint32_t value);
	void (*flag)(void *ctx, const char *name, bool value);
	void (*none)(void *ctx, const char *name);
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

/**
 * omci_publish_services() - Write the rules of the services in force at the
 *	last programming of the VLAN rules (me/svcrule.h): {"rules":[{"fibre":
 *	F,"action":"move","host":H,"auto":B},...]}, a person's first, then the
 *	moves the programming made for clashes; "host" of a move alone.
 * @ops: the writer
 * @ctx: passed to @ops
 */
void omci_publish_services(const struct omci_publish_ops *ops, void *ctx);

/**
 * omci_publish_login() - Write the PPPoE login an OLT sent (Authentication
 *	security method, class 148, G.988 9.12.4): {"user":U,"secret":S,
 *	"realm":R} of the first instance whose username is not blank and
 *	that is no SIP account (omci_publish_sip_account()), its usernames 1
 *	and 2 together; {} when there is none.
 * @mib: the MIB
 * @ops: the writer
 * @ctx: passed to @ops
 *
 * A byte that is no printable ASCII ends a field.
 */
void omci_publish_login(const struct omci_mib *mib,
			const struct omci_publish_ops *ops, void *ctx);

/**
 * omci_publish_phone() - Write the phone the OLT set up on the phone
 *	sockets (G.988 9.9, sys/phone.c), the shape of pond's value "phone":
 *	{"method":M,"lines":[{"port":P,"number":N,"display":D,"user":U,
 *	"password_set":B},...],"server":{"proxy","outbound_proxy","registrar",
 *	"domain","port","transport"},"codecs":[...],"dtmf":T,"vlan":{"fibre",
 *	"host"}}, null for what is not there and never a password.
 * @mib: the MIB
 * @ops: the writer
 * @ctx: passed to @ops
 *
 * The method is "tr069" when VoIP config data says the OLT sets the phone
 * up through TR-069, "omci" when through OMCI or when it sent a SIP account,
 * "none" otherwise. A line is a POTS UNI, numbered by its port, with its SIP
 * user data's account; the server, codecs and DTMF are those of the first.
 */
void omci_publish_phone(const struct omci_mib *mib,
			const struct omci_publish_ops *ops, void *ctx);

/**
 * omci_publish_phone_secrets() - Write the SIP passwords of the lines:
 *	{"lines":[{"port":P,"secret":S},...]}, of each POTS UNI with SIP user
 *	data.
 * @mib: the MIB
 * @ops: the writer
 * @ctx: passed to @ops
 */
void omci_publish_phone_secrets(const struct omci_mib *mib,
				const struct omci_publish_ops *ops, void *ctx);

/**
 * omci_publish_sip_account() - Whether an authentication security method is
 *	a SIP account: SIP user data points at it.
 * @mib: the MIB
 * @auth: its instance
 *
 * Return: true when it is.
 */
bool omci_publish_sip_account(const struct omci_mib *mib, uint16_t auth);

#endif
