// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests of the calculations in rtl9602c-pon-calc.c, which includes this
 * file when CONFIG_PON_RTL9602C_KUNIT_TEST is set: the parsing of the serial
 * number and the password, the length of a burst's overhead, the split of the
 * equalization delay, the transmit power level modes, the units of a queue's
 * rates, the numbering of T-CONTs and their queues, the queues a T-CONT's GEM
 * ports send through, the names of the PLOAM messages, and the state
 * Disable_Serial_Number leaves the ONU in.
 */

#include <kunit/test.h>

/*
 * A serial number as devlink gives it: the four characters of the Vendor_ID
 * and eight hexadecimal digits, assembled here so that the source holds no
 * serial number.
 */
static void rtl9602c_test_make_sn(char *buf, size_t len, u32 tail, bool upper)
{
	snprintf(buf, len, upper ? "%c%c%c%c%08X" : "%c%c%c%c%08x",
		 'T', 'E', 'S', 'T', tail);
}

static void rtl9602c_test_sn_parse(struct kunit *test)
{
	static const u8 want[RTL9602C_GPON_SN_BYTES] = {
		'T', 'E', 'S', 'T', 0x12, 0xab, 0x34, 0xcd
	};
	u8 sn[RTL9602C_GPON_SN_BYTES];
	char s[16];

	/* Either case of the hexadecimal digits gives the same octets. */
	rtl9602c_test_make_sn(s, sizeof(s), 0x12ab34cd, false);
	KUNIT_ASSERT_EQ(test, rtl9602c_gpon_sn_parse(s, sn), 0);
	KUNIT_EXPECT_MEMEQ(test, sn, want, sizeof(want));

	rtl9602c_test_make_sn(s, sizeof(s), 0x12ab34cd, true);
	memset(sn, 0, sizeof(sn));
	KUNIT_ASSERT_EQ(test, rtl9602c_gpon_sn_parse(s, sn), 0);
	KUNIT_EXPECT_MEMEQ(test, sn, want, sizeof(want));

	/* Characters after the twelfth are not read. */
	s[12] = 'x';
	s[13] = '\0';
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_sn_parse(s, sn), 0);
}

static void rtl9602c_test_sn_parse_refuses(struct kunit *test)
{
	u8 sn[RTL9602C_GPON_SN_BYTES];
	char s[16];
	unsigned int i;

	/* Twelve characters at least. */
	rtl9602c_test_make_sn(s, sizeof(s), 0x12ab34cd, false);
	s[11] = '\0';
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_sn_parse(s, sn), -EINVAL);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_sn_parse("", sn), -EINVAL);

	/* A Vendor_ID of printable ASCII: 0x20 to 0x7e. */
	for (i = 0; i < 4; i++) {
		rtl9602c_test_make_sn(s, sizeof(s), 0x12ab34cd, false);
		s[i] = 0x1f;
		KUNIT_EXPECT_EQ(test, rtl9602c_gpon_sn_parse(s, sn), -EINVAL);
		s[i] = 0x7f;
		KUNIT_EXPECT_EQ(test, rtl9602c_gpon_sn_parse(s, sn), -EINVAL);
		s[i] = 0x20;
		KUNIT_EXPECT_EQ(test, rtl9602c_gpon_sn_parse(s, sn), 0);
		KUNIT_EXPECT_EQ(test, sn[i], 0x20);
		s[i] = 0x7e;
		KUNIT_EXPECT_EQ(test, rtl9602c_gpon_sn_parse(s, sn), 0);
		KUNIT_EXPECT_EQ(test, sn[i], 0x7e);
	}

	/* The other eight characters are hexadecimal digits, each of them. */
	for (i = 4; i < 12; i++) {
		rtl9602c_test_make_sn(s, sizeof(s), 0x12ab34cd, false);
		s[i] = 'g';
		KUNIT_EXPECT_EQ(test, rtl9602c_gpon_sn_parse(s, sn), -EINVAL);
		s[i] = ' ';
		KUNIT_EXPECT_EQ(test, rtl9602c_gpon_sn_parse(s, sn), -EINVAL);
	}
}

