/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2022-2023 Rivos, Inc
 */

#ifndef _ASM_CPUFEATURE_H
#define _ASM_CPUFEATURE_H

#include <linux/bitmap.h>
#include <linux/jump_label.h>
#include <asm/hwcap.h>
#include <asm/alternative-macros.h>
#include <asm/errno.h>

/*
 * These are probed via a device_initcall(), via either the SBI or directly
 * from the corresponding CSRs.
 */
struct riscv_cpuinfo {
	unsigned long mvendorid;
	unsigned long marchid;
	unsigned long mimpid;
};

struct riscv_isainfo {
	DECLARE_BITMAP(isa, RISCV_ISA_EXT_MAX);
};

DECLARE_PER_CPU(struct riscv_cpuinfo, riscv_cpuinfo);

DECLARE_PER_CPU(long, misaligned_access_speed);

/* Per-cpu ISA extensions. */
extern struct riscv_isainfo hart_isa[NR_CPUS];

void riscv_user_isa_enable(void);

#ifdef CONFIG_RISCV_MISALIGNED
bool unaligned_ctl_available(void);
bool check_unaligned_access_emulated(int cpu);
void unaligned_emulation_finish(void);
#else
static inline bool unaligned_ctl_available(void)
{
	return false;
}

static inline bool check_unaligned_access_emulated(int cpu)
{
	return false;
}

static inline void unaligned_emulation_finish(void) {}
#endif

unsigned long riscv_get_elf_hwcap(void);

struct riscv_isa_ext_data {
	const unsigned int id;
	const char *name;
	const char *property;
};

extern const struct riscv_isa_ext_data riscv_isa_ext[];
extern const size_t riscv_isa_ext_count;
extern bool riscv_isa_fallback;

unsigned long riscv_isa_extension_base(const unsigned long *isa_bitmap);

bool __riscv_isa_extension_available(const unsigned long *isa_bitmap, int bit);
#define riscv_isa_extension_available(isa_bitmap, ext)	\
	__riscv_isa_extension_available(isa_bitmap, RISCV_ISA_EXT_##ext)

static __always_inline bool
riscv_has_extension_likely(const unsigned long ext)
{
	compiletime_assert(ext < RISCV_ISA_EXT_MAX,
			   "ext must be < RISCV_ISA_EXT_MAX");

	if (IS_ENABLED(CONFIG_RISCV_ALTERNATIVE)) {
		asm_volatile_goto(
		ALTERNATIVE("j	%l[l_no]", "nop", 0, %[ext], 1)
		:
		: [ext] "i" (ext)
		:
		: l_no);
	} else {
		if (!__riscv_isa_extension_available(NULL, ext))
			goto l_no;
	}

	return true;
l_no:
	return false;
}

static __always_inline bool
riscv_has_extension_unlikely(const unsigned long ext)
{
	compiletime_assert(ext < RISCV_ISA_EXT_MAX,
			   "ext must be < RISCV_ISA_EXT_MAX");

	if (IS_ENABLED(CONFIG_RISCV_ALTERNATIVE)) {
		asm_volatile_goto(
		ALTERNATIVE("nop", "j	%l[l_yes]", 0, %[ext], 1)
		:
		: [ext] "i" (ext)
		:
		: l_yes);
	} else {
		if (__riscv_isa_extension_available(NULL, ext))
			goto l_yes;
	}

	return false;
l_yes:
	return true;
}

static __always_inline bool riscv_cpu_has_extension_likely(int cpu, const unsigned long ext)
{
	if (IS_ENABLED(CONFIG_RISCV_ALTERNATIVE) && riscv_has_extension_likely(ext))
		return true;

	return __riscv_isa_extension_available(hart_isa[cpu].isa, ext);
}

static __always_inline bool riscv_cpu_has_extension_unlikely(int cpu, const unsigned long ext)
{
	if (IS_ENABLED(CONFIG_RISCV_ALTERNATIVE) && riscv_has_extension_unlikely(ext))
		return true;

	return __riscv_isa_extension_available(hart_isa[cpu].isa, ext);
}

#endif
