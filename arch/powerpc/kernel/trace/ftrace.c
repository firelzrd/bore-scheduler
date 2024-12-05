// SPDX-License-Identifier: GPL-2.0
/*
 * Code for replacing ftrace calls with jumps.
 *
 * Copyright (C) 2007-2008 Steven Rostedt <srostedt@redhat.com>
 *
 * Thanks goes out to P.A. Semi, Inc for supplying me with a PPC64 box.
 *
 * Added function graph tracer code, taken from x86 that was written
 * by Frederic Weisbecker, and ported to PPC by Steven Rostedt.
 *
 */

#define pr_fmt(fmt) "ftrace-powerpc: " fmt

#include <linux/spinlock.h>
#include <linux/hardirq.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/ftrace.h>
#include <linux/percpu.h>
#include <linux/init.h>
#include <linux/list.h>

#include <asm/cacheflush.h>
#include <asm/text-patching.h>
#include <asm/ftrace.h>
#include <asm/syscall.h>
#include <asm/inst.h>
#include <asm/sections.h>

#define	NUM_FTRACE_TRAMPS	2
static unsigned long ftrace_tramps[NUM_FTRACE_TRAMPS];

unsigned long ftrace_call_adjust(unsigned long addr)
{
	if (addr >= (unsigned long)__exittext_begin && addr < (unsigned long)__exittext_end)
		return 0;

	if (IS_ENABLED(CONFIG_ARCH_USING_PATCHABLE_FUNCTION_ENTRY) &&
	    !IS_ENABLED(CONFIG_PPC_FTRACE_OUT_OF_LINE)) {
		addr += MCOUNT_INSN_SIZE;
		if (IS_ENABLED(CONFIG_DYNAMIC_FTRACE_WITH_CALL_OPS))
			addr += MCOUNT_INSN_SIZE;
	}

	return addr;
}

static ppc_inst_t ftrace_create_branch_inst(unsigned long ip, unsigned long addr, int link)
{
	ppc_inst_t op;

	WARN_ON(!is_offset_in_branch_range(addr - ip));
	create_branch(&op, (u32 *)ip, addr, link ? BRANCH_SET_LINK : 0);

	return op;
}

static inline int ftrace_read_inst(unsigned long ip, ppc_inst_t *op)
{
	if (copy_inst_from_kernel_nofault(op, (void *)ip)) {
		pr_err("0x%lx: fetching instruction failed\n", ip);
		return -EFAULT;
	}

	return 0;
}

static inline int ftrace_validate_inst(unsigned long ip, ppc_inst_t inst)
{
	ppc_inst_t op;
	int ret;

	ret = ftrace_read_inst(ip, &op);
	if (!ret && !ppc_inst_equal(op, inst)) {
		pr_err("0x%lx: expected (%08lx) != found (%08lx)\n",
		       ip, ppc_inst_as_ulong(inst), ppc_inst_as_ulong(op));
		ret = -EINVAL;
	}

	return ret;
}

static inline int ftrace_modify_code(unsigned long ip, ppc_inst_t old, ppc_inst_t new)
{
	int ret = ftrace_validate_inst(ip, old);

	if (!ret && !ppc_inst_equal(old, new))
		ret = patch_instruction((u32 *)ip, new);

	return ret;
}

static int is_bl_op(ppc_inst_t op)
{
	return (ppc_inst_val(op) & ~PPC_LI_MASK) == PPC_RAW_BL(0);
}

static unsigned long find_ftrace_tramp(unsigned long ip)
{
	int i;

	for (i = 0; i < NUM_FTRACE_TRAMPS; i++)
		if (!ftrace_tramps[i])
			continue;
		else if (is_offset_in_branch_range(ftrace_tramps[i] - ip))
			return ftrace_tramps[i];

	return 0;
}

#ifdef CONFIG_MODULES
static unsigned long ftrace_lookup_module_stub(unsigned long ip, unsigned long addr)
{
	struct module *mod = NULL;

	preempt_disable();
	mod = __module_text_address(ip);
	preempt_enable();

	if (!mod)
		pr_err("No module loaded at addr=%lx\n", ip);

	return (addr == (unsigned long)ftrace_caller ? mod->arch.tramp : mod->arch.tramp_regs);
}
#else
static unsigned long ftrace_lookup_module_stub(unsigned long ip, unsigned long addr)
{
	return 0;
}
#endif

