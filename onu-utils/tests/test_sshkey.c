// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Tests of the public keys root logs in with (sshkey.h). The keys were made
 * with OpenSSH's ssh-keygen, and their fingerprints are the ones its -l
 * printed; the keys that are wrong inside are built here.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sshkey.h"

static int checks, failures;

#define CHECK(cond)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(cond)) {                                           \
			failures++;                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			       #cond);                                   \
		}                                                        \
	} while (0)

static const char g_ed25519[] =
	"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJUZp+xf8C/t3NT+0VYL5nV7MNOQXr21ORnzDVmED9xE test-ed25519@example";
static const char g_p256[] =
	"ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBKi6OUiZeRdSZadzWqI/9qS1pPepAzshYaS90KLBFnpy8aHIQZ8IWWdP4jHEn8D0ylXqGBBWU3mMd/CsU/Ueedg= p256";
static const char g_p384[] =
	"ecdsa-sha2-nistp384 AAAAE2VjZHNhLXNoYTItbmlzdHAzODQAAAAIbmlzdHAzODQAAABhBOxStJC04/gsEYBf6FTihDE0jj7spzwpf/REZWwVFlgoOMe/nN5Getdm3TVkJ5PsBCE2xv9MS8YFgJLvgEvdPqHF39rYJLWNP3pGv7OPz/I6bKzDfOxCxQStU3FT+GY/jA==";
static const char g_p521[] =
	"ecdsa-sha2-nistp521 AAAAE2VjZHNhLXNoYTItbmlzdHA1MjEAAAAIbmlzdHA1MjEAAACFBAFbPZuVZWzfnAsWYR4ic9PvRS77fRxeT18foYN2wuISYsZze6tC1qI2RzRuhHhnIJoxRaTLq33mZi1BZ24bxmqrnAEW5YwnZ0tR146Mpa2Fbm2ggukvWzdMGH8mF8lXLjVoB8xXCx1Hs6gnv4HNTjKm+QN9pxUWtKQGfn3YCLQHRFpREQ== p521 key";
static const char g_rsa1024[] =
	"ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQC4WKVwESjp9sdRo62oW55S64G3eVqIYUlT+q+xVqp03ARK0Oj2yoFl8Df/VkE02pdFdlRASR0aD+o5EoHaKu21mxYU5mwShl85lftmV7po42nmwn7QZu8aoWAtXdDFtK7yvQrn0XBGn3ealTZSGOEHpR52e7BcjqN/o8CIOiQZhw== rsa1024";
static const char g_rsa2048[] =
	"ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQDYS+/3Tm0ccRFLiBDR1aFzi3C4V/uIVx1AgIwmu3DW4d4v+UfpdkJNpFWPCS6zmX6N/yEoSADn0SEdQDzF95oJhVyNGefyJS6pEQ4q+4yVK+bLJL8HhyX2lHG2DFmyXDzGVZsNhK6KuyAa1zp26Dm/HeBOanqiwIx4b/iBnWIsDGCySeo1N/wpkQ17RoQRP5P2oyPYoOjm4BfBFGPVFwW5QT/3Ryw+zxtYRSq/jj1CdzbtkxsO+OhA154fY2PwvID7CC1gAsr/caCo/LOmyE/SCKdtaz30HDOtCUNu9YSmPmY7vNp932HEK2Y+qss1n4dSM1OQatVknaLo+KnE+kAF rsa2048";