static void rtl9602c_test_pw_parse(struct kunit *test)
{
	static const u8 text[RTL9602C_GPON_PASSWORD_BYTES] = { 'a', 'b', 'c' };
	static const u8 full[RTL9602C_GPON_PASSWORD_BYTES] = {
		'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'
	};
	static const u8 octets[RTL9602C_GPON_PASSWORD_BYTES] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x9a
	};
	u8 pw[RTL9602C_GPON_PASSWORD_BYTES];

	/* A short text is padded with zeros. */
	memset(pw, 0xff, sizeof(pw));
	KUNIT_ASSERT_EQ(test, rtl9602c_gpon_password_parse("abc", pw), 0);
	KUNIT_EXPECT_MEMEQ(test, pw, text, sizeof(text));

	/* Ten characters are the most. */
	KUNIT_ASSERT_EQ(test, rtl9602c_gpon_password_parse("0123456789", pw), 0);
	KUNIT_EXPECT_MEMEQ(test, pw, full, sizeof(full));
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_password_parse("01234567890", pw),
			-EINVAL);

	/* "0x" and twenty hexadecimal digits are ten octets. */
	KUNIT_ASSERT_EQ(test, rtl9602c_gpon_password_parse("0x00112233445566778899", pw), 0);
	KUNIT_EXPECT_EQ(test, pw[0], 0x00);
	KUNIT_EXPECT_EQ(test, pw[9], 0x99);
	KUNIT_ASSERT_EQ(test, rtl9602c_gpon_password_parse("0X00112233445566778899", pw), 0);
	KUNIT_ASSERT_EQ(test, rtl9602c_gpon_password_parse("0x0011223344556677889a", pw), 0);
	KUNIT_EXPECT_MEMEQ(test, pw, octets, sizeof(octets));
	KUNIT_ASSERT_EQ(test, rtl9602c_gpon_password_parse("0x0011223344556677889A", pw), 0);
	KUNIT_EXPECT_MEMEQ(test, pw, octets, sizeof(octets));

	/* Ten characters that start with "0x" are text, not octets. */
	KUNIT_ASSERT_EQ(test, rtl9602c_gpon_password_parse("0x12345678", pw), 0);
	KUNIT_EXPECT_EQ(test, pw[0], '0');
	KUNIT_EXPECT_EQ(test, pw[1], 'x');
	KUNIT_EXPECT_EQ(test, pw[9], '8');
}

static void rtl9602c_test_pw_parse_refuses(struct kunit *test)
{
	u8 pw[RTL9602C_GPON_PASSWORD_BYTES];

	/* A digit that is none, and the wrong number of digits after "0x". */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_password_parse("0x0011223344556677889g", pw), -EINVAL);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_password_parse("0x001122334455667788", pw), -EINVAL);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_password_parse("0x001122334455667788990", pw), -EINVAL);

	/* A text of printable ASCII only. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_password_parse("ab\x1f", pw), -EINVAL);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_password_parse("ab\x7f", pw), -EINVAL);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_password_parse("ab \x7e", pw), 0);
}

/*
 * The octets of an Upstream_Overhead message that the length reads: the guard
 * time, and the type 1 and type 2 preambles, in bits.
 */
static void rtl9602c_test_overhead(u8 *ovh, u8 guard, u8 type1, u8 type2)
{
	memset(ovh, 0, RTL9602C_GPON_PLOAM_BYTES);
	ovh[RTL9602C_PLOAM_OVERHEAD_GUARD_BITS] = guard;
	ovh[RTL9602C_PLOAM_OVERHEAD_TYPE1_BITS] = type1;
	ovh[RTL9602C_PLOAM_OVERHEAD_TYPE2_BITS] = type2;
}

