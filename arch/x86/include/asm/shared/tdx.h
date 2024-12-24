/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SHARED_TDX_H
#define _ASM_X86_SHARED_TDX_H

#include <linux/bits.h>
#include <linux/types.h>

#define TDX_HYPERCALL_STANDARD  0

#define TDX_CPUID_LEAF_ID	0x21
#define TDX_IDENT		"IntelTDX    "

/* TDX module Call Leaf IDs */
#define TDG_VP_INFO			1
#define TDG_VP_VEINFO_GET		3
#define TDG_MR_REPORT			4
#define TDG_MEM_PAGE_ACCEPT		6
#define TDG_VM_RD			7
#define TDG_VM_WR			8

/* TDX TD-Scope Metadata. To be used by TDG.VM.WR and TDG.VM.RD */
#define TDCS_CONFIG_FLAGS		0x1110000300000016
#define TDCS_TD_CTLS			0x1110000300000017
#define TDCS_NOTIFY_ENABLES		0x9100000000000010

/* TDCS_CONFIG_FLAGS bits */
#define TDCS_CONFIG_FLEXIBLE_PENDING_VE	BIT_ULL(1)

/* TDCS_TD_CTLS bits */
#define TD_CTLS_PENDING_VE_DISABLE	BIT_ULL(0)

/* TDX hypercall Leaf IDs */
#define TDVMCALL_MAP_GPA		0x10001
#define TDVMCALL_REPORT_FATAL_ERROR	0x10003

#ifndef __ASSEMBLY__

/*
 * Used in __tdx_hypercall() to pass down and get back registers' values of
 * the TDCALL instruction when requesting services from the VMM.
 *
 * This is a software only structure and not part of the TDX module/VMM ABI.
 */
struct tdx_hypercall_args {
	u64 r8;
	u64 r9;
	u64 r10;
	u64 r11;
	u64 r12;
	u64 r13;
	u64 r14;
	u64 r15;
	u64 rdi;
	u64 rsi;
	u64 rbx;
	u64 rdx;
};

/* Used to request services from the VMM */
u64 __tdx_hypercall(struct tdx_hypercall_args *args);
u64 __tdx_hypercall_ret(struct tdx_hypercall_args *args);

/*
 * Wrapper for standard use of __tdx_hypercall with no output aside from
 * return code.
 */
static inline u64 _tdx_hypercall(u64 fn, u64 r12, u64 r13, u64 r14, u64 r15)
{
	struct tdx_hypercall_args args = {
		.r10 = TDX_HYPERCALL_STANDARD,
		.r11 = fn,
		.r12 = r12,
		.r13 = r13,
		.r14 = r14,
		.r15 = r15,
	};

	return __tdx_hypercall(&args);
}


/* Called from __tdx_hypercall() for unrecoverable failure */
void __tdx_hypercall_failed(void);

/*
 * Used in __tdcall*() to gather the input/output registers' values of the
 * TDCALL instruction when requesting services from the TDX module. This is a
 * software only structure and not part of the TDX module/VMM ABI
 */
struct tdx_module_args {
	u64 rcx;
	u64 rdx;
	u64 r8;
	u64 r9;
	u64 r10;
	u64 r11;
};

/* Used to communicate with the TDX module */
u64 __tdcall(u64 fn, struct tdx_module_args *args);
u64 __tdcall_ret(u64 fn, struct tdx_module_args *args);

bool tdx_accept_memory(phys_addr_t start, phys_addr_t end);

/*
 * The TDG.VP.VMCALL-Instruction-execution sub-functions are defined
 * independently from but are currently matched 1:1 with VMX EXIT_REASONs.
 * Reusing the KVM EXIT_REASON macros makes it easier to connect the host and
 * guest sides of these calls.
 */
static __always_inline u64 hcall_func(u64 exit_reason)
{
        return exit_reason;
}

#endif /* !__ASSEMBLY__ */
#endif /* _ASM_X86_SHARED_TDX_H */
