/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_TLBMISC_H
#define __ASM_TLBMISC_H

/*
 * - add_wired_entry() add a fixed TLB entry, and move wired register
 */
extern void add_wired_entry(unsigned long entrylo0, unsigned long entrylo1,
	unsigned long entryhi, unsigned long pagemask);

/*
 * - r3k_wired_entries: entries at the bottom of an R3000-style TLB that a
 *   full flush leaves alone
 */
extern int r3k_wired_entries;

#endif /* __ASM_TLBMISC_H */
