// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL9602C GPON MAC: the calculations of the driver that touch no
 * register, apart so that they can be tested (rtl9602c-pon-test.c, which this
 * file includes with CONFIG_PON_RTL9602C_KUNIT_TEST): the parsing of the
 * serial number and the password, the length of a burst's overhead, the
 * split of the equalization delay, the units of a queue's rates, and the names
 * of the PLOAM messages.
 */

#define pr_fmt(fmt) "rtl9602c-switch: " fmt

#include <linux/bitops.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/string.h>

#include "rtl9602c-pon.h"

/**
 * rtl9602c_gpon_sn_parse() - parse the ONU's serial number
 * @s: the string devlink gives: the four characters of the Vendor_ID, then
 *	the eight hexadecimal digits of the vendor-specific serial number
 * @sn: where the eight octets go
 *
 * ITU-T G.984.3 clause 9.2.4.1: the serial number is the Vendor_ID, four
 * printable octets, and the vendor-specific serial number, four octets.
 * Characters after the twelfth are not read.
 *
 * Return: 0, or -EINVAL for a string shorter than twelve characters, a
 * Vendor_ID that is not printable ASCII or a vendor-specific serial number
 * that is not hexadecimal; @sn is then undefined.
 */
int rtl9602c_gpon_sn_parse(const char *s, u8 sn[RTL9602C_GPON_SN_BYTES])
{
	unsigned int i;

	if (strlen(s) < 12)
		return -EINVAL;

	for (i = 0; i < 4; i++) {
		if (s[i] < 0x20 || s[i] > 0x7e)
			return -EINVAL;
		sn[i] = s[i];
	}
	for (i = 0; i < 4; i++) {
		int hi = hex_to_bin(s[4 + i * 2]);
		int lo = hex_to_bin(s[5 + i * 2]);

		if (hi < 0 || lo < 0)
			return -EINVAL;
		sn[4 + i] = (hi << 4) | lo;
	}
	return 0;
}

/**
 * rtl9602c_gpon_password_parse() - parse the ONU's registration password
 * @s: a non-empty string: up to ten printable ASCII characters, or "0x" and
 *	twenty hexadecimal digits
 * @pw: where the ten octets go, a short ASCII password padded with zeros
 *
 * Return: 0, or -EINVAL for a string that is neither, with @pw undefined.
 */
int rtl9602c_gpon_password_parse(const char *s,
				 u8 pw[RTL9602C_GPON_PASSWORD_BYTES])
{
	size_t len = strlen(s);
	unsigned int i;

	memset(pw, 0, RTL9602C_GPON_PASSWORD_BYTES);
	if (len == 2 + 2 * RTL9602C_GPON_PASSWORD_BYTES && s[0] == '0' &&
	    (s[1] == 'x' || s[1] == 'X'))
		return hex2bin(pw, s + 2, RTL9602C_GPON_PASSWORD_BYTES) ?
		       -EINVAL : 0;

	if (len > RTL9602C_GPON_PASSWORD_BYTES)
		return -EINVAL;
	for (i = 0; i < len; i++) {
		if (s[i] < 0x20 || s[i] > 0x7e)
			return -EINVAL;
		pw[i] = s[i];
	}
	return 0;
}

/**
 * rtl9602c_gpon_boh_bytes_of() - the bytes of a burst's overhead
 * @ovh: the last Upstream_Overhead message's octets
 * @type3: the bytes of the type 3 preamble
 *
 * The guard time (capped at 32 bits), the type 3 preamble, the delimiter and
 * the type 1 and type 2 preambles (ITU-T G.984.3 clause 9.2.3.1).
 *
 * Return: the number of bytes.
 */
unsigned int rtl9602c_gpon_boh_bytes_of(const u8 *ovh, u8 type3)
{
	return min_t(unsigned int, ovh[RTL9602C_PLOAM_OVERHEAD_GUARD_BITS], 32) / 8 +
	       type3 + RTL9602C_PLOAM_OVERHEAD_DELIMITER_LEN +
	       (ovh[RTL9602C_PLOAM_OVERHEAD_TYPE1_BITS] +
		ovh[RTL9602C_PLOAM_OVERHEAD_TYPE2_BITS]) / 8;
}

/**
 * rtl9602c_gpon_boh_len_of() - the length field of the burst overhead
 * @ovh: the last Upstream_Overhead message's octets
 * @type3: the bytes of the type 3 preamble
 *
 * Return: the default length when there is no type 3 preamble, else the bytes
 * of the overhead within the field's limits.
 */
