// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The OMCI engine. Each frame from the OLT is checked, executed against the
 * management information base and answered; when the request changes what
 * the data path needs, the hardware is programmed again from the MIB.
 */

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "hw/hw.h"
#include "mcast/acl.h"
#include "mcast/agent.h"
#include "me/action.h"
#include "me/defaults.h"
#include "me/bridge.h"
#include "me/fdb.h"
#include "me/filter.h"
#include "me/gem.h"
#include "me/table.h"
#include "me/tables.h"
#include "me/upload.h"
#include "me/values.h"
#include "me/vlan.h"
#include "omci/attrmask.h"
#include "omcid/array_size.h"
#include "omcid/byteorder.h"
#include "omcid/ctl_proto.h"
#include "me/pm.h"
#include "omci/alarm.h"
#include "omci/ber.h"
#include "omci/clock.h"
#include "omci/engine.h"
#include "omci/notify.h"
#include "omci/test.h"
#include "sys/log.h"
#include "sys/wire.h"

/* The message types the engine knows (G.988 11.2.2). */
enum omci_msg_type {
	MT_CREATE = 4,
	MT_DELETE = 6,
	MT_SET = 8,
	MT_GET = 9,
	MT_GET_ALL_ALARMS = 11,
	MT_GET_ALL_ALARMS_N = 12,
	MT_MIB_UPLOAD = 13,
	MT_MIB_UPLOAD_N = 14,
	MT_MIB_RESET = 15,
	MT_TEST = 18,
	MT_DOWNLOAD_SECTION = 20,
	MT_SYNC_TIME = 24,
	MT_GET_NEXT = 26,
	MT_GET_CURRENT_DATA = 28,
};

/* A Get next response carries 29 bytes of a table attribute (G.988 A.3.38). */
#define GET_NEXT_BYTES 29u

/*
 * Room for values in a Get response: the contents hold the result, the
 * attribute mask, the values, and the two masks in their last four bytes
 * (G.988 A.3.8).
 */
#define GET_VALUE_BYTES 25u
static_assert(2u + GET_VALUE_BYTES < OMCI_CONTENTS_LEN,
	      "the body of a Get leaves room for the result byte");

/*
 * A table read in progress. The Get that opened it fixed the attribute,
 * the size and the number of Get next segments it takes.
 */
struct get_next_transfer {
	bool active;
	uint16_t class_id;
	uint16_t instance;
	uint8_t attr_no;
	uint8_t entry_size;
	uint32_t size;
	uint16_t max_seq;

	/* the table generation of the MIB when the read opened */
	uint32_t generation;
};

/*
 * The attributes of the ANI-G the alarms are judged by, and those that
 * report the optical levels (G.988 9.2.1).
 */
#define ANI_G_SF 6u
#define ANI_G_SD 7u
#define ANI_G_ARC 8u
#define ANI_G_ARC_INTERVAL 9u
#define ANI_G_RX_LEVEL 10u
#define ANI_G_RX_LOW 11u
#define ANI_G_RX_HIGH 12u
#define ANI_G_TX_LEVEL 14u
#define ANI_G_TX_LOW 15u
#define ANI_G_TX_HIGH 16u

/*
 * The entities under alarm-reporting control (G.988 A.1.4.3), with the
 * attribute that turns it on and the one that gives its interval.
 */
static const struct {
	uint16_t class_id;
	uint16_t instance;
	uint8_t arc;
	uint8_t interval;
} g_arc[] = {
	{ OMCI_ALARM_ANI_G, OMCI_ALARM_ANI_G_INSTANCE, ANI_G_ARC,
	  ANI_G_ARC_INTERVAL },
};

/* An ARC interval that never ends. */
#define ARC_FOREVER 255u

/*
 * The time the OLT may leave between the commands of an alarm upload
 * before the ONU takes it as ended (G.988 A.1.4.2).
 */
#define ALARM_UPLOAD_IDLE_SEC 60u

/* The result codes of a response (G.988 A.1.1). */
enum omci_result {
	RESULT_OK = 0,
	RESULT_PROCESSING_ERROR = 1,
	RESULT_CMD_NOT_SUPPORTED = 2,
	RESULT_PARAM_ERROR = 3,
	RESULT_UNKNOWN_ME = 4,
	RESULT_UNKNOWN_INST = 5,
	RESULT_DEVICE_BUSY = 6,
	RESULT_INSTANCE_EXISTS = 7,
	RESULT_ATTR_FAILED = 9,
};

/* An instance in a MIB upload, and the upload unit its attributes start at. */
struct upload_row {
	uint16_t class_id;
	uint16_t instance;
	uint16_t first_unit;
};

/*
 * The last answers sent on each channel. A request that comes again with
 * the same transaction identifier is a retransmission: it gets the stored
 * answer and is not executed twice (G.988 B.2.1).
 */
#define OMCI_RESP_CACHE_DEPTH 16u

struct resp_cache_entry {
	bool valid;
	uint16_t tci;
	uint16_t class_id;
	uint16_t instance;
	uint8_t msg_type;
	uint8_t frame[OMCI_FRAME_LEN];
};

/*
 * The engine. Of each pair, element 0 serves the low-priority channel and
 * element 1 the high-priority one (G.988 B.2.2).
 */
struct omci_engine {
	struct omci_mib *mib;
	struct omci_alarm *alarm;
	struct omci_pm *pm;
	struct omci_mcast *mcast;
	const struct omci_tx_ops *tx;
	void *tx_ctx;

	const struct omci_tap_ops *tap;
	void *tap_ctx;

	struct resp_cache_entry resp_cache[2][OMCI_RESP_CACHE_DEPTH];
	unsigned int resp_next[2];
	uint8_t last_tx[OMCI_FRAME_LEN];
	bool did_tx;
	uint16_t cur_tci;

	struct get_next_transfer get_next[2];

	/*
	 * The MIB upload, one for both channels: the OLT may not start one on
	 * a channel while one runs on the other (G.988 B.2.3), and an OLT that
	 * counts its identifiers into the priority bit sends the commands of
	 * one upload on both.
	 */
	struct {
		bool active;
		uint16_t units;
		size_t row_count;
		struct upload_row rows[OMCI_MIB_MAX_ROWS];
	} upload;

	struct omci_clock clock;
	uint64_t now_sec;

	/*
	 * The alarms: the second they were last judged, the time of the last
	 * command of the alarm upload, and for each entity of g_arc whether its
	 * alarm-reporting control is on and since when it has been trouble
	 * free.
	 */
	bool alarms_judged;
	uint64_t alarm_sec;
	uint64_t alarm_upload_sec;
	struct {
		bool on;
		uint64_t since;
	} arc[ARRAY_SIZE(g_arc)];

	/* changes with every request of the OLT that may change its set-up */
	uint32_t changes;
};

static struct omci_engine g_engine;
static bool g_engine_taken;

static void tap(const struct omci_engine *eng, uint8_t verdict, uint8_t detail,
		const uint8_t wire[static OMCI_FRAME_LEN])
{
	if (eng->tap != NULL && eng->tap->frame != NULL)
		eng->tap->frame(eng->tap_ctx, verdict, detail, wire);
}

/* Send a frame, and keep it when it answers the request being handled. */
static void transmit(struct omci_engine *eng,
		     const uint8_t frame[static OMCI_FRAME_LEN])
{
	const uint16_t ftci =
		(uint16_t)((((uint16_t)frame[0] << 8) | frame[1]) & 0x7FFFu);
	const bool fak = (frame[2] & 0x20u) != 0u;

	if (fak && ftci == eng->cur_tci) {
		omci_wire_copy(eng->last_tx, frame, OMCI_FRAME_LEN);
		eng->did_tx = true;
	}

	tap(eng, OMCI_CTL_TAP_TX, frame[2] & 0x1Fu, frame);

	if (eng->tx != NULL && eng->tx->send != NULL)
		eng->tx->send(eng->tx_ctx, frame);
}

static struct resp_cache_entry *cache_find(struct omci_engine *eng,
					   unsigned int ch,
					   const struct omci_frame *req)
{
	for (unsigned int i = 0; i < OMCI_RESP_CACHE_DEPTH; i++) {
		struct resp_cache_entry *e = &eng->resp_cache[ch][i];

		if (e->valid && e->tci == req->tci &&
		    e->class_id == req->class_id &&
		    e->instance == req->instance &&
		    e->msg_type == req->msg_type)
			return e;
	}
	return NULL;
}

static void cache_store(struct omci_engine *eng, unsigned int ch,
			const struct omci_frame *req,
			const uint8_t frame[static OMCI_FRAME_LEN])
{
	struct resp_cache_entry *e = &eng->resp_cache[ch][eng->resp_next[ch]];

	eng->resp_next[ch] = (eng->resp_next[ch] + 1u) % OMCI_RESP_CACHE_DEPTH;

	e->valid = true;
	e->tci = req->tci;
	e->class_id = req->class_id;
	e->instance = req->instance;
	e->msg_type = req->msg_type;
	omci_wire_copy(e->frame, frame, OMCI_FRAME_LEN);
}

/*
 * The ONU changed an attribute on its own: the OLT hears of it through an
 * attribute value change notification (G.988 A.3.20).
 */
static void on_attr_changed(void *ctx, uint16_t class_id, uint16_t instance,
			    uint8_t attr, const void *value, size_t len)
{
	struct omci_engine *eng = ctx;
	uint8_t frame[OMCI_FRAME_LEN];

	omci_notify_build_avc(class_id, instance, attr, value, len, frame);
	transmit(eng, frame);
}

static const struct omci_mib_notify_ops notify_ops = {
	.attr_changed = on_attr_changed
};

