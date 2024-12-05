// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2024 Meta Platforms, Inc. and affiliates. */

#define BPF_NO_KFUNC_PROTOTYPES
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "bpf_misc.h"
#include "bpf_experimental.h"
#include "bpf_arena_common.h"

#define ARENA_SIZE (1ull << 32)

struct {
	__uint(type, BPF_MAP_TYPE_ARENA);
	__uint(map_flags, BPF_F_MMAPABLE);
	__uint(max_entries, ARENA_SIZE / PAGE_SIZE);
} arena SEC(".maps");

SEC("syscall")
__success __retval(0)
int big_alloc1(void *ctx)
{
#if defined(__BPF_FEATURE_ADDR_SPACE_CAST)
	volatile char __arena *page1, *page2, *no_page, *page3;
	void __arena *base;

	page1 = base = bpf_arena_alloc_pages(&arena, NULL, 1, NUMA_NO_NODE, 0);
	if (!page1)
		return 1;
	*page1 = 1;
	page2 = bpf_arena_alloc_pages(&arena, base + ARENA_SIZE - PAGE_SIZE * 2,
				      1, NUMA_NO_NODE, 0);
	if (!page2)
		return 2;
	*page2 = 2;
	no_page = bpf_arena_alloc_pages(&arena, base + ARENA_SIZE - PAGE_SIZE,
					1, NUMA_NO_NODE, 0);
	if (no_page)
		return 3;
	if (*page1 != 1)
		return 4;
	if (*page2 != 2)
		return 5;
	bpf_arena_free_pages(&arena, (void __arena *)page1, 1);
	if (*page2 != 2)
		return 6;
	if (*page1 != 0) /* use-after-free should return 0 */
		return 7;
	page3 = bpf_arena_alloc_pages(&arena, NULL, 1, NUMA_NO_NODE, 0);
	if (!page3)
		return 8;
	*page3 = 3;
	if (page1 != page3)
		return 9;
	if (*page2 != 2)
		return 10;
	if (*(page1 + PAGE_SIZE) != 0)
		return 11;
	if (*(page1 - PAGE_SIZE) != 0)
		return 12;
	if (*(page2 + PAGE_SIZE) != 0)
		return 13;
	if (*(page2 - PAGE_SIZE) != 0)
		return 14;
#endif
	return 0;
}

#if defined(__BPF_FEATURE_ADDR_SPACE_CAST)
#define PAGE_CNT 100
__u8 __arena * __arena page[PAGE_CNT]; /* occupies the first page */
__u8 __arena *base;

/*
 * Check that arena's range_tree algorithm allocates pages sequentially
 * on the first pass and then fills in all gaps on the second pass.
 */
__noinline int alloc_pages(int page_cnt, int pages_atonce, bool first_pass,
		int max_idx, int step)
{
	__u8 __arena *pg;
	int i, pg_idx;

	for (i = 0; i < page_cnt; i++) {
		pg = bpf_arena_alloc_pages(&arena, NULL, pages_atonce,
					   NUMA_NO_NODE, 0);
		if (!pg)
			return step;
		pg_idx = (unsigned long) (pg - base) / PAGE_SIZE;
		if (first_pass) {
			/* Pages must be allocated sequentially */
			if (pg_idx != i)
				return step + 100;
		} else {
			/* Allocator must fill into gaps */
			if (pg_idx >= max_idx || (pg_idx & 1))
				return step + 200;
		}
		*pg = pg_idx;
		page[pg_idx] = pg;
		cond_break;
	}
	return 0;
}

SEC("syscall")
__success __retval(0)
int big_alloc2(void *ctx)
{
	__u8 __arena *pg;
	int i, err;

	base = bpf_arena_alloc_pages(&arena, NULL, 1, NUMA_NO_NODE, 0);
	if (!base)
		return 1;
	bpf_arena_free_pages(&arena, (void __arena *)base, 1);

	err = alloc_pages(PAGE_CNT, 1, true, PAGE_CNT, 2);
	if (err)
		return err;

	/* Clear all even pages */
	for (i = 0; i < PAGE_CNT; i += 2) {
		pg = page[i];
		if (*pg != i)
			return 3;
		bpf_arena_free_pages(&arena, (void __arena *)pg, 1);
		page[i] = NULL;
		cond_break;
	}

	/* Allocate into freed gaps */
	err = alloc_pages(PAGE_CNT / 2, 1, false, PAGE_CNT, 4);
	if (err)
		return err;

	/* Free pairs of pages */
	for (i = 0; i < PAGE_CNT; i += 4) {
		pg = page[i];
		if (*pg != i)
			return 5;
		bpf_arena_free_pages(&arena, (void __arena *)pg, 2);
		page[i] = NULL;
		page[i + 1] = NULL;
		cond_break;
	}

	/* Allocate 2 pages at a time into freed gaps */
	err = alloc_pages(PAGE_CNT / 4, 2, false, PAGE_CNT, 6);
	if (err)
		return err;

	/* Check pages without freeing */
	for (i = 0; i < PAGE_CNT; i += 2) {
		pg = page[i];
		if (*pg != i)
			return 7;
		cond_break;
	}

	pg = bpf_arena_alloc_pages(&arena, NULL, 1, NUMA_NO_NODE, 0);

	if (!pg)
		return 8;
	/*
	 * The first PAGE_CNT pages are occupied. The new page
	 * must be above.
	 */
	if ((pg - base) / PAGE_SIZE < PAGE_CNT)
		return 9;
	return 0;
}
#endif
char _license[] SEC("license") = "GPL";