static void rtl9602c_test_boh_bytes(struct kunit *test)
{
	u8 ovh[RTL9602C_GPON_PLOAM_BYTES];

	/* 32 guard bits, no type 1 or 2: 4 bytes, the type 3 preamble, the delimiter. */
	rtl9602c_test_overhead(ovh, 32, 0, 0);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_boh_bytes_of(ovh, 0),
			4 + RTL9602C_PLOAM_OVERHEAD_DELIMITER_LEN);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_boh_bytes_of(ovh, 60),
			4 + 60 + RTL9602C_PLOAM_OVERHEAD_DELIMITER_LEN);

	/* The guard time counts for 32 bits at most. */
	rtl9602c_test_overhead(ovh, 255, 0, 0);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_boh_bytes_of(ovh, 0),
			4 + RTL9602C_PLOAM_OVERHEAD_DELIMITER_LEN);
	rtl9602c_test_overhead(ovh, 8, 0, 0);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_boh_bytes_of(ovh, 0),
			1 + RTL9602C_PLOAM_OVERHEAD_DELIMITER_LEN);

	/* The type 1 and type 2 preambles are given in bits. */
	rtl9602c_test_overhead(ovh, 32, 32, 16);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_boh_bytes_of(ovh, 0),
			4 + RTL9602C_PLOAM_OVERHEAD_DELIMITER_LEN + 6);
}

static void rtl9602c_test_boh_len(struct kunit *test)
{
	u8 ovh[RTL9602C_GPON_PLOAM_BYTES];

	rtl9602c_test_overhead(ovh, 32, 0, 0);

	/* With no type 3 preamble the length is the default. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_boh_len_of(ovh, 0), RTL9602C_BURST_CFG_LEN_DEF);

	/* Within the limits it is the bytes of the overhead. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_boh_len_of(ovh, 60),
			rtl9602c_gpon_boh_bytes_of(ovh, 60));

	/* Below the default it is the default, above the field it is its largest. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_boh_len_of(ovh, 1), RTL9602C_BURST_CFG_LEN_DEF);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_boh_len_of(ovh, 255), RTL9602C_BURST_CFG_LEN_MAX);
}

static void rtl9602c_test_power_mode_of(struct kunit *test)
{
	u8 ovh[RTL9602C_GPON_PLOAM_BYTES] = {};

	/* pp, bits 1:0 of the flags octet: modes 0, 1 and 2. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_mode_of(ovh), 0);
	ovh[RTL9602C_PLOAM_OVERHEAD_FLAGS] = 0x01;
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_mode_of(ovh), 1);
	ovh[RTL9602C_PLOAM_OVERHEAD_FLAGS] = 0x02;
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_mode_of(ovh), 2);

	/* '11' is reserved: mode 0. */
	ovh[RTL9602C_PLOAM_OVERHEAD_FLAGS] = 0x03;
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_mode_of(ovh), 0);

	/* The other flags do not count: e set, as the test OLT sends it. */
	ovh[RTL9602C_PLOAM_OVERHEAD_FLAGS] = 0x20;
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_mode_of(ovh), 0);
	ovh[RTL9602C_PLOAM_OVERHEAD_FLAGS] = 0xfe;
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_mode_of(ovh), 2);
}

