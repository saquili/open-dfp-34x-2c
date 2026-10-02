/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Integer division by zero. The compiler checks the divisor and executes a break
 * instruction, which the kernel turns into SIGFPE with the code FPE_INTDIV; the
 * 64-bit division routine of the compiler's runtime library does the same. Each
 * case jumps out of the handler and the program goes on. The emulator qemu-mips
 * sends SIGTRAP for the instruction, so "run" does not run this program under it.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include "check.h"

static volatile sig_atomic_t code;
static sigjmp_buf env;

static void on_fpe(int sig, siginfo_t *si, void *ctx)
{
	(void)sig;
	(void)ctx;
	code = si->si_code;
	siglongjmp(env, 1);
}

static int32_t sdiv32(int32_t a, int32_t b)
{
	return a / b;
}

static uint32_t umod32(uint32_t a, uint32_t b)
{
	return a % b;
}

static uint64_t udiv64(uint64_t a, uint64_t b)
{
	return a / b;
}

static int64_t smod64(int64_t a, int64_t b)
{
	return a % b;
}

int main(void)
{
	struct sigaction sa;
	int32_t (*volatile s32)(int32_t, int32_t) = sdiv32;
	uint32_t (*volatile u32)(uint32_t, uint32_t) = umod32;
	uint64_t (*volatile u64)(uint64_t, uint64_t) = udiv64;
	int64_t (*volatile s64)(int64_t, int64_t) = smod64;
	volatile int32_t zero = 0;
	volatile uint32_t result32 = 0;
	volatile uint64_t result64 = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_fpe;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&sa.sa_mask);
	CHECK(sigaction(SIGFPE, &sa, NULL) == 0);

	CHECK(s32(84, 2) == 42 && u32(17, 5) == 2 && u64(1ULL << 40, 1024) == (1ULL << 30));

	code = 0;
	if (sigsetjmp(env, 1) == 0)
		result32 = (uint32_t)s32(1, zero);
	CHECK(code == FPE_INTDIV);

	code = 0;
	if (sigsetjmp(env, 1) == 0)
		result32 = u32(1, (uint32_t)zero);
	CHECK(code == FPE_INTDIV);

	code = 0;
	if (sigsetjmp(env, 1) == 0)
		result64 = u64(1, (uint64_t)zero);
	CHECK(code == FPE_INTDIV);

	code = 0;
	if (sigsetjmp(env, 1) == 0)
		result64 = (uint64_t)s64(1, (int64_t)zero);
	CHECK(code == FPE_INTDIV);

	CHECK(result32 == 0 && result64 == 0);
	CHECK(s32(-9, 3) == -3);			/* and the program goes on */
	DONE("divzero");
}
