// SPDX-License-Identifier: GPL-2.0-only
/*
 * Contains CPU specific errata definitions
 *
 * Copyright (C) 2014 ARM Ltd.
 */

#include <linux/arm-smccc.h>
#include <linux/psci.h>
#include <linux/types.h>
#include <linux/cpu.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/cpufeature.h>
#include <asm/smp_plat.h>
#include <asm/vectors.h>

static bool __maybe_unused
is_affected_midr_range(const struct arm64_cpu_capabilities *entry, int scope)
{
	const struct arm64_midr_revidr *fix;
	u32 midr = read_cpuid_id(), revidr;

	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());
	if (!is_midr_in_range(midr, &entry->midr_range))
		return false;

	midr &= MIDR_REVISION_MASK | MIDR_VARIANT_MASK;
	revidr = read_cpuid(REVIDR_EL1);
	for (fix = entry->fixed_revs; fix && fix->revidr_mask; fix++)
		if (midr == fix->midr_rv && (revidr & fix->revidr_mask))
			return false;

	return true;
}

static bool __maybe_unused
is_affected_midr_range_list(const struct arm64_cpu_capabilities *entry,
			    int scope)
{
	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());
	return is_midr_in_range_list(read_cpuid_id(), entry->midr_range_list);
}

static bool __maybe_unused
is_kryo_midr(const struct arm64_cpu_capabilities *entry, int scope)
{
	u32 model;

	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());

	model = read_cpuid_id();
	model &= MIDR_IMPLEMENTOR_MASK | (0xf00 << MIDR_PARTNUM_SHIFT) |
		 MIDR_ARCHITECTURE_MASK;

	return model == entry->midr_range.model;
}

static bool
has_mismatched_cache_type(const struct arm64_cpu_capabilities *entry,
			  int scope)
{
	u64 mask = arm64_ftr_reg_ctrel0.strict_mask;
	u64 sys = arm64_ftr_reg_ctrel0.sys_val & mask;
	u64 ctr_raw, ctr_real;

	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());

	/*
	 * We want to make sure that all the CPUs in the system expose
	 * a consistent CTR_EL0 to make sure that applications behaves
	 * correctly with migration.
	 *
	 * If a CPU has CTR_EL0.IDC but does not advertise it via CTR_EL0 :
	 *
	 * 1) It is safe if the system doesn't support IDC, as CPU anyway
	 *    reports IDC = 0, consistent with the rest.
	 *
	 * 2) If the system has IDC, it is still safe as we trap CTR_EL0
	 *    access on this CPU via the ARM64_HAS_CACHE_IDC capability.
	 *
	 * So, we need to make sure either the raw CTR_EL0 or the effective
	 * CTR_EL0 matches the system's copy to allow a secondary CPU to boot.
	 */
	ctr_raw = read_cpuid_cachetype() & mask;
	ctr_real = read_cpuid_effective_cachetype() & mask;

	return (ctr_real != sys) && (ctr_raw != sys);
}

static void
cpu_enable_trap_ctr_access(const struct arm64_cpu_capabilities *cap)
{
	u64 mask = arm64_ftr_reg_ctrel0.strict_mask;
	bool enable_uct_trap = false;

	/* Trap CTR_EL0 access on this CPU, only if it has a mismatch */
	if ((read_cpuid_cachetype() & mask) !=
	    (arm64_ftr_reg_ctrel0.sys_val & mask))
		enable_uct_trap = true;

	/* ... or if the system is affected by an erratum */
	if (cap->capability == ARM64_WORKAROUND_1542419)
		enable_uct_trap = true;

	if (enable_uct_trap)
		sysreg_clear_set(sctlr_el1, SCTLR_EL1_UCT, 0);
}

atomic_t arm64_el2_vector_last_slot = ATOMIC_INIT(-1);

#include <asm/mmu_context.h>
#include <asm/cacheflush.h>

DEFINE_PER_CPU_READ_MOSTLY(struct bp_hardening_data, bp_hardening_data);

#ifdef CONFIG_KVM_INDIRECT_VECTORS
extern char __smccc_workaround_1_smc_start[];
extern char __smccc_workaround_1_smc_end[];
extern char __smccc_workaround_3_smc_start[];
extern char __smccc_workaround_3_smc_end[];
extern char __spectre_bhb_loop_k8_start[];
extern char __spectre_bhb_loop_k8_end[];
extern char __spectre_bhb_loop_k24_start[];
extern char __spectre_bhb_loop_k24_end[];
extern char __spectre_bhb_loop_k32_start[];
extern char __spectre_bhb_loop_k32_end[];
extern char __spectre_bhb_clearbhb_start[];
extern char __spectre_bhb_clearbhb_end[];

static void __copy_hyp_vect_bpi(int slot, const char *hyp_vecs_start,
				const char *hyp_vecs_end)
{
	void *dst = lm_alias(__bp_harden_hyp_vecs_start + slot * SZ_2K);
	int i;

	for (i = 0; i < SZ_2K; i += 0x80)
		memcpy(dst + i, hyp_vecs_start, hyp_vecs_end - hyp_vecs_start);

	__flush_icache_range((uintptr_t)dst, (uintptr_t)dst + SZ_2K);
}

static DEFINE_RAW_SPINLOCK(bp_lock);
static void install_bp_hardening_cb(bp_hardening_cb_t fn,
				    const char *hyp_vecs_start,
				    const char *hyp_vecs_end)
{
	int cpu, slot = -1;

	/*
	 * detect_harden_bp_fw() passes NULL for the hyp_vecs start/end if
	 * we're a guest. Skip the hyp-vectors work.
	 */
	if (!hyp_vecs_start) {
		__this_cpu_write(bp_hardening_data.fn, fn);
		return;
	}

	raw_spin_lock(&bp_lock);
	for_each_possible_cpu(cpu) {
		if (per_cpu(bp_hardening_data.fn, cpu) == fn) {
			slot = per_cpu(bp_hardening_data.hyp_vectors_slot, cpu);
			break;
		}
	}

	if (slot == -1) {
		slot = atomic_inc_return(&arm64_el2_vector_last_slot);
		BUG_ON(slot >= BP_HARDEN_EL2_SLOTS);
		__copy_hyp_vect_bpi(slot, hyp_vecs_start, hyp_vecs_end);
	}

	if (fn != __this_cpu_read(bp_hardening_data.fn)) {
		__this_cpu_write(bp_hardening_data.hyp_vectors_slot, slot);
		__this_cpu_write(bp_hardening_data.fn, fn);
		__this_cpu_write(bp_hardening_data.template_start,
				 hyp_vecs_start);
	}
	raw_spin_unlock(&bp_lock);
}
#else
#define __smccc_workaround_1_smc_start		NULL
#define __smccc_workaround_1_smc_end		NULL