static const char g_rsa4096[] =
	"ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAACAQClwHPntBSVYjrernMrqfVsUuLGWU5acxU552ov7cBaGc62UcGOgMW2yVsSaJ8utN89tOY1Jk3PE57aX65a0WoDuVXJlG74gjb25LnSLlXgr4ua5TXIUK5FYj58iMcv5fEd6gzQMYrj764eMtWwC+ATZ5YB0sSCE/CtuMtF1m+fV1ZjJWRfUyIK8e8w1vmAGyiQkhOPAii9QZq9s/B22ZreQSaG+ae30yRYLqz1fOk4c6Tvdi8OZadcFmTUm2h50Hp8H62g/6FdsCa5zmN9agJOROCvhU7kD7rwm3MaMlqjk07m6i0IhK9aV4D8GqZ++il57B1cpdV4z44+exkVQvWes04MrE+9IwGhFnmilCxvEad4duLGFLjmsT/EMrqDmzdMOV648QsQwq6OTeTtjNTkVGIM5wvB3TjREfQ0hNBQJPCSLAs//Y4TGVLlr+zsdZiEp0Palgbn1MONpj8Zy8MSsfsgCQ6MhQ3RACqHE4JKelyjKaMk7pYw633HzaupxHDE7MijmgmHHva7n2dXUmwU454UwqhdNH2xx7uE6N3aVjF7oku070E30wpzSllcMkNeugMmwSPTQ60FK2TMw7/D5wRr/CNSKWuxaEKhp/QwQ3lqPGvCBaQ89evCovcSINMHfmNi9SrV25uzdFSIZfVlgrH8ecbe7j900Vc0tNbnFw== rsa4096";
static const char g_rsa4104[] =
	"ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAACAgCUyotWGk4vqqdOy4IR8ziDOg6KOG6wVYfx6CvVhF4hcTFeiEs3PCEiqs3AhH5/GDlgPPnkogiJoCVD/54QVdK8c3tGHa2VcwIVcOlONHVcg0U4Guj8TZ+RG7Uc/dR8WFK5aXMlNa3wlSkpItGsMgjNu6dayXfyjjeTJ+0ZNbsMFPfe+18l67KgmyOiZlta+ObxUGEKMFOqHdZZ0bkhoZwN9mWqa+/6CgH2r7bJp45n1L+0TGBnRRRTvWMSnq789H5I2+TQVOhyGKLMzRhRQY6UJQRAlGT4N86KuDWmDhQBKlbpzlH/Q9TyyIGM9SfgbpH61C7EujZJnlIlEvMwK8j+gcs4sxA9F8mtVBCjXTolbJgjdSmIxZS7RvF7V9sP7kYL2vSJ/tMCNAno7EGyaUX0/PhomX3Xz1zAM+lF7ktkPO4O2fFIW9qj5hKMBVH4CB0gVY4CN48OSU9vs2DluaIFMzBkYM1Khnks+AH7OkuMq/vrVnY9kHnyKak7/In71/xcdUZTfTaSk+y3aZ3OHwiR/ySFukTRW64Qu63FdMi0m/OSuhbBIUSJMvfRbtYrUoYy1hd7oii8UdMKbk1O9d6sOYL+/5bs2ApmQgO4Ci3NjwRl4dEq89RlGEbEqNwfuuwx4P/53jTbZMZQtImLomP1dFk/KBGHSw++HQ2SjNqCtzU= rsa4104";

static char g_err[256];

static bool taken(const char *line, struct sshkey *k)
{
	g_err[0] = '\0';
	return sshkey_parse(line, k, g_err, sizeof(g_err)) == 0;
}

/* Refused, with @why in what it says. */
static bool refused(const char *line, const char *why)
{
	struct sshkey k;

	g_err[0] = '\0';
	if (sshkey_parse(line, &k, g_err, sizeof(g_err)) != -EINVAL)
		return false;
	if (!strstr(g_err, why)) {
		printf("  said: %s\n", g_err);
		return false;
	}
	return true;
}