/* Answer a request: the result code first, then @extra. */
static void reply(struct omci_engine *eng, const struct omci_frame *req,
		  uint8_t result, const uint8_t *extra, size_t extra_len)
{
	struct omci_frame rsp = {
		.tci = req->tci,
		.priority = req->priority,
		.msg_type = req->msg_type,
		.ar = false,
		.ak = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = req->class_id,
		.instance = req->instance,
	};

	rsp.contents[0] = result;
	if (extra != NULL && extra_len > 0)
		omci_wire_copy(&rsp.contents[1], extra, extra_len);

	if (result != RESULT_OK)
		OMCI_INFO("tx: %u/%#06x mt=%u answered result %u",
			  req->class_id, req->instance, req->msg_type, result);

	uint8_t frame[OMCI_FRAME_LEN];
	(void)!omci_frame_build(&rsp, frame);
	transmit(eng, frame);
}

/* The result code the OLT sees for a status of the daemon. */
static uint8_t result_for(enum omci_status rc)
{
	switch (rc) {
	case OMCI_OK:
		return RESULT_OK;
	case OMCI_ERR_ME_UNKNOWN:
		return RESULT_UNKNOWN_ME;
	case OMCI_ERR_INST_UNKNOWN:
		return RESULT_UNKNOWN_INST;
	case OMCI_ERR_EXISTS:
		return RESULT_INSTANCE_EXISTS;

	case OMCI_DECLINED:
		return RESULT_PARAM_ERROR;
	case OMCI_ERR_ATTR_UNKNOWN:
		return RESULT_PARAM_ERROR;
	case OMCI_ERR_INVAL:
		return RESULT_PARAM_ERROR;
	case OMCI_ERR_NOMEM:
		return RESULT_DEVICE_BUSY;
	case OMCI_ERR_BUSY:
		return RESULT_DEVICE_BUSY;

	case OMCI_ERR_HW:
		return RESULT_PROCESSING_ERROR;
	case OMCI_ERR_IO:
		return RESULT_PROCESSING_ERROR;
	default:
		return RESULT_PARAM_ERROR;
	}
}

/* The classes that shape the data path, and the rule table of class 171. */
enum {
	ME_EXT_VLAN_TAG_OPER = 171,

	ME_PPTP_ETH_UNI = 11,
	ME_MAC_BRIDGE = 45,
	ME_MAC_BRIDGE_PORT = 47,
	ME_BRIDGE_PORT_FILTER = 49,
	ME_BRIDGE_PORT_TABLE = 50,
	ME_BRIDGE_PORT_PREASSIGN = 79,
	ME_VEIP = 329,
	VEIP_OPER_STATE = 2,
	VEIP_ENABLED = 0,
	VEIP_DISABLED = 1,
	ME_VLAN_TAG_FILTER = 84,
	ME_8021P_MAPPER = 130,
	ME_GEM_IW_TP = 266,
	ME_MCAST_GEM_IW_TP = 281,

	ME_GEM_PORT_NET_CTP = 268,
	ME_TCONT = 262,
	ME_PRIORITY_QUEUE = 277,
	ME_TRAFFIC_SCHED = 278,
	ME_TRAFFIC_DESCRIPTOR = 280,

	ME171_RULE_ATTR = 6,

	ME_OLT_G = 131,
	OLT_G_TOD_ATTR = 4,
	OLT_G_TOD_LEN = 14,
};

static enum omci_status reprogram_vlan(struct omci_engine *eng)
{
	return omci_vlan_program(eng->mib, ME_EXT_VLAN_TAG_OPER,
				 ME171_RULE_ATTR, NULL);
}

/*
 * The VLAN rules again, when the GEM ports or the bridges just programmed
 * change which rules take the priorities apart (omci_vlan_per_pri_stale()).
 */
static void reprogram_vlan_per_pri(struct omci_engine *eng)
{
	if (omci_vlan_per_pri_stale())
		(void)reprogram_vlan(eng);
}

static enum omci_status reprogram_gem(struct omci_engine *eng)
{
	const enum omci_status rc = omci_gem_program(eng->mib, NULL);

	reprogram_vlan_per_pri(eng);
	return rc;
}

/*
 * True for a class whose instances shape the GEM ports: the ports
 * themselves, what gives them their queue, weight, rates and priorities,
 * and the multicast termination points, which make ports multicast and name
 * further ones.
 */
static bool class_shapes_gem(uint16_t class_id)
{
	return class_id == ME_GEM_PORT_NET_CTP || class_id == ME_TCONT ||
	       class_id == ME_PRIORITY_QUEUE || class_id == ME_TRAFFIC_SCHED ||
	       class_id == ME_TRAFFIC_DESCRIPTOR ||
	       class_id == ME_8021P_MAPPER || class_id == ME_GEM_IW_TP ||
	       class_id == ME_MCAST_GEM_IW_TP;
}

/* True for a class whose instances make up a bridge or the GEM ports behind it. */
static bool class_builds_bridge(uint16_t class_id)
{
	return class_id == ME_MAC_BRIDGE || class_id == ME_MAC_BRIDGE_PORT ||
	       class_id == ME_VLAN_TAG_FILTER || class_id == ME_8021P_MAPPER ||
	       class_id == ME_GEM_IW_TP || class_id == ME_MCAST_GEM_IW_TP ||
	       class_id == ME_GEM_PORT_NET_CTP;
}

static enum omci_status reprogram_bridge(struct omci_engine *eng)
{
	const enum omci_status rc = omci_bridge_program(eng->mib, NULL);

	reprogram_vlan_per_pri(eng);
	return rc;
}

/*
 * True for a class whose instances make up the multicast policy of the
 * subscriber side: the multicast classes, and the bridge ports and mappers
 * they are linked to.
 */
static bool class_shapes_mcast(uint16_t class_id)
{
	return class_id == OMCI_MCAST_PROFILE ||
	       class_id == OMCI_MCAST_SUBSCRIBER ||
	       class_id == OMCI_MCAST_MONITOR ||
	       class_id == ME_MAC_BRIDGE_PORT || class_id == ME_8021P_MAPPER;
}

static void reprogram_mcast(struct omci_engine *eng, uint16_t class_id)
{
	if (class_shapes_mcast(class_id))
		omci_mcast_program(eng->mcast, eng->mib, eng->now_sec);
}

/*
 * The classes whose instances make the bridge port filters (me/filter.h). A
 * filter the switch does not take is kept and said in the log, and the
 * request that set it is answered as done.
 */
static bool class_shapes_filters(uint16_t class_id)
{
	return class_id == ME_PPTP_ETH_UNI || class_id == ME_MAC_BRIDGE_PORT ||
	       class_id == ME_BRIDGE_PORT_FILTER ||
	       class_id == ME_BRIDGE_PORT_PREASSIGN;
}

static void reprogram_filters(struct omci_engine *eng, uint16_t class_id)
{
	if (class_shapes_filters(class_id))
		(void)omci_filter_apply(eng->mib);
}

/*
 * The classes whose instances limit the rate of the UNI towards the
 * subscriber: the bridge ports, and the traffic descriptors they name.
 */
static void reprogram_uni_rate(struct omci_engine *eng, uint16_t class_id)
{
	if (class_id == ME_MAC_BRIDGE_PORT || class_id == ME_TRAFFIC_DESCRIPTOR)
		omci_bridge_uni_rate(eng->mib);
}

/*
 * The Time of day information of OLT-G (ITU-T G.988 clause 9.12.2), when a
 * Set wrote it: the superframe N in its first four bytes and TstampN, an
 * IEEE 1588 timestamp, in the other ten, for the hardware to set the clock
 * at frame N. All zero is no time: the attribute as the ONU starts.
 */
static void reprogram_tod(struct omci_engine *eng, uint16_t class_id,
			  uint16_t instance, uint16_t written)
{
	uint8_t v[OLT_G_TOD_LEN];
	static const uint8_t none[OLT_G_TOD_LEN];

	if (class_id != ME_OLT_G || omci_hw()->tod_info == NULL ||
	    !(written & omci_attr_bit(OLT_G_TOD_ATTR)) ||
	    omci_mib_attr_get(eng->mib, class_id, instance, OLT_G_TOD_ATTR, v,
			      sizeof(v), NULL) != OMCI_OK ||
	    memcmp(v, none, sizeof(v)) == 0)
		return;

	omci_hw()->tod_info(omci_be32(&v[0]),
			    (uint64_t)omci_be16(&v[4]) << 32 | omci_be32(&v[6]),
			    omci_be32(&v[10]));
}

/*
 * A data-path program that declines part of a set-up fails the request
 * only when the declined instance is the one the request configures.
 */
static enum omci_status about_this(enum omci_status rc, bool mine)
{
	if (rc == OMCI_DECLINED)
		return mine ? OMCI_DECLINED : OMCI_OK;
	return rc;
}

/* The status a Create or Set answers with, once the data path is programmed. */
static enum omci_status data_path_status(uint16_t class_id, uint16_t instance,
					 enum omci_status gem,
					 enum omci_status bridge)
{
	const enum omci_status g =
		class_id == ME_GEM_PORT_NET_CTP ?
			about_this(gem, omci_gem_instance_rejected(instance)) :
		class_id == ME_TCONT ?
			about_this(gem,
				   omci_tcont_instance_rejected(instance)) :
			OMCI_OK;
	const enum omci_status b =
		about_this(bridge, omci_bridge_instance_rejected(instance));

	if (g == OMCI_ERR_HW || b == OMCI_ERR_HW)
		return OMCI_ERR_HW;
	if (g != OMCI_OK)
		return g;
	return b;
}

static void keep_levels(struct omci_engine *eng);

/*
 * MIB reset (G.988 A.3.17, A.3.18). The answer goes first. Then every read
 * in progress ends, the MIB is rebuilt from the instances the ONU creates
 * itself, with the optical levels last read, and the hardware is brought
 * back to that state.
 */
static void forget_transfers(struct omci_engine *eng)
{
	omci_wire_fill(eng->resp_cache, 0, sizeof(eng->resp_cache));
	omci_wire_fill(eng->resp_next, 0, sizeof(eng->resp_next));
	omci_wire_fill(eng->get_next, 0, sizeof(eng->get_next));
	eng->upload.active = false;
	eng->upload.units = 0;
	eng->upload.row_count = 0;
}

