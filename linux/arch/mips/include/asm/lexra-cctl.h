/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Cache control of the Lexra LX5281 core: CP0 register 20, which MIPS I
 * leaves unassigned, takes one operation word at a time, each preceded by a
 * write of 0. Each operation acts on a whole cache.
 */

#ifndef __ASM_LEXRA_CCTL_H
#define __ASM_LEXRA_CCTL_H

/* Invalidate the whole instruction cache. */
#define LEXRA_CCTL_ICACHE_INV		0x00000002
/* Write every dirty line of the data cache back to memory. */
#define LEXRA_CCTL_DCACHE_WB		0x00000100
/* Write the data cache back, then invalidate it. */
#define LEXRA_CCTL_DCACHE_WB_INV	0x00000200

#endif /* __ASM_LEXRA_CCTL_H */