static unsigned long ftrace_get_ool_stub(struct dyn_ftrace *rec)
{
#ifdef CONFIG_PPC_FTRACE_OUT_OF_LINE
	return rec->arch.ool_stub;
#else
	BUILD_BUG();
#endif
}

static int ftrace_get_call_inst(struct dyn_ftrace *rec, unsigned long addr, ppc_inst_t *call_inst)
{
	unsigned long ip;
	unsigned long stub;

	if (IS_ENABLED(CONFIG_PPC_FTRACE_OUT_OF_LINE))
		ip = ftrace_get_ool_stub(rec) + MCOUNT_INSN_SIZE; /* second instruction in stub */
	else
		ip = rec->ip;

	if (!is_offset_in_branch_range(addr - ip) && addr != FTRACE_ADDR &&
	    addr != FTRACE_REGS_ADDR) {
		/* This can only happen with ftrace direct */
		if (!IS_ENABLED(CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS)) {
			pr_err("0x%lx (0x%lx): Unexpected target address 0x%lx\n",
			       ip, rec->ip, addr);
			return -EINVAL;
		}
		addr = FTRACE_ADDR;
	}

	if (is_offset_in_branch_range(addr - ip))
		/* Within range */
		stub = addr;
	else if (core_kernel_text(ip))
		/* We would be branching to one of our ftrace stubs */
		stub = find_ftrace_tramp(ip);
	else
		stub = ftrace_lookup_module_stub(ip, addr);

	if (!stub) {
		pr_err("0x%lx (0x%lx): No ftrace stubs reachable\n", ip, rec->ip);
		return -EINVAL;
	}

	*call_inst = ftrace_create_branch_inst(ip, stub, 1);
	return 0;
}

static int ftrace_init_ool_stub(struct module *mod, struct dyn_ftrace *rec)
{
#ifdef CONFIG_PPC_FTRACE_OUT_OF_LINE
	static int ool_stub_text_index, ool_stub_text_end_index, ool_stub_inittext_index;
	int ret = 0, ool_stub_count, *ool_stub_index;
	ppc_inst_t inst;
	/*
	 * See ftrace_entry.S if changing the below instruction sequence, as we rely on
	 * decoding the last branch instruction here to recover the correct function ip.
	 */
	struct ftrace_ool_stub *ool_stub, ool_stub_template = {
		.insn = {
			PPC_RAW_MFLR(_R0),
			PPC_RAW_NOP(),		/* bl ftrace_caller */
			PPC_RAW_MTLR(_R0),
			PPC_RAW_NOP()		/* b rec->ip + 4 */
		}
	};

	WARN_ON(rec->arch.ool_stub);

	if (is_kernel_inittext(rec->ip)) {
		ool_stub = ftrace_ool_stub_inittext;
		ool_stub_index = &ool_stub_inittext_index;
		ool_stub_count = ftrace_ool_stub_inittext_count;
	} else if (is_kernel_text(rec->ip)) {
		/*
		 * ftrace records are sorted, so we first use up the stub area within .text
		 * (ftrace_ool_stub_text) before using the area at the end of .text
		 * (ftrace_ool_stub_text_end), unless the stub is out of range of the record.
		 */
		if (ool_stub_text_index >= ftrace_ool_stub_text_count ||
		    !is_offset_in_branch_range((long)rec->ip -
					       (long)&ftrace_ool_stub_text[ool_stub_text_index])) {
			ool_stub = ftrace_ool_stub_text_end;
			ool_stub_index = &ool_stub_text_end_index;
			ool_stub_count = ftrace_ool_stub_text_end_count;
		} else {
			ool_stub = ftrace_ool_stub_text;
			ool_stub_index = &ool_stub_text_index;
			ool_stub_count = ftrace_ool_stub_text_count;
		}
#ifdef CONFIG_MODULES
	} else if (mod) {
		ool_stub = mod->arch.ool_stubs;
		ool_stub_index = &mod->arch.ool_stub_index;
		ool_stub_count = mod->arch.ool_stub_count;
#endif
	} else {
		return -EINVAL;
	}

	ool_stub += (*ool_stub_index)++;

	if (WARN_ON(*ool_stub_index > ool_stub_count))
		return -EINVAL;

	if (!is_offset_in_branch_range((long)rec->ip - (long)&ool_stub->insn[0]) ||
	    !is_offset_in_branch_range((long)(rec->ip + MCOUNT_INSN_SIZE) -
				       (long)&ool_stub->insn[3])) {
		pr_err("%s: ftrace ool stub out of range (%p -> %p).\n",
					__func__, (void *)rec->ip, (void *)&ool_stub->insn[0]);
		return -EINVAL;
	}

	rec->arch.ool_stub = (unsigned long)&ool_stub->insn[0];

	/* bl ftrace_caller */
	if (!mod)
		ret = ftrace_get_call_inst(rec, (unsigned long)ftrace_caller, &inst);
#ifdef CONFIG_MODULES
	else
		/*
		 * We can't use ftrace_get_call_inst() since that uses
		 * __module_text_address(rec->ip) to look up the module.
		 * But, since the module is not fully formed at this stage,
		 * the lookup fails. We know the target though, so generate
		 * the branch inst directly.
		 */
		inst = ftrace_create_branch_inst(ftrace_get_ool_stub(rec) + MCOUNT_INSN_SIZE,
						 mod->arch.tramp, 1);
#endif
	ool_stub_template.insn[1] = ppc_inst_val(inst);

	/* b rec->ip + 4 */
	if (!ret && create_branch(&inst, &ool_stub->insn[3], rec->ip + MCOUNT_INSN_SIZE, 0))
		return -EINVAL;
	ool_stub_template.insn[3] = ppc_inst_val(inst);

	if (!ret)
		ret = patch_instructions((u32 *)ool_stub, (u32 *)&ool_stub_template,
					 sizeof(ool_stub_template), false);

	return ret;
#else /* !CONFIG_PPC_FTRACE_OUT_OF_LINE */
	BUILD_BUG();
#endif
}