u8 rtl9602c_gpon_boh_len_of(const u8 *ovh, u8 type3)
{
	if (!type3)
		return RTL9602C_BURST_CFG_LEN_DEF;
	return clamp_t(unsigned int, rtl9602c_gpon_boh_bytes_of(ovh, type3),
		       RTL9602C_BURST_CFG_LEN_DEF, RTL9602C_BURST_CFG_LEN_MAX);
}

/**
 * rtl9602c_gpon_eqd_split() - split an equalization delay
 * @eqd: the delay in bits: the one the OLT gave plus the coarse response delay
 * @frames: where the whole upstream frames go
 * @bits: where the bits within the last frame go
 */
void rtl9602c_gpon_eqd_split(u32 eqd, u32 *frames, u32 *bits)
{
	*frames = eqd / RTL9602C_GPON_EQD_FRAME_BITS;
	*bits = eqd % RTL9602C_GPON_EQD_FRAME_BITS;
}

/**
 * rtl9602c_gem_rate_units() - the units of a queue's rates
 * @cir: the sum of the committed rates of the GEM ports in the queue, in
 *	bytes a second
 * @pir: the sum of their peak rates
 * @limited: every port has a peak rate: otherwise the queue has no limit
 * @cir_u: where the committed rate goes, in units of the queue's field
 * @pir_u: where the peak rate goes
 *
 * A committed rate is rounded up to whole units and a peak rate down, to one
 * unit at least; both are kept within the field, and the committed rate
 * within the peak rate. A queue with no limit gets the field's largest value.
 */
void rtl9602c_gem_rate_units(u64 cir, u64 pir, bool limited, u32 *cir_u,
			     u32 *pir_u)
{
	u64 units;
	u32 peak = RTL9602C_PONSCH_QUEUE_RATE_MAX;

	if (limited && pir)
		peak = clamp_t(u64, pir / RTL9602C_GEM_RATE_UNIT, 1,
			       RTL9602C_PONSCH_QUEUE_RATE_MAX);
	units = cir / RTL9602C_GEM_RATE_UNIT + !!(cir % RTL9602C_GEM_RATE_UNIT);
	*cir_u = min_t(u64, units, peak);
	*pir_u = peak;
}

/*
 * Downstream message names by identifier, as G.984.3 clause 9.2.3 and its
 * annexes give them; 2 and 7 are deprecated.
 */
const char *rtl9602c_ploam_ds_name(u8 id)
{
	const char * const names[] = {
		[1]  = "Upstream_Overhead",	[2]  = "Serial_number_mask",
		[3]  = "Assign_ONU-ID",		[4]  = "Ranging_Time",
		[5]  = "Deactivate_ONU-ID",	[6]  = "Disable_serial_number",
		[7]  = "Configure_VP/VC",	[8]  = "Encrypted_Port-ID",
		[9]  = "Request_password",	[10] = "Assign_Alloc-ID",
		[11] = "No_message",		[12] = "POPUP",
		[13] = "Request_Key",		[14] = "Configure_Port-ID",
		[15] = "Physical_Equipment_Error", [16] = "Change_Power_Level",
		[17] = "PST",			[18] = "BER_Interval",
		[19] = "Key_switching_time",	[20] = "Extended_Burst_Length",
		[21] = "PON-ID",		[22] = "Swift_POPUP",
		[23] = "Ranging_adjustment",	[24] = "Sleep_Allow",
	};

	return id < ARRAY_SIZE(names) && names[id] ? names[id] : "unknown";
}

/*
 * Upstream message names by identifier, as G.984.3 clause 9.2.4 and its annex E
 * give them.
 */
const char *rtl9602c_ploam_us_name(u8 id)
{
	const char * const names[] = {
		[1] = "Serial_Number_ONU",	[2] = "Password",
		[3] = "Dying_Gasp",		[4] = "No_message",
		[5] = "Encryption_Key",		[6] = "Physical_Equipment_Error",
		[7] = "PST",			[8] = "REI",
		[9] = "Acknowledge",		[10] = "Sleep_Request",
	};

	return id < ARRAY_SIZE(names) && names[id] ? names[id] : "unknown";
}

#if IS_ENABLED(CONFIG_PON_RTL9602C_KUNIT_TEST)
#include "rtl9602c-pon-test.c"
#endif
