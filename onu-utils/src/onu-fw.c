// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * onu-fw: the stick's own tables of nf_tables (fwmsg.h): "inet onu", which
 * onu-apply puts in place for the local networks, and the guard of the
 * stick's own programs, "inet onu_guard", which it puts in place always.
 *
 *   onu-fw apply FILE   put the table that FILE describes in place of the one
 *                       there is
 *   onu-fw clear        take the table away
 *   onu-fw guard FILE   put the guard that FILE describes in place of the one
 *                       there is
 *   onu-fw acl FILE STATUS
 *                       put the firewall's rules that FILE describes into
 *                       the switch's ACL (fwacl.h) in place of those there
 *                       are, and write what became of each rule to STATUS
 *   onu-fw acl-show FILE
 *                       print the tc commands that would add them, and what
 *                       became of each rule, without asking the kernel
 *   onu-fw acl-clear    take the rules out of the ACL
 *
 * The kernel takes a batch whole or not at all, so a table that fails leaves
 * the one there was. When the kernel refuses the flowtable, as one whose
 * devices cannot carry flows does, the table is put in place without it, and
 * the CPU carries the flows. Exit status 0, 1 when the kernel refused the
 * table, 2 for a file or a command that is none. The guard is taken whole or
 * not at all too, with the same exit status.
 *
 * The rules go into the ACL as flower filters on the ingress of sfp and pon,
 * at the priorities ACL_PREF to ACL_PREF + ACL_PREFS - 1, which no other
 * writer takes. Those there are go first, then the new ones are added, a
 * rule's at a time, in the rules' order: for the few milliseconds between,
 * no rule holds. The kernel takes a filter at a time, and a rule of which it
 * refuses an entry is taken out again whole; it and every rule after it,
 * which may count on it, are left out: "room" where the ACL had no entry or
 * template left (ENOSPC), else "refused" for it and "after" for the rest.
 * Exit status 0 when every rule was put in that had room; 1 when the kernel
 * refused an entry for another reason, or sfp or pon is not there.
 * ONU_FW_ACL_SW set keeps the filters in software too, without skip_sw, for a
 * test on interfaces of no switch.
 */

#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#include <libmnl/libmnl.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/rtnetlink.h>

#include "aclmsg.h"
#include "fwacl.h"
#include "fwmsg.h"
#include "tcrule.h"

/* A batch and room for the last request of it (mnl_nlmsg_batch_start()). */
static char g_buf[2 * FW_BATCH_MAX];

static void print_usage(void)
{
	fputs("usage: onu-fw apply FILE | clear | guard FILE | acl FILE STATUS | acl-show FILE | acl-clear\n",
	      stderr);
}

/*
 * Send the batch of @len bytes, whose last acknowledged request is @last,
 * and read the acknowledgements: 0, or a negative errno, the first the
 * kernel gave.
 */
