// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
/*
 * Load and start a new kernel from the running one, without the bootloader.
 *
 * The board's flash is read-only from Linux (the MTD window is map_rom), the
 * switch driver is built in, so a module cannot be pushed, and the
 * bootloader's prompt is reached through the serial console. When the console
 * is gone, or only a trial kernel is wanted, kexec has the failure mode that
 * is wanted: it writes nothing, and a kernel that does not come up costs a
 * reset, which returns to the image of the bank the stick runs from.
 *
 * THE ELF AND NOT THE uImage. The uImage is LZMA-compressed and carries its
 * load and entry addresses in a header that the build computes; using it here
 * would mean decompressing in user space and trusting two numbers copied out
 * of a header. vmlinux carries the same numbers as ELF program headers, which
 * is where the build got them from, so this reads them from the source. It is
 * also what kexec-tools does on MIPS, for the same reason.
 *
 * WHAT THE NEW KERNEL IS TOLD, which is nothing. MIPS hands the command line
 * over in a0/a1 as argc/argv, read by fw_init_cmdline(). Those come from the
 * kernel's kexec_args[], which only a platform's _machine_kexec_prepare()
 * fills in, and this platform's is NULL, as it is for every platform without
 * CONFIG_UHI_BOOT. So the new kernel starts with an empty firmware command
 * line, and arch/mips/kernel/setup.c appends CONFIG_CMDLINE to it: a kernel
 * built with one comes up with exactly that, and one built without comes up
 * with nothing. Passing a real command line through would mean inventing a
 * hand-off this platform does not have, which would be separate work.
 */

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef __NR_kexec_load
#define __NR_kexec_load 4311		/* o32 */
#endif

#define KEXEC_ARCH_MIPS		(8 << 16)
#define KEXEC_SEGMENT_MAX	16

#define REBOOT_MAGIC1		0xfee1dead
#define REBOOT_MAGIC2		672274793
#define REBOOT_CMD_KEXEC	0x45584543

struct kexec_segment {
	const void *buf;
	size_t bufsz;
	const void *mem;
	size_t memsz;
};

static const char *me = "kexecboot";

static void fail(const char *what)
{
	fprintf(stderr, "%s: %s: %s\n", me, what, strerror(errno));
	exit(1);
}

static void failx(const char *fmt, ...)
	__attribute__((format(printf, 1, 2), noreturn));

static void failx(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", me);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

/*
 * KSEG0 and KSEG1 are the unmapped windows onto the first 512 MB, and the
 * kernel is linked in one of them. kexec_load wants PHYSICAL addresses, and
 * machine_kexec.c turns them back with phys_to_virt(), so stripping the
 * window bits is exactly the inverse of what it will do.
 */
static unsigned long to_phys(unsigned long addr, const char *what)
{
	if (addr < 0x80000000UL || addr >= 0xc0000000UL)
		failx("%s %#lx is not in KSEG0 or KSEG1; this program only "
		      "handles an unmapped kernel", what, addr);
	return addr & 0x1fffffffUL;
}

/* totalram_pages()/2, halved again, is the kernel's per-segment ceiling. */
static unsigned long ram_kb(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[128];
	unsigned long kb = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "MemTotal: %lu kB", &kb) == 1)
			break;
	fclose(f);
	return kb;
}