static void handle_mib_reset(struct omci_engine *eng,
			     const struct omci_frame *req)
{
	reply(eng, req, RESULT_OK, NULL, 0);

	forget_transfers(eng);

	omci_pm_forget_all(eng->pm);
	omci_alarm_reset_sequence(eng->alarm);

	omci_clock_forget(&eng->clock);

	struct omci_hw_caps caps;
	enum omci_status rc = omci_hw()->caps(&caps);

	if (rc == OMCI_OK) {
		omci_mib_notify_mute(eng->mib, true);
		rc = omci_me_defaults_apply(eng->mib, &caps);
		keep_levels(eng);
		omci_mib_notify_mute(eng->mib, false);
	}
	if (rc != OMCI_OK)
		OMCI_ERR("mib reset: repopulation failed: %s",
			 omci_status_str(rc));

	omci_vlan_forget_in_force();

	(void)reprogram_vlan(eng);

	(void)reprogram_gem(eng);
	(void)reprogram_bridge(eng);
	omci_mcast_program(eng->mcast, eng->mib, eng->now_sec);
	omci_filter_forget();
	(void)omci_filter_apply(eng->mib);
	omci_bridge_uni_rate_forget();
	omci_bridge_uni_rate(eng->mib);
}

void omci_engine_reprogram(struct omci_engine *eng)
{
	(void)reprogram_gem(eng);
	(void)reprogram_bridge(eng);
	(void)reprogram_vlan(eng);
	omci_mcast_program(eng->mcast, eng->mib, eng->now_sec);
	omci_filter_forget();
	(void)omci_filter_apply(eng->mib);
	omci_bridge_uni_rate_forget();
	omci_bridge_uni_rate(eng->mib);
}

uint32_t omci_engine_changes(const struct omci_engine *eng)
{
	return eng != NULL ? eng->changes : 0u;
}

/*
 * What the engine keeps across a start again: a header, the clock that
 * Synchronize time set and where the intervals stand, then the MIB
 * (omci_mib_save()).
 *
 *   "OENG", version (2), 0 (2), the clock's time of day (8) and monotonic
 *   time (8) at the synchronisation, whether it is set (1), whether an
 *   interval runs (1), the number of the last interval closed (1), 0 (1),
 *   the second the interval in progress ends (8)
 */
#define KEEP_MAGIC 0x4f454e47u
#define KEEP_VERSION 1u
#define KEEP_HEADER 36u
static_assert(KEEP_HEADER + OMCI_MIB_SAVE_MAX == OMCI_ENGINE_SAVE_MAX,
	      "the engine's bytes are its header and the MIB's");

static void be64_store(uint8_t *p, uint64_t v)
{
	omci_be32_store(p, (unsigned int)(v >> 32));
	omci_be32_store(p + 4, (unsigned int)v);
}

static uint64_t be64(const uint8_t *p)
{
	return (uint64_t)omci_be32(p) << 32 | omci_be32(p + 4);
}

enum omci_status omci_engine_save(const struct omci_engine *eng, uint8_t *out,
				  size_t cap, size_t *out_len)
{
	if (eng == NULL || out == NULL || out_len == NULL)
		return OMCI_ERR_INVAL;
	if (cap < KEEP_HEADER)
		return OMCI_ERR_TRUNCATED;

	struct omci_pm_schedule pm;
	size_t mib_len = 0;
	const enum omci_status rc = omci_mib_save(eng->mib, out + KEEP_HEADER,
						  cap - KEEP_HEADER, &mib_len);

	if (rc != OMCI_OK)
		return rc;
	omci_pm_schedule_get(eng->pm, &pm);
	omci_wire_fill(out, 0, KEEP_HEADER);
	omci_be32_store(out, KEEP_MAGIC);
	omci_be16_store(out + 4, KEEP_VERSION);
	be64_store(out + 8, eng->clock.wall_at_sync);
	be64_store(out + 16, eng->clock.mono_at_sync);
	out[24] = eng->clock.set;
	out[25] = pm.started;
	out[26] = pm.interval_end_time;
	be64_store(out + 28, pm.next_end);
	*out_len = KEEP_HEADER + mib_len;
	return OMCI_OK;
}

enum omci_status omci_engine_load(struct omci_engine *eng, const uint8_t *in,
				  size_t len)
{
	if (eng == NULL || in == NULL)
		return OMCI_ERR_INVAL;

	enum omci_status rc;

	if (len < KEEP_HEADER)
		rc = OMCI_ERR_TRUNCATED;
	else if (omci_be32(in) != KEEP_MAGIC ||
		 omci_be16(in + 4) != KEEP_VERSION)
		rc = OMCI_ERR_PROTO;
	else
		rc = omci_mib_load(eng->mib, in + KEEP_HEADER,
				   len - KEEP_HEADER);
	if (rc != OMCI_OK) {
		(void)omci_mib_reset(eng->mib);
		return rc;
	}

	const struct omci_pm_schedule pm = {
		.started = in[25] != 0u,
		.next_end = be64(in + 28),
		.interval_end_time = in[26],
	};

	forget_transfers(eng);
	omci_pm_forget_all(eng->pm);
	omci_pm_schedule_set(eng->pm, &pm);
	eng->clock.wall_at_sync = be64(in + 8);
	eng->clock.mono_at_sync = be64(in + 16);
	eng->clock.set = in[24] != 0u;
	omci_vlan_forget_in_force();
	omci_engine_reprogram(eng);
	return OMCI_OK;
}

int omci_engine_mcast_fd(const struct omci_engine *eng)
{
	(void)eng;
	return omci_hw()->mcast_fd != NULL ? omci_hw()->mcast_fd() : -1;
}

void omci_engine_mcast_service(struct omci_engine *eng)
{
	if (eng != NULL)
		omci_mcast_service(eng->mcast, eng->mib, eng->now_sec);
}

/* The order of a MIB upload: by class, then by instance. */
static bool upload_before(uint16_t class_id, uint16_t instance,
			  const struct upload_row *row)
{
	if (class_id != row->class_id)
		return class_id < row->class_id;
	return instance < row->instance;
}

/*
 * Freeze the instances a MIB upload reports, and number the upload units
 * that their attributes take.
 */
static uint16_t upload_snapshot(struct omci_engine *eng)
{
	__typeof__(eng->upload) *up = &eng->upload;

	size_t n = 0;

	struct omci_mib_cursor cur;

	for (enum omci_status rc = omci_mib_walk_first(eng->mib, &cur);
	     rc == OMCI_OK; rc = omci_mib_walk_next(eng->mib, &cur)) {
		if (!omci_me_upload_includes(omci_me_class_by_id(cur.class_id)))
			continue;

		size_t at = n++;

		while (at > 0 && upload_before(cur.class_id, cur.instance,
					       &up->rows[at - 1u])) {
			up->rows[at].class_id = up->rows[at - 1u].class_id;
			up->rows[at].instance = up->rows[at - 1u].instance;
			at--;
		}
		up->rows[at].class_id = cur.class_id;
		up->rows[at].instance = cur.instance;
	}

	uint16_t units = 0;

	for (size_t i = 0; i < n; i++) {
		up->rows[i].first_unit = units;
		units = (uint16_t)(units +
				   omci_me_upload_segments(omci_me_class_by_id(
					   up->rows[i].class_id)));
	}

	up->row_count = n;
	return units;
}

/* The instance that upload unit @seq belongs to, and the unit's place in it. */
static const struct upload_row *upload_locate(const struct omci_engine *eng,
					      uint16_t seq,
					      unsigned int *unit_index)
{
	const __typeof__(eng->upload) *up = &eng->upload;

	size_t i = up->row_count - 1u;

	while (up->rows[i].first_unit > seq)
		i--;
	*unit_index = seq - up->rows[i].first_unit;
	return &up->rows[i];
}

/*
 * MIB upload (G.988 A.3.13, A.3.14, I.1.1.2): the answer is the number of
 * MIB upload next commands the upload takes.
 */
static void handle_mib_upload(struct omci_engine *eng,
			      const struct omci_frame *req)
{
	const unsigned int ch = req->priority ? 1u : 0u;
	__typeof__(eng->upload) *up = &eng->upload;
	const uint16_t units = upload_snapshot(eng);

	up->units = units;
	up->active = units > 0;

	OMCI_INFO("mib upload: %u units over %zu instances (channel %u)", units,
		  up->row_count, ch);

	struct omci_frame rsp = {
		.tci = req->tci,

		.priority = req->priority,
		.msg_type = req->msg_type,
		.ak = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = req->class_id,
		.instance = req->instance,
	};
	omci_be16_store(&rsp.contents[0], units);

	uint8_t frame[OMCI_FRAME_LEN];
	(void)!omci_frame_build(&rsp, frame);
	transmit(eng, frame);
}

/*
 * MIB upload next (G.988 A.3.15, A.3.16): one class, instance and attribute
 * mask, then the values of those attributes. A command out of sequence is
 * answered with empty contents.
 */