static int run(size_t len, uint32_t last)
{
	struct timeval tv = { .tv_sec = 2 };
	struct mnl_socket *nl = mnl_socket_open(NETLINK_NETFILTER);
	char ack[MNL_SOCKET_BUFFER_SIZE];
	int sndbuf = (int)len, rc = -ETIMEDOUT;

	if (!nl || mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		rc = -errno;
		goto out;
	}
	setsockopt(mnl_socket_get_fd(nl), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	/*
	 * The kernel takes a message no longer than the socket's send buffer,
	 * some 200 KiB by default (net.core.wmem_default): a batch with every
	 * forward and every port moved goes past it, at 255 KiB. The buffer
	 * asked for is doubled, up to net.core.wmem_max, 4 MiB.
	 */
	setsockopt(mnl_socket_get_fd(nl), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
	if (mnl_socket_sendto(nl, g_buf, len) < 0) {
		rc = -errno;
		goto out;
	}
	for (;;) {
		ssize_t n = mnl_socket_recvfrom(nl, ack, sizeof(ack));
		struct nlmsghdr *nlh = (struct nlmsghdr *)ack;
		int left = (int)n;

		if (n < 0) {
			rc = errno == EAGAIN ? -ETIMEDOUT : -errno;
			goto out;
		}
		for (; mnl_nlmsg_ok(nlh, left); nlh = mnl_nlmsg_next(nlh, &left)) {
			const struct nlmsgerr *e = mnl_nlmsg_get_payload(nlh);

			if (nlh->nlmsg_type != NLMSG_ERROR)
				continue;
			if (e->error) {
				rc = e->error;
				goto out;
			}
			if (nlh->nlmsg_seq == last) {
				rc = 0;
				goto out;
			}
		}
	}
out:
	if (nl)
		mnl_socket_close(nl);
	return rc;
}

static int apply(const struct fw_spec *s)
{
	uint32_t seq = (uint32_t)time(NULL), last;
	size_t len = fw_batch(s, g_buf, sizeof(g_buf), seq, &last);

	if (!len) {
		fputs("onu-fw: the table does not fit a batch\n", stderr);
		return -E2BIG;
	}
	return run(len, last);
}

static int apply_file(const char *path)
{
	char line[256], err[128];
	struct fw_spec s;
	FILE *f = fopen(path, "r");
	int rc;

	if (!f) {
		fprintf(stderr, "onu-fw: %s: %s\n", path, strerror(errno));
		return 2;
	}
	fw_spec_init(&s);
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (fw_spec_line(&s, line, err, sizeof(err))) {
			fprintf(stderr, "onu-fw: %s: %s\n", path, err);
			fclose(f);
			return 2;
		}
	}
	fclose(f);

	rc = apply(&s);
	if (rc && s.offload) {
		fprintf(stderr,
			"onu-fw: the flowtable was refused (%s): the CPU carries the flows\n",
			strerror(-rc));
		s.offload = false;
		rc = apply(&s);
	}
	if (rc) {
		fprintf(stderr, "onu-fw: the kernel refused the table: %s\n", strerror(-rc));
		return 1;
	}
	return 0;
}

/*
 * The guard of FILE in place of the one there is, which a guard that fails
 * leaves: 0, 1 when the kernel refused it, 2 for a file that is none.
 */
static int guard_file(const char *path)
{
	static struct fw_guard g;
	char line[256], err[128];
	uint32_t last;
	size_t len;
	FILE *f = fopen(path, "r");
	int rc;

	if (!f) {
		fprintf(stderr, "onu-fw: %s: %s\n", path, strerror(errno));
		return 2;
	}
	fw_guard_init(&g);
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (fw_guard_line(&g, line, err, sizeof(err))) {
			fprintf(stderr, "onu-fw: %s: %s\n", path, err);
			fclose(f);
			return 2;
		}
	}
	fclose(f);
	if (!g.nmgmt) {
		fprintf(stderr, "onu-fw: %s: no management interface: the guard would keep everyone off\n",
			path);
		return 2;
	}
	len = fw_guard_batch(&g, g_buf, sizeof(g_buf), (uint32_t)time(NULL), &last);
	if (!len) {
		fputs("onu-fw: the guard does not fit a batch\n", stderr);
		return 1;
	}
	rc = run(len, last);
	if (rc) {
		fprintf(stderr, "onu-fw: the kernel refused the guard: %s\n", strerror(-rc));
		return 1;
	}
	return 0;
}

static int clear(void)
{
	uint32_t last;
	size_t len = fw_batch_clear(g_buf, sizeof(g_buf), (uint32_t)time(NULL), &last);
	int rc = run(len, last);

	if (rc) {
		fprintf(stderr, "onu-fw: the table was not taken away: %s\n", strerror(-rc));
		return 1;
	}
	return 0;
}

/* The firewall's rules in the switch's ACL. */

static struct acl_spec g_acl_spec;
static struct acl_set g_acl;
static uint32_t g_seq;

/*
 * What the kernel answered a request: its error and its message about it;
 * of a dump, the priorities of the firewall's range it lists.
 */
struct answer {
	int err;
	char why[160];
	uint16_t prefs[ACL_PREFS];
	int nprefs;
};

static int why_attr(const struct nlattr *a, void *data)
{
	struct answer *t = data;

	if (mnl_attr_get_type(a) == NLMSGERR_ATTR_MSG &&
	    mnl_attr_validate(a, MNL_TYPE_NUL_STRING) == 0)
		snprintf(t->why, sizeof(t->why), "%s", mnl_attr_get_str(a));
	return MNL_CB_OK;
}

/* The acknowledgement, or the error and the kernel's message about it. */
static int error_cb(const struct nlmsghdr *nlh, void *data)
{
	struct answer *t = data;
	const struct nlmsgerr *e = mnl_nlmsg_get_payload(nlh);
	size_t off = sizeof(*e);

	if (nlh->nlmsg_len < mnl_nlmsg_size(sizeof(*e))) {
		t->err = -EBADMSG;
		return MNL_CB_ERROR;
	}
	if (!(nlh->nlmsg_flags & NLM_F_CAPPED) && e->msg.nlmsg_len > sizeof(e->msg))
		off += e->msg.nlmsg_len - sizeof(e->msg);
	if (nlh->nlmsg_flags & NLM_F_ACK_TLVS && off < mnl_nlmsg_get_payload_len(nlh))
		mnl_attr_parse(nlh, (unsigned int)off, why_attr, t);
	if (!e->error)
		return MNL_CB_STOP;
	t->err = e->error;
	return MNL_CB_ERROR;
}

static int done_cb(const struct nlmsghdr *nlh, void *data)
{
	(void)nlh;
	(void)data;
	return MNL_CB_STOP;
}