static void test_taken(void)
{
	char line[300];
	struct sshkey k;

	puts("the types the stick takes, with ssh-keygen's fingerprints");
	CHECK(taken(g_ed25519, &k) && !strcmp(k.type, "ssh-ed25519") && k.bits == 256 &&
	      !strcmp(k.fingerprint, "SHA256:lxckqci4MwTwEjJFI7q4oX1lApk+VkklJ0/B5U0TTc0") &&
	      !strcmp(k.comment, "test-ed25519@example") && !strcmp(k.line, g_ed25519));
	CHECK(taken(g_p256, &k) && k.bits == 256 &&
	      !strcmp(k.fingerprint, "SHA256:QVzpbdO2XiR+U/GoiobfqifyWwLvSY5QBcJvz5JlvTc") &&
	      !strcmp(k.line, g_p256));
	/* No comment, a blank and a line end after it: the line is the key alone. */
	snprintf(line, sizeof(line), "%s %c", g_p384, '\n');
	CHECK(taken(line, &k) && k.bits == 384 && !k.comment[0] &&
	      !strcmp(k.fingerprint, "SHA256:8xMwjoOOM6gx7/JfmbcVwZ8o/W8gFtW7j/zseOFWJeQ") &&
	      !strcmp(k.line, g_p384));
	CHECK(taken(g_p521, &k) && k.bits == 521 && !strcmp(k.comment, "p521 key") &&
	      !strcmp(k.fingerprint, "SHA256:W7sODCmJPET/EzjJx1xxSDWzsbr8gEgfhh5m8Nbg3dQ"));
	CHECK(taken(g_rsa2048, &k) && !strcmp(k.type, "ssh-rsa") && k.bits == 2048 &&
	      !strcmp(k.fingerprint, "SHA256:FfdmUTng6/uN+QeKtmfkTUlfrbXx+C45gyGUEm8XIQ8"));
	CHECK(taken(g_rsa4096, &k) && k.bits == 4096 &&
	      !strcmp(k.fingerprint, "SHA256:0qqVhTQl0e1W4jbygI28Q+szapdt/qJJAg9qJXsVZIU") &&
	      !strcmp(k.line, g_rsa4096));
}

static void test_written_anew(void)
{
	char line[1200];
	struct sshkey k;

	puts("the line is written anew: blanks, line ends, the longest comment");
	snprintf(line, sizeof(line), " \t%.11s \t %s\r\n", g_ed25519, g_ed25519 + 12);
	CHECK(taken(line, &k) && !strcmp(k.line, g_ed25519));
	snprintf(line, sizeof(line), "%.*s %0100d", (int)(strchr(g_rsa4096 + 8, ' ') - g_rsa4096),
		 g_rsa4096, 0);
	CHECK(taken(line, &k) && strlen(k.comment) == SSHKEY_COMMENT_MAX &&
	      strlen(k.line) == 825 && !strcmp(k.line, line));
	/* A comment of letters beyond ASCII is kept as it is. */
	snprintf(line, sizeof(line), "%.80s Pawe\xc5\x82@laptop", g_ed25519);
	CHECK(taken(line, &k) && !strcmp(k.comment, "Pawe\xc5\x82@laptop"));
}