static void handle_mib_upload_next(struct omci_engine *eng,
				   const struct omci_frame *req)
{
	const uint16_t seq = (uint16_t)omci_be16(&req->contents[0]);

	struct omci_frame rsp = {
		.tci = req->tci,

		.priority = req->priority,
		.msg_type = req->msg_type,
		.ak = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = req->class_id,
		.instance = req->instance,
	};

	const __typeof__(eng->upload) *up = &eng->upload;

	unsigned int unit_index = 0;
	const struct upload_row *row = NULL;

	if (!up->active) {
		OMCI_WARN(
			"mib upload next: sequence %u with no upload in progress",
			seq);
	} else if (seq >= up->units) {
		OMCI_WARN("mib upload next: sequence %u past the %u promised",
			  seq, up->units);
	} else {
		row = upload_locate(eng, seq, &unit_index);
	}

	if (row != NULL) {
		struct omci_me_upload_unit unit;
		const struct omci_me_class *cls =
			omci_me_class_by_id(row->class_id);

		if (omci_me_upload_unit_at(cls, unit_index, &unit)) {
			omci_be16_store(&rsp.contents[0], row->class_id);
			omci_be16_store(&rsp.contents[2], row->instance);
			omci_be16_store(&rsp.contents[4], unit.mask);

			size_t at = 6;

			for (uint8_t i = 0; i < unit.count; i++) {
				const struct omci_me_attr *a =
					omci_me_attr_get(cls, unit.attrs[i]);
				if (a == NULL ||
				    at + a->size > OMCI_CONTENTS_LEN)
					break;

				size_t got = 0;

				if (omci_mib_attr_get(eng->mib, row->class_id,
						      row->instance,
						      unit.attrs[i],
						      &rsp.contents[at],
						      OMCI_CONTENTS_LEN - at,
						      &got) != OMCI_OK)
					OMCI_WARN(
						"mib upload next: %u/%u attribute %u is gone mid-upload",
						row->class_id, row->instance,
						unit.attrs[i]);
				at += a->size;
			}
		}
	}

	uint8_t frame[OMCI_FRAME_LEN];
	(void)!omci_frame_build(&rsp, frame);
	transmit(eng, frame);
}

/*
 * The rows of a table attribute: from the hardware for a table rendered
 * from it, from the MIB otherwise.
 */
static size_t table_entries(const struct omci_engine *eng, uint16_t class_id,
			    uint16_t instance, uint8_t attr_no)
{
	const struct omci_me_table_source *src;

	if (omci_me_table_rendered(class_id, attr_no, &src))
		return src->count != NULL ? src->count(instance) : 0;

	return omci_mib_table_count(eng->mib, class_id, instance, attr_no);
}

static bool is_table(uint16_t class_id, uint8_t attr_no)
{
	return omci_me_attr_is_table(
		omci_me_attr_get(omci_me_class_by_id(class_id), attr_no));
}

static bool table_entry_at(const struct omci_engine *eng, uint16_t class_id,
			   uint16_t instance, uint8_t attr_no, size_t index,
			   uint8_t *out, size_t cap)
{
	const struct omci_me_table_source *src;

	if (omci_me_table_rendered(class_id, attr_no, &src))
		return src->entry != NULL &&
		       src->entry(instance, index, out, cap);

	return omci_mib_table_get(eng->mib, class_id, instance, attr_no, index,
				  out, cap) == OMCI_OK;
}

/* Copy @len bytes of a table attribute, from byte @offset of its rows end to end. */
static void table_serialise(const struct omci_engine *eng,
			    const struct get_next_transfer *t, uint32_t offset,
			    uint8_t *out, size_t len)
{
	size_t done = 0;

	while (done < len) {
		const size_t at = (size_t)offset + done;
		const size_t index = at / t->entry_size;
		const size_t skip = at % t->entry_size;

		uint8_t entry[OMCI_MIB_TABLE_ENTRY_MAX] = { 0 };
		(void)table_entry_at(eng, t->class_id, t->instance, t->attr_no,
				     index, entry, sizeof(entry));

		size_t take = (size_t)t->entry_size - skip;

		if (take > len - done)
			take = len - done;
		omci_wire_copy(&out[done], &entry[skip], take);
		done += take;
	}
}

/*
 * The table read that a Get next of attribute @attr continues: the one the
 * last Get opened on the request's channel or, failing that, the one on the
 * other channel, where an OLT that counts its identifiers into the priority
 * bit opened it.
 */
static struct get_next_transfer *get_next_find(struct omci_engine *eng,
					       const struct omci_frame *req,
					       uint8_t attr)
{
	const unsigned int own = req->priority ? 1u : 0u;

	for (unsigned int i = 0; i < 2u; i++) {
		struct get_next_transfer *t = &eng->get_next[own ^ i];

		if (t->active && t->class_id == req->class_id &&
		    t->instance == req->instance && t->attr_no == attr)
			return t;
	}
	return NULL;
}

/*
 * Get next (G.988 A.3.37, A.3.38): segment @seq of the table attribute that
 * a Get opened. A request names one attribute; a table that changed since
 * the Get ends the read.
 */
static void handle_get_next(struct omci_engine *eng,
			    const struct omci_frame *req)
{
	const uint16_t mask = (uint16_t)omci_be16(&req->contents[0]);
	const uint16_t seq = (uint16_t)omci_be16(&req->contents[2]);

	uint8_t attr = 0;
	unsigned int named = 0;

	for (uint8_t i = 1; i <= OMCI_ATTR_MAX; i++)
		if (omci_attr_in_mask(mask, i)) {
			if (attr == 0u)
				attr = i;
			named++;
		}

	if (named > 1u) {
		OMCI_DEBUG(
			"get next: %u/%u named %u attributes; G.988 allows one",
			req->class_id, req->instance, named);
		reply(eng, req, RESULT_PARAM_ERROR, NULL, 0);
		return;
	}

	struct get_next_transfer *t = get_next_find(eng, req, attr);

	if (t == NULL) {
		OMCI_DEBUG(
			"get next: %u/%u attr %u does not match the read in progress",
			req->class_id, req->instance, attr);
		reply(eng, req, RESULT_PARAM_ERROR, NULL, 0);
		return;
	}
	if (seq >= t->max_seq) {
		OMCI_DEBUG(
			"get next: sequence %u past the %u segments promised",
			seq, t->max_seq);
		reply(eng, req, RESULT_PARAM_ERROR, NULL, 0);
		return;
	}

	if (t->generation != omci_mib_table_generation(eng->mib)) {
		OMCI_WARN(
			"get next: %u/%u attribute %u changed mid-read — refusing rather than serving bytes from a table the OLT was not promised",
			t->class_id, t->instance, attr);
		t->active = false;
		reply(eng, req, RESULT_PARAM_ERROR, NULL, 0);
		return;
	}

	const uint32_t offset = (uint32_t)GET_NEXT_BYTES * seq;
	size_t want = GET_NEXT_BYTES;

	if (seq + 1u == t->max_seq)
		want = t->size - offset;

	uint8_t body[OMCI_CONTENTS_LEN];

	omci_wire_fill(body, 0, sizeof(body));
	body[0] = RESULT_OK;
	omci_be16_store(&body[1], mask);

	table_serialise(eng, t, offset, &body[3], want);

	struct omci_frame rsp = {
		.tci = req->tci,
		.priority = req->priority,
		.msg_type = req->msg_type,
		.ak = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = req->class_id,
		.instance = req->instance,
	};
	omci_wire_copy(rsp.contents, body, sizeof(rsp.contents));

	uint8_t frame[OMCI_FRAME_LEN];
	(void)!omci_frame_build(&rsp, frame);
	transmit(eng, frame);

	if (seq + 1u == t->max_seq)
		t->active = false;
}

/*
 * Create (G.988 A.3.1, A.3.2). The contents carry the set-by-create
 * attributes, in attribute order. One with a value it does not take fails
 * the Create, the instance is not made, and the answer names the attributes
 * in its execution mask.
 */
/*
 * The tables the ONU keeps for each bridge port: its MAC filter table, its
 * bridge table and its filter preassign table (G.988 9.3.6 to 9.3.8), made
 * with the port and removed with it.
 */
static void bridge_port_tables(struct omci_engine *eng, uint16_t instance,
			       bool made)
{
	static const uint16_t tables[] = {
		ME_BRIDGE_PORT_FILTER,
		ME_BRIDGE_PORT_TABLE,
		ME_BRIDGE_PORT_PREASSIGN,
	};

	for (size_t i = 0; i < ARRAY_SIZE(tables); i++)
		if (made)
			(void)!omci_mib_create(eng->mib, tables[i], instance);
		else
			(void)!omci_mib_delete(eng->mib, tables[i], instance);
}

static void handle_create(struct omci_engine *eng, const struct omci_frame *req)
{
	enum omci_status rc =
		omci_mib_create(eng->mib, req->class_id, req->instance);
	if (rc != OMCI_OK) {
		reply(eng, req, result_for(rc), NULL, 0);
		return;
	}

	const struct omci_me_class *cls = omci_me_class_by_id(req->class_id);
	uint16_t refused = 0;
	size_t off = 0;

	for (uint8_t n = 1; n <= cls->attr_count; n++) {
		const struct omci_me_attr *a = omci_me_attr_get(cls, n);

		if (a == NULL || (a->access & OMCI_ME_ACC_SET_BY_CREATE) == 0)
			continue;

		if (!omci_me_value_allowed(req->class_id, n,
					   &req->contents[off]))
			refused |= omci_attr_bit(n);
		(void)!omci_mib_attr_set_by_olt(eng->mib, req->class_id,
						req->instance, n,
						&req->contents[off], a->size);
		off += a->size;
	}

	if (refused != 0u) {
		const uint8_t mask[2] = { (uint8_t)(refused >> 8),
					  (uint8_t)refused };

		OMCI_WARN(
			"create: %u/%u: attributes %#06x take no such value — refused",
			req->class_id, req->instance, refused);
		(void)!omci_mib_delete(eng->mib, req->class_id, req->instance);
		reply(eng, req, RESULT_PARAM_ERROR, mask, sizeof(mask));
		return;
	}

	enum omci_status gem = OMCI_OK, bridge = OMCI_OK;

	if (req->class_id == ME_MAC_BRIDGE_PORT)
		bridge_port_tables(eng, req->instance, true);
	if (class_shapes_gem(req->class_id))
		gem = reprogram_gem(eng);
	if (class_builds_bridge(req->class_id))
		bridge = reprogram_bridge(eng);
	reprogram_mcast(eng, req->class_id);
	reprogram_filters(eng, req->class_id);
	reprogram_uni_rate(eng, req->class_id);
	omci_pm_changed(eng->pm, req->class_id, req->instance);

	reply(eng, req,
	      result_for(data_path_status(req->class_id, req->instance, gem,
					  bridge)),
	      NULL, 0);
}