/* The priorities of the firewall's range among the filters of a dump. */
static int pref_cb(const struct nlmsghdr *nlh, void *data)
{
	struct answer *t = data;
	unsigned int pref = tc_rule_pref(nlh);

	if (pref < ACL_PREF || pref >= ACL_PREF + ACL_PREFS)
		return MNL_CB_OK;
	for (int i = 0; i < t->nprefs; i++)
		if (t->prefs[i] == pref)
			return MNL_CB_OK;
	t->prefs[t->nprefs++] = (uint16_t)pref;
	return MNL_CB_OK;
}

/* Send @nlh and read its answer to the end into @t: 0, or a negative errno. */
static int talk(struct mnl_socket *nl, struct nlmsghdr *nlh, mnl_cb_t cb, struct answer *t)
{
	static const mnl_cb_t ctl[NLMSG_MIN_TYPE] = {
		[NLMSG_ERROR] = error_cb,
		[NLMSG_DONE] = done_cb,
	};
	char buf[MNL_SOCKET_BUFFER_SIZE];
	const uint32_t port = mnl_socket_get_portid(nl), seq = nlh->nlmsg_seq;
	int ret;

	t->err = 0;
	t->why[0] = '\0';
	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0)
		return t->err = -errno;
	do {
		ssize_t n = mnl_socket_recvfrom(nl, buf, sizeof(buf));

		if (n < 0)
			return t->err = -errno;
		errno = 0;
		ret = mnl_cb_run2(buf, (size_t)n, seq, port, cb, t, ctl, NLMSG_MIN_TYPE);
		if (ret < 0 && !t->err)
			t->err = errno ? -errno : -EPROTO;
	} while (ret > MNL_CB_STOP);
	return t->err;
}

static struct mnl_socket *route_bus(void)
{
	struct mnl_socket *nl = mnl_socket_open(NETLINK_ROUTE);
	int one = 1;

	if (!nl || mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		fprintf(stderr, "onu-fw: no netlink socket: %s\n", strerror(errno));
		if (nl)
			mnl_socket_close(nl);
		return NULL;
	}
	/* Errors without the request, and with the kernel's message. */
	mnl_socket_setsockopt(nl, NETLINK_CAP_ACK, &one, sizeof(one));
	mnl_socket_setsockopt(nl, NETLINK_EXT_ACK, &one, sizeof(one));
	return nl;
}

/* Take the filters of the firewall's range off the ingress of @ifindex. */
static void acl_remove(struct mnl_socket *nl, int ifindex)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct answer t = { 0 };

	/* Without the clsact qdisc there is no filter to take away. */
	if (talk(nl, tc_rule_dump_msg(buf, ifindex, ++g_seq), pref_cb, &t))
		return;
	for (int i = 0; i < t.nprefs; i++)
		talk(nl, tc_rule_del_msg(buf, ifindex, t.prefs[i], ++g_seq), NULL, &t);
}

/* Take the entries of @rule out, -1 for the passes of the local destinations. */
static void acl_remove_entries(struct mnl_socket *nl, const int ifindex[2], int rule)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct answer t;

	for (int i = 0; i < g_acl.n; i++)
		if (g_acl.e[i].rule == rule)
			talk(nl,
			     tc_rule_del_msg(buf, ifindex[g_acl.e[i].port], g_acl.e[i].pref, ++g_seq),
			     NULL, &t);
}

/* Add the entries of @rule, all or none: 0, or the kernel's error, in @t. */
static int acl_add_entries(struct mnl_socket *nl, const int ifindex[2], int rule, bool hw,
			   struct answer *t)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];

	for (int i = 0; i < g_acl.n; i++) {
		const struct acl_entry *e = &g_acl.e[i];

		if (e->rule != rule)
			continue;
		if (talk(nl, acl_entry_msg(buf, ifindex[e->port], e, hw, ++g_seq), NULL, t)) {
			struct answer keep = *t;

			acl_remove_entries(nl, ifindex, rule);
			*t = keep;
			return t->err;
		}
	}
	return 0;
}

static int acl_read(const char *path)
{
	char line[256], err[128];
	FILE *f = fopen(path, "r");

	if (!f) {
		fprintf(stderr, "onu-fw: %s: %s\n", path, strerror(errno));
		return 2;
	}
	acl_spec_init(&g_acl_spec);
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (acl_spec_line(&g_acl_spec, line, err, sizeof(err))) {
			fprintf(stderr, "onu-fw: %s: %s\n", path, err);
			fclose(f);
			return 2;
		}
	}
	fclose(f);
	acl_compile(&g_acl_spec, ACL_ROOM, &g_acl);
	return 0;
}