static void rtl9602c_test_power_cpl(struct kunit *test)
{
	const u8 up = RTL9602C_PLOAM_CPL_IND_INCREASE;
	const u8 down = RTL9602C_PLOAM_CPL_IND_DECREASE;

	/* More power is a lower mode, less power a higher one. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_cpl(1, up), 0);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_cpl(2, up), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_cpl(0, down), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_cpl(1, down), 2);

	/* No step past either end. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_cpl(0, up), 0);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_cpl(2, down), 2);

	/* '00' and '11' ask for nothing; the bits above ID do not count. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_cpl(1, 0), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_cpl(1, 3), 1);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_power_cpl(1, 0xfc | down), 2);
}

static void rtl9602c_test_power_sn_step(struct kunit *test)
{
	u32 answered;
	u8 mode = 0;
	unsigned int i;

	/* Below the threshold nothing changes. */
	answered = RTL9602C_GPON_SN_REQUEST_THRESHOLD - 1;
	KUNIT_EXPECT_FALSE(test, rtl9602c_gpon_power_sn_step(&answered, &mode));
	KUNIT_EXPECT_EQ(test, mode, 0);
	KUNIT_EXPECT_EQ(test, answered, RTL9602C_GPON_SN_REQUEST_THRESHOLD - 1);

	/* At the threshold the next mode, modulo 3, counted again from none. */
	for (i = 1; i <= 4; i++) {
		answered = RTL9602C_GPON_SN_REQUEST_THRESHOLD;
		KUNIT_EXPECT_TRUE(test, rtl9602c_gpon_power_sn_step(&answered, &mode));
		KUNIT_EXPECT_EQ(test, mode, i % 3);
		KUNIT_EXPECT_EQ(test, answered, 0);
	}

	/* Requests beyond the threshold make one step, and none carries over. */
	mode = 2;
	answered = 3 * RTL9602C_GPON_SN_REQUEST_THRESHOLD + 5;
	KUNIT_EXPECT_TRUE(test, rtl9602c_gpon_power_sn_step(&answered, &mode));
	KUNIT_EXPECT_EQ(test, mode, 0);
	KUNIT_EXPECT_EQ(test, answered, 0);
	KUNIT_EXPECT_FALSE(test, rtl9602c_gpon_power_sn_step(&answered, &mode));
}

static void rtl9602c_test_tod_delta(struct kunit *test)
{
	/* 200 us of round trip, 248832 bits: 0.500085 of it. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_tod_delta_ns(248832), 100017);
	/* A live ranging's delay with the default response time. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_tod_delta_ns(97331 + 43520), 56614);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_tod_delta_ns(0), 0);
	/* The largest delay does not overflow: 3.452 s of round trip. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_tod_delta_ns(U32_MAX), 1726344984);
}

static void rtl9602c_test_eqd_split(struct kunit *test)
{
	u32 frames, bits;

	/* A delay of a live ranging: less than a frame. */
	rtl9602c_gpon_eqd_split(97331 + 290 * RTL9602C_GTC_US_RESP_DELAY_COARSE_BITS,
				&frames, &bits);
	KUNIT_EXPECT_EQ(test, frames, 0);
	KUNIT_EXPECT_EQ(test, bits, 134451);

	rtl9602c_gpon_eqd_split(0, &frames, &bits);
	KUNIT_EXPECT_EQ(test, frames, 0);
	KUNIT_EXPECT_EQ(test, bits, 0);

	/* The frame's size is the edge. */
	rtl9602c_gpon_eqd_split(RTL9602C_GPON_EQD_FRAME_BITS - 1, &frames, &bits);
	KUNIT_EXPECT_EQ(test, frames, 0);
	KUNIT_EXPECT_EQ(test, bits, RTL9602C_GPON_EQD_FRAME_BITS - 1);
	rtl9602c_gpon_eqd_split(RTL9602C_GPON_EQD_FRAME_BITS, &frames, &bits);
	KUNIT_EXPECT_EQ(test, frames, 1);
	KUNIT_EXPECT_EQ(test, bits, 0);
	rtl9602c_gpon_eqd_split(2 * RTL9602C_GPON_EQD_FRAME_BITS + 5, &frames, &bits);
	KUNIT_EXPECT_EQ(test, frames, 2);
	KUNIT_EXPECT_EQ(test, bits, 5);
}