static void test_refused(void)
{
	char line[1600];

	puts("what is refused, and what it says");
	CHECK(refused("", "no key"));
	CHECK(refused(" \r\n", "no key"));
	CHECK(refused(g_rsa1024, "an RSA key of 1024 bits: the stick takes 2048 to 4096 bits"));
	CHECK(refused(g_rsa4104, "an RSA key of 4104 bits"));
	CHECK(refused("ssh-dss AAAAB3NzaC1kc3M= x", "a key of the type ssh-dss"));
	CHECK(refused("sk-ssh-ed25519@openssh.com AAAA x", "a key of the type sk-ssh-ed25519"));
	snprintf(line, sizeof(line), "command=\"/bin/true\" %s", g_ed25519);
	CHECK(refused(line, "options in front of it"));
	snprintf(line, sizeof(line), "no-pty,no-agent-forwarding %s", g_ed25519);
	CHECK(refused(line, "options in front of it"));
	snprintf(line, sizeof(line), "restrict %s", g_ed25519);
	CHECK(refused(line, "a key of the type restrict"));
	CHECK(refused("ssh-ed25519", "not base64"));
	CHECK(refused("ssh-ed25519 AAAA@AAA", "not base64"));
	CHECK(refused("ssh-ed25519 AAA", "not base64"));
	CHECK(refused("ssh-ed25519 A=AA", "not base64"));
	/* The blob of an Ed25519 key under the name of another type. */
	snprintf(line, sizeof(line), "ecdsa-sha2-nistp256 %s", g_ed25519 + 12);
	CHECK(refused(line, "not of the type ecdsa-sha2-nistp256"));
	/* A blob too long for one value of the store. */
	memset(line, 'A', sizeof(line) - 1);
	memcpy(line, "ssh-rsa ", 8);
	line[8 + 1368] = '\0';
	CHECK(refused(line, "too long for the store"));
	/* A control character in the comment, and a second line. */
	snprintf(line, sizeof(line), "%.80s a\tb", g_ed25519);
	CHECK(refused(line, "control character"));
	snprintf(line, sizeof(line), "%.80s a\nssh-rsa", g_ed25519);
	CHECK(refused(line, "one key at a time"));
	snprintf(line, sizeof(line), "%.80s\r\n%.80s\n", g_ed25519, g_ed25519);
	CHECK(refused(line, "one key at a time"));
	snprintf(line, sizeof(line), "%.80s a\x7f", g_ed25519);
	CHECK(refused(line, "control character"));
	snprintf(line, sizeof(line), "%.80s %0101d", g_ed25519, 0);
	CHECK(refused(line, "longer than 100 bytes"));
}

/* The base64 of @n bytes of @in, padded, after "TYPE ", in @out. */
static void encode(const char *type, const uint8_t *in, size_t n, char *out)
{
	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t len = (size_t)sprintf(out, "%s ", type);

	for (size_t i = 0; i < n; i += 3) {
		uint32_t w = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) |
			     (i + 2 < n ? in[i + 2] : 0);

		out[len++] = b64[w >> 18 & 63];
		out[len++] = b64[w >> 12 & 63];
		out[len++] = i + 1 < n ? b64[w >> 6 & 63] : '=';
		out[len++] = i + 2 < n ? b64[w & 63] : '=';
	}
	out[len] = '\0';
}

/* A blob being built: strings of RFC 4251 section 5. */
struct blob {
	uint8_t b[1100];
	size_t n;
};

static void put(struct blob *b, const void *s, size_t n)
{
	b->b[b->n++] = (uint8_t)(n >> 24);
	b->b[b->n++] = (uint8_t)(n >> 16);
	b->b[b->n++] = (uint8_t)(n >> 8);
	b->b[b->n++] = (uint8_t)n;
	memcpy(b->b + b->n, s, n);
	b->n += n;
}

static void puts_str(struct blob *b, const char *s)
{
	put(b, s, strlen(s));
}

/* Refused, the blob @b under the type @type. */
static bool blob_refused(const char *type, const struct blob *b, const char *why)
{
	char line[1600];

	encode(type, b->b, b->n, line);
	return refused(line, why);
}

