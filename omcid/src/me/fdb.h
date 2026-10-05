/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The bridge table data of the bridge ports (G.988 9.3.8): the addresses the
 * switch forwards by, read from it when the OLT reads the table.
 */

#ifndef OMCI_ME_FDB_H
#define OMCI_ME_FDB_H

#include "me/tables.h"
#include "mib/store.h"

/**
 * omci_fdb_bind() - Name the MIB the bridge ports are in.
 * @mib: the MIB, or NULL to forget it
 */
void omci_fdb_bind(const struct omci_mib *mib);

/*
 * The bridge table of a bridge port, made on demand: the addresses on the
 * port's side of the switch, the UNI or the PON side, each a row of two
 * bytes of information, learned (bit 3) or not, always forwarded, its age
 * not known (0), and the address.
 */
extern const struct omci_me_table_source omci_fdb_bridge_table;

#endif
