/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/pon.yaml */
/* YNL-GEN uapi header */

#ifndef _UAPI_LINUX_PON_H
#define _UAPI_LINUX_PON_H

#define PON_FAMILY_NAME		"pon"
#define PON_FAMILY_VERSION	1

/**
 * enum pon_type - The kind of passive optical network, by the ITU-T
 *   recommendation that its transmission convergence layer follows.
 * @PON_TYPE_GPON: G-PON, ITU-T G.984: 2.48832 Gbit/s downstream, 1.24416 or
 *   2.48832 Gbit/s upstream.
 * @PON_TYPE_XGPON: XG-PON, ITU-T G.987: 9.95328 Gbit/s downstream, 2.48832
 *   Gbit/s upstream.
 * @PON_TYPE_XGSPON: XGS-PON, ITU-T G.9807.1: 9.95328 Gbit/s both ways.
 * @PON_TYPE_NGPON2: NG-PON2, ITU-T G.989: several TWDM wavelength channels.
 */
enum pon_type {
	PON_TYPE_GPON = 1,
	PON_TYPE_XGPON,
	PON_TYPE_XGSPON,
	PON_TYPE_NGPON2,
};

/**
 * enum pon_onu_state - The activation states of an ONU, O1 to O7, as ITU-T
 *   G.984.3 clause 10.2.1 defines them, and the corresponding states of the
 *   other transmission convergence layers. An ONU transmits upstream only in
 *   O3, O4 and O5, and carries traffic only in O5.
 * @PON_ONU_STATE_O1: Initial: LOS or LOF is asserted.
 * @PON_ONU_STATE_O2: Standby: the downstream is synchronised, and the ONU
 *   waits for Upstream_Overhead.
 * @PON_ONU_STATE_O3: Serial number: the ONU answers serial number requests and
 *   waits for Assign_ONU-ID.
 * @PON_ONU_STATE_O4: Ranging: the ONU has its ONU-ID, answers ranging requests
 *   and waits for its equalisation delay (Ranging_Time).
 * @PON_ONU_STATE_O5: Operation: the ONU sends data and PLOAM messages as the
 *   OLT directs.
 * @PON_ONU_STATE_O6: POPUP: entered from O5 on LOS or LOF, with upstream
 *   transmission stopped. A directed POPUP message returns the ONU to O5, a
 *   broadcast one to O4, and expiry of timer TO2 to O1.
 * @PON_ONU_STATE_O7: Emergency stop: after Disable_Serial_Number the laser is
 *   off and the ONU sends nothing upstream.
 */
enum pon_onu_state {
	PON_ONU_STATE_O1 = 1,
	PON_ONU_STATE_O2,
	PON_ONU_STATE_O3,
	PON_ONU_STATE_O4,
	PON_ONU_STATE_O5,
	PON_ONU_STATE_O6,
	PON_ONU_STATE_O7,
};

/**
 * enum pon_alarm - Alarms of the downstream: conditions present now, not
 *   events seen. A MAC reports the flags it has.
 * @PON_ALARM_LOS: Loss of signal: no valid downstream signal is received
 *   (ITU-T G.984.3 clause 11.1.2).
 * @PON_ALARM_LOF: Loss of frame: five consecutive invalid PSync fields (ITU-T
 *   G.984.3 clause 11.1.2).
 * @PON_ALARM_LOM: Loss of PLOAM: the frames are synchronised but the PLOAM
 *   channel is not delineated. It is not an ONU alarm of ITU-T G.984.3, which
 *   lists loss of PLOAM only among the OLT's alarms, but a condition some MACs
 *   report; that a MAC's flag means exactly this is an assumption, not
 *   verified.
 */
enum pon_alarm {
	PON_ALARM_LOS = 1,
	PON_ALARM_LOF = 2,
	PON_ALARM_LOM = 4,
};

enum {
	PON_A_DEV_STATS_SN_REQUESTS = 1,
	PON_A_DEV_STATS_RANGING_REQUESTS,
	PON_A_DEV_STATS_PLOAM_RX,
	PON_A_DEV_STATS_PLOAM_CRC_ERRORS,
	PON_A_DEV_STATS_PLOAM_OVERFLOWS,
	PON_A_DEV_STATS_BWMAP_RX,
	PON_A_DEV_STATS_BWMAP_CRC_ERRORS,
	PON_A_DEV_STATS_BWMAP_OVERFLOWS,
	PON_A_DEV_STATS_GEM_FRAMES,
	PON_A_DEV_STATS_BIP_ERRORS,

	__PON_A_DEV_STATS_MAX,
	PON_A_DEV_STATS_MAX = (__PON_A_DEV_STATS_MAX - 1)
};

enum {
	PON_A_DEV_ID = 1,
	PON_A_DEV_IFINDEX,
	PON_A_DEV_TYPE,
	PON_A_DEV_ONU_STATE,
	PON_A_DEV_ONU_ID,
	PON_A_DEV_ALLOC_ID,
	PON_A_DEV_OMCC_PORT_ID,
	PON_A_DEV_ALARMS,
	PON_A_DEV_STATS,
	PON_A_DEV_SERIAL_NUMBER,
	PON_A_DEV_NUM_GEM_PORTS,
	PON_A_DEV_NUM_TCONTS,

	__PON_A_DEV_MAX,
	PON_A_DEV_MAX = (__PON_A_DEV_MAX - 1)
};

enum {
	PON_A_TCONT_ID = 1,
	PON_A_TCONT_INDEX,
	PON_A_TCONT_ALLOC_ID,

	__PON_A_TCONT_MAX,
	PON_A_TCONT_MAX = (__PON_A_TCONT_MAX - 1)
};

enum {
	PON_CMD_DEV_GET = 1,
	PON_CMD_DEV_CHANGE_NTF,
	PON_CMD_TCONT_GET,

	__PON_CMD_MAX,
	PON_CMD_MAX = (__PON_CMD_MAX - 1)
};

#define PON_MCGRP_MONITOR	"monitor"

#endif /* _UAPI_LINUX_PON_H */