/* Delete (G.988 A.3.3, A.3.4). */
static void handle_delete(struct omci_engine *eng, const struct omci_frame *req)
{
	const enum omci_status rc =
		omci_mib_delete(eng->mib, req->class_id, req->instance);

	if (rc == OMCI_OK)
		omci_pm_forget(eng->pm, req->class_id, req->instance);
	if (rc == OMCI_OK && req->class_id == ME_MAC_BRIDGE_PORT)
		bridge_port_tables(eng, req->instance, false);

	enum omci_status gem = OMCI_OK, bridge = OMCI_OK, vlan = OMCI_OK;

	if (rc == OMCI_OK && req->class_id == ME_EXT_VLAN_TAG_OPER)
		vlan = reprogram_vlan(eng);
	if (rc == OMCI_OK && class_shapes_gem(req->class_id))
		gem = reprogram_gem(eng);
	if (rc == OMCI_OK && class_builds_bridge(req->class_id))
		bridge = reprogram_bridge(eng);
	if (rc == OMCI_OK)
		reprogram_mcast(eng, req->class_id);
	if (rc == OMCI_OK)
		reprogram_filters(eng, req->class_id);
	if (rc == OMCI_OK)
		reprogram_uni_rate(eng, req->class_id);

	enum omci_status answer = rc;

	if (rc == OMCI_OK) {
		if (vlan == OMCI_ERR_HW || gem == OMCI_ERR_HW ||
		    bridge == OMCI_ERR_HW)
			answer = OMCI_ERR_HW;
		else if (vlan == OMCI_ERR_IO || gem == OMCI_ERR_IO ||
			 bridge == OMCI_ERR_IO)
			answer = OMCI_ERR_IO;
	}

	reply(eng, req, result_for(answer), NULL, 0);
}

/* True when the table holds a row with this key. */
static bool table_has_key(const struct omci_engine *eng, uint16_t class_id,
			  uint16_t instance, uint8_t attr_no,
			  const uint8_t *entry, size_t key_len)
{
	const size_t n =
		omci_mib_table_count(eng->mib, class_id, instance, attr_no);
	for (size_t i = 0; i < n; i++) {
		uint8_t have[OMCI_MIB_TABLE_ENTRY_MAX] = { 0 };
		(void)!omci_mib_table_get(eng->mib, class_id, instance, attr_no,
					  i, have, sizeof(have));
		if (omci_wire_equal(have, entry, key_len))
			return true;
	}
	return false;
}

static bool is_vlan_rule_table(uint16_t class_id, uint8_t attr_no)
{
	return class_id == ME_EXT_VLAN_TAG_OPER && attr_no == ME171_RULE_ATTR;
}

/*
 * Set a row part of a table with a table control field. The ONU refuses a
 * row of a dynamic access control list that would overlap another, and
 * keeps the time left of an allowed preview group itself, ending the row
 * when a shorter duration leaves none (G.988 9.3.27, 9.3.28).
 */
static enum omci_status
set_ctrl_row(struct omci_engine *eng, const struct omci_frame *req,
	     uint8_t attr_no, const struct omci_me_attr *a,
	     const struct omci_me_table_policy *policy, const uint8_t *value)
{
	uint8_t row[OMCI_MIB_TABLE_ENTRY_MAX];

	omci_wire_copy(row, value, a->size);
	if (req->class_id == OMCI_MCAST_PROFILE &&
	    attr_no == OMCI_MCAST_PROFILE_DYNAMIC_ACL &&
	    omci_mcast_acl_conflicts(eng->mib, req->instance, row, a->size)) {
		OMCI_WARN(
			"set: %u/%u a row of the dynamic access control list overlaps another, or its range ends before it starts — refused",
			req->class_id, req->instance);
		return OMCI_DECLINED;
	}
	if (req->class_id == OMCI_MCAST_SUBSCRIBER &&
	    attr_no == OMCI_MCAST_SUBSCRIBER_PREVIEW &&
	    !omci_mcast_preview_written(eng->mib, req->instance, row, a->size))
		omci_me_table_op_set(row, OMCI_ME_TABLE_OP_DELETE);

	return omci_me_table_ctrl_set(eng->mib, req->class_id, req->instance,
				      attr_no, policy, row, a->size);
}

/*
 * Set one attribute. A table attribute takes one row per Set: the class's
 * table policy says whether it adds, replaces (same key) or removes a row.
 */
static enum omci_status set_one(struct omci_engine *eng,
				const struct omci_frame *req, uint8_t attr_no,
				const struct omci_me_attr *a,
				const uint8_t *value)
{
	struct omci_me_table_policy policy;

	if (!omci_me_value_allowed(req->class_id, attr_no, value)) {
		OMCI_WARN(
			"set: %u/%u attribute %u (%s) does not take %u — refused",
			req->class_id, req->instance, attr_no, a->name,
			value[0]);
		return OMCI_DECLINED;
	}

	if (!omci_me_table_policy(req->class_id, attr_no, &policy)) {
		(void)!omci_mib_attr_set_by_olt(eng->mib, req->class_id,
						req->instance, attr_no, value,
						a->size);

		enum omci_status gem = OMCI_OK, bridge = OMCI_OK;

		if (class_shapes_gem(req->class_id))
			gem = reprogram_gem(eng);
		if (class_builds_bridge(req->class_id))
			bridge = reprogram_bridge(eng);
		return data_path_status(req->class_id, req->instance, gem,
					bridge);
	}

	if (policy.ctrl != OMCI_ME_TABLE_CTRL_NONE)
		return set_ctrl_row(eng, req, attr_no, a, &policy, value);

	if (omci_me_table_is_delete(&policy, value, a->size)) {
		enum omci_status rc = omci_mib_table_remove(
			eng->mib, req->class_id, req->instance, attr_no, value,
			policy.key_len);

		if (rc == OMCI_DECLINED)
			rc = OMCI_OK;
		if (rc == OMCI_OK && req->class_id == ME_MCAST_GEM_IW_TP) {
			(void)reprogram_gem(eng);
			(void)reprogram_bridge(eng);
		}

		if (rc == OMCI_OK &&
		    is_vlan_rule_table(req->class_id, attr_no)) {
			const enum omci_status applied = omci_vlan_program(
				eng->mib, req->class_id, attr_no, NULL);
			if (applied != OMCI_OK && applied != OMCI_DECLINED)
				return applied;
		}
		return rc;
	}

	if (policy.max_entries != 0 &&
	    omci_mib_table_count(eng->mib, req->class_id, req->instance,
				 attr_no) >= policy.max_entries &&
	    !table_has_key(eng, req->class_id, req->instance, attr_no, value,
			   policy.key_len)) {
		OMCI_WARN("set: %u/%u attribute table is full at %u entries",
			  req->class_id, req->instance, policy.max_entries);
		return OMCI_ERR_NOMEM;
	}

	if (is_vlan_rule_table(req->class_id, attr_no) &&
	    !omci_vlan_has_room(eng->mib, req->class_id, attr_no, value,
				a->size)) {
		OMCI_WARN(
			"set: %u/%u — the classifier already holds its %u translations; refusing a new one",
			req->class_id, req->instance,
			(unsigned int)OMCI_HW_VLAN_MAX_RULES);
		return OMCI_ERR_NOMEM;
	}

	const enum omci_status rc = omci_mib_table_put(
		eng->mib, req->class_id, req->instance, attr_no, value, a->size,
		policy.key_len, policy.order);

	/* A row of a multicast address table can name a further GEM port. */
	if (rc == OMCI_OK && req->class_id == ME_MCAST_GEM_IW_TP) {
		(void)reprogram_gem(eng);
		(void)reprogram_bridge(eng);
	}

	if (rc == OMCI_OK && is_vlan_rule_table(req->class_id, attr_no)) {
		const enum omci_status applied = omci_vlan_program(
			eng->mib, req->class_id, attr_no, NULL);

		if (applied != OMCI_OK && applied != OMCI_DECLINED)
			return applied;

		if (omci_vlan_row_refused(value, a->size))
			return OMCI_DECLINED;

		struct omci_vlan_rule d;

		if (omci_vlan_decode(value, a->size, &d) &&
		    d.op == OMCI_VLAN_OP_UNSUPPORTED)
			return OMCI_DECLINED;
	}
	return rc;
}

/*
 * Set (G.988 A.3.5, A.3.6). Attributes that the class does not have, or
 * that cannot be written, are named in the two masks of the response
 * (G.988 A.1.3).
 */