static void rtl9602c_test_rate_units(struct kunit *test)
{
	u32 c, p;

	/* No rates: no commitment, and a peak at the field's largest value. */
	rtl9602c_gem_rate_units(0, 0, true, &c, &p);
	KUNIT_EXPECT_EQ(test, c, 0);
	KUNIT_EXPECT_EQ(test, p, RTL9602C_PONSCH_QUEUE_RATE_MAX);

	/* 10 Mbit/s and 100 Mbit/s in bytes a second: up for the first, down for the second. */
	rtl9602c_gem_rate_units(1250000, 12500000, true, &c, &p);
	KUNIT_EXPECT_EQ(test, c, 1221);
	KUNIT_EXPECT_EQ(test, p, 12207);

	/* Whole units stay, one byte more takes the next. */
	rtl9602c_gem_rate_units(2 * RTL9602C_GEM_RATE_UNIT, 0, true, &c, &p);
	KUNIT_EXPECT_EQ(test, c, 2);
	rtl9602c_gem_rate_units(2 * RTL9602C_GEM_RATE_UNIT + 1, 0, true, &c, &p);
	KUNIT_EXPECT_EQ(test, c, 3);

	/* A peak below a unit is one unit, and the commitment keeps within it. */
	rtl9602c_gem_rate_units(100, 100, true, &c, &p);
	KUNIT_EXPECT_EQ(test, p, 1);
	KUNIT_EXPECT_EQ(test, c, 1);

	/* A commitment above the peak is cut to it. */
	rtl9602c_gem_rate_units(10 * RTL9602C_GEM_RATE_UNIT, 4 * RTL9602C_GEM_RATE_UNIT,
				true, &c, &p);
	KUNIT_EXPECT_EQ(test, p, 4);
	KUNIT_EXPECT_EQ(test, c, 4);

	/* A queue with a port that has no peak rate has no limit, whatever the others add up to. */
	rtl9602c_gem_rate_units(10 * RTL9602C_GEM_RATE_UNIT, 4 * RTL9602C_GEM_RATE_UNIT,
				false, &c, &p);
	KUNIT_EXPECT_EQ(test, p, RTL9602C_PONSCH_QUEUE_RATE_MAX);
	KUNIT_EXPECT_EQ(test, c, 10);

	/* Nothing passes the field's largest value, however large the rates. */
	rtl9602c_gem_rate_units(U64_MAX, U64_MAX, true, &c, &p);
	KUNIT_EXPECT_EQ(test, p, RTL9602C_PONSCH_QUEUE_RATE_MAX);
	KUNIT_EXPECT_EQ(test, c, RTL9602C_PONSCH_QUEUE_RATE_MAX);
	rtl9602c_gem_rate_units(U64_MAX, 0, false, &c, &p);
	KUNIT_EXPECT_EQ(test, c, RTL9602C_PONSCH_QUEUE_RATE_MAX);
}

/*
 * The PON class numbers T-CONTs from 0; the scheduler has groups of eight, of
 * which each GEM T-CONT uses four, so 0 to 3 are the scheduler's 0 to 3 and 4
 * to 7 its 8 to 11. A T-CONT's queues take the group's bits from the top:
 * priority 0 the highest bit of the T-CONT's byte.
 */
static void rtl9602c_test_tcont_numbers(struct kunit *test)
{
	static const unsigned int sched[RTL9602C_GEM_NUM_TCONT] = {
		0, 1, 2, 3, 8, 9, 10, 11
	};
	unsigned int n, k;
	u32 seen = 0;

	for (n = 0; n < RTL9602C_GEM_NUM_TCONT; n++)
		KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont(n), sched[n]);

	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qbit(0, 0), 7);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qbit(0, 7), 0);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qbit(1, 0), 15);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qbit(3, 0), 31);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qbit(3, 7), 24);

	/* Four T-CONTs and eight priorities each have a bit of 32; no two share one. */
	for (n = 0; n < 4; n++) {
		for (k = 0; k < RTL9602C_GEM_TCONT_QUEUES; k++) {
			unsigned int bit = rtl9602c_gem_tcont_qbit(n, k);

			KUNIT_ASSERT_LT(test, bit, 32);
			KUNIT_EXPECT_EQ(test, seen & BIT(bit), 0);
			seen |= BIT(bit);
		}
	}
	KUNIT_EXPECT_EQ(test, seen, U32_MAX);

	/* The second group has the same bits: the group is the scheduler's index. */
	for (k = 0; k < RTL9602C_GEM_TCONT_QUEUES; k++)
		KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qbit(4, k),
				rtl9602c_gem_tcont_qbit(0, k));
}

