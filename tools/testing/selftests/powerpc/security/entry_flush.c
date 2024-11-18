// SPDX-License-Identifier: GPL-2.0+

/*
 * Copyright 2018 IBM Corporation.
 */

#define __SANE_USERSPACE_TYPES__

#include <sys/types.h>
#include <stdint.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "utils.h"

#define CACHELINE_SIZE 128

struct perf_event_read {
	__u64 nr;
	__u64 l1d_misses;
};

static inline __u64 load(void *addr)
{
	__u64 tmp;

	asm volatile("ld %0,0(%1)" : "=r"(tmp) : "b"(addr));

	return tmp;
}

static void syscall_loop(char *p, unsigned long iterations,
		  unsigned long zero_size)
{
	for (unsigned long i = 0; i < iterations; i++) {
		for (unsigned long j = 0; j < zero_size; j += CACHELINE_SIZE)
			load(p + j);
		getppid();
	}
}

int entry_flush_test(void)
{
	char *p;
	int repetitions = 10;
	int fd, passes = 0, iter, rc = 0;
	struct perf_event_read v;
	__u64 l1d_misses_total = 0;
	unsigned long iterations = 100000, zero_size = 24 * 1024;
	unsigned long l1d_misses_expected;
	int rfi_flush_orig;
	int entry_flush, entry_flush_orig;

	SKIP_IF(geteuid() != 0);

	// The PMU event we use only works on Power7 or later
	SKIP_IF(!have_hwcap(PPC_FEATURE_ARCH_2_06));

	if (read_debugfs_file("powerpc/rfi_flush", &rfi_flush_orig) < 0) {
		perror("Unable to read powerpc/rfi_flush debugfs file");
		SKIP_IF(1);
	}

	if (read_debugfs_file("powerpc/entry_flush", &entry_flush_orig) < 0) {
		perror("Unable to read powerpc/entry_flush debugfs file");
		SKIP_IF(1);
	}

	if (rfi_flush_orig != 0) {
		if (write_debugfs_file("powerpc/rfi_flush", 0) < 0) {
			perror("error writing to powerpc/rfi_flush debugfs file");
			FAIL_IF(1);
		}
	}

	entry_flush = entry_flush_orig;

	fd = perf_event_open_counter(PERF_TYPE_RAW, /* L1d miss */ 0x400f0, -1);
	FAIL_IF(fd < 0);

	p = (char *)memalign(zero_size, CACHELINE_SIZE);

	FAIL_IF(perf_event_enable(fd));

	// disable L1 prefetching
	set_dscr(1);

	iter = repetitions;

	/*
	 * We expect to see l1d miss for each cacheline access when entry_flush
	 * is set. Allow a small variation on this.
	 */
	l1d_misses_expected = iterations * (zero_size / CACHELINE_SIZE - 2);

again:
	FAIL_IF(perf_event_reset(fd));

	syscall_loop(p, iterations, zero_size);

	FAIL_IF(read(fd, &v, sizeof(v)) != sizeof(v));

	if (entry_flush && v.l1d_misses >= l1d_misses_expected)
		passes++;
	else if (!entry_flush && v.l1d_misses < (l1d_misses_expected / 2))
		passes++;

	l1d_misses_total += v.l1d_misses;

	while (--iter)
		goto again;

	if (passes < repetitions) {
		printf("FAIL (L1D misses with entry_flush=%d: %llu %c %lu) [%d/%d failures]\n",
		       entry_flush, l1d_misses_total, entry_flush ? '<' : '>',
		       entry_flush ? repetitions * l1d_misses_expected :
		       repetitions * l1d_misses_expected / 2,
		       repetitions - passes, repetitions);
		rc = 1;
	} else
		printf("PASS (L1D misses with entry_flush=%d: %llu %c %lu) [%d/%d pass]\n",
		       entry_flush, l1d_misses_total, entry_flush ? '>' : '<',
		       entry_flush ? repetitions * l1d_misses_expected :
		       repetitions * l1d_misses_expected / 2,
		       passes, repetitions);

	if (entry_flush == entry_flush_orig) {
		entry_flush = !entry_flush_orig;
		if (write_debugfs_file("powerpc/entry_flush", entry_flush) < 0) {
			perror("error writing to powerpc/entry_flush debugfs file");
			return 1;
		}
		iter = repetitions;
		l1d_misses_total = 0;
		passes = 0;
		goto again;
	}

	perf_event_disable(fd);
	close(fd);

	set_dscr(0);

	if (write_debugfs_file("powerpc/rfi_flush", rfi_flush_orig) < 0) {
		perror("unable to restore original value of powerpc/rfi_flush debugfs file");
		return 1;
	}

	if (write_debugfs_file("powerpc/entry_flush", entry_flush_orig) < 0) {
		perror("unable to restore original value of powerpc/entry_flush debugfs file");
		return 1;
	}

	return rc;
}

int main(int argc, char *argv[])
{
	return test_harness(entry_flush_test, "entry_flush_test");
}