static int acl_status_write(const char *path)
{
	char tmp[256], line[128];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.new", path);
	f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "onu-fw: %s: %s\n", tmp, strerror(errno));
		return 1;
	}
	for (int i = 0; i < g_acl.nst; i++) {
		acl_status_format(&g_acl.st[i], line, sizeof(line));
		fprintf(f, "%s\n", line);
	}
	if (fclose(f) || rename(tmp, path)) {
		fprintf(stderr, "onu-fw: %s: %s\n", path, strerror(errno));
		return 1;
	}
	return 0;
}

/* Rule @r and the rules after it are left out, @err being what the kernel said of @r. */
static void acl_fail(int r, int err)
{
	for (int i = r; i < g_acl.nst; i++) {
		struct acl_status *st = &g_acl.st[i];

		if (st->state == ACL_OUT)
			continue;
		st->state = ACL_OUT;
		st->entries = 0;
		st->why = err == -ENOSPC ? "room" : i == r ? "refused" : "after";
	}
}

static int acl_file(const char *path, const char *status)
{
	const bool hw = !getenv("ONU_FW_ACL_SW");
	char buf[MNL_SOCKET_BUFFER_SIZE];
	int ifindex[2], rc = acl_read(path), failed = 0, r;
	bool passes = false, internet = false;
	struct mnl_socket *nl;
	struct answer t = { 0 };

	if (rc)
		return rc;
	ifindex[ACL_SFP] = (int)if_nametoindex("sfp");
	ifindex[ACL_PON] = (int)if_nametoindex("pon");
	if (!ifindex[ACL_SFP] || !ifindex[ACL_PON]) {
		fputs("onu-fw: no interface sfp or pon for the ACL\n", stderr);
		return 1;
	}
	nl = route_bus();
	if (!nl)
		return 1;
	for (int p = 0; p < 2; p++)
		acl_remove(nl, ifindex[p]);
	/* The qdisc the filters hang from, which may be there already. */
	for (int p = 0; p < 2 && g_acl.n && !failed; p++)
		if (talk(nl, tc_clsact_msg(buf, ifindex[p], true, ++g_seq), NULL, &t) &&
		    t.err != -EEXIST)
			failed = t.err;
	for (r = 0; r < g_acl.nst && !failed; r++) {
		const struct acl_status *st = &g_acl.st[r];

		if (st->state == ACL_OUT)
			continue;
		if (st->internet && !passes) {
			failed = acl_add_entries(nl, ifindex, -1, hw, &t);
			if (failed)
				break;
			passes = true;
		}
		failed = acl_add_entries(nl, ifindex, r, hw, &t);
		if (failed)
			break;
		internet |= st->internet;
	}
	if (failed) {
		if (r >= g_acl.nst)
			r = 0;
		acl_fail(r, failed);
		fprintf(stderr, "onu-fw: the ACL took rule %u and those after it not: %s%s%s\n",
			r < g_acl.nst ? g_acl.st[r].id : 0, strerror(-failed), t.why[0] ? ": " : "",
			t.why);
	}
	/* The passes are of no use without a rule to the internet after them. */
	if (passes && !internet)
		acl_remove_entries(nl, ifindex, -1);
	mnl_socket_close(nl);
	rc = acl_status_write(status);
	return rc || (failed && failed != -ENOSPC);
}

static int acl_show(const char *path)
{
	char line[256];
	int rc = acl_read(path);

	if (rc)
		return rc;
	for (int i = 0; i < g_acl.n; i++) {
		acl_entry_format(&g_acl.e[i], true, line, sizeof(line));
		printf("%s\n", line);
	}
	for (int i = 0; i < g_acl.nst; i++) {
		acl_status_format(&g_acl.st[i], line, sizeof(line));
		printf("%s\n", line);
	}
	return 0;
}

static int acl_clear(void)
{
	int ifindex[2] = { (int)if_nametoindex("sfp"), (int)if_nametoindex("pon") };
	struct mnl_socket *nl;

	if (!ifindex[ACL_SFP] && !ifindex[ACL_PON])
		return 0;
	nl = route_bus();
	if (!nl)
		return 1;
	for (int p = 0; p < 2; p++)
		if (ifindex[p])
			acl_remove(nl, ifindex[p]);
	mnl_socket_close(nl);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "apply"))
		return apply_file(argv[2]);
	if (argc == 2 && !strcmp(argv[1], "clear"))
		return clear();
	if (argc == 3 && !strcmp(argv[1], "guard"))
		return guard_file(argv[2]);
	if (argc == 4 && !strcmp(argv[1], "acl"))
		return acl_file(argv[2], argv[3]);
	if (argc == 3 && !strcmp(argv[1], "acl-show"))
		return acl_show(argv[2]);
	if (argc == 2 && !strcmp(argv[1], "acl-clear"))
		return acl_clear();
	print_usage();
	return 2;
}
