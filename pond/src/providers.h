/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The providers of table.c, and the decoders and rules behind them that the
 * tests call directly.
 */

#ifndef POND_PROVIDERS_H
#define POND_PROVIDERS_H

#include <stddef.h>

#include "prov.h"

int prov_banks(struct prov_buf *out);
int prov_bridge(struct prov_buf *out);
int prov_gem_counters(struct prov_buf *out);
int prov_gem_ports(struct prov_buf *out);
int prov_health(struct prov_buf *out);
int prov_image(struct prov_buf *out);
int prov_kernel(struct prov_buf *out);
int prov_log(struct prov_buf *out);
int prov_memory(struct prov_buf *out);
int prov_model(struct prov_buf *out);
int prov_nl_fdb(const char *links, size_t llen, const char *fdb, size_t flen,
		struct prov_buf *out);
int prov_nl_filters(const char *filters, size_t len, struct prov_buf *out);
int prov_nl_gem_ports(const char *links, size_t len, struct prov_buf *out);
int prov_nl_onu(const char *links, size_t llen, const char *dev, size_t dlen,
		struct prov_buf *out);
int prov_nl_tconts(const char *dump, size_t dlen, struct prov_buf *out);
int prov_nl_vlans(const char *brlinks, size_t len, struct prov_buf *out);
int prov_onu(struct prov_buf *out);
int prov_optics(struct prov_buf *out);
int prov_port_counters(struct prov_buf *out);
int prov_ports(struct prov_buf *out);
int prov_root_mtd(const char *cmdline);
int prov_rootfs_bank(const char *part);
int prov_settings(struct prov_buf *out);
int prov_settings_set(const char *val, char *err, size_t errcap);
void prov_settings_reap(void);
int prov_tc_rules(struct prov_buf *out);
int prov_tconts(struct prov_buf *out);
int prov_uboot_bank(const char *value);
int prov_uptime(struct prov_buf *out);

#endif