static void handle_set(struct omci_engine *eng, const struct omci_frame *req)
{
	const struct omci_me_class *cls = omci_me_class_by_id(req->class_id);

	if (!omci_mib_exists(eng->mib, req->class_id, req->instance)) {
		reply(eng, req, RESULT_UNKNOWN_INST, NULL, 0);
		return;
	}

	const uint16_t mask =
		(uint16_t)((req->contents[0] << 8) | req->contents[1]);
	size_t off = 2;
	uint16_t unsupported = 0;
	uint16_t failed = 0;
	uint8_t message_level = 0;

	for (uint8_t attr = 1; attr <= OMCI_ATTR_MAX; attr++) {
		if (!omci_attr_in_mask(mask, attr))
			continue;

		const struct omci_me_attr *a = omci_me_attr_get(cls, attr);

		if (a == NULL) {
			unsupported |= omci_attr_bit(attr);
			break;
		}
		if (off + a->size > OMCI_CONTENTS_LEN) {
			failed |= omci_attr_bit(attr);
			break;
		}

		if ((a->access & OMCI_ME_ACC_WRITE) == 0) {
			OMCI_WARN(
				"set: %u/%u attribute %u (%s) is not writable — refused",
				req->class_id, req->instance, attr, a->name);
			failed |= omci_attr_bit(attr);
			off += a->size;
			continue;
		}

		const enum omci_status rc =
			set_one(eng, req, attr, a, &req->contents[off]);
		if (rc == OMCI_ERR_NOMEM || rc == OMCI_ERR_BUSY) {
			message_level = RESULT_DEVICE_BUSY;
			break;
		}
		if (rc == OMCI_DECLINED) {
			if (message_level == 0u)
				message_level = RESULT_PARAM_ERROR;
			off += a->size;
			continue;
		}
		if (rc != OMCI_OK)
			failed |= omci_attr_bit(attr);
		off += a->size;
	}

	for (uint8_t attr = 1; attr <= OMCI_ATTR_MAX; attr++) {
		const uint16_t bit = omci_attr_bit(attr);

		if (!omci_attr_in_mask(mask, attr) || (failed & bit) != 0u ||
		    (unsupported & bit) != 0u)
			continue;
		if (omci_me_attr_get(cls, attr) == NULL)
			unsupported |= bit;
	}
	reprogram_mcast(eng, req->class_id);
	reprogram_filters(eng, req->class_id);
	reprogram_uni_rate(eng, req->class_id);
	reprogram_tod(eng, req->class_id, req->instance,
		      mask & (uint16_t)~(failed | unsupported));
	omci_pm_changed(eng->pm, req->class_id, req->instance);

	if (message_level != 0u) {
		reply(eng, req, message_level, NULL, 0);
		return;
	}

	if (unsupported != 0u || failed != 0u) {
		const uint8_t masks[4] = {
			(uint8_t)(unsupported >> 8),
			(uint8_t)unsupported,
			(uint8_t)(failed >> 8),
			(uint8_t)failed,
		};
		reply(eng, req, RESULT_ATTR_FAILED, masks, sizeof(masks));
		return;
	}

	reply(eng, req, RESULT_OK, NULL, 0);
}

static void pm_refresh_for(struct omci_engine *eng,
			   const struct omci_frame *req, bool current);

/*
 * Get and Get current data (G.988 A.3.7, A.3.8, A.3.40, A.3.41). A table
 * attribute answers with its size and opens a Get next read. Attributes
 * that do not exist, or do not fit, are named in the masks at the end.
 */
static void handle_get_inner(struct omci_engine *eng,
			     const struct omci_frame *req, bool current)
{
	const struct omci_me_class *cls = omci_me_class_by_id(req->class_id);

	if (!omci_mib_exists(eng->mib, req->class_id, req->instance)) {
		reply(eng, req, RESULT_UNKNOWN_INST, NULL, 0);
		return;
	}

	pm_refresh_for(eng, req, current);

	const uint16_t want =
		(uint16_t)((req->contents[0] << 8) | req->contents[1]);

	uint8_t body[OMCI_CONTENTS_LEN];
	uint16_t got = 0;
	uint16_t unsupported = 0;
	uint16_t failed = 0;
	size_t off = 2;

	omci_wire_fill(body, 0, sizeof(body));

	unsigned int tables_named = 0;

	for (uint8_t attr = 1; attr <= OMCI_ATTR_MAX; attr++)
		if (omci_attr_in_mask(want, attr) &&
		    is_table(req->class_id, attr))
			tables_named++;

	for (uint8_t attr = 1; attr <= OMCI_ATTR_MAX; attr++) {
		if (!omci_attr_in_mask(want, attr))
			continue;

		const struct omci_me_attr *a = omci_me_attr_get(cls, attr);

		if (a == NULL) {
			unsupported |= omci_attr_bit(attr);
			continue;
		}

		if (is_table(req->class_id, attr)) {
			const size_t bytes = table_entries(eng, req->class_id,
							   req->instance,
							   attr) *
					     a->size;

			omci_be32_store(&body[off], (unsigned int)bytes);
			off += 4u;
			got |= omci_attr_bit(attr);

			if (tables_named == 1) {
				struct get_next_transfer *t =
					&eng->get_next[req->priority ? 1 : 0];
				t->active = bytes > 0;
				t->class_id = req->class_id;
				t->instance = req->instance;
				t->attr_no = attr;
				t->entry_size = (uint8_t)a->size;
				t->size = (uint32_t)bytes;
				t->max_seq = (uint16_t)((bytes +
							 GET_NEXT_BYTES - 1u) /
							GET_NEXT_BYTES);
				t->generation =
					omci_mib_table_generation(eng->mib);
			} else {
				eng->get_next[req->priority ? 1 : 0].active =
					false;
				OMCI_DEBUG(
					"get: %u/%u named %u table attributes in one mask — no walk opened",
					req->class_id, req->instance,
					tables_named);
			}
			continue;
		}

		if (off + a->size > 2u + GET_VALUE_BYTES) {
			failed |= omci_attr_bit(attr);
			break;
		}

		size_t n = 0;
		(void)!omci_mib_attr_get(eng->mib, req->class_id, req->instance,
					 attr, &body[off],
					 OMCI_CONTENTS_LEN - off, &n);
		off += n;
		got |= omci_attr_bit(attr);
	}

	for (uint8_t attr = 1; attr <= OMCI_ATTR_MAX; attr++) {
		const uint16_t bit = omci_attr_bit(attr);

		if ((want & bit) == 0u || (got & bit) != 0u ||
		    (unsupported & bit) != 0u || (failed & bit) != 0u)
			continue;
		if (omci_me_attr_get(cls, attr) == NULL)
			unsupported |= bit;
		else
			failed |= bit;
	}

	body[0] = (uint8_t)(got >> 8);
	body[1] = (uint8_t)(got & 0xffu);

	if (unsupported != 0u || failed != 0u) {
		body[OMCI_CONTENTS_LEN - 5u] = (uint8_t)(unsupported >> 8);
		body[OMCI_CONTENTS_LEN - 4u] = (uint8_t)(unsupported & 0xffu);
		body[OMCI_CONTENTS_LEN - 3u] = (uint8_t)(failed >> 8);
		body[OMCI_CONTENTS_LEN - 2u] = (uint8_t)(failed & 0xffu);
		reply(eng, req, RESULT_ATTR_FAILED, body,
		      OMCI_CONTENTS_LEN - 1u);
		return;
	}

	reply(eng, req, RESULT_OK, body, off);
}

/* A one-byte attribute of the ANI-G, or @absent. */
static uint8_t ani_g_u8(const struct omci_engine *eng, uint8_t attr,
			uint8_t absent)
{
	uint8_t v = absent;

	(void)!omci_mib_attr_get(eng->mib, OMCI_ALARM_ANI_G,
				 OMCI_ALARM_ANI_G_INSTANCE, attr, &v, sizeof(v),
				 NULL);
	return v;
}

/*
 * Alarm-reporting control (G.988 A.1.4.3). While the OLT has it on, the
 * alarms of the entity are kept but not notified. It ends by itself once
 * the entity has been trouble free for the interval, in minutes, which an
 * alarm starts again; the ONU then turns it off and says so with an
 * attribute value change. An interval of 255 never ends.
 */
static void arc_tick(struct omci_engine *eng)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_arc); i++) {
		const uint16_t cls = g_arc[i].class_id;
		const uint16_t inst = g_arc[i].instance;
		uint8_t on = 0, minutes = 0;

		(void)!omci_mib_attr_get(eng->mib, cls, inst, g_arc[i].arc, &on,
					 sizeof(on), NULL);
		(void)!omci_mib_attr_get(eng->mib, cls, inst, g_arc[i].interval,
					 &minutes, sizeof(minutes), NULL);

		if (on != 1u) {
			eng->arc[i].on = false;
			omci_alarm_suppress(eng->alarm, cls, inst, false);
			continue;
		}

		if (!eng->arc[i].on ||
		    omci_alarm_standing(eng->alarm, cls, inst))
			eng->arc[i].since = eng->now_sec;
		eng->arc[i].on = true;
		omci_alarm_suppress(eng->alarm, cls, inst, true);

		if (minutes == ARC_FOREVER ||
		    eng->now_sec - eng->arc[i].since < (uint64_t)minutes * 60u)
			continue;

		const uint8_t off = 0;

		OMCI_INFO(
			"alarm: %u/%u trouble free for %u min — reporting resumes",
			cls, inst, minutes);
		(void)!omci_mib_attr_set(eng->mib, cls, inst, g_arc[i].arc,
					 &off, sizeof(off));
		eng->arc[i].on = false;
		omci_alarm_suppress(eng->alarm, cls, inst, false);
	}
}

/* Keep an optical level of the ANI-G as the transceiver last read it. */
static void ani_g_level(struct omci_engine *eng, uint8_t attr, int16_t level)
{
	uint8_t v[2];

	omci_be16_store(v, (uint16_t)level);
	(void)!omci_mib_attr_set(eng->mib, OMCI_ALARM_ANI_G,
				 OMCI_ALARM_ANI_G_INSTANCE, attr, v, sizeof(v));
}

/* Put the optical levels last read into the ANI-G. */
static void keep_levels(struct omci_engine *eng)
{
	int16_t rx = 0, tx = 0;
	const uint8_t levels = omci_alarm_levels(eng->alarm, &rx, &tx);

	if ((levels & OMCI_ALARM_LEVEL_RX) != 0u)
		ani_g_level(eng, ANI_G_RX_LEVEL, rx);
	if ((levels & OMCI_ALARM_LEVEL_TX) != 0u)
		ani_g_level(eng, ANI_G_TX_LEVEL, tx);
}

/*
 * The threshold crossing alerts of the performance monitoring, which are
 * reported as alarms of the PM entities (G.988 I.4).
 */
