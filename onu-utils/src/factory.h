/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The factory configuration of the stick: a JFFS2 file system in the
 * partition "config", written big-endian, that holds XML files of
 * Name="..." Value="..." pairs. Two of its files carry the identity of the
 * unit; reading them is all this module does.
 */

#ifndef ONU_FACTORY_H
#define ONU_FACTORY_H

#include <stddef.h>
#include <stdint.h>

/**
 * factory_file() - Extract a file of a JFFS2 image.
 * @img: the image
 * @len: its size
 * @name: the name of the file
 * @out: its contents
 * @cap: room in @out
 * @outlen: the size of the contents
 *
 * The data nodes of the file are laid down in the order of their versions,
 * so that a later write wins over an earlier one; zlib-compressed and
 * uncompressed nodes are read.
 *
 * Return: 0, or a negative errno: -ENOENT without the file, -EFBIG when it
 * does not fit @out, -EILSEQ for data that does not decompress, -ENOTSUP
 * for another compression.
 */
int factory_file(const uint8_t *img, size_t len, const char *name, uint8_t *out,
		 size_t cap, size_t *outlen);

/**
 * factory_value() - Find the value of a name in an XML file of the configuration.
 * @doc: the file, a C string
 * @name: the name
 * @out: the value
 * @cap: room in @out
 *
 * Return: 0, -ENOENT without the name, -EILSEQ for an entry without a
 * value, -EFBIG when the value does not fit @out.
 */
int factory_value(const char *doc, const char *name, char *out, size_t cap);

/**
 * factory_text() - Undo the escapes of XML in a value, in place.
 * @value: the value, a C string
 *
 * &amp;, &lt;, &gt;, &quot; and &apos; become the character they stand for.
 *
 * Return: 0, or -EILSEQ for another '&' and its value left as it was.
 */
int factory_text(char *value);

/**
 * factory_mac() - Read a MAC address of the configuration.
 * @value: twelve hexadecimal digits, the pairs apart by colons or not
 * @out: the address, six pairs of lowercase digits apart by colons
 * @cap: room in @out, 18 bytes at least
 *
 * Return: 0, or -EINVAL for another form, a group address or zeros, and
 * -EFBIG when @out is too small.
 */
int factory_mac(const char *value, char *out, size_t cap);

/**
 * factory_prefix() - Join an IPv4 address and its netmask.
 * @addr: the address, a dotted quad
 * @mask: the netmask, a dotted quad of leading ones, 8 to 30 of them
 * @out: "address/length"
 * @cap: room in @out, 19 bytes at least
 *
 * The address has to be one a host can take: unicast, not of 0.0.0.0/8 or
 * 127.0.0.0/8, nor the network's or the broadcast address of its prefix.
 *
 * Return: 0, or -EINVAL for an address or a netmask that is not such, and
 * -EFBIG when @out is too small.
 */
int factory_prefix(const char *addr, const char *mask, char *out, size_t cap);

#endif