static void install_bp_hardening_cb(bp_hardening_cb_t fn,
				      const char *hyp_vecs_start,
				      const char *hyp_vecs_end)
{
	__this_cpu_write(bp_hardening_data.fn, fn);
}
#endif	/* CONFIG_KVM_INDIRECT_VECTORS */

#include <uapi/linux/psci.h>
#include <linux/arm-smccc.h>
#include <linux/psci.h>

static void call_smc_arch_workaround_1(void)
{
	arm_smccc_1_1_smc(ARM_SMCCC_ARCH_WORKAROUND_1, NULL);
}

static void call_hvc_arch_workaround_1(void)
{
	arm_smccc_1_1_hvc(ARM_SMCCC_ARCH_WORKAROUND_1, NULL);
}

static void qcom_link_stack_sanitization(void)
{
	u64 tmp;

	asm volatile("mov	%0, x30		\n"
		     ".rept	16		\n"
		     "bl	. + 4		\n"
		     ".endr			\n"
		     "mov	x30, %0		\n"
		     : "=&r" (tmp));
}

static bool __nospectre_v2;
static int __init parse_nospectre_v2(char *str)
{
	__nospectre_v2 = true;
	return 0;
}
early_param("nospectre_v2", parse_nospectre_v2);

/*
 * -1: No workaround
 *  0: No workaround required
 *  1: Workaround installed
 */
static int detect_harden_bp_fw(void)
{
	bp_hardening_cb_t cb;
	void *smccc_start, *smccc_end;
	struct arm_smccc_res res;
	u32 midr = read_cpuid_id();

	if (psci_ops.smccc_version == SMCCC_VERSION_1_0)
		return -1;

	switch (psci_ops.conduit) {
	case PSCI_CONDUIT_HVC:
		arm_smccc_1_1_hvc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_1, &res);
		switch ((int)res.a0) {
		case 1:
			/* Firmware says we're just fine */
			return 0;
		case 0:
			cb = call_hvc_arch_workaround_1;
			/* This is a guest, no need to patch KVM vectors */
			smccc_start = NULL;
			smccc_end = NULL;
			break;
		default:
			return -1;
		}
		break;

	case PSCI_CONDUIT_SMC:
		arm_smccc_1_1_smc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_1, &res);
		switch ((int)res.a0) {
		case 1:
			/* Firmware says we're just fine */
			return 0;
		case 0:
			cb = call_smc_arch_workaround_1;
			smccc_start = __smccc_workaround_1_smc_start;
			smccc_end = __smccc_workaround_1_smc_end;
			break;
		default:
			return -1;
		}
		break;

	default:
		return -1;
	}

	if (((midr & MIDR_CPU_MODEL_MASK) == MIDR_QCOM_FALKOR) ||
	    ((midr & MIDR_CPU_MODEL_MASK) == MIDR_QCOM_FALKOR_V1))
		cb = qcom_link_stack_sanitization;

	if (IS_ENABLED(CONFIG_HARDEN_BRANCH_PREDICTOR))
		install_bp_hardening_cb(cb, smccc_start, smccc_end);

	return 1;
}

DEFINE_PER_CPU_READ_MOSTLY(u64, arm64_ssbd_callback_required);

int ssbd_state __read_mostly = ARM64_SSBD_KERNEL;
static bool __ssb_safe = true;

static const struct ssbd_options {
	const char	*str;
	int		state;
} ssbd_options[] = {
	{ "force-on",	ARM64_SSBD_FORCE_ENABLE, },
	{ "force-off",	ARM64_SSBD_FORCE_DISABLE, },
	{ "kernel",	ARM64_SSBD_KERNEL, },
};

static int __init ssbd_cfg(char *buf)
{
	int i;

	if (!buf || !buf[0])
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ssbd_options); i++) {
		int len = strlen(ssbd_options[i].str);

		if (strncmp(buf, ssbd_options[i].str, len))
			continue;

		ssbd_state = ssbd_options[i].state;
		return 0;
	}

	return -EINVAL;
}
early_param("ssbd", ssbd_cfg);

void __init arm64_update_smccc_conduit(struct alt_instr *alt,
				       __le32 *origptr, __le32 *updptr,
				       int nr_inst)
{
	u32 insn;

	BUG_ON(nr_inst != 1);

	switch (psci_ops.conduit) {
	case PSCI_CONDUIT_HVC:
		insn = aarch64_insn_get_hvc_value();
		break;
	case PSCI_CONDUIT_SMC:
		insn = aarch64_insn_get_smc_value();
		break;
	default:
		return;
	}

	*updptr = cpu_to_le32(insn);
}

void __init arm64_enable_wa2_handling(struct alt_instr *alt,
				      __le32 *origptr, __le32 *updptr,
				      int nr_inst)
{
	BUG_ON(nr_inst != 1);
	/*
	 * Only allow mitigation on EL1 entry/exit and guest
	 * ARCH_WORKAROUND_2 handling if the SSBD state allows it to
	 * be flipped.
	 */
	if (arm64_get_ssbd_state() == ARM64_SSBD_KERNEL)
		*updptr = cpu_to_le32(aarch64_insn_gen_nop());
}

void arm64_set_ssbd_mitigation(bool state)
{
	if (!IS_ENABLED(CONFIG_ARM64_SSBD)) {
		pr_info_once("SSBD disabled by kernel configuration\n");
		return;
	}

	if (this_cpu_has_cap(ARM64_SSBS)) {
		if (state)
			asm volatile(SET_PSTATE_SSBS(0));
		else
			asm volatile(SET_PSTATE_SSBS(1));

		/*
		 * SSBS is self-synchronizing and is intended to affect
		 * subsequent speculative instructions, but some CPUs can
		 * speculate with a stale value of SSBS.
		 *
		 * Mitigate this with an unconditional speculation barrier, as
		 * CPUs could mis-speculate branches and bypass a conditional
		 * barrier.
		 */
		if (IS_ENABLED(CONFIG_ARM64_ERRATUM_3194386))
			spec_bar();

		return;
	}

	switch (psci_ops.conduit) {
	case PSCI_CONDUIT_HVC:
		arm_smccc_1_1_hvc(ARM_SMCCC_ARCH_WORKAROUND_2, state, NULL);
		break;

	case PSCI_CONDUIT_SMC:
		arm_smccc_1_1_smc(ARM_SMCCC_ARCH_WORKAROUND_2, state, NULL);
		break;

	default:
		WARN_ON_ONCE(1);
		break;
	}
}

