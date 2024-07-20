// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <asm/sbi.h>
#include <asm/mmu_context.h>

/*
 * Flush entire TLB if number of entries to be flushed is greater
 * than the threshold below.
 */
static unsigned long tlb_flush_all_threshold __read_mostly = 64;

static void local_flush_tlb_range_threshold_asid(unsigned long start,
						 unsigned long size,
						 unsigned long stride,
						 unsigned long asid)
{
	unsigned long nr_ptes_in_range = DIV_ROUND_UP(size, stride);
	int i;

	if (nr_ptes_in_range > tlb_flush_all_threshold) {
		local_flush_tlb_all_asid(asid);
		return;
	}

	for (i = 0; i < nr_ptes_in_range; ++i) {
		local_flush_tlb_page_asid(start, asid);
		start += stride;
	}
}

static inline void local_flush_tlb_range_asid(unsigned long start,
		unsigned long size, unsigned long stride, unsigned long asid)
{
	if (size <= stride)
		local_flush_tlb_page_asid(start, asid);
	else if (size == FLUSH_TLB_MAX_SIZE)
		local_flush_tlb_all_asid(asid);
	else
		local_flush_tlb_range_threshold_asid(start, size, stride, asid);
}

/* Flush a range of kernel pages without broadcasting */
void local_flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	local_flush_tlb_range_asid(start, end - start, PAGE_SIZE, FLUSH_TLB_NO_ASID);
}

static void __ipi_flush_tlb_all(void *info)
{
	local_flush_tlb_all();
}

void flush_tlb_all(void)
{
	if (riscv_use_ipi_for_rfence())
		on_each_cpu(__ipi_flush_tlb_all, NULL, 1);
	else
		sbi_remote_sfence_vma_asid(NULL, 0, FLUSH_TLB_MAX_SIZE, FLUSH_TLB_NO_ASID);
}

struct flush_tlb_range_data {
	unsigned long asid;
	unsigned long start;
	unsigned long size;
	unsigned long stride;
};

static void __ipi_flush_tlb_range_asid(void *info)
{
	struct flush_tlb_range_data *d = info;

	local_flush_tlb_range_asid(d->start, d->size, d->stride, d->asid);
}

static void __flush_tlb_range(struct mm_struct *mm, unsigned long start,
			      unsigned long size, unsigned long stride)
{
	struct flush_tlb_range_data ftd;
	const struct cpumask *cmask;
	unsigned long asid = FLUSH_TLB_NO_ASID;
	bool broadcast;

	if (mm) {
		unsigned int cpuid;

		cmask = mm_cpumask(mm);
		if (cpumask_empty(cmask))
			return;

		cpuid = get_cpu();
		/* check if the tlbflush needs to be sent to other CPUs */
		broadcast = cpumask_any_but(cmask, cpuid) < nr_cpu_ids;

		if (static_branch_unlikely(&use_asid_allocator))
			asid = atomic_long_read(&mm->context.id) & asid_mask;
	} else {
		cmask = cpu_online_mask;
		broadcast = true;
	}

	if (broadcast) {
		if (riscv_use_ipi_for_rfence()) {
			ftd.asid = asid;
			ftd.start = start;
			ftd.size = size;
			ftd.stride = stride;
			on_each_cpu_mask(cmask,
					 __ipi_flush_tlb_range_asid,
					 &ftd, 1);
		} else
			sbi_remote_sfence_vma_asid(cmask,
						   start, size, asid);
	} else {
		local_flush_tlb_range_asid(start, size, stride, asid);
	}

	if (mm)
		put_cpu();
}

void flush_tlb_mm(struct mm_struct *mm)
{
	__flush_tlb_range(mm, 0, FLUSH_TLB_MAX_SIZE, PAGE_SIZE);
}

void flush_tlb_mm_range(struct mm_struct *mm,
			unsigned long start, unsigned long end,
			unsigned int page_size)
{
	__flush_tlb_range(mm, start, end - start, page_size);
}

void flush_tlb_page(struct vm_area_struct *vma, unsigned long addr)
{
	__flush_tlb_range(vma->vm_mm, addr, PAGE_SIZE, PAGE_SIZE);
}

void flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
		     unsigned long end)
{
	__flush_tlb_range(vma->vm_mm, start, end - start, PAGE_SIZE);
}

void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	__flush_tlb_range(NULL, start, end - start, PAGE_SIZE);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
void flush_pmd_tlb_range(struct vm_area_struct *vma, unsigned long start,
			unsigned long end)
{
	__flush_tlb_range(vma->vm_mm, start, end - start, PMD_SIZE);
}
#endif