#ifdef CONFIG_DYNAMIC_FTRACE_WITH_CALL_OPS
static const struct ftrace_ops *powerpc_rec_get_ops(struct dyn_ftrace *rec)
{
	const struct ftrace_ops *ops = NULL;

	if (rec->flags & FTRACE_FL_CALL_OPS_EN) {
		ops = ftrace_find_unique_ops(rec);
		WARN_ON_ONCE(!ops);
	}

	if (!ops)
		ops = &ftrace_list_ops;

	return ops;
}

static int ftrace_rec_set_ops(struct dyn_ftrace *rec, const struct ftrace_ops *ops)
{
	if (IS_ENABLED(CONFIG_PPC_FTRACE_OUT_OF_LINE))
		return patch_ulong((void *)(ftrace_get_ool_stub(rec) - sizeof(unsigned long)),
				   (unsigned long)ops);
	else
		return patch_ulong((void *)(rec->ip - MCOUNT_INSN_SIZE - sizeof(unsigned long)),
				   (unsigned long)ops);
}

static int ftrace_rec_set_nop_ops(struct dyn_ftrace *rec)
{
	return ftrace_rec_set_ops(rec, &ftrace_nop_ops);
}

static int ftrace_rec_update_ops(struct dyn_ftrace *rec)
{
	return ftrace_rec_set_ops(rec, powerpc_rec_get_ops(rec));
}
#else
static int ftrace_rec_set_nop_ops(struct dyn_ftrace *rec) { return 0; }
static int ftrace_rec_update_ops(struct dyn_ftrace *rec) { return 0; }
#endif

#ifdef CONFIG_DYNAMIC_FTRACE_WITH_REGS
int ftrace_modify_call(struct dyn_ftrace *rec, unsigned long old_addr, unsigned long addr)
{
	/* This should never be called since we override ftrace_replace_code() */
	WARN_ON(1);
	return -EINVAL;
}
#endif

int ftrace_make_call(struct dyn_ftrace *rec, unsigned long addr)
{
	ppc_inst_t old, new;
	unsigned long ip = rec->ip;
	int ret = 0;

	/* This can only ever be called during module load */
	if (WARN_ON(!IS_ENABLED(CONFIG_MODULES) || core_kernel_text(ip)))
		return -EINVAL;

	old = ppc_inst(PPC_RAW_NOP());
	if (IS_ENABLED(CONFIG_PPC_FTRACE_OUT_OF_LINE)) {
		ip = ftrace_get_ool_stub(rec) + MCOUNT_INSN_SIZE; /* second instruction in stub */
		ret = ftrace_get_call_inst(rec, (unsigned long)ftrace_caller, &old);
	}

	ret |= ftrace_get_call_inst(rec, addr, &new);

	if (!ret)
		ret = ftrace_modify_code(ip, old, new);

	ret = ftrace_rec_update_ops(rec);
	if (ret)
		return ret;

	if (!ret && IS_ENABLED(CONFIG_PPC_FTRACE_OUT_OF_LINE))
		ret = ftrace_modify_code(rec->ip, ppc_inst(PPC_RAW_NOP()),
			 ppc_inst(PPC_RAW_BRANCH((long)ftrace_get_ool_stub(rec) - (long)rec->ip)));

	return ret;
}

