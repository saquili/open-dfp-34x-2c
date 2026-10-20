/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * Atomic operations, which the compiler makes in place as ll/sc sequences:
 * the __sync and the C11 operations on objects of 1, 2 and 4 bytes, the bytes
 * beside a small object left as they were, and a count that the program and a
 * signal handler both add to, so that the sum is right only if every addition
 * that the handler interrupts is made again.
 */
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>
#include "check.h"

static struct {
	unsigned char before, c, after, pad;
	unsigned short s, beside;
} small = { 0x5a, 250, 0xa5, 0, 65530, 0x1234 };

static volatile unsigned int count;
static volatile sig_atomic_t ticks;

static void on_alrm(int sig)
{
	(void)sig;
	__sync_fetch_and_add(&count, 1000);
	ticks++;
}

int main(void)
{
	unsigned int w = 7;
	atomic_uint a = 5;
	atomic_flag flag = ATOMIC_FLAG_INIT;
	unsigned int expected;
	struct sigaction sa;
	struct itimerval tv;
	sigset_t set;
	unsigned long i, sum;

	/* 1 and 2 bytes: the sequence works on the word around the object. */
	CHECK(__sync_fetch_and_add(&small.c, 10) == 250 && small.c == 4);
	CHECK(__sync_fetch_and_sub(&small.s, 10) == 65530 && small.s == 65520);
	CHECK(__sync_val_compare_and_swap(&small.c, 4, 99) == 4 && small.c == 99);
	CHECK(small.before == 0x5a && small.after == 0xa5 && small.beside == 0x1234);

	/* 4 bytes. */
	CHECK(__sync_fetch_and_add(&w, 5) == 7 && w == 12);
	CHECK(__sync_add_and_fetch(&w, 1) == 13);
	CHECK(__sync_fetch_and_or(&w, 0x100) == 13 && w == 0x10d);
	CHECK(__sync_fetch_and_and(&w, 0xff) == 0x10d && w == 0x0d);
	CHECK(__sync_fetch_and_xor(&w, 0x0f) == 0x0d && w == 0x02);
	CHECK(__sync_val_compare_and_swap(&w, 2, 40) == 2 && w == 40);
	CHECK(__sync_val_compare_and_swap(&w, 2, 50) == 40 && w == 40);
	CHECK(__sync_bool_compare_and_swap(&w, 40, 41) && w == 41);
	CHECK(!__sync_bool_compare_and_swap(&w, 40, 42) && w == 41);
	CHECK(__sync_lock_test_and_set(&w, 1) == 41 && w == 1);
	__sync_lock_release(&w);
	CHECK(w == 0);
	__sync_synchronize();

	/* C11. */
	CHECK(atomic_fetch_add(&a, 3) == 5 && atomic_load(&a) == 8);
	CHECK(atomic_exchange(&a, 20) == 8 && atomic_load(&a) == 20);
	expected = 20;
	CHECK(atomic_compare_exchange_strong(&a, &expected, 21) && atomic_load(&a) == 21);
	CHECK(!atomic_compare_exchange_strong(&a, &expected, 22) && expected == 21);
	CHECK(atomic_fetch_or(&a, 0x40) == 21 && atomic_load(&a) == 0x55);
	CHECK(!atomic_flag_test_and_set(&flag) && atomic_flag_test_and_set(&flag));
	atomic_flag_clear(&flag);
	CHECK(!atomic_flag_test_and_set(&flag));
	atomic_thread_fence(memory_order_seq_cst);

	/*
	 * The program and a handler of a timer's signal add to one count. A
	 * signal between the load and the store of the program's addition ends
	 * the sequence's reservation, and the addition is made again.
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alrm;
	sigemptyset(&sa.sa_mask);
	CHECK(sigaction(SIGALRM, &sa, NULL) == 0);
	memset(&tv, 0, sizeof(tv));
	tv.it_interval.tv_usec = 1000;
	tv.it_value.tv_usec = 1000;
	CHECK(setitimer(ITIMER_REAL, &tv, NULL) == 0);
	for (i = 0; i < 200000 || (ticks < 20 && i < 100000000); i++)
		__sync_fetch_and_add(&count, 1);
	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0);
	memset(&tv, 0, sizeof(tv));
	CHECK(setitimer(ITIMER_REAL, &tv, NULL) == 0);
	sum = i + 1000UL * (unsigned long)ticks;
	CHECK(ticks >= 20);
	CHECK(count == sum);
	DONE("atomic");
}
