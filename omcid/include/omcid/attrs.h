/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* The compiler attributes the daemon uses, under the names the kernel gives them. */

#ifndef OMCI_ATTRS_H
#define OMCI_ATTRS_H

#define __must_check __attribute__((__warn_unused_result__))

#define __printf(a, b) __attribute((__format__(printf, a, b)))

#define noinline __attribute((__noinline__))

#endif
