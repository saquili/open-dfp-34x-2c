/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The providers of table.c, and the decoders and rules behind them that the
 * tests call directly.
 */

#ifndef POND_PROVIDERS_H
#define POND_PROVIDERS_H

#include <stdbool.h>
#include <stddef.h>

#include "prov.h"

int prov_banks(struct prov_buf *out);
int prov_bridge(struct prov_buf *out);
int prov_clock(struct prov_buf *out);
int prov_cpu(struct prov_buf *out);
int prov_firmware(struct prov_buf *out);
int prov_gem_counters(struct prov_buf *out);
int prov_gem_ports(struct prov_buf *out);
int prov_health(struct prov_buf *out);
int prov_hostname(struct prov_buf *out);
int prov_hostname_write(const char *val, char *err, size_t errcap,
			prov_done done, void *ctx);
int prov_hostname_set(const char *val, char *err, size_t errcap);
int prov_mgmt(struct prov_buf *out);
int prov_product(struct prov_buf *out);
int prov_product_write(const char *val, char *err, size_t errcap,
		       prov_done done, void *ctx);
int prov_product_set(const char *val, char *err, size_t errcap);
int prov_sfp_lane(struct prov_buf *out);
int prov_sfp_lane_write(const char *val, char *err, size_t errcap,
			prov_done done, void *ctx);
int prov_sfp_lane_set(const char *val, char *err, size_t errcap);
int prov_tls(struct prov_buf *out);

/*
 * The provider "ssh": the ssh server's host key, the public keys root logs in
 * with and whether the server takes them alone, as onu-config ssh show says,
 * through onu-act (the method ssh_state), which alone reads them; and
 * SSH_KEYS_MAX, the most keys onu-config keeps (SSHKEY_MAX of
 * onu-utils/src/sshkey.h). The keys are those of the store, which the panel
 * changes, of the source "panel", and those of the image, which the server
 * takes too and nothing on the stick changes, of the source "image", which
 * do not count towards SSH_KEYS_MAX. Its answer is at most SSH_STATE_MAX
 * bytes.
 */
#define SSH_KEYS_MAX 8
#define SSH_STATE_MAX 8192
int prov_ssh(struct prov_buf *out);

/*
 * A key of onu-config ssh-keys, the line "TYPE BITS FINGERPRINT COMMENT"
 * after its "KEY=" or "IMAGE=", as {"type", "bits", "fingerprint",
 * "comment", "source"}, @source "panel" or "image".
 */
void ssh_key_json(struct prov_buf *out, const char *line, const char *source);
int prov_image(struct prov_buf *out);
int prov_kernel(struct prov_buf *out);
int prov_log(struct prov_buf *out);
int prov_memory(struct prov_buf *out);
int prov_model(struct prov_buf *out);
int prov_nl_fdb(const char *links, size_t llen, const char *fdb, size_t flen,
		struct prov_buf *out);
int prov_nl_filters(const char *filters, size_t len, struct prov_buf *out);
int prov_nl_gem_ports(const char *links, size_t len, struct prov_buf *out);
int prov_nl_links(const char *links, size_t llen, const char *addrs,
		  size_t alen, struct prov_buf *out);
int prov_nl_neighbours(const char *links, size_t llen, const char *neigh,
		       size_t nlen, struct prov_buf *out);
int prov_nl_routes(const char *links, size_t llen, const char *routes,
		   size_t rlen, struct prov_buf *out);
int prov_nl_onu(const char *links, size_t llen, const char *dev, size_t dlen,
		struct prov_buf *out);
int prov_nl_tconts(const char *dump, size_t dlen, struct prov_buf *out);
int prov_nl_vlans(const char *brlinks, size_t len, struct prov_buf *out);
int prov_network(struct prov_buf *out);
int prov_leases(struct prov_buf *out);
int prov_lan_write(const char *val, char *err, size_t errcap, prov_done done,
		   void *ctx);
int prov_lan_set(const char *val, char *err, size_t errcap);
int prov_ipoe_write(const char *val, char *err, size_t errcap, prov_done done,
		    void *ctx);
int prov_ipoe_set(const char *val, char *err, size_t errcap);
int prov_fixed_write(const char *val, char *err, size_t errcap,
		     prov_done done, void *ctx);
int prov_fixed_set(const char *val, char *err, size_t errcap);
int prov_forward_write(const char *val, char *err, size_t errcap,
		       prov_done done, void *ctx);
int prov_forward_set(const char *val, char *err, size_t errcap);
int prov_dmz_write(const char *val, char *err, size_t errcap, prov_done done,
		   void *ctx);
int prov_dmz_set(const char *val, char *err, size_t errcap);
int prov_rule_write(const char *val, char *err, size_t errcap, prov_done done,
		    void *ctx);
int prov_rule_set(const char *val, char *err, size_t errcap);
int prov_fw_write(const char *val, char *err, size_t errcap, prov_done done,
		  void *ctx);
int prov_fw_set(const char *val, char *err, size_t errcap);
int prov_omci(struct prov_buf *out);
int prov_omci_frames(struct prov_buf *out);
int prov_omci_frames_from(struct prov_buf *out, unsigned long since);
int prov_onu(struct prov_buf *out);
int prov_optics(struct prov_buf *out);
int prov_port_counters(struct prov_buf *out);
int prov_ports(struct prov_buf *out);
int prov_root_mtd(const char *cmdline);
int prov_rootfs_bank(const char *part);
int prov_settings(struct prov_buf *out);
int prov_settings_write(const char *val, char *err, size_t errcap,
			prov_done done, void *ctx);
int prov_settings_set(const char *val, char *err, size_t errcap);
void prov_settings_reap(void);
int prov_session_write(const char *val, char *err, size_t errcap,
		       prov_done done, void *ctx);
int prov_session_set(const char *val, char *err, size_t errcap);
int prov_identity(struct prov_buf *out);
int prov_identity_write(const char *val, char *err, size_t errcap,
			prov_done done, void *ctx);
int prov_identity_set(const char *val, char *err, size_t errcap);
int prov_tc_rules(struct prov_buf *out);
int prov_tconts(struct prov_buf *out);
int prov_uboot_bank(const char *value);
int prov_uptime(struct prov_buf *out);

/*
 * The product name: what the panel calls this system, in its title, and
 * what the files pond hands out are named after; PRODUCT_NAME of the
 * stick's own settings (/run/onu-system), which onu-config keeps and checks
 * as product_name_ok() does: 1 to %PRODUCT_NAME_MAX characters of ASCII,
 * letters, digits, blanks, hyphens and dots, a letter or a digit first and
 * last: ASCII, so that it counts the same in every program that checks it
 * and means nothing to HTML, a header or a file system; 20, about as many
 * as the heading of the panel's menu shows whole. Without one, or with
 * one that is not one, %PRODUCT_NAME_DEFAULT, the author's own name for the
 * system. It labels the panel and its downloads alone.
 */
#define PRODUCT_NAME_DEFAULT "BuchOS"
#define PRODUCT_NAME_MAX 20

bool product_name_ok(const char *v);

/* The product name in force, in @out of @cap bytes, %PRODUCT_NAME_MAX + 1 or more. */
void product_name(char *out, size_t cap);

/*
 * The product name in force as the start of a file's name, in @out of @cap
 * bytes, %PRODUCT_NAME_MAX + 1 or more: its letters in lower case and its
 * digits, each run of the other characters one hyphen ("Fibre Box 2.0" is
 * "fibre-box-2-0").
 */
void product_slug(char *out, size_t cap);

#endif