static void test_inside(void)
{
	static const uint8_t point[133] = { 0x04 }, flat[133] = { 0 };
	static const uint8_t e3[] = { 3 }, e4[] = { 4 };
	static const uint8_t neg[] = { 0x80, 1 }, zeros[] = { 0, 0x7f }, zero[] = { 0 };
	uint8_t n[257] = { 0 }, key[32] = { 0 };
	struct blob b = { .n = 0 };
	struct sshkey k;
	char line[1600];

	puts("what is wrong inside a key");
	n[1] = 0xc5;
	n[256] = 0x01;
	puts_str(&b, "ssh-ed25519");
	put(&b, key, 31);
	CHECK(blob_refused("ssh-ed25519", &b, "not an Ed25519 key of 32 bytes"));
	b.n = 0;
	puts_str(&b, "ssh-ed25519");
	put(&b, key, 32);
	b.b[b.n++] = 0;
	CHECK(blob_refused("ssh-ed25519", &b, "bytes after its end"));
	/* A string longer than the blob. */
	b.n = 0;
	puts_str(&b, "ssh-ed25519");
	put(&b, key, 32);
	b.b[b.n - 33] = 0x21;
	CHECK(blob_refused("ssh-ed25519", &b, "not an Ed25519 key"));
	b.n = 0;
	puts_str(&b, "ssh-ed25519");
	b.b[b.n++] = 0;
	CHECK(blob_refused("ssh-ed25519", &b, "not an Ed25519 key"));

	b.n = 0;
	puts_str(&b, "ecdsa-sha2-nistp256");
	puts_str(&b, "nistp384");
	put(&b, point, 65);
	CHECK(blob_refused("ecdsa-sha2-nistp256", &b, "not a point of the curve nistp256"));
	b.n = 0;
	puts_str(&b, "ecdsa-sha2-nistp256");
	puts_str(&b, "nistp256");
	put(&b, point, 64);
	CHECK(blob_refused("ecdsa-sha2-nistp256", &b, "not a point of the curve nistp256"));
	b.n = 0;
	puts_str(&b, "ecdsa-sha2-nistp521");
	puts_str(&b, "nistp521");
	put(&b, flat, 133);
	CHECK(blob_refused("ecdsa-sha2-nistp521", &b, "not a point of the curve nistp521"));
	b.n = 0;
	puts_str(&b, "ecdsa-sha2-nistp521");
	puts_str(&b, "nistp521");
	put(&b, point, 133);
	encode("ecdsa-sha2-nistp521", b.b, b.n, line);
	CHECK(taken(line, &k) && k.bits == 521);

	/* RSA: an odd exponent of two bits at least, a positive modulus. */
	b.n = 0;
	puts_str(&b, "ssh-rsa");
	put(&b, e3, 1);
	put(&b, n, sizeof(n));
	encode("ssh-rsa", b.b, b.n, line);
	CHECK(taken(line, &k) && k.bits == 2048);
	b.n = 0;
	puts_str(&b, "ssh-rsa");
	put(&b, e4, 1);
	put(&b, n, sizeof(n));
	CHECK(blob_refused("ssh-rsa", &b, "not an RSA key"));
	b.n = 0;
	puts_str(&b, "ssh-rsa");
	put(&b, neg, 2);
	put(&b, n, sizeof(n));
	CHECK(blob_refused("ssh-rsa", &b, "not an RSA key"));
	b.n = 0;
	puts_str(&b, "ssh-rsa");
	put(&b, e3, 0);
	put(&b, n, sizeof(n));
	CHECK(blob_refused("ssh-rsa", &b, "not an RSA key"));
	b.n = 0;
	puts_str(&b, "ssh-rsa");
	put(&b, e3, 1);
	put(&b, zeros, 2);
	CHECK(blob_refused("ssh-rsa", &b, "not an RSA key"));
	b.n = 0;
	puts_str(&b, "ssh-rsa");
	put(&b, e3, 1);
	put(&b, zero, 1);
	CHECK(blob_refused("ssh-rsa", &b, "not an RSA key"));
	b.n = 0;
	puts_str(&b, "ssh-rsa");
	put(&b, e3, 1);
	CHECK(blob_refused("ssh-rsa", &b, "not an RSA key"));
	/* A modulus of 2047 bits. */
	n[1] = 0x45;
	b.n = 0;
	puts_str(&b, "ssh-rsa");
	put(&b, e3, 1);
	put(&b, n + 1, sizeof(n) - 1);
	CHECK(blob_refused("ssh-rsa", &b, "an RSA key of 2047 bits"));
}

int main(void)
{
	test_taken();
	test_written_anew();
	test_refused();
	test_inside();
	printf("test_sshkey: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