int ftrace_make_nop(struct module *mod, struct dyn_ftrace *rec, unsigned long addr)
{
	/*
	 * This should never be called since we override ftrace_replace_code(),
	 * as well as ftrace_init_nop()
	 */
	WARN_ON(1);
	return -EINVAL;
}

void ftrace_replace_code(int enable)
{
	ppc_inst_t old, new, call_inst, new_call_inst;
	ppc_inst_t nop_inst = ppc_inst(PPC_RAW_NOP());
	unsigned long ip, new_addr, addr;
	struct ftrace_rec_iter *iter;
	struct dyn_ftrace *rec;
	int ret = 0, update;

	for_ftrace_rec_iter(iter) {
		rec = ftrace_rec_iter_record(iter);
		ip = rec->ip;

		if (rec->flags & FTRACE_FL_DISABLED && !(rec->flags & FTRACE_FL_ENABLED))
			continue;

		addr = ftrace_get_addr_curr(rec);
		new_addr = ftrace_get_addr_new(rec);
		update = ftrace_update_record(rec, enable);

		if (IS_ENABLED(CONFIG_PPC_FTRACE_OUT_OF_LINE) && update != FTRACE_UPDATE_IGNORE) {
			ip = ftrace_get_ool_stub(rec) + MCOUNT_INSN_SIZE;
			ret = ftrace_get_call_inst(rec, (unsigned long)ftrace_caller, &nop_inst);
			if (ret)
				goto out;
		}

		switch (update) {
		case FTRACE_UPDATE_IGNORE:
		default:
			continue;
		case FTRACE_UPDATE_MODIFY_CALL:
			ret = ftrace_get_call_inst(rec, new_addr, &new_call_inst);
			ret |= ftrace_get_call_inst(rec, addr, &call_inst);
			ret |= ftrace_rec_update_ops(rec);
			old = call_inst;
			new = new_call_inst;
			break;
		case FTRACE_UPDATE_MAKE_NOP:
			ret = ftrace_get_call_inst(rec, addr, &call_inst);
			ret |= ftrace_rec_set_nop_ops(rec);
			old = call_inst;
			new = nop_inst;
			break;
		case FTRACE_UPDATE_MAKE_CALL:
			ret = ftrace_get_call_inst(rec, new_addr, &call_inst);
			ret |= ftrace_rec_update_ops(rec);
			old = nop_inst;
			new = call_inst;
			break;
		}

		if (!ret)
			ret = ftrace_modify_code(ip, old, new);

		if (!ret && IS_ENABLED(CONFIG_PPC_FTRACE_OUT_OF_LINE) &&
		    (update == FTRACE_UPDATE_MAKE_NOP || update == FTRACE_UPDATE_MAKE_CALL)) {
			/* Update the actual ftrace location */
			call_inst = ppc_inst(PPC_RAW_BRANCH((long)ftrace_get_ool_stub(rec) -
							    (long)rec->ip));
			nop_inst = ppc_inst(PPC_RAW_NOP());
			ip = rec->ip;

			if (update == FTRACE_UPDATE_MAKE_NOP)
				ret = ftrace_modify_code(ip, call_inst, nop_inst);
			else
				ret = ftrace_modify_code(ip, nop_inst, call_inst);

			if (ret)
				goto out;
		}

		if (ret)
			goto out;
	}

out:
	if (ret)
		ftrace_bug(ret, rec);
	return;
}