static bool has_ssbd_mitigation(const struct arm64_cpu_capabilities *entry,
				    int scope)
{
	struct arm_smccc_res res;
	bool required = true;
	s32 val;
	bool this_cpu_safe = false;

	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());

	if (cpu_mitigations_off())
		ssbd_state = ARM64_SSBD_FORCE_DISABLE;

	/* delay setting __ssb_safe until we get a firmware response */
	if (is_midr_in_range_list(read_cpuid_id(), entry->midr_range_list))
		this_cpu_safe = true;

	if (this_cpu_has_cap(ARM64_SSBS)) {
		if (!this_cpu_safe)
			__ssb_safe = false;
		required = false;
		goto out_printmsg;
	}

	if (psci_ops.smccc_version == SMCCC_VERSION_1_0) {
		ssbd_state = ARM64_SSBD_UNKNOWN;
		if (!this_cpu_safe)
			__ssb_safe = false;
		return false;
	}

	switch (psci_ops.conduit) {
	case PSCI_CONDUIT_HVC:
		arm_smccc_1_1_hvc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_2, &res);
		break;

	case PSCI_CONDUIT_SMC:
		arm_smccc_1_1_smc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_2, &res);
		break;

	default:
		ssbd_state = ARM64_SSBD_UNKNOWN;
		if (!this_cpu_safe)
			__ssb_safe = false;
		return false;
	}

	val = (s32)res.a0;

	switch (val) {
	case SMCCC_RET_NOT_SUPPORTED:
		ssbd_state = ARM64_SSBD_UNKNOWN;
		if (!this_cpu_safe)
			__ssb_safe = false;
		return false;

	/* machines with mixed mitigation requirements must not return this */
	case SMCCC_RET_NOT_REQUIRED:
		pr_info_once("%s mitigation not required\n", entry->desc);
		ssbd_state = ARM64_SSBD_MITIGATED;
		return false;

	case SMCCC_RET_SUCCESS:
		__ssb_safe = false;
		required = true;
		break;

	case 1:	/* Mitigation not required on this CPU */
		required = false;
		break;

	default:
		WARN_ON(1);
		if (!this_cpu_safe)
			__ssb_safe = false;
		return false;
	}

	switch (ssbd_state) {
	case ARM64_SSBD_FORCE_DISABLE:
		arm64_set_ssbd_mitigation(false);
		required = false;
		break;

	case ARM64_SSBD_KERNEL:
		if (required) {
			__this_cpu_write(arm64_ssbd_callback_required, 1);
			arm64_set_ssbd_mitigation(true);
		}
		break;

	case ARM64_SSBD_FORCE_ENABLE:
		arm64_set_ssbd_mitigation(true);
		required = true;
		break;

	default:
		WARN_ON(1);
		break;
	}

out_printmsg:
	switch (ssbd_state) {
	case ARM64_SSBD_FORCE_DISABLE:
		pr_info_once("%s disabled from command-line\n", entry->desc);
		break;

	case ARM64_SSBD_FORCE_ENABLE:
		pr_info_once("%s forced from command-line\n", entry->desc);
		break;
	}

	return required;
}

static void cpu_enable_ssbd_mitigation(const struct arm64_cpu_capabilities *cap)
{
	if (ssbd_state != ARM64_SSBD_FORCE_DISABLE)
		cap->matches(cap, SCOPE_LOCAL_CPU);
}

/* known invulnerable cores */
static const struct midr_range arm64_ssb_cpus[] = {
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A35),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A53),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A55),
	MIDR_ALL_VERSIONS(MIDR_BRAHMA_B53),
	{},
};

#ifdef CONFIG_ARM64_ERRATUM_1463225
DEFINE_PER_CPU(int, __in_cortex_a76_erratum_1463225_wa);

static bool
has_cortex_a76_erratum_1463225(const struct arm64_cpu_capabilities *entry,
			       int scope)
{
	u32 midr = read_cpuid_id();
	/* Cortex-A76 r0p0 - r3p1 */
	struct midr_range range = MIDR_RANGE(MIDR_CORTEX_A76, 0, 0, 3, 1);

	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());
	return is_midr_in_range(midr, &range) && is_kernel_in_hyp_mode();
}
#endif

static void __maybe_unused
cpu_enable_cache_maint_trap(const struct arm64_cpu_capabilities *__unused)
{
	sysreg_clear_set(sctlr_el1, SCTLR_EL1_UCI, 0);
}

#define CAP_MIDR_RANGE(model, v_min, r_min, v_max, r_max)	\
	.matches = is_affected_midr_range,			\
	.midr_range = MIDR_RANGE(model, v_min, r_min, v_max, r_max)

#define CAP_MIDR_ALL_VERSIONS(model)					\
	.matches = is_affected_midr_range,				\
	.midr_range = MIDR_ALL_VERSIONS(model)

#define MIDR_FIXED(rev, revidr_mask) \
	.fixed_revs = (struct arm64_midr_revidr[]){{ (rev), (revidr_mask) }, {}}

#define ERRATA_MIDR_RANGE(model, v_min, r_min, v_max, r_max)		\
	.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,				\
	CAP_MIDR_RANGE(model, v_min, r_min, v_max, r_max)

#define CAP_MIDR_RANGE_LIST(list)				\
	.matches = is_affected_midr_range_list,			\
	.midr_range_list = list

/* Errata affecting a range of revisions of  given model variant */
#define ERRATA_MIDR_REV_RANGE(m, var, r_min, r_max)	 \
	ERRATA_MIDR_RANGE(m, var, r_min, var, r_max)

/* Errata affecting a single variant/revision of a model */
#define ERRATA_MIDR_REV(model, var, rev)	\
	ERRATA_MIDR_RANGE(model, var, rev, var, rev)

