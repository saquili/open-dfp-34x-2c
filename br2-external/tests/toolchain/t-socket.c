/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Sockets that need no network: a connected pair of local sockets, an IPv4
 * datagram socket bound to any address, and the address conversion functions.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "check.h"

int main(void)
{
	int sv[2], s, type = 0, one = 1;
	socklen_t len;
	char buf[16];
	struct sockaddr_in sa, got;
	struct in_addr in;
	unsigned char *bytes;
	uint32_t n32;
	uint16_t n16;

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	CHECK(send(sv[0], "ping", 4, 0) == 4);
	CHECK(recv(sv[1], buf, sizeof(buf), 0) == 4 && memcmp(buf, "ping", 4) == 0);
	CHECK(send(sv[1], "pong!", 5, 0) == 5);
	CHECK(recv(sv[0], buf, sizeof(buf), 0) == 5 && memcmp(buf, "pong!", 5) == 0);
	shutdown(sv[0], SHUT_WR);
	CHECK(recv(sv[1], buf, sizeof(buf), 0) == 0);	/* the other side finished writing */
	close(sv[0]);
	close(sv[1]);

	s = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(s >= 0);
	len = sizeof(type);
	CHECK(getsockopt(s, SOL_SOCKET, SO_TYPE, &type, &len) == 0 && type == SOCK_DGRAM);
	CHECK(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	CHECK(bind(s, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	len = sizeof(got);
	CHECK(getsockname(s, (struct sockaddr *)&got, &len) == 0 && got.sin_family == AF_INET
	      && ntohs(got.sin_port) != 0);
	close(s);

	n32 = htonl(0x01020304);			/* network order is big endian */
	bytes = (unsigned char *)&n32;
	CHECK(bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 4);
	CHECK(ntohl(n32) == 0x01020304u);
	n16 = htons(0xa1b2);
	bytes = (unsigned char *)&n16;
	CHECK(bytes[0] == 0xa1 && bytes[1] == 0xb2);

	CHECK(inet_pton(AF_INET, "192.0.2.77", &in) == 1);
	bytes = (unsigned char *)&in;
	CHECK(bytes[0] == 192 && bytes[1] == 0 && bytes[2] == 2 && bytes[3] == 77);
	CHECK(inet_ntop(AF_INET, &in, buf, sizeof(buf)) != NULL && strcmp(buf, "192.0.2.77") == 0);
	CHECK(inet_pton(AF_INET, "300.1.1.1", &in) == 0);
	CHECK(inet_addr("198.51.100.9") == htonl(0xc6336409));
	DONE("socket");
}
