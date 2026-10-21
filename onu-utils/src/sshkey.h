/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * A public key that root may log in to the ssh server with: a line of
 * OpenSSH's authorized_keys form, "TYPE BASE64 [COMMENT]", as ssh-keygen
 * writes it in a .pub file. The stick takes the types the owner chose, which
 * the ssh server knows: ssh-ed25519, ecdsa-sha2-nistp256, -nistp384 and
 * -nistp521, and ssh-rsa of SSHKEY_RSA_MIN to SSHKEY_RSA_MAX bits; no key
 * with options in front of it, of a security key (sk-), or of DSA. The
 * blob in base64 is read as RFC 4253 section 6.6 and RFC 5656 section 3.1
 * lay it out, and each of its parts is checked, so that a line that would
 * only fail at a login is refused when it is added.
 *
 * A key is kept as one value of the configuration store, which holds at most
 * CFG_VALUE_MAX bytes (cfgstore.h): its line, written anew from the blob, with
 * the comment of at most SSHKEY_COMMENT_MAX bytes. An RSA key of 4096 bits
 * and the longest comment take 825 of them.
 */

#ifndef ONU_SSHKEY_H
#define ONU_SSHKEY_H

#include <stddef.h>

/* The most keys the store keeps, SSH_KEY1 to SSH_KEY8. */
#define SSHKEY_MAX 8

#define SSHKEY_RSA_MIN 2048u
#define SSHKEY_RSA_MAX 4096u
#define SSHKEY_COMMENT_MAX 100u
/* The longest line kept: one value of the store. */
#define SSHKEY_LINE_MAX 1024u
/* "SHA256:" and 43 characters of base64, as ssh-keygen -l prints it. */
#define SSHKEY_FP_LEN 50u

/**
 * struct sshkey - A public key, checked.
 * @type: its type, as the line names it
 * @bits: the size of its key: of the RSA modulus, or of the curve
 * @fingerprint: the SHA-256 of its blob, "SHA256:" and unpadded base64
 * @comment: what follows the blob, which may be empty
 * @line: "TYPE BASE64[ COMMENT]", the base64 written anew from the blob
 */
struct sshkey {
	char type[24];
	unsigned int bits;
	char fingerprint[SSHKEY_FP_LEN + 1];
	char comment[SSHKEY_COMMENT_MAX + 1];
	char line[SSHKEY_LINE_MAX + 1];
};

/**
 * sshkey_parse() - Check a public key of the authorized_keys form.
 * @text: the line, with blanks and a line end around it or not
 * @k: the key, filled in when it is taken
 * @err: why it is not, in words, @errcap bytes
 *
 * Return: 0, or -EINVAL with why in @err.
 */
int sshkey_parse(const char *text, struct sshkey *k, char *err, size_t errcap);

#endif