/* Errata affecting all variants/revisions of a given a model */
#define ERRATA_MIDR_ALL_VERSIONS(model)				\
	.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,			\
	CAP_MIDR_ALL_VERSIONS(model)

/* Errata affecting a list of midr ranges, with same work around */
#define ERRATA_MIDR_RANGE_LIST(midr_list)			\
	.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,			\
	CAP_MIDR_RANGE_LIST(midr_list)

/* Track overall mitigation state. We are only mitigated if all cores are ok */
static bool __hardenbp_enab = true;
static bool __spectrev2_safe = true;

int get_spectre_v2_workaround_state(void)
{
	if (__spectrev2_safe)
		return ARM64_BP_HARDEN_NOT_REQUIRED;

	if (!__hardenbp_enab)
		return ARM64_BP_HARDEN_UNKNOWN;

	return ARM64_BP_HARDEN_WA_NEEDED;
}

/*
 * List of CPUs that do not need any Spectre-v2 mitigation at all.
 */
static const struct midr_range spectre_v2_safe_list[] = {
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A35),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A53),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A55),
	MIDR_ALL_VERSIONS(MIDR_BRAHMA_B53),
	MIDR_ALL_VERSIONS(MIDR_HISI_TSV110),
	{ /* sentinel */ }
};

/*
 * Track overall bp hardening for all heterogeneous cores in the machine.
 * We are only considered "safe" if all booted cores are known safe.
 */
static bool __maybe_unused
check_branch_predictor(const struct arm64_cpu_capabilities *entry, int scope)
{
	int need_wa;

	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());

	/* If the CPU has CSV2 set, we're safe */
	if (cpuid_feature_extract_unsigned_field(read_cpuid(ID_AA64PFR0_EL1),
						 ID_AA64PFR0_CSV2_SHIFT))
		return false;

	/* Alternatively, we have a list of unaffected CPUs */
	if (is_midr_in_range_list(read_cpuid_id(), spectre_v2_safe_list))
		return false;

	/* Fallback to firmware detection */
	need_wa = detect_harden_bp_fw();
	if (!need_wa)
		return false;

	__spectrev2_safe = false;

	if (!IS_ENABLED(CONFIG_HARDEN_BRANCH_PREDICTOR)) {
		pr_warn_once("spectrev2 mitigation disabled by kernel configuration\n");
		__hardenbp_enab = false;
		return false;
	}

	/* forced off */
	if (__nospectre_v2 || cpu_mitigations_off()) {
		pr_info_once("spectrev2 mitigation disabled by command line option\n");
		__hardenbp_enab = false;
		return false;
	}

	if (need_wa < 0) {
		pr_warn_once("ARM_SMCCC_ARCH_WORKAROUND_1 missing from firmware\n");
		__hardenbp_enab = false;
	}

	return (need_wa > 0);
}

static void
cpu_enable_branch_predictor_hardening(const struct arm64_cpu_capabilities *cap)
{
	cap->matches(cap, SCOPE_LOCAL_CPU);
}

static const __maybe_unused struct midr_range tx2_family_cpus[] = {
	MIDR_ALL_VERSIONS(MIDR_BRCM_VULCAN),
	MIDR_ALL_VERSIONS(MIDR_CAVIUM_THUNDERX2),
	{},
};

static bool __maybe_unused
needs_tx2_tvm_workaround(const struct arm64_cpu_capabilities *entry,
			 int scope)
{
	int i;

	if (!is_affected_midr_range_list(entry, scope) ||
	    !is_hyp_mode_available())
		return false;

	for_each_possible_cpu(i) {
		if (MPIDR_AFFINITY_LEVEL(cpu_logical_map(i), 0) != 0)
			return true;
	}

	return false;
}

static bool __maybe_unused
has_neoverse_n1_erratum_1542419(const struct arm64_cpu_capabilities *entry,
				int scope)
{
	u32 midr = read_cpuid_id();
	bool has_dic = read_cpuid_cachetype() & BIT(CTR_DIC_SHIFT);
	const struct midr_range range = MIDR_ALL_VERSIONS(MIDR_NEOVERSE_N1);

	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());
	return is_midr_in_range(midr, &range) && has_dic;
}

#ifdef CONFIG_HARDEN_EL2_VECTORS

static const struct midr_range arm64_harden_el2_vectors[] = {
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A57),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A72),
	{},
};

#endif