/*
 * The queues a T-CONT's GEM ports send through, which decide whether a queue
 * stays attached: one bit for each priority in use, which the ports of one
 * priority share. Ports that send nothing upstream, ports of other T-CONTs,
 * flows without a GEM port and flows without a stream do not count.
 */
static void rtl9602c_test_tcont_qmap(struct kunit *test)
{
	const struct pon_gem_config both = {
		.direction = IFLA_GEM_DIR_BOTH, .tcont = 1, .queue = 0,
	};
	struct pon_gem_config *cfg;
	unsigned long *flows;

	cfg = kunit_kcalloc(test, RTL9602C_GPON_NUM_FLOW, sizeof(*cfg),
			    GFP_KERNEL);
	flows = kunit_kcalloc(test, BITS_TO_LONGS(RTL9602C_GPON_NUM_FLOW),
			      sizeof(*flows), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, cfg);
	KUNIT_ASSERT_NOT_NULL(test, flows);

	/* No GEM port, no queue. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qmap(cfg, flows, 0), 0);

	/* Two ports of priority 0 on T-CONT 1 share its highest queue, bit 15. */
	cfg[0] = both;
	cfg[3] = both;
	__set_bit(0, flows);
	__set_bit(3, flows);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qmap(cfg, flows, 1), BIT(15));
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qmap(cfg, flows, 0), 0);

	/* The first port leaves: the queue stays for the second. */
	__clear_bit(0, flows);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qmap(cfg, flows, 1), BIT(15));

	/* The second moves to priority 7: the queue of bit 8 takes its place. */
	cfg[3].queue = 7;
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qmap(cfg, flows, 1), BIT(8));

	/* An upstream-only port counts; a downstream-only one does not. */
	cfg[5] = both;
	cfg[5].direction = IFLA_GEM_DIR_UPSTREAM;
	cfg[5].queue = 2;
	cfg[6] = both;
	cfg[6].direction = IFLA_GEM_DIR_DOWNSTREAM;
	cfg[6].queue = 4;
	__set_bit(5, flows);
	__set_bit(6, flows);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qmap(cfg, flows, 1),
			BIT(8) | BIT(13));

	/* A configuration whose flow holds no GEM port does not count. */
	cfg[9] = both;
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qmap(cfg, flows, 1),
			BIT(8) | BIT(13));

	/* T-CONT 5 is the scheduler's 9 and uses the same bits as T-CONT 1. */
	cfg[10] = both;
	cfg[10].tcont = 5;
	__set_bit(10, flows);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qmap(cfg, flows, 5), BIT(15));
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qmap(cfg, flows, 1),
			BIT(8) | BIT(13));

	/* Only flows with a stream are looked at; the OMCC's, 64, is one. */
	cfg[RTL9602C_PON_NUM_SID] = both;
	__set_bit(RTL9602C_PON_NUM_SID, flows);
	KUNIT_EXPECT_EQ(test, rtl9602c_gem_tcont_qmap(cfg, flows, 1),
			BIT(8) | BIT(13));
}

static void rtl9602c_test_ploam_names(struct kunit *test)
{
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_ds_name(1), "Upstream_Overhead");
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_ds_name(3), "Assign_ONU-ID");
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_ds_name(4), "Ranging_Time");
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_ds_name(10), "Assign_Alloc-ID");
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_ds_name(14), "Configure_Port-ID");
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_ds_name(24), "Sleep_Allow");
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_us_name(1), "Serial_Number_ONU");
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_us_name(3), "Dying_Gasp");
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_us_name(9), "Acknowledge");

	/* Identifier 0 and every one above the tables are unknown. */
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_ds_name(0), "unknown");
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_ds_name(25), "unknown");
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_ds_name(255), "unknown");
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_us_name(0), "unknown");
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_us_name(11), "unknown");
	KUNIT_EXPECT_STREQ(test, rtl9602c_ploam_us_name(255), "unknown");
}