static void pm_alerts(struct omci_engine *eng)
{
	uint16_t inst[OMCI_ALARM_MAX_RAISED];
	uint16_t cls;

	for (size_t i = 0; (cls = omci_pm_class_at(i)) != 0u; i++)
		for (uint8_t bit = 0; bit < OMCI_PM_TCA_BITS; bit++)
			omci_alarm_raised(eng->alarm, cls, bit, inst,
					  omci_pm_tca(eng->pm, cls, bit, inst,
						      ARRAY_SIZE(inst)));
}

/*
 * Once a second: judge the alarms the hardware does not raise, and keep
 * the optical levels of the ANI-G current; take the lost multicast group
 * alarms of the multicast operations profiles from the multicast agent and
 * the threshold crossing alerts from the performance monitoring; read the
 * alarms, apply alarm-reporting control, and notify the OLT of each entity
 * whose alarms changed (G.988 A.1.4). Only an entity the MIB holds is
 * notified.
 */
static void alarms_tick(struct omci_engine *eng)
{
	uint16_t lost[OMCI_ALARM_MAX_ENTITIES];

	const struct omci_alarm_thresholds t = {
		.sf = ani_g_u8(eng, ANI_G_SF, OMCI_BER_SF_DEFAULT),
		.sd = ani_g_u8(eng, ANI_G_SD, OMCI_BER_SD_DEFAULT),
		.optical = {
			.rx_low = ani_g_u8(eng, ANI_G_RX_LOW,
					   OMCI_OPTICAL_RX_POLICY),
			.rx_high = ani_g_u8(eng, ANI_G_RX_HIGH,
					    OMCI_OPTICAL_RX_POLICY),
			.tx_low = ani_g_u8(eng, ANI_G_TX_LOW,
					   OMCI_OPTICAL_TX_POLICY),
			.tx_high = ani_g_u8(eng, ANI_G_TX_HIGH,
					    OMCI_OPTICAL_TX_POLICY),
		},
	};
	(void)!omci_alarm_judge(eng->alarm, eng->now_sec, &t);
	keep_levels(eng);
	omci_alarm_raised(
		eng->alarm, OMCI_MCAST_PROFILE, OMCI_MCAST_ALARM_LOST, lost,
		omci_mcast_lost_profiles(eng->mcast, lost, ARRAY_SIZE(lost)));
	pm_alerts(eng);

	if (omci_alarm_poll(eng->alarm, NULL) != OMCI_OK) {
		OMCI_DEBUG("alarm: the hardware did not answer");
		return;
	}
	arc_tick(eng);

	struct omci_alarm_entry changed[OMCI_ALARM_MAX_ENTITIES];
	const size_t n = omci_alarm_take_changes(eng->alarm, changed,
						 ARRAY_SIZE(changed));

	for (size_t i = 0; i < n; i++) {
		if (!omci_mib_exists(eng->mib, changed[i].class_id,
				     changed[i].instance))
			continue;

		uint8_t frame[OMCI_FRAME_LEN];

		OMCI_INFO(
			"alarm: notifying %u/%u, alarms %02x %02x, sequence %u",
			changed[i].class_id, changed[i].instance,
			changed[i].bitmap[0], changed[i].bitmap[1],
			omci_alarm_sequence(eng->alarm));
		omci_alarm_build_notification(eng->alarm, &changed[i], frame);
		transmit(eng, frame);
	}
}

/*
 * Get all alarms (G.988 A.3.9, A.3.10): read the alarms, freeze them, and
 * answer the number of Get all alarms next commands they take.
 */
static void handle_get_all_alarms(struct omci_engine *eng,
				  const struct omci_frame *req)
{
	struct omci_frame rsp = {
		.tci = req->tci,

		.priority = req->priority,
		.msg_type = req->msg_type,
		.ak = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = req->class_id,
		.instance = req->instance,
	};

	/*
	 * The alarm sequence number starts again, and with the retrieval mode
	 * at 1 the entities under alarm-reporting control are left out
	 * (G.988 A.1.4.2).
	 */
	const bool skip_suppressed = req->contents[0] == 1u;

	omci_alarm_reset_sequence(eng->alarm);

	if (omci_alarm_poll(eng->alarm, NULL) != OMCI_OK)
		OMCI_WARN(
			"get-all-alarms: hardware read failed; answering from the last known state");

	const uint16_t count = omci_alarm_snapshot(eng->alarm, skip_suppressed);

	eng->alarm_upload_sec = eng->now_sec;
	omci_be16_store(&rsp.contents[0], count);

	uint8_t frame[OMCI_FRAME_LEN];
	(void)!omci_frame_build(&rsp, frame);
	transmit(eng, frame);
}

/*
 * The operational state of each VEIP: enabled while the stick routes, the
 * router being the domain behind it, and disabled while it bridges (G.988
 * 9.5.5). A change is announced.
 */
static void keep_veip(struct omci_engine *eng)
{
	const uint8_t state = omci_hw()->routing != NULL &&
					      omci_hw()->routing() ?
				      VEIP_ENABLED :
				      VEIP_DISABLED;
	struct omci_mib_cursor cur;

	for (enum omci_status rc = omci_mib_walk_first(eng->mib, &cur);
	     rc == OMCI_OK; rc = omci_mib_walk_next(eng->mib, &cur)) {
		uint8_t now = 0xffu;
		size_t len = 0;

		if (cur.class_id != ME_VEIP ||
		    (omci_mib_attr_get(eng->mib, ME_VEIP, cur.instance,
				       VEIP_OPER_STATE, &now, 1u,
				       &len) == OMCI_OK &&
		     now == state))
			continue;
		(void)!omci_mib_attr_set(eng->mib, ME_VEIP, cur.instance,
					 VEIP_OPER_STATE, &state, 1u);
	}
}

void omci_engine_tick(struct omci_engine *eng, uint64_t now_sec)
{
	if (eng == NULL)
		return;

	eng->now_sec = now_sec;

	(void)omci_pm_tick(eng->pm, now_sec);

	if (!eng->alarms_judged || now_sec != eng->alarm_sec) {
		eng->alarms_judged = true;
		eng->alarm_sec = now_sec;
		alarms_tick(eng);
		keep_veip(eng);
		omci_mcast_tick(eng->mcast, eng->mib, now_sec);
	}
}

static void handle_get(struct omci_engine *eng, const struct omci_frame *req)
{
	handle_get_inner(eng, req, false);
}

static void handle_get_current(struct omci_engine *eng,
			       const struct omci_frame *req)
{
	handle_get_inner(eng, req, true);
}

/* Bring the counters of a performance monitoring instance up to date for a Get. */
static void pm_refresh_for(struct omci_engine *eng,
			   const struct omci_frame *req, bool current)
{
	if (!omci_pm_owns(req->class_id))
		return;
	if (omci_pm_refresh(eng->pm, req->class_id, req->instance, current) !=
	    OMCI_OK)
		OMCI_WARN(
			"pm: %u/%u could not be refreshed; serving the last known counters",
			req->class_id, req->instance);
}

/* Get all alarms next (G.988 A.3.11, A.3.12): one entity and its alarm bitmap. */
static void handle_get_all_alarms_next(struct omci_engine *eng,
				       const struct omci_frame *req)
{
	struct omci_frame rsp = {
		.tci = req->tci,

		.priority = req->priority,
		.msg_type = req->msg_type,
		.ak = true,
		.device_id = OMCI_DEVICE_ID_BASELINE,
		.class_id = req->class_id,
		.instance = req->instance,
	};

	const uint16_t seq = (uint16_t)omci_be16(&req->contents[0]);

	/* An upload left idle for a minute has ended (G.988 A.1.4.2). */
	if (eng->now_sec - eng->alarm_upload_sec > ALARM_UPLOAD_IDLE_SEC)
		omci_alarm_snapshot_drop(eng->alarm);
	eng->alarm_upload_sec = eng->now_sec;

	struct omci_alarm_entry entry;

	if (omci_alarm_snapshot_get(eng->alarm, seq, &entry)) {
		omci_be16_store(&rsp.contents[0], entry.class_id);
		omci_be16_store(&rsp.contents[2], entry.instance);
		omci_wire_copy(&rsp.contents[4], entry.bitmap,
			       OMCI_ALARM_BITMAP_LEN);
	}

	uint8_t frame[OMCI_FRAME_LEN];
	(void)!omci_frame_build(&rsp, frame);
	transmit(eng, frame);
}

/* What the success of Synchronize time says was set (G.988 A.3.34). */
#define SYNC_TICK_SET 0u
#define SYNC_DATE_AND_TICK_SET 1u

/*
 * Synchronize time (G.988 A.3.33, A.3.34): the performance monitoring
 * starts again, its intervals ending on the quarter hours of the time the
 * OLT gives, or fifteen minutes from now when it gives none. With a date
 * the clock is set too, and handed to the backend for the stick's time
 * daemon; without one, the year, month and day 0, only the time of day
 * places the boundary. A date that does not exist, or a time
 * of day that does not, is refused with a parameter error and changes
 * nothing.
 */
static void handle_sync_time(struct omci_engine *eng,
			     const struct omci_frame *req)
{
	const uint16_t year = (uint16_t)omci_be16(&req->contents[0]);
	const uint8_t month = req->contents[2];
	const uint8_t day = req->contents[3];
	const uint8_t hour = req->contents[4];
	const uint8_t minute = req->contents[5];
	const uint8_t second = req->contents[6];
	const bool dated = year != 0u || month != 0u || day != 0u;
	unsigned int offset;
	uint8_t success;

	if (dated && omci_clock_sync(&eng->clock, year, month, day, hour,
				     minute, second, eng->now_sec)) {
		OMCI_INFO(
			"sync-time: clock set to %04u-%02u-%02u %02u:%02u:%02u UTC",
			year, month, day, hour, minute, second);
		if (omci_hw()->time_of_day != NULL)
			omci_hw()->time_of_day(
				omci_clock_now(&eng->clock, eng->now_sec));
		offset = (unsigned int)(omci_clock_now(&eng->clock,
						       eng->now_sec) %
					OMCI_PM_INTERVAL_SEC);
		success = SYNC_DATE_AND_TICK_SET;
	} else if (!dated && hour <= 23u && minute <= 59u && second <= 60u) {
		OMCI_INFO("sync-time: no date; time of day %02u:%02u:%02u",
			  hour, minute, second);
		offset = ((unsigned int)hour * 3600u +
			  (unsigned int)minute * 60u + second) %
			 OMCI_PM_INTERVAL_SEC;
		success = SYNC_TICK_SET;
	} else {
		OMCI_WARN(
			"sync-time: %04u-%02u-%02u %02u:%02u:%02u is not a real instant — refused",
			year, month, day, hour, minute, second);
		reply(eng, req, RESULT_PARAM_ERROR, NULL, 0);
		return;
	}

	omci_pm_synchronize(eng->pm, eng->now_sec, offset);
	reply(eng, req, RESULT_OK, &success, sizeof(success));
}