#ifdef CONFIG_ARM64_WORKAROUND_REPEAT_TLBI
static const struct arm64_cpu_capabilities arm64_repeat_tlbi_list[] = {
#ifdef CONFIG_QCOM_FALKOR_ERRATUM_1009
	{
		ERRATA_MIDR_REV(MIDR_QCOM_FALKOR_V1, 0, 0)
	},
	{
		.midr_range.model = MIDR_QCOM_KRYO,
		.matches = is_kryo_midr,
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_1286807
	{
		ERRATA_MIDR_RANGE(MIDR_CORTEX_A76, 0, 0, 3, 0),
	},
#endif
	{},
};
#endif

#ifdef CONFIG_CAVIUM_ERRATUM_27456
const struct midr_range cavium_erratum_27456_cpus[] = {
	/* Cavium ThunderX, T88 pass 1.x - 2.1 */
	MIDR_RANGE(MIDR_THUNDERX, 0, 0, 1, 1),
	/* Cavium ThunderX, T81 pass 1.0 */
	MIDR_REV(MIDR_THUNDERX_81XX, 0, 0),
	{},
};
#endif

#ifdef CONFIG_CAVIUM_ERRATUM_30115
static const struct midr_range cavium_erratum_30115_cpus[] = {
	/* Cavium ThunderX, T88 pass 1.x - 2.2 */
	MIDR_RANGE(MIDR_THUNDERX, 0, 0, 1, 2),
	/* Cavium ThunderX, T81 pass 1.0 - 1.2 */
	MIDR_REV_RANGE(MIDR_THUNDERX_81XX, 0, 0, 2),
	/* Cavium ThunderX, T83 pass 1.0 */
	MIDR_REV(MIDR_THUNDERX_83XX, 0, 0),
	{},
};
#endif

#ifdef CONFIG_QCOM_FALKOR_ERRATUM_1003
static const struct arm64_cpu_capabilities qcom_erratum_1003_list[] = {
	{
		ERRATA_MIDR_REV(MIDR_QCOM_FALKOR_V1, 0, 0),
	},
	{
		.midr_range.model = MIDR_QCOM_KRYO,
		.matches = is_kryo_midr,
	},
	{},
};
#endif

#ifdef CONFIG_ARM64_WORKAROUND_CLEAN_CACHE
static const struct midr_range workaround_clean_cache[] = {
#if	defined(CONFIG_ARM64_ERRATUM_826319) || \
	defined(CONFIG_ARM64_ERRATUM_827319) || \
	defined(CONFIG_ARM64_ERRATUM_824069)
	/* Cortex-A53 r0p[012]: ARM errata 826319, 827319, 824069 */
	MIDR_REV_RANGE(MIDR_CORTEX_A53, 0, 0, 2),
#endif
#ifdef	CONFIG_ARM64_ERRATUM_819472
	/* Cortex-A53 r0p[01] : ARM errata 819472 */
	MIDR_REV_RANGE(MIDR_CORTEX_A53, 0, 0, 1),
#endif
	{},
};
#endif

#ifdef CONFIG_ARM64_ERRATUM_1418040
/*
 * - 1188873 affects r0p0 to r2p0
 * - 1418040 affects r0p0 to r3p1
 */
static const struct midr_range erratum_1418040_list[] = {
	/* Cortex-A76 r0p0 to r3p1 */
	MIDR_RANGE(MIDR_CORTEX_A76, 0, 0, 3, 1),
	/* Neoverse-N1 r0p0 to r3p1 */
	MIDR_RANGE(MIDR_NEOVERSE_N1, 0, 0, 3, 1),
	{},
};
#endif

#ifdef CONFIG_ARM64_ERRATUM_845719
static const struct midr_range erratum_845719_list[] = {
	/* Cortex-A53 r0p[01234] */
	MIDR_REV_RANGE(MIDR_CORTEX_A53, 0, 0, 4),
	/* Brahma-B53 r0p[0] */
	MIDR_REV(MIDR_BRAHMA_B53, 0, 0),
	{},
};
#endif

#ifdef CONFIG_ARM64_ERRATUM_843419
static const struct arm64_cpu_capabilities erratum_843419_list[] = {
	{
		/* Cortex-A53 r0p[01234] */
		.matches = is_affected_midr_range,
		ERRATA_MIDR_REV_RANGE(MIDR_CORTEX_A53, 0, 0, 4),
		MIDR_FIXED(0x4, BIT(8)),
	},
	{
		/* Brahma-B53 r0p[0] */
		.matches = is_affected_midr_range,
		ERRATA_MIDR_REV(MIDR_BRAHMA_B53, 0, 0),
	},
	{},
};
#endif

#ifdef CONFIG_ARM64_ERRATUM_1742098
static struct midr_range broken_aarch32_aes[] = {
	MIDR_RANGE(MIDR_CORTEX_A57, 0, 1, 0xf, 0xf),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A72),
	{},
};
#endif

#ifdef CONFIG_ARM64_ERRATUM_3194386
static const struct midr_range erratum_spec_ssbs_list[] = {
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A76),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A77),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A78),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A78C),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A710),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A715),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A720),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A725),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_X1),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_X1C),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_X2),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_X3),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_X4),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_X925),
	MIDR_ALL_VERSIONS(MIDR_NEOVERSE_N1),
	MIDR_ALL_VERSIONS(MIDR_NEOVERSE_N2),
	MIDR_ALL_VERSIONS(MIDR_NEOVERSE_N3),
	MIDR_ALL_VERSIONS(MIDR_NEOVERSE_V1),
	MIDR_ALL_VERSIONS(MIDR_NEOVERSE_V2),
	MIDR_ALL_VERSIONS(MIDR_NEOVERSE_V3),
	{}
};
#endif