int ftrace_init_nop(struct module *mod, struct dyn_ftrace *rec)
{
	unsigned long addr, ip = rec->ip;
	ppc_inst_t old, new;
	int ret = 0;

	/* Verify instructions surrounding the ftrace location */
	if (IS_ENABLED(CONFIG_ARCH_USING_PATCHABLE_FUNCTION_ENTRY)) {
		/* Expect nops */
		if (!IS_ENABLED(CONFIG_PPC_FTRACE_OUT_OF_LINE))
			ret = ftrace_validate_inst(ip - 4, ppc_inst(PPC_RAW_NOP()));
		if (!ret)
			ret = ftrace_validate_inst(ip, ppc_inst(PPC_RAW_NOP()));
	} else if (IS_ENABLED(CONFIG_PPC32)) {
		/* Expected sequence: 'mflr r0', 'stw r0,4(r1)', 'bl _mcount' */
		ret = ftrace_validate_inst(ip - 8, ppc_inst(PPC_RAW_MFLR(_R0)));
		if (ret)
			return ret;
		ret = ftrace_modify_code(ip - 4, ppc_inst(PPC_RAW_STW(_R0, _R1, 4)),
					 ppc_inst(PPC_RAW_NOP()));
	} else if (IS_ENABLED(CONFIG_MPROFILE_KERNEL)) {
		/* Expected sequence: 'mflr r0', ['std r0,16(r1)'], 'bl _mcount' */
		ret = ftrace_read_inst(ip - 4, &old);
		if (!ret && !ppc_inst_equal(old, ppc_inst(PPC_RAW_MFLR(_R0)))) {
			/* Gcc v5.x emit the additional 'std' instruction, gcc v6.x don't */
			ret = ftrace_validate_inst(ip - 8, ppc_inst(PPC_RAW_MFLR(_R0)));
			if (ret)
				return ret;
			ret = ftrace_modify_code(ip - 4, ppc_inst(PPC_RAW_STD(_R0, _R1, 16)),
						 ppc_inst(PPC_RAW_NOP()));
		}
	} else {
		return -EINVAL;
	}

	if (ret)
		return ret;

	/* Set up out-of-line stub */
	if (IS_ENABLED(CONFIG_PPC_FTRACE_OUT_OF_LINE))
		return ftrace_init_ool_stub(mod, rec);

	/* Nop-out the ftrace location */
	new = ppc_inst(PPC_RAW_NOP());
	addr = MCOUNT_ADDR;
	if (IS_ENABLED(CONFIG_ARCH_USING_PATCHABLE_FUNCTION_ENTRY)) {
		/* we instead patch-in the 'mflr r0' */
		old = ppc_inst(PPC_RAW_NOP());
		new = ppc_inst(PPC_RAW_MFLR(_R0));
		ret = ftrace_modify_code(ip - 4, old, new);
	} else if (is_offset_in_branch_range(addr - ip)) {
		/* Within range */
		old = ftrace_create_branch_inst(ip, addr, 1);
		ret = ftrace_modify_code(ip, old, new);
	} else if (core_kernel_text(ip) || (IS_ENABLED(CONFIG_MODULES) && mod)) {
		/*
		 * We would be branching to a linker-generated stub, or to the module _mcount
		 * stub. Let's just confirm we have a 'bl' here.
		 */
		ret = ftrace_read_inst(ip, &old);
		if (ret)
			return ret;
		if (!is_bl_op(old)) {
			pr_err("0x%lx: expected (bl) != found (%08lx)\n", ip, ppc_inst_as_ulong(old));
			return -EINVAL;
		}
		ret = patch_instruction((u32 *)ip, new);
	} else {
		return -EINVAL;
	}

	return ret;
}

int ftrace_update_ftrace_func(ftrace_func_t func)
{
	unsigned long ip = (unsigned long)(&ftrace_call);
	ppc_inst_t old, new;
	int ret;

	/*
	 * When using CALL_OPS, the function to call is associated with the
	 * call site, and we don't have a global function pointer to update.
	 */
	if (IS_ENABLED(CONFIG_DYNAMIC_FTRACE_WITH_CALL_OPS))
		return 0;

	old = ppc_inst_read((u32 *)&ftrace_call);
	new = ftrace_create_branch_inst(ip, ppc_function_entry(func), 1);
	ret = ftrace_modify_code(ip, old, new);

	/* Also update the regs callback function */
	if (IS_ENABLED(CONFIG_DYNAMIC_FTRACE_WITH_REGS) && !ret) {
		ip = (unsigned long)(&ftrace_regs_call);
		old = ppc_inst_read((u32 *)&ftrace_regs_call);
		new = ftrace_create_branch_inst(ip, ppc_function_entry(func), 1);
		ret = ftrace_modify_code(ip, old, new);
	}

	return ret;
}

/*
 * Use the default ftrace_modify_all_code, but without
 * stop_machine().
 */
void arch_ftrace_update_code(int command)
{
	ftrace_modify_all_code(command);
}