int main(int argc, char **argv)
{
	struct kexec_segment seg[KEXEC_SEGMENT_MAX];
	unsigned long entry, total_pages = 0, limit;
	const Elf32_Ehdr *eh;
	const Elf32_Phdr *ph;
	int fd, i, nr = 0, load_only = 0;
	long page = sysconf(_SC_PAGESIZE);
	const char *path;
	struct stat st;
	const char *img;

	if (argc > 1 && !strcmp(argv[1], "-n")) {
		load_only = 1;
		argc--; argv++;
	}
	if (argc != 2) {
		fprintf(stderr,
			"usage: %s [-n] <vmlinux>\n"
			"  loads the ELF and boots it immediately;\n"
			"  -n loads it and returns, so `reboot` starts it later\n",
			me);
		return 2;
	}
	path = argv[1];

	fd = open(path, O_RDONLY);
	if (fd < 0)
		fail(path);
	if (fstat(fd, &st) < 0)
		fail("fstat");
	if ((size_t)st.st_size < sizeof(*eh))
		failx("%s is %lld bytes; that is not an ELF",
		      path, (long long)st.st_size);

	img = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (img == MAP_FAILED)
		fail("mmap");

	eh = (const Elf32_Ehdr *)img;
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG))
		failx("%s has no ELF magic", path);
	if (eh->e_ident[EI_CLASS] != ELFCLASS32 ||
	    eh->e_ident[EI_DATA] != ELFDATA2MSB)
		failx("%s is not a 32-bit big-endian ELF; this board is both",
		      path);
	if (eh->e_machine != EM_MIPS)
		failx("%s is for machine %u, not MIPS", path, eh->e_machine);
	if (eh->e_phoff == 0 || eh->e_phnum == 0)
		failx("%s has no program headers, so nothing says where to "
		      "load it", path);
	if (eh->e_phentsize != sizeof(*ph))
		failx("%s has %u-byte program headers, expected %zu",
		      path, eh->e_phentsize, sizeof(*ph));
	if ((size_t)eh->e_phoff + (size_t)eh->e_phnum * sizeof(*ph) >
	    (size_t)st.st_size)
		failx("%s claims program headers past the end of the file",
		      path);

	entry = to_phys(eh->e_entry, "entry point");
	ph = (const Elf32_Phdr *)(img + eh->e_phoff);

	/*
	 * THE CEILING IS PER SEGMENT, AND THAT IS WHY THIS SPLITS.
	 *
	 * sanity_check_segment_list() refuses any single segment bigger than
	 * a quarter of RAM, and separately refuses a whole image bigger than
	 * half of it. Measured on this board: a 27 592 kB machine, so 6.7 MB
	 * per segment against 13.5 MB in total -- and the development kernel
	 * is one 7.6 MB PT_LOAD, which fails the first test with 5.9 MB of
	 * room still left under the second.
	 *
	 * A segment is a copy instruction, not a unit of meaning. Cutting one
	 * contiguous load into several contiguous pieces produces the same
	 * bytes at the same addresses, so the fix belongs here rather than in
	 * a smaller image -- an image that has to stay under a limit is a
	 * treadmill, and the limit moves with the board's RAM anyway.
	 *
	 * The split is into the FEWEST pieces that fit, so the common case
	 * stays one segment and the printout stays readable.
	 */
	limit = ram_kb() / 4 * 1024;
	if (limit)
		limit &= ~(unsigned long)(page - 1);

	for (i = 0; i < eh->e_phnum; i++) {
		unsigned long mem, memsz, skew, chunk, off;
		const char *buf;
		size_t bufsz;
		int pieces;

		if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0)
			continue;
		if ((size_t)ph[i].p_offset + (size_t)ph[i].p_filesz >
		    (size_t)st.st_size)
			failx("segment %d runs past the end of the file", i);
		if (ph[i].p_filesz > ph[i].p_memsz)
			failx("segment %d has more file than memory", i);

		/*
		 * kexec_load rejects a segment whose mem or mem+memsz is not
		 * page aligned, so round the destination out to whole pages
		 * and keep buf pointing at the same byte by moving it back by
		 * the same amount. The extra bytes at the front are read from
		 * the file, which for a kernel image means the bytes that
		 * really do belong there.
		 */
		mem = to_phys(ph[i].p_paddr, "segment address");
		skew = mem & (unsigned long)(page - 1);
		if (skew > ph[i].p_offset)
			failx("segment %d starts %#lx into a page but only "
			      "%#x into the file", i, skew, ph[i].p_offset);
		mem -= skew;
		buf = img + ph[i].p_offset - skew;
		bufsz = ph[i].p_filesz + skew;
		memsz = ph[i].p_memsz + skew;
		memsz = (memsz + page - 1) & ~(unsigned long)(page - 1);

		pieces = 1;
		if (limit && memsz > limit)
			pieces = (int)((memsz + limit - 1) / limit);
		chunk = (memsz + pieces - 1) / pieces;
		chunk = (chunk + page - 1) & ~(unsigned long)(page - 1);

		for (off = 0; off < memsz; off += chunk) {
			unsigned long this = memsz - off < chunk ?
					     memsz - off : chunk;

			if (nr == KEXEC_SEGMENT_MAX)
				failx("this image needs more than %d segments; "
				      "the board has too little RAM for it",
				      KEXEC_SEGMENT_MAX);

			seg[nr].mem = (const void *)(mem + off);
			seg[nr].memsz = this;
			/*
			 * The tail of a load is .bss: memsz runs past filesz,
			 * and a piece that starts beyond the file carries no
			 * bytes at all. kexec zero-fills whatever memsz does
			 * not cover, which is exactly what .bss wants.
			 */
			if (off < bufsz) {
				seg[nr].buf = buf + off;
				seg[nr].bufsz = bufsz - off < this ?
						bufsz - off : this;
			} else {
				seg[nr].buf = NULL;
				seg[nr].bufsz = 0;
			}
			total_pages += this / page;
			nr++;
		}
	}
	if (nr == 0)
		failx("%s has no PT_LOAD segment to load", path);

	printf("%s: %d segment(s), %lu page(s), entry %#lx\n",
	       me, nr, total_pages, entry);
	for (i = 0; i < nr; i++)
		printf("  seg %2d: %8zu B of file -> %#10lx, %8zu B of memory\n",
		       i, seg[i].bufsz, (unsigned long)seg[i].mem, seg[i].memsz);
	if (limit && total_pages * (unsigned long)page > limit * 2)
		failx("the whole image is %lu B and the kernel's ceiling is "
		      "%lu B (half of RAM); splitting cannot help with that",
		      total_pages * (unsigned long)page, limit * 2);

	if (syscall(__NR_kexec_load, entry, (unsigned long)nr, seg,
		    (unsigned long)KEXEC_ARCH_MIPS) < 0)
		fail("kexec_load");
	printf("%s: loaded\n", me);

	if (load_only) {
		printf("%s: not booting; `reboot` will start it\n", me);
		return 0;
	}

	sync();
	if (syscall(__NR_reboot, REBOOT_MAGIC1, REBOOT_MAGIC2,
		    REBOOT_CMD_KEXEC, (void *)0) < 0)
		fail("reboot");

	/* Only reached if the kernel declined to go. */
	failx("reboot returned, which means the new kernel was not started");
}