/* Test (G.988 A.3.21, A.3.22): answer, then send the test result (A.3.39). */
static void handle_test(struct omci_engine *eng, const struct omci_frame *req)
{
	if (!omci_mib_exists(eng->mib, req->class_id, req->instance)) {
		reply(eng, req, RESULT_UNKNOWN_INST, NULL, 0);
		return;
	}

	reply(eng, req, RESULT_OK, NULL, 0);

	uint8_t frame[OMCI_FRAME_LEN];

	if (omci_test_run(req->class_id, req->instance, req->contents[0],
			  req->tci, req->priority, frame))
		transmit(eng, frame);
}

enum omci_status omci_engine_init(struct omci_engine **out,
				  struct omci_mib *mib,
				  const struct omci_tx_ops *tx, void *tx_ctx)
{
	if (out == NULL || mib == NULL)
		return OMCI_ERR_INVAL;
	if (g_engine_taken)
		return OMCI_ERR_BUSY;

	omci_wire_fill(&g_engine, 0, sizeof(g_engine));

	enum omci_status rc = omci_alarm_init(&g_engine.alarm);

	if (rc != OMCI_OK)
		return rc;

	rc = omci_pm_init(&g_engine.pm, mib);
	if (rc != OMCI_OK) {
		omci_alarm_fini(g_engine.alarm);
		return rc;
	}

	omci_fdb_bind(mib);
	rc = omci_mcast_init(&g_engine.mcast);
	if (rc != OMCI_OK) {
		omci_pm_fini(g_engine.pm);
		omci_alarm_fini(g_engine.alarm);
		return rc;
	}

	g_engine_taken = true;
	g_engine.mib = mib;
	g_engine.tx = tx;
	g_engine.tx_ctx = tx_ctx;

	omci_mib_notify_set(mib, &notify_ops, &g_engine);

	*out = &g_engine;
	return OMCI_OK;
}

void omci_engine_tap_set(struct omci_engine *eng,
			 const struct omci_tap_ops *ops, void *ctx)
{
	if (eng == NULL)
		return;
	eng->tap = ops;
	eng->tap_ctx = ctx;
}

void omci_engine_fini(struct omci_engine *eng)
{
	if (eng == NULL)
		return;
	omci_mib_notify_set(eng->mib, NULL, NULL);
	omci_fdb_bind(NULL);
	omci_mcast_fini(eng->mcast);
	omci_pm_fini(eng->pm);
	omci_alarm_fini(eng->alarm);
	eng->mib = NULL;
	eng->tx = NULL;
	eng->tap = NULL;
	eng->tap_ctx = NULL;
	g_engine_taken = false;
}

static bool engine_handles(uint8_t mt)
{
	return omci_me_msg_implemented(mt);
}

/*
 * Answer a request that is not executed. The answers to MIB upload and Get
 * all alarms carry no result code; they report no command to follow.
 */
static void refuse(struct omci_engine *eng, const struct omci_frame *req,
		   uint8_t result)
{
	switch (req->msg_type) {
	case MT_MIB_UPLOAD:
	case MT_MIB_UPLOAD_N:
	case MT_GET_ALL_ALARMS:
	case MT_GET_ALL_ALARMS_N: {
		const struct omci_frame rsp = {
			.tci = req->tci,
			.priority = req->priority,
			.msg_type = req->msg_type,
			.ak = true,
			.device_id = OMCI_DEVICE_ID_BASELINE,
			.class_id = req->class_id,
			.instance = req->instance,
		};
		uint8_t frame[OMCI_FRAME_LEN];
		(void)!omci_frame_build(&rsp, frame);
		transmit(eng, frame);
		break;
	}
	default:
		reply(eng, req, result, NULL, 0);
		break;
	}
}

enum omci_status omci_engine_rx(struct omci_engine *eng,
				const uint8_t wire[static OMCI_FRAME_LEN])
{
	if (eng == NULL)
		return OMCI_ERR_INVAL;

	struct omci_frame req;
	enum omci_status rc = omci_frame_parse(wire, &req);

	if (rc != OMCI_OK) {
		OMCI_WARN("rx: not a frame this device speaks: %s",
			  omci_status_str(rc));
		tap(eng, OMCI_CTL_TAP_RX_PARSE, (uint8_t)(wire[2] & 0x1Fu),
		    wire);
		return rc;
	}

	if (!omci_frame_mic_ok(wire)) {
		OMCI_WARN("rx: MIC check failed, tid=%u mt=%u — dropping",
			  req.tci, req.msg_type);
		tap(eng, OMCI_CTL_TAP_RX_MIC, req.msg_type, wire);
		return OMCI_ERR_MIC;
	}

	/*
	 * A request asks for an answer and is not an answer itself; only a download
	 * section may come without the request bit (G.988 11.2.2).
	 */
	if (req.ak || (!req.ar && req.msg_type != MT_DOWNLOAD_SECTION)) {
		OMCI_WARN(
			"rx: malformed request tid=%u mt=%u (ar=%d ak=%d) — dropping",
			req.tci, req.msg_type, req.ar, req.ak);
		tap(eng, OMCI_CTL_TAP_RX_ARAK, req.msg_type, wire);
		return OMCI_ERR_PROTO;
	}

	OMCI_TRACE("rx tid=%u mt=%u class=%u inst=%u", req.tci, req.msg_type,
		   req.class_id, req.instance);

	eng->cur_tci = req.tci;

	/*
	 * Identifier 0 is the one the ONU's own notifications carry (G.988
	 * 11.2.1); neither such a request nor a download section is kept for
	 * a retransmission.
	 */
	const bool cacheable = (req.msg_type != MT_DOWNLOAD_SECTION) &&
			       req.tci != 0u;
	const unsigned int ch = req.priority ? 1u : 0u;

	if (cacheable) {
		struct resp_cache_entry *cached = cache_find(eng, ch, &req);

		if (cached != NULL) {
			OMCI_DEBUG(
				"rx: retransmission tid=%u mt=%u — replaying cached answer",
				req.tci, req.msg_type);
			tap(eng, OMCI_CTL_TAP_RX_REPLAY, req.msg_type, wire);
			transmit(eng, cached->frame);
			return OMCI_OK;
		}
	}

	eng->did_tx = false;

	if (!engine_handles(req.msg_type)) {
		OMCI_DEBUG("rx: message type %u not implemented", req.msg_type);
		tap(eng, OMCI_CTL_TAP_RX_UNHANDLED, req.msg_type, wire);
		refuse(eng, &req, RESULT_CMD_NOT_SUPPORTED);
		goto answered;
	}

	if (omci_me_class_by_id(req.class_id) == NULL) {
		OMCI_DEBUG("rx: class %u is not implemented by this device",
			   req.class_id);
		tap(eng, OMCI_CTL_TAP_RX_UNKNOWN_ME, req.msg_type, wire);
		refuse(eng, &req, RESULT_UNKNOWN_ME);
		goto answered;
	}

	if (!omci_me_action_allowed(req.class_id, req.msg_type)) {
		OMCI_DEBUG(
			"rx: class %u does not accept message type %u — refused",
			req.class_id, req.msg_type);
		tap(eng, OMCI_CTL_TAP_RX_ACTION, req.msg_type, wire);
		refuse(eng, &req, RESULT_CMD_NOT_SUPPORTED);
		goto answered;
	}

	tap(eng, OMCI_CTL_TAP_RX_DISPATCH, req.msg_type, wire);

	switch (req.msg_type) {
	case MT_MIB_RESET:
		handle_mib_reset(eng, &req);
		break;
	case MT_MIB_UPLOAD:
		handle_mib_upload(eng, &req);
		break;
	case MT_MIB_UPLOAD_N:
		handle_mib_upload_next(eng, &req);
		break;
	case MT_CREATE:
		handle_create(eng, &req);
		break;
	case MT_DELETE:
		handle_delete(eng, &req);
		break;
	case MT_SET:
		handle_set(eng, &req);
		break;
	case MT_GET:
		handle_get(eng, &req);
		break;
	case MT_GET_NEXT:
		handle_get_next(eng, &req);
		break;

	case MT_GET_CURRENT_DATA:
		handle_get_current(eng, &req);
		break;

	case MT_GET_ALL_ALARMS:
		handle_get_all_alarms(eng, &req);
		break;
	case MT_GET_ALL_ALARMS_N:
		handle_get_all_alarms_next(eng, &req);
		break;
	case MT_SYNC_TIME:
		handle_sync_time(eng, &req);
		break;
	case MT_TEST:
		handle_test(eng, &req);
		break;
	}
	if (req.msg_type == MT_MIB_RESET || req.msg_type == MT_CREATE ||
	    req.msg_type == MT_DELETE || req.msg_type == MT_SET ||
	    req.msg_type == MT_SYNC_TIME)
		eng->changes++;

answered:

	/* Keep the answer for a retransmission of the same request. */
	if (eng->did_tx && cacheable)
		cache_store(eng, ch, &req, eng->last_tx);
	return OMCI_OK;
}