void ftrace_free_init_tramp(void)
{
	int i;

	for (i = 0; i < NUM_FTRACE_TRAMPS && ftrace_tramps[i]; i++)
		if (ftrace_tramps[i] == (unsigned long)ftrace_tramp_init) {
			ftrace_tramps[i] = 0;
			return;
		}
}

static void __init add_ftrace_tramp(unsigned long tramp)
{
	int i;

	for (i = 0; i < NUM_FTRACE_TRAMPS; i++)
		if (!ftrace_tramps[i]) {
			ftrace_tramps[i] = tramp;
			return;
		}
}

int __init ftrace_dyn_arch_init(void)
{
	unsigned int *tramp[] = { ftrace_tramp_text, ftrace_tramp_init };
	unsigned long addr = FTRACE_REGS_ADDR;
	long reladdr;
	int i;
	u32 stub_insns[] = {
#ifdef CONFIG_PPC_KERNEL_PCREL
		/* pla r12,addr */
		PPC_PREFIX_MLS | __PPC_PRFX_R(1),
		PPC_INST_PADDI | ___PPC_RT(_R12),
		PPC_RAW_MTCTR(_R12),
		PPC_RAW_BCTR()
#elif defined(CONFIG_PPC64)
		PPC_RAW_LD(_R12, _R13, offsetof(struct paca_struct, kernel_toc)),
		PPC_RAW_ADDIS(_R12, _R12, 0),
		PPC_RAW_ADDI(_R12, _R12, 0),
		PPC_RAW_MTCTR(_R12),
		PPC_RAW_BCTR()
#else
		PPC_RAW_LIS(_R12, 0),
		PPC_RAW_ADDI(_R12, _R12, 0),
		PPC_RAW_MTCTR(_R12),
		PPC_RAW_BCTR()
#endif
	};

	if (IS_ENABLED(CONFIG_PPC_KERNEL_PCREL)) {
		for (i = 0; i < 2; i++) {
			reladdr = addr - (unsigned long)tramp[i];

			if (reladdr >= (long)SZ_8G || reladdr < -(long)SZ_8G) {
				pr_err("Address of %ps out of range of pcrel address.\n",
					(void *)addr);
				return -1;
			}

			memcpy(tramp[i], stub_insns, sizeof(stub_insns));
			tramp[i][0] |= IMM_H18(reladdr);
			tramp[i][1] |= IMM_L(reladdr);
			add_ftrace_tramp((unsigned long)tramp[i]);
		}
	} else if (IS_ENABLED(CONFIG_PPC64)) {
		reladdr = addr - kernel_toc_addr();

		if (reladdr >= (long)SZ_2G || reladdr < -(long long)SZ_2G) {
			pr_err("Address of %ps out of range of kernel_toc.\n",
				(void *)addr);
			return -1;
		}

		for (i = 0; i < 2; i++) {
			memcpy(tramp[i], stub_insns, sizeof(stub_insns));
			tramp[i][1] |= PPC_HA(reladdr);
			tramp[i][2] |= PPC_LO(reladdr);
			add_ftrace_tramp((unsigned long)tramp[i]);
		}
	} else {
		for (i = 0; i < 2; i++) {
			memcpy(tramp[i], stub_insns, sizeof(stub_insns));
			tramp[i][0] |= PPC_HA(addr);
			tramp[i][1] |= PPC_LO(addr);
			add_ftrace_tramp((unsigned long)tramp[i]);
		}
	}

	return 0;
}

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
void ftrace_graph_func(unsigned long ip, unsigned long parent_ip,
		       struct ftrace_ops *op, struct ftrace_regs *fregs)
{
	unsigned long sp = arch_ftrace_regs(fregs)->regs.gpr[1];
	int bit;

	if (unlikely(ftrace_graph_is_dead()))
		goto out;

	if (unlikely(atomic_read(&current->tracing_graph_pause)))
		goto out;

	bit = ftrace_test_recursion_trylock(ip, parent_ip);
	if (bit < 0)
		goto out;

	if (!function_graph_enter(parent_ip, ip, 0, (unsigned long *)sp))
		parent_ip = ppc_function_entry(return_to_handler);

	ftrace_test_recursion_unlock(bit);
out:
	arch_ftrace_regs(fregs)->regs.link = parent_ip;
}
#endif /* CONFIG_FUNCTION_GRAPH_TRACER */
