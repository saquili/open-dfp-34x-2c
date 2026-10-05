// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The bridge table data of fdb.h. */

#include <stdbool.h>
#include <string.h>

#include "hw/hw.h"
#include "me/bridge.h"
#include "me/fdb.h"

#define ME_BRIDGE_PORT 47u
#define BRIDGE_PORT_TP_TYPE 3u

/* The information of a row (G.988 9.3.8): bit 3, dynamically learned. */
#define FDB_LEARNED 0x0004u
#define FDB_ROW_LEN 8u

/*
 * The MIB, and the rows of the instance read last: the engine reads a
 * table's size, then its rows one by one.
 */
static const struct omci_mib *g_mib;
static struct omci_hw_fdb g_rows[OMCI_HW_FDB_MAX];
static size_t g_n;
static uint16_t g_instance;
static bool g_read;

void omci_fdb_bind(const struct omci_mib *mib)
{
	g_mib = mib;
	g_read = false;
}

/* The side of the bridge a port is on: 1 the UNI, 2 the PON side, 0 none. */
static unsigned int side_of(uint16_t instance)
{
	uint8_t tp_type = 0;
	size_t len = 0;

	if (g_mib == NULL || omci_mib_attr_get(g_mib, ME_BRIDGE_PORT, instance,
					       BRIDGE_PORT_TP_TYPE, &tp_type,
					       1u, &len) != OMCI_OK)
		return 0;
	switch (tp_type) {
	case OMCI_BRIDGE_TP_PPTP_ETH:
		return 1;
	case OMCI_BRIDGE_TP_MAPPER:
	case OMCI_BRIDGE_TP_GEM_IW:
	case OMCI_BRIDGE_TP_MCAST_GEM:
		return 2;
	default:
		return 0;
	}
}

/* Read the switch's addresses on the side of a port. */
static size_t fdb_count(uint16_t instance)
{
	struct omci_hw_fdb all[OMCI_HW_FDB_MAX];
	const unsigned int side = side_of(instance);
	size_t n = 0;

	g_n = 0;
	g_instance = instance;
	g_read = true;
	if (side == 0 || omci_hw()->fdb == NULL ||
	    omci_hw()->fdb(all, OMCI_HW_FDB_MAX, &n) != OMCI_OK)
		return 0;
	for (size_t i = 0; i < n; i++)
		if (all[i].ani == (side == 2))
			g_rows[g_n++] = all[i];
	return g_n;
}

static bool fdb_entry(uint16_t instance, size_t index, void *out, size_t cap)
{
	uint8_t *p = out;

	if (!g_read || instance != g_instance)
		(void)fdb_count(instance);
	if (out == NULL || cap < FDB_ROW_LEN || index >= g_n)
		return false;

	const uint16_t info = g_rows[index].dynamic ? FDB_LEARNED : 0u;

	p[0] = (uint8_t)(info >> 8);
	p[1] = (uint8_t)info;
	memcpy(&p[2], g_rows[index].mac, 6);
	return true;
}

const struct omci_me_table_source omci_fdb_bridge_table = {
	.count = fdb_count,
	.entry = fdb_entry,
};