const struct arm64_cpu_capabilities arm64_errata[] = {
#ifdef CONFIG_ARM64_WORKAROUND_CLEAN_CACHE
	{
		.desc = "ARM errata 826319, 827319, 824069, 819472",
		.capability = ARM64_WORKAROUND_CLEAN_CACHE,
		ERRATA_MIDR_RANGE_LIST(workaround_clean_cache),
		.cpu_enable = cpu_enable_cache_maint_trap,
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_832075
	{
	/* Cortex-A57 r0p0 - r1p2 */
		.desc = "ARM erratum 832075",
		.capability = ARM64_WORKAROUND_DEVICE_LOAD_ACQUIRE,
		ERRATA_MIDR_RANGE(MIDR_CORTEX_A57,
				  0, 0,
				  1, 2),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_834220
	{
	/* Cortex-A57 r0p0 - r1p2 */
		.desc = "ARM erratum 834220",
		.capability = ARM64_WORKAROUND_834220,
		ERRATA_MIDR_RANGE(MIDR_CORTEX_A57,
				  0, 0,
				  1, 2),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_843419
	{
		.desc = "ARM erratum 843419",
		.capability = ARM64_WORKAROUND_843419,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.matches = cpucap_multi_entry_cap_matches,
		.match_list = erratum_843419_list,
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_845719
	{
		.desc = "ARM erratum 845719",
		.capability = ARM64_WORKAROUND_845719,
		ERRATA_MIDR_RANGE_LIST(erratum_845719_list),
	},
#endif
#ifdef CONFIG_CAVIUM_ERRATUM_23154
	{
	/* Cavium ThunderX, pass 1.x */
		.desc = "Cavium erratum 23154",
		.capability = ARM64_WORKAROUND_CAVIUM_23154,
		ERRATA_MIDR_REV_RANGE(MIDR_THUNDERX, 0, 0, 1),
	},
#endif
#ifdef CONFIG_CAVIUM_ERRATUM_27456
	{
		.desc = "Cavium erratum 27456",
		.capability = ARM64_WORKAROUND_CAVIUM_27456,
		ERRATA_MIDR_RANGE_LIST(cavium_erratum_27456_cpus),
	},
#endif
#ifdef CONFIG_CAVIUM_ERRATUM_30115
	{
		.desc = "Cavium erratum 30115",
		.capability = ARM64_WORKAROUND_CAVIUM_30115,
		ERRATA_MIDR_RANGE_LIST(cavium_erratum_30115_cpus),
	},
#endif
	{
		.desc = "Mismatched cache type (CTR_EL0)",
		.capability = ARM64_MISMATCHED_CACHE_TYPE,
		.matches = has_mismatched_cache_type,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.cpu_enable = cpu_enable_trap_ctr_access,
	},
#ifdef CONFIG_QCOM_FALKOR_ERRATUM_1003
	{
		.desc = "Qualcomm Technologies Falkor/Kryo erratum 1003",
		.capability = ARM64_WORKAROUND_QCOM_FALKOR_E1003,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.matches = cpucap_multi_entry_cap_matches,
		.match_list = qcom_erratum_1003_list,
	},
#endif
#ifdef CONFIG_ARM64_WORKAROUND_REPEAT_TLBI
	{
		.desc = "Qualcomm erratum 1009, ARM erratum 1286807",
		.capability = ARM64_WORKAROUND_REPEAT_TLBI,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.matches = cpucap_multi_entry_cap_matches,
		.match_list = arm64_repeat_tlbi_list,
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_858921
	{
	/* Cortex-A73 all versions */
		.desc = "ARM erratum 858921",
		.capability = ARM64_WORKAROUND_858921,
		ERRATA_MIDR_ALL_VERSIONS(MIDR_CORTEX_A73),
	},
#endif
	{
		.desc = "Branch predictor hardening",
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.matches = check_branch_predictor,
		.cpu_enable = cpu_enable_branch_predictor_hardening,
	},
#ifdef CONFIG_HARDEN_EL2_VECTORS
	{
		.desc = "EL2 vector hardening",
		.capability = ARM64_HARDEN_EL2_VECTORS,
		ERRATA_MIDR_RANGE_LIST(arm64_harden_el2_vectors),
	},
#endif
	{
		.desc = "Speculative Store Bypass Disable",
		.capability = ARM64_SSBD,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.matches = has_ssbd_mitigation,
		.cpu_enable = cpu_enable_ssbd_mitigation,
		.midr_range_list = arm64_ssb_cpus,
	},
	{
		.desc = "Spectre-BHB",
		.capability = ARM64_SPECTRE_BHB,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.matches = is_spectre_bhb_affected,
		.cpu_enable = spectre_bhb_enable_mitigation,
	},
#ifdef CONFIG_ARM64_ERRATUM_1418040
	{
		.desc = "ARM erratum 1418040",
		.capability = ARM64_WORKAROUND_1418040,
		ERRATA_MIDR_RANGE_LIST(erratum_1418040_list),
		/*
		 * We need to allow affected CPUs to come in late, but
		 * also need the non-affected CPUs to be able to come
		 * in at any point in time. Wonderful.
		 */
		.type = ARM64_CPUCAP_WEAK_LOCAL_CPU_FEATURE,
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_1165522
	{
		/* Cortex-A76 r0p0 to r2p0 */
		.desc = "ARM erratum 1165522",
		.capability = ARM64_WORKAROUND_1165522,
		ERRATA_MIDR_RANGE(MIDR_CORTEX_A76, 0, 0, 2, 0),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_1463225
	{
		.desc = "ARM erratum 1463225",
		.capability = ARM64_WORKAROUND_1463225,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.matches = has_cortex_a76_erratum_1463225,
	},
#endif
#ifdef CONFIG_CAVIUM_TX2_ERRATUM_219
	{
		.desc = "Cavium ThunderX2 erratum 219 (KVM guest sysreg trapping)",
		.capability = ARM64_WORKAROUND_CAVIUM_TX2_219_TVM,
		ERRATA_MIDR_RANGE_LIST(tx2_family_cpus),
		.matches = needs_tx2_tvm_workaround,
	},
	{
		.desc = "Cavium ThunderX2 erratum 219 (PRFM removal)",
		.capability = ARM64_WORKAROUND_CAVIUM_TX2_219_PRFM,
		ERRATA_MIDR_RANGE_LIST(tx2_family_cpus),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_1542419
	{
		/* we depend on the firmware portion for correctness */
		.desc = "ARM erratum 1542419 (kernel portion)",
		.capability = ARM64_WORKAROUND_1542419,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.matches = has_neoverse_n1_erratum_1542419,
		.cpu_enable = cpu_enable_trap_ctr_access,
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_1742098
	{
		.desc = "ARM erratum 1742098",
		.capability = ARM64_WORKAROUND_1742098,
		CAP_MIDR_RANGE_LIST(broken_aarch32_aes),
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_3194386
	{
		.desc = "SSBS not fully self-synchronizing",
		.capability = ARM64_WORKAROUND_SPECULATIVE_SSBS,
		ERRATA_MIDR_RANGE_LIST(erratum_spec_ssbs_list),
	},
#endif
	{
	}
};

ssize_t cpu_show_spectre_v1(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sprintf(buf, "Mitigation: __user pointer sanitization\n");
}

static const char *get_bhb_affected_string(enum mitigation_state bhb_state)
{
	switch (bhb_state) {
	case SPECTRE_UNAFFECTED:
		return "";
	default:
	case SPECTRE_VULNERABLE:
		return ", but not BHB";
	case SPECTRE_MITIGATED:
		return ", BHB";
	}
}

ssize_t cpu_show_spectre_v2(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	enum mitigation_state bhb_state = arm64_get_spectre_bhb_state();
	const char *bhb_str = get_bhb_affected_string(bhb_state);
	const char *v2_str = "Branch predictor hardening";

	switch (get_spectre_v2_workaround_state()) {
	case ARM64_BP_HARDEN_NOT_REQUIRED:
		if (bhb_state == SPECTRE_UNAFFECTED)
			return sprintf(buf, "Not affected\n");

		/*
		 * Platforms affected by Spectre-BHB can't report
		 * "Not affected" for Spectre-v2.
		 */
		v2_str = "CSV2";
		fallthrough;
	case ARM64_BP_HARDEN_WA_NEEDED:
		return sprintf(buf, "Mitigation: %s%s\n", v2_str, bhb_str);
	case ARM64_BP_HARDEN_UNKNOWN:
		fallthrough;
	default:
		return sprintf(buf, "Vulnerable\n");
	}
}

ssize_t cpu_show_spec_store_bypass(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	if (__ssb_safe)
		return sprintf(buf, "Not affected\n");

	switch (ssbd_state) {
	case ARM64_SSBD_KERNEL:
	case ARM64_SSBD_FORCE_ENABLE:
		if (IS_ENABLED(CONFIG_ARM64_SSBD))
			return sprintf(buf,
			    "Mitigation: Speculative Store Bypass disabled via prctl\n");
	}

	return sprintf(buf, "Vulnerable\n");
}

/*
 * We try to ensure that the mitigation state can never change as the result of
 * onlining a late CPU.
 */
static void update_mitigation_state(enum mitigation_state *oldp,
				    enum mitigation_state new)
{
	enum mitigation_state state;

	do {
		state = READ_ONCE(*oldp);
		if (new <= state)
			break;
	} while (cmpxchg_relaxed(oldp, state, new) != state);
}

/*
 * Spectre BHB.
 *
 * A CPU is either:
 * - Mitigated by a branchy loop a CPU specific number of times, and listed
 *   in our "loop mitigated list".
 * - Mitigated in software by the firmware Spectre v2 call.
 * - Has the ClearBHB instruction to perform the mitigation.
 * - Has the 'Exception Clears Branch History Buffer' (ECBHB) feature, so no
 *   software mitigation in the vectors is needed.
 * - Has CSV2.3, so is unaffected.
 */
static enum mitigation_state spectre_bhb_state;

enum mitigation_state arm64_get_spectre_bhb_state(void)
{
	return spectre_bhb_state;
}

/*
 * This must be called with SCOPE_LOCAL_CPU for each type of CPU, before any
 * SCOPE_SYSTEM call will give the right answer.
 */
u8 spectre_bhb_loop_affected(int scope)
{
	u8 k = 0;
	static u8 max_bhb_k;

	if (scope == SCOPE_LOCAL_CPU) {
		static const struct midr_range spectre_bhb_k32_list[] = {
			MIDR_ALL_VERSIONS(MIDR_CORTEX_A78),
			MIDR_ALL_VERSIONS(MIDR_CORTEX_A78C),
			MIDR_ALL_VERSIONS(MIDR_CORTEX_X1),
			MIDR_ALL_VERSIONS(MIDR_CORTEX_A710),
			MIDR_ALL_VERSIONS(MIDR_CORTEX_X2),
			MIDR_ALL_VERSIONS(MIDR_NEOVERSE_N2),
			MIDR_ALL_VERSIONS(MIDR_NEOVERSE_V1),
			{},
		};
		static const struct midr_range spectre_bhb_k24_list[] = {
			MIDR_ALL_VERSIONS(MIDR_CORTEX_A76),
			MIDR_ALL_VERSIONS(MIDR_CORTEX_A77),
			MIDR_ALL_VERSIONS(MIDR_NEOVERSE_N1),
			{},
		};
		static const struct midr_range spectre_bhb_k11_list[] = {
			MIDR_ALL_VERSIONS(MIDR_AMPERE1),
			{},
		};
		static const struct midr_range spectre_bhb_k8_list[] = {
			MIDR_ALL_VERSIONS(MIDR_CORTEX_A72),
			MIDR_ALL_VERSIONS(MIDR_CORTEX_A57),
			{},
		};

		if (is_midr_in_range_list(read_cpuid_id(), spectre_bhb_k32_list))
			k = 32;
		else if (is_midr_in_range_list(read_cpuid_id(), spectre_bhb_k24_list))
			k = 24;
		else if (is_midr_in_range_list(read_cpuid_id(), spectre_bhb_k11_list))
			k = 11;
		else if (is_midr_in_range_list(read_cpuid_id(), spectre_bhb_k8_list))
			k =  8;

		max_bhb_k = max(max_bhb_k, k);
	} else {
		k = max_bhb_k;
	}

	return k;
}

static enum mitigation_state spectre_bhb_get_cpu_fw_mitigation_state(void)
{
	int ret;
	struct arm_smccc_res res;

	if (psci_ops.smccc_version == SMCCC_VERSION_1_0)
		return SPECTRE_VULNERABLE;

	switch (psci_ops.conduit) {
	case PSCI_CONDUIT_HVC:
		arm_smccc_1_1_hvc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_3, &res);
		break;

	case PSCI_CONDUIT_SMC:
		arm_smccc_1_1_smc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_3, &res);
		break;

	default:
		return SPECTRE_VULNERABLE;
	}

	ret = res.a0;
	switch (ret) {
	case SMCCC_RET_SUCCESS:
		return SPECTRE_MITIGATED;
	case SMCCC_ARCH_WORKAROUND_RET_UNAFFECTED:
		return SPECTRE_UNAFFECTED;
	default:
		fallthrough;
	case SMCCC_RET_NOT_SUPPORTED:
		return SPECTRE_VULNERABLE;
	}
}

static bool is_spectre_bhb_fw_affected(int scope)
{
	static bool system_affected;
	enum mitigation_state fw_state;
	bool has_smccc = (psci_ops.smccc_version >= SMCCC_VERSION_1_1);
	static const struct midr_range spectre_bhb_firmware_mitigated_list[] = {
		MIDR_ALL_VERSIONS(MIDR_CORTEX_A73),
		MIDR_ALL_VERSIONS(MIDR_CORTEX_A75),
		{},
	};
	bool cpu_in_list = is_midr_in_range_list(read_cpuid_id(),
					 spectre_bhb_firmware_mitigated_list);

	if (scope != SCOPE_LOCAL_CPU)
		return system_affected;

	fw_state = spectre_bhb_get_cpu_fw_mitigation_state();
	if (cpu_in_list || (has_smccc && fw_state == SPECTRE_MITIGATED)) {
		system_affected = true;
		return true;
	}

	return false;
}

static bool supports_ecbhb(int scope)
{
	u64 mmfr1;

	if (scope == SCOPE_LOCAL_CPU)
		mmfr1 = read_sysreg_s(SYS_ID_AA64MMFR1_EL1);
	else
		mmfr1 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR1_EL1);

	return cpuid_feature_extract_unsigned_field(mmfr1,
						    ID_AA64MMFR1_ECBHB_SHIFT);
}

bool is_spectre_bhb_affected(const struct arm64_cpu_capabilities *entry,
			     int scope)
{
	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());

	if (supports_csv2p3(scope))
		return false;

	if (supports_clearbhb(scope))
		return true;

	if (spectre_bhb_loop_affected(scope))
		return true;

	if (is_spectre_bhb_fw_affected(scope))
		return true;

	return false;
}

static void this_cpu_set_vectors(enum arm64_bp_harden_el1_vectors slot)
{
	const char *v = arm64_get_bp_hardening_vector(slot);

	if (slot < 0)
		return;

	__this_cpu_write(this_cpu_vector, v);

	/*
	 * When KPTI is in use, the vectors are switched when exiting to
	 * user-space.
	 */
	if (arm64_kernel_unmapped_at_el0())
		return;

	write_sysreg(v, vbar_el1);
	isb();
}

#ifdef CONFIG_KVM_INDIRECT_VECTORS
static const char *kvm_bhb_get_vecs_end(const char *start)
{
	if (start == __smccc_workaround_3_smc_start)
		return __smccc_workaround_3_smc_end;
	else if (start == __spectre_bhb_loop_k8_start)
		return __spectre_bhb_loop_k8_end;
	else if (start == __spectre_bhb_loop_k24_start)
		return __spectre_bhb_loop_k24_end;
	else if (start == __spectre_bhb_loop_k32_start)
		return __spectre_bhb_loop_k32_end;
	else if (start == __spectre_bhb_clearbhb_start)
		return __spectre_bhb_clearbhb_end;

	return NULL;
}

static void kvm_setup_bhb_slot(const char *hyp_vecs_start)
{
	int cpu, slot = -1;
	const char *hyp_vecs_end;

	if (!IS_ENABLED(CONFIG_KVM) || !is_hyp_mode_available())
		return;

	hyp_vecs_end = kvm_bhb_get_vecs_end(hyp_vecs_start);
	if (WARN_ON_ONCE(!hyp_vecs_start || !hyp_vecs_end))
		return;

	raw_spin_lock(&bp_lock);
	for_each_possible_cpu(cpu) {
		if (per_cpu(bp_hardening_data.template_start, cpu) == hyp_vecs_start) {
			slot = per_cpu(bp_hardening_data.hyp_vectors_slot, cpu);
			break;
		}
	}

	if (slot == -1) {
		slot = atomic_inc_return(&arm64_el2_vector_last_slot);
		BUG_ON(slot >= BP_HARDEN_EL2_SLOTS);
		__copy_hyp_vect_bpi(slot, hyp_vecs_start, hyp_vecs_end);
	}

	if (hyp_vecs_start != __this_cpu_read(bp_hardening_data.template_start)) {
		__this_cpu_write(bp_hardening_data.hyp_vectors_slot, slot);
		__this_cpu_write(bp_hardening_data.template_start,
				 hyp_vecs_start);
	}
	raw_spin_unlock(&bp_lock);
}
#else
#define __smccc_workaround_3_smc_start NULL
#define __spectre_bhb_loop_k8_start NULL
#define __spectre_bhb_loop_k24_start NULL
#define __spectre_bhb_loop_k32_start NULL
#define __spectre_bhb_clearbhb_start NULL

static void kvm_setup_bhb_slot(const char *hyp_vecs_start) { }
#endif

void spectre_bhb_enable_mitigation(const struct arm64_cpu_capabilities *entry)
{
	enum mitigation_state fw_state, state = SPECTRE_VULNERABLE;

	if (!is_spectre_bhb_affected(entry, SCOPE_LOCAL_CPU))
		return;

	if (get_spectre_v2_workaround_state() == ARM64_BP_HARDEN_UNKNOWN) {
		/* No point mitigating Spectre-BHB alone. */
	} else if (!IS_ENABLED(CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY)) {
		pr_info_once("spectre-bhb mitigation disabled by compile time option\n");
	} else if (cpu_mitigations_off()) {
		pr_info_once("spectre-bhb mitigation disabled by command line option\n");
	} else if (supports_ecbhb(SCOPE_LOCAL_CPU)) {
		state = SPECTRE_MITIGATED;
	} else if (supports_clearbhb(SCOPE_LOCAL_CPU)) {
		kvm_setup_bhb_slot(__spectre_bhb_clearbhb_start);
		this_cpu_set_vectors(EL1_VECTOR_BHB_CLEAR_INSN);

		state = SPECTRE_MITIGATED;
	} else if (spectre_bhb_loop_affected(SCOPE_LOCAL_CPU)) {
		switch (spectre_bhb_loop_affected(SCOPE_SYSTEM)) {
		case 8:
			/*
			 * A57/A72-r0 will already have selected the
			 * spectre-indirect vector, which is sufficient
			 * for BHB too.
			 */
			if (!__this_cpu_read(bp_hardening_data.fn))
				kvm_setup_bhb_slot(__spectre_bhb_loop_k8_start);
			break;
		case 24:
			kvm_setup_bhb_slot(__spectre_bhb_loop_k24_start);
			break;
		case 32:
			kvm_setup_bhb_slot(__spectre_bhb_loop_k32_start);
			break;
		default:
			WARN_ON_ONCE(1);
		}
		this_cpu_set_vectors(EL1_VECTOR_BHB_LOOP);

		state = SPECTRE_MITIGATED;
	} else if (is_spectre_bhb_fw_affected(SCOPE_LOCAL_CPU)) {
		fw_state = spectre_bhb_get_cpu_fw_mitigation_state();
		if (fw_state == SPECTRE_MITIGATED) {
			kvm_setup_bhb_slot(__smccc_workaround_3_smc_start);
			this_cpu_set_vectors(EL1_VECTOR_BHB_FW);

			/*
			 * With WA3 in the vectors, the WA1 calls can be
			 * removed.
			 */
			__this_cpu_write(bp_hardening_data.fn, NULL);

			state = SPECTRE_MITIGATED;
		}
	}

	update_mitigation_state(&spectre_bhb_state, state);
}

/* Patched to correct the immediate */
void noinstr spectre_bhb_patch_loop_iter(struct alt_instr *alt,
				   __le32 *origptr, __le32 *updptr, int nr_inst)
{
	u8 rd;
	u32 insn;
	u16 loop_count = spectre_bhb_loop_affected(SCOPE_SYSTEM);

	BUG_ON(nr_inst != 1); /* MOV -> MOV */

	if (!IS_ENABLED(CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY))
		return;

	insn = le32_to_cpu(*origptr);
	rd = aarch64_insn_decode_register(AARCH64_INSN_REGTYPE_RD, insn);
	insn = aarch64_insn_gen_movewide(rd, loop_count, 0,
					 AARCH64_INSN_VARIANT_64BIT,
					 AARCH64_INSN_MOVEWIDE_ZERO);
	*updptr++ = cpu_to_le32(insn);
}
