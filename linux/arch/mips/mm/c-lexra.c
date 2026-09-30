// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cache maintenance for the Lexra LX5281 core of the Realtek RTL9602C.
 *
 * The core runs as an R3000, but the R3000 cache code cannot be used on it:
 * that code invalidates by isolating the cache with Status.IsC and SwC and
 * storing to it, and with it the boot stops silently at the first call into
 * code generated at run time (measured on this board). The likely reason,
 * not verified, is that the core does not implement cache isolation. The
 * caches are maintained here instead through CP0 register 20, which MIPS I
 * leaves unassigned, one operation word at a time.
 *
 * Every operation acts on a whole cache: the driver has no line-granular
 * form (whether the core offers one is not verified), so every range
 * request is served by a whole-cache operation. That is correct, since
 * writing back all of the cache covers any range, and slow: each DMA sync
 * writes back and invalidates the whole data cache. The cache sizes are not
 * probed, as nothing needs them. Whether the data cache is write-back or
 * write-through is not verified either; the code is correct both ways.
 */

#include <linux/cacheflush.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmu_context.h>

#include <asm/bcache.h>
#include <asm/cpu-features.h>
#include <asm/page.h>

/*
 * Operation words for CP0 register 20: invalidate the whole instruction
 * cache; write every dirty line of the data cache back to memory; write the
 * data cache back, then invalidate it.
 */
#define RTL9602C_LEXRA_CACHE_CTRL_ICACHE_INV		0x00000002
#define RTL9602C_LEXRA_CACHE_CTRL_DCACHE_WB		0x00000100
#define RTL9602C_LEXRA_CACHE_CTRL_DCACHE_WB_INV		0x00000200

/*
 * Issue one operation: write 0 to CP0 register 20, then the operation word.
 * Whether the zero is needed is not verified; the pair works. noreorder and
 * the "memory" clobber keep every memory access on its side of the
 * operation.
 */
static inline void cctl_op(unsigned long op)
{
	__asm__ __volatile__(".set	push\n\t"
		".set	noreorder\n\t"
		"mtc0	$0, $20\n\t"
		"mtc0	%0, $20\n\t"
		".set	pop"
		:
		: "r" (op)
		: "memory");
}

static void lexra_flush_dcache_all(void)
{
	cctl_op(RTL9602C_LEXRA_CACHE_CTRL_DCACHE_WB_INV);
}

static void lexra_flush_cache_all(void)
{
	cctl_op(RTL9602C_LEXRA_CACHE_CTRL_DCACHE_WB_INV);
	cctl_op(RTL9602C_LEXRA_CACHE_CTRL_ICACHE_INV);
}

static void lexra___flush_cache_all(void)
{
	lexra_flush_cache_all();
}

/*
 * An mm without an ASID on this CPU has never run here, so nothing is cached
 * through its mappings; the R3000 code follows the same rule.
 */
static void lexra_flush_cache_mm(struct mm_struct *mm)
{
	if (cpu_context(smp_processor_id(), mm) != 0)
		lexra_flush_cache_all();
}

/* The data cache, and the instruction cache too for an executable mapping */
static void lexra_flush_cache_range(struct vm_area_struct *vma,
				    unsigned long start, unsigned long end)
{
	if (cpu_context(smp_processor_id(), vma->vm_mm) == 0)
		return;

	if (vma->vm_flags & VM_EXEC)
		lexra_flush_cache_all();
	else
		lexra_flush_dcache_all();
}

static void lexra_flush_cache_page(struct vm_area_struct *vma,
				   unsigned long addr, unsigned long pfn)
{
	if (cpu_context(smp_processor_id(), vma->vm_mm) == 0)
		return;

	if (vma->vm_flags & VM_EXEC)
		lexra_flush_cache_all();
	else
		lexra_flush_dcache_all();
}

/*
 * Instructions written through the data side must be in memory before the
 * instruction cache fetches them again: write the data cache back first,
 * then invalidate the instruction cache. The range is ignored.
 */
static void lexra_flush_icache_range(unsigned long start, unsigned long end)
{
	cctl_op(RTL9602C_LEXRA_CACHE_CTRL_DCACHE_WB);
	cctl_op(RTL9602C_LEXRA_CACHE_CTRL_ICACHE_INV);
}

static void lexra_flush_kernel_vmap_range(unsigned long vaddr, int size)
{
	lexra_flush_dcache_all();
}

static void lexra_flush_data_cache_page(unsigned long addr)
{
	lexra_flush_dcache_all();
}

/*
 * The DMA hooks, which arch_sync_dma_for_device() calls: write-back for a
 * buffer the device reads, write-back and invalidate for one it writes or
 * both, and for a coherent allocation before it is used uncached through
 * KSEG1. Nothing is done when the CPU takes a buffer back, so the CPU must
 * not read a buffer while the device owns it.
 */
static void lexra_dma_cache_wback_inv(unsigned long start, unsigned long size)
{
	cctl_op(RTL9602C_LEXRA_CACHE_CTRL_DCACHE_WB_INV);
}

static void lexra_dma_cache_wback(unsigned long start, unsigned long size)
{
	cctl_op(RTL9602C_LEXRA_CACHE_CTRL_DCACHE_WB);
}

/**
 * lexra_cache_init() - install the cache maintenance of the Lexra core
 *
 * Called by cpu_cache_init() with CONFIG_CPU_LEXRA_CACHE, in place of the
 * R3000 cache code. Installs the whole-cache operations, flushes both caches
 * and assembles clear_page() and copy_page().
 */
void lexra_cache_init(void)
{
	flush_cache_all = lexra_flush_cache_all;
	__flush_cache_all = lexra___flush_cache_all;
	flush_cache_mm = lexra_flush_cache_mm;
	flush_cache_range = lexra_flush_cache_range;
	flush_cache_page = lexra_flush_cache_page;

	flush_icache_range = lexra_flush_icache_range;
	local_flush_icache_range = lexra_flush_icache_range;
	__flush_icache_user_range = lexra_flush_icache_range;
	__local_flush_icache_user_range = lexra_flush_icache_range;

	__flush_kernel_vmap_range = lexra_flush_kernel_vmap_range;
	flush_data_cache_page = lexra_flush_data_cache_page;

	/*
	 * Never an invalidate without write-back: over the whole cache it
	 * would discard dirty lines that have nothing to do with the request.
	 */
	_dma_cache_wback_inv = lexra_dma_cache_wback_inv;
	_dma_cache_wback = lexra_dma_cache_wback;
	_dma_cache_inv = lexra_dma_cache_wback_inv;

	/*
	 * Put both caches in a known state before anything relies on them;
	 * what the boot loader leaves in them is not known.
	 */
	lexra_flush_cache_all();

	/*
	 * clear_page() and copy_page() are assembled at run time and made
	 * executable with the instruction-cache flush installed above. Until
	 * then each is a placeholder that jumps to itself, and the first page
	 * allocation that asks for zeroed memory would never return.
	 */
	build_clear_page();
	build_copy_page();
}