/*
 * Disable_Serial_Number (G.984.3 clause 10.2.5.1 i) and j)): the disable with
 * the ONU's serial number stops it in O7 from any other state; only the enable
 * with its serial number, or the enable of every ONU, takes it out, to O2.
 */
static void rtl9602c_test_disable_sn(struct kunit *test)
{
	static const u8 others[] = { 0x01, 0x0e, 0x10, 0xf0, 0xfe };
	const u8 disable = RTL9602C_PLOAM_DISABLE_SN_DISABLE;
	const u8 enable = RTL9602C_PLOAM_DISABLE_SN_ENABLE;
	const u8 all = RTL9602C_PLOAM_DISABLE_SN_ENABLE_ALL;
	const u8 o7 = RTL9602C_ONU_O7_EMERGENCY_STOP;
	const u8 o5 = RTL9602C_ONU_O5_OPERATION;
	const u8 o2 = RTL9602C_ONU_O2_STANDBY;
	u8 state;
	unsigned int i;

	for (state = RTL9602C_ONU_O1_INITIAL; state < o7; state++) {
		KUNIT_EXPECT_EQ(test, rtl9602c_gpon_disable_sn(state, disable, true), o7);
		/* Another ONU's disable, and any enable, leave it as it is. */
		KUNIT_EXPECT_EQ(test, rtl9602c_gpon_disable_sn(state, disable, false), state);
		KUNIT_EXPECT_EQ(test, rtl9602c_gpon_disable_sn(state, enable, true), state);
		KUNIT_EXPECT_EQ(test, rtl9602c_gpon_disable_sn(state, all, true), state);
	}

	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_disable_sn(o7, enable, true), o2);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_disable_sn(o7, enable, false), o7);
	/* The enable of every disabled ONU does not look at the serial number. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_disable_sn(o7, all, false), o2);
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_disable_sn(o7, all, true), o2);
	/* A second disable keeps it stopped. */
	KUNIT_EXPECT_EQ(test, rtl9602c_gpon_disable_sn(o7, disable, true), o7);

	/* Any other octet 3 changes nothing, in O7 or out of it. */
	for (i = 0; i < ARRAY_SIZE(others); i++) {
		KUNIT_EXPECT_EQ(test, rtl9602c_gpon_disable_sn(o7, others[i], true), o7);
		KUNIT_EXPECT_EQ(test, rtl9602c_gpon_disable_sn(o5, others[i], true), o5);
	}
}

static struct kunit_case rtl9602c_pon_test_cases[] = {
	KUNIT_CASE(rtl9602c_test_sn_parse),
	KUNIT_CASE(rtl9602c_test_sn_parse_refuses),
	KUNIT_CASE(rtl9602c_test_pw_parse),
	KUNIT_CASE(rtl9602c_test_pw_parse_refuses),
	KUNIT_CASE(rtl9602c_test_boh_bytes),
	KUNIT_CASE(rtl9602c_test_boh_len),
	KUNIT_CASE(rtl9602c_test_eqd_split),
	KUNIT_CASE(rtl9602c_test_power_mode_of),
	KUNIT_CASE(rtl9602c_test_power_cpl),
	KUNIT_CASE(rtl9602c_test_power_sn_step),
	KUNIT_CASE(rtl9602c_test_tod_delta),
	KUNIT_CASE(rtl9602c_test_rate_units),
	KUNIT_CASE(rtl9602c_test_tcont_numbers),
	KUNIT_CASE(rtl9602c_test_tcont_qmap),
	KUNIT_CASE(rtl9602c_test_ploam_names),
	KUNIT_CASE(rtl9602c_test_disable_sn),
	{ }
};

static struct kunit_suite rtl9602c_pon_test_suite = {
	.name = "rtl9602c-pon",
	.test_cases = rtl9602c_pon_test_cases,
};

kunit_test_suite(rtl9602c_pon_test_suite);
