// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of SHA-256, of the boot loader's environment and of the update file
 * format, its signature included, on data built here. The signatures are
 * made with Monocypher, which checks them on the stick too; the tests of
 * onu-update check ones that OpenSSL made.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "bootenv.h"
#include "monocypher-ed25519.h"
#include "sha256.h"
#include "update.h"

static int failures;
static int checks;

#define CHECK(cond)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(cond)) {                                           \
			failures++;                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                                   \
		}                                                        \
	} while (0)

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void hex(const uint8_t *d, char *out)
{
	unsigned int i;

	for (i = 0; i < SHA256_LEN; i++)
		sprintf(out + 2 * i, "%02x", d[i]);
}

static int digest_is(const void *data, size_t len, const char *want)
{
	uint8_t d[SHA256_LEN];
	char got[2 * SHA256_LEN + 1];
	struct sha256 s;

	sha256_init(&s);
	sha256_update(&s, data, len);
	sha256_final(&s, d);
	hex(d, got);
	return !strcmp(got, want);
}

/* The test vectors of FIPS 180-4's examples. */
static void test_sha256(void)
{
	static const char two_blocks[] =
		"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	uint8_t one[SHA256_LEN], pieces[SHA256_LEN], *a;
	struct sha256 s;
	size_t i, step;

	CHECK(digest_is("", 0,
			"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	CHECK(digest_is("abc", 3,
			"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	CHECK(digest_is(two_blocks, strlen(two_blocks),
			"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

	a = malloc(1000000);
	CHECK(a != NULL);
	if (!a)
		return;
	memset(a, 'a', 1000000);
	CHECK(digest_is(a, 1000000,
			"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

	/* Fed in pieces of every size from 1 to 130, the digest is the same. */
	sha256_init(&s);
	sha256_update(&s, a, 4099);
	sha256_final(&s, one);
	for (step = 1; step <= 130; step++) {
		sha256_init(&s);
		for (i = 0; i < 4099; i += step)
			sha256_update(&s, a + i, 4099 - i < step ? 4099 - i : step);
		sha256_final(&s, pieces);
		CHECK(!memcmp(one, pieces, SHA256_LEN));
	}
	free(a);
}

/* A copy of the environment with the variables @vars, NUL-separated. */
static void env_copy(uint8_t *copy, const char *vars, size_t len, uint8_t flag)
{
	memset(copy, 0, BOOTENV_SIZE);
	memcpy(copy + BOOTENV_HEADER, vars, len);
	bootenv_seal(copy, flag);
}

/* A copy holding the variables of the literal @s, which ends in "\0". */
#define ENV(c, s, flag) env_copy(c, s, sizeof(s), flag)

static void test_bootenv_current(void)
{
	static uint8_t a[BOOTENV_SIZE], b[BOOTENV_SIZE];

	ENV(a, "x=1\0", BOOTENV_ACTIVE);
	ENV(b, "x=2\0", BOOTENV_OBSOLETE);
	CHECK(bootenv_valid(a) && bootenv_valid(b));
	CHECK(bootenv_current(a, b) == 0);
	CHECK(bootenv_current(b, a) == 1);

	/* Equal flags, as after a save cut before the old copy's flag: env2. */
	ENV(b, "x=2\0", BOOTENV_ACTIVE);
	CHECK(bootenv_current(a, b) == 1);
	ENV(a, "x=1\0", BOOTENV_OBSOLETE);
	ENV(b, "x=2\0", BOOTENV_OBSOLETE);
	CHECK(bootenv_current(a, b) == 1);

	/* A copy whose data does not check out is never taken. */
	ENV(a, "x=1\0", BOOTENV_ACTIVE);
	ENV(b, "x=2\0", BOOTENV_OBSOLETE);
	a[BOOTENV_HEADER + 2] = '9';
	CHECK(!bootenv_valid(a));
	CHECK(bootenv_current(a, b) == 1);
	b[100] = 1;
	CHECK(bootenv_current(a, b) == -ENOENT);

	/* An erased copy, all 0xff, is not valid either. */
	memset(a, 0xff, BOOTENV_SIZE);
	ENV(b, "x=2\0", BOOTENV_OBSOLETE);
	CHECK(bootenv_current(a, b) == 1);
}

static void test_bootenv_vars(void)
{
	static uint8_t c[BOOTENV_SIZE], want[BOOTENV_SIZE];
	const char *v;
	size_t pos = 0;
	char *big;

	ENV(c, "bootdelay=5\0sw_commit=0\0sw_tryactive=2\0", BOOTENV_ACTIVE);
	v = bootenv_get(c, "sw_commit");
	CHECK(v && !strcmp(v, "0"));
	CHECK(!bootenv_get(c, "sw_com"));
	CHECK(!bootenv_get(c, "sw_commitx"));
	CHECK(!strcmp(bootenv_next(c, &pos), "bootdelay=5"));
	CHECK(!strcmp(bootenv_next(c, &pos), "sw_commit=0"));
	CHECK(!strcmp(bootenv_next(c, &pos), "sw_tryactive=2"));
	CHECK(!bootenv_next(c, &pos));

	/* A change, an addition and a removal keep the keys sorted. */
	CHECK(!bootenv_set(c, "sw_tryactive", "1"));
	CHECK(!bootenv_set(c, "sw_valid1", "0"));
	CHECK(!bootenv_set(c, "baudrate", "115200"));
	CHECK(!bootenv_set(c, "bootdelay", NULL));
	CHECK(!bootenv_set(c, "absent", NULL));
	bootenv_seal(c, BOOTENV_ACTIVE);
	ENV(want, "baudrate=115200\0sw_commit=0\0sw_tryactive=1\0sw_valid1=0\0", BOOTENV_ACTIVE);
	CHECK(!memcmp(c, want, BOOTENV_SIZE));

	/* Keys that the loader could not take back. */
	CHECK(bootenv_set(c, "", "1") == -EINVAL);
	CHECK(bootenv_set(c, "a=b", "1") == -EINVAL);
	CHECK(bootenv_set(c, "a", "1\n2") == -EINVAL);

	/* What does not fit is refused, and the copy stays as it was. */
	big = malloc(BOOTENV_DATA);
	CHECK(big != NULL);
	if (big) {
		memset(big, 'x', BOOTENV_DATA - 1);
		big[BOOTENV_DATA - 1] = '\0';
		CHECK(bootenv_set(c, "big", big) == -ENOSPC);
		bootenv_seal(c, BOOTENV_ACTIVE);
		CHECK(!memcmp(c, want, BOOTENV_SIZE));
		free(big);
	}

	/* Data that is not ended inside the copy is not touched. */
	memset(c, 'y', BOOTENV_SIZE);
	CHECK(bootenv_set(c, "a", "1") == -EBADMSG);
	CHECK(!bootenv_get(c, "y"));
}

static void test_header(void)
{
	struct upd_header h = { .kernel_len = 1648697, .rootfs_len = 1736200,
				.time = 1759800000 }, back;
	uint8_t raw[UPD_HEADER];

	snprintf(h.board, sizeof(h.board), "%s", "odi,dfp-34x-2c3");
	snprintf(h.version, sizeof(h.version), "%s", "7c5fba548abc");
	upd_build(raw, &h);
	CHECK(!memcmp(raw, "ONUF", 4));
	CHECK(!upd_parse(raw, &back));
	CHECK(back.kernel_len == h.kernel_len && back.rootfs_len == h.rootfs_len);
	CHECK(back.time == h.time);
	CHECK(!strcmp(back.board, h.board) && !strcmp(back.version, h.version));

	raw[40] ^= 1;
	CHECK(upd_parse(raw, &back) == -EBADMSG);
	raw[40] ^= 1;
	raw[0] = 'X';
	CHECK(upd_parse(raw, &back) == -EBADMSG);
	upd_build(raw, &h);
	put32(raw + 4, 2);
	put32(raw + 252, (uint32_t)crc32(crc32(0L, Z_NULL, 0), raw, 252));
	CHECK(upd_parse(raw, &back) == -EPROTONOSUPPORT);

	/* An empty version is no version. */
	h.version[0] = '\0';
	upd_build(raw, &h);
	CHECK(upd_parse(raw, &back) == -EBADMSG);
}

static void test_images(void)
{
	uint8_t u[UIMAGE_HEADER] = { 0 }, sq[SQUASHFS_SUPER] = { 0 };
	uint32_t len, dcrc, time, used;

	put32(u, UIMAGE_MAGIC);
	put32(u + 8, 1759800000);
	put32(u + 12, 1000);
	put32(u + 24, 0x12345678);
	put32(u + 4, (uint32_t)crc32(crc32(0L, Z_NULL, 0), u, UIMAGE_HEADER));
	CHECK(!uimage_check(u, &len, &dcrc, &time));
	CHECK(len == UIMAGE_HEADER + 1000 && dcrc == 0x12345678 && time == 1759800000);
	u[40] ^= 1;
	CHECK(uimage_check(u, &len, &dcrc, &time) == -EBADMSG);

	memcpy(sq, "hsqs", 4);
	sq[40] = 0x00;
	sq[41] = 0x10;		/* 4096, little-endian */
	CHECK(!squashfs_check(sq, &used) && used == 4096);
	sq[44] = 1;		/* more than 4 GiB */
	CHECK(squashfs_check(sq, &used) == -EBADMSG);
	sq[44] = 0;
	sq[0] = 'x';
	CHECK(squashfs_check(sq, &used) == -EBADMSG);
}

/* A signature block of update.h over @digest, made with the key of @seed. */
static void sign_block(uint8_t raw[UPD_SIG], uint8_t pub[UPD_KEY],
		       const uint8_t digest[SHA256_LEN], uint8_t fill)
{
	uint8_t seed[32], secret[64], msg[sizeof(UPD_SIG_CONTEXT) + SHA256_LEN];
	struct sha256 s;

	memset(seed, fill, sizeof(seed));
	crypto_ed25519_key_pair(secret, pub, seed);
	memcpy(msg, UPD_SIG_CONTEXT, sizeof(UPD_SIG_CONTEXT));
	memcpy(msg + sizeof(UPD_SIG_CONTEXT), digest, SHA256_LEN);
	memset(raw, 0, UPD_SIG);
	put32(raw, UPD_SIG_MAGIC);
	put32(raw + 4, UPD_SIG_FORMAT);
	sha256_init(&s);
	sha256_update(&s, pub, UPD_KEY);
	sha256_final(&s, raw + 8);
	crypto_ed25519_sign(raw + 8 + SHA256_LEN, secret, msg, sizeof(msg));
}

static void test_signature(void)
{
	uint8_t raw[UPD_SIG], pub[UPD_KEY], other[UPD_SIG], other_pub[UPD_KEY];
	uint8_t digest[SHA256_LEN], id[SHA256_LEN];
	struct upd_sig sig;
	struct sha256 s;

	memset(digest, 0x5a, sizeof(digest));
	sign_block(raw, pub, digest, 1);
	CHECK(!upd_sig_parse(raw, &sig));
	sha256_init(&s);
	sha256_update(&s, pub, UPD_KEY);
	sha256_final(&s, id);
	CHECK(!memcmp(sig.key_id, id, SHA256_LEN));
	CHECK(!upd_sig_check(&sig, pub, digest));

	/* Another digest, another key, a changed signature: no. */
	digest[31] ^= 1;
	CHECK(upd_sig_check(&sig, pub, digest) == -EBADMSG);
	digest[31] ^= 1;
	sign_block(other, other_pub, digest, 2);
	CHECK(upd_sig_check(&sig, other_pub, digest) == -EBADMSG);
	sig.sig[10] ^= 1;
	CHECK(upd_sig_check(&sig, pub, digest) == -EBADMSG);

	/* What is no signature, one of another format, a reserved byte set. */
	raw[0] = 'X';
	CHECK(upd_sig_parse(raw, &sig) == -ENOENT);
	raw[0] = 'O';
	put32(raw + 4, 2);
	CHECK(upd_sig_parse(raw, &sig) == -EPROTONOSUPPORT);
	put32(raw + 4, UPD_SIG_FORMAT);
	raw[UPD_SIG - 1] = 1;
	CHECK(upd_sig_parse(raw, &sig) == -EBADMSG);
}

int main(void)
{
	test_sha256();
	test_bootenv_current();
	test_bootenv_vars();
	test_header();
	test_images();
	test_signature();
	printf("test_update: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
