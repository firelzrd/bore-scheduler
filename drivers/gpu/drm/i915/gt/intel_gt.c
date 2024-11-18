// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_gt.h"
#include "intel_gt_pm.h"
#include "intel_uncore.h"

void intel_gt_init_early(struct intel_gt *gt, struct drm_i915_private *i915)
{
	gt->i915 = i915;
	gt->uncore = &i915->uncore;

	spin_lock_init(&gt->irq_lock);

	mutex_init(&gt->tlb_invalidate_lock);

	INIT_LIST_HEAD(&gt->closed_vma);
	spin_lock_init(&gt->closed_lock);

	intel_gt_init_hangcheck(gt);
	intel_gt_init_reset(gt);
	intel_gt_pm_init_early(gt);
	intel_uc_init_early(&gt->uc);
}

void intel_gt_init_hw(struct drm_i915_private *i915)
{
	i915->gt.ggtt = &i915->ggtt;
}

static void rmw_set(struct intel_uncore *uncore, i915_reg_t reg, u32 set)
{
	intel_uncore_rmw(uncore, reg, 0, set);
}

static void rmw_clear(struct intel_uncore *uncore, i915_reg_t reg, u32 clr)
{
	intel_uncore_rmw(uncore, reg, clr, 0);
}

static void clear_register(struct intel_uncore *uncore, i915_reg_t reg)
{
	intel_uncore_rmw(uncore, reg, 0, 0);
}

static void gen8_clear_engine_error_register(struct intel_engine_cs *engine)
{
	GEN6_RING_FAULT_REG_RMW(engine, RING_FAULT_VALID, 0);
	GEN6_RING_FAULT_REG_POSTING_READ(engine);
}

void
intel_gt_clear_error_registers(struct intel_gt *gt,
			       intel_engine_mask_t engine_mask)
{
	struct drm_i915_private *i915 = gt->i915;
	struct intel_uncore *uncore = gt->uncore;
	u32 eir;

	if (!IS_GEN(i915, 2))
		clear_register(uncore, PGTBL_ER);

	if (INTEL_GEN(i915) < 4)
		clear_register(uncore, IPEIR(RENDER_RING_BASE));
	else
		clear_register(uncore, IPEIR_I965);

	clear_register(uncore, EIR);
	eir = intel_uncore_read(uncore, EIR);
	if (eir) {
		/*
		 * some errors might have become stuck,
		 * mask them.
		 */
		DRM_DEBUG_DRIVER("EIR stuck: 0x%08x, masking\n", eir);
		rmw_set(uncore, EMR, eir);
		intel_uncore_write(uncore, GEN2_IIR,
				   I915_MASTER_ERROR_INTERRUPT);
	}

	if (INTEL_GEN(i915) >= 12) {
		rmw_clear(uncore, GEN12_RING_FAULT_REG, RING_FAULT_VALID);
		intel_uncore_posting_read(uncore, GEN12_RING_FAULT_REG);
	} else if (INTEL_GEN(i915) >= 8) {
		rmw_clear(uncore, GEN8_RING_FAULT_REG, RING_FAULT_VALID);
		intel_uncore_posting_read(uncore, GEN8_RING_FAULT_REG);
	} else if (INTEL_GEN(i915) >= 6) {
		struct intel_engine_cs *engine;
		enum intel_engine_id id;

		for_each_engine_masked(engine, i915, engine_mask, id)
			gen8_clear_engine_error_register(engine);
	}
}

static void gen6_check_faults(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	u32 fault;

	for_each_engine(engine, gt->i915, id) {
		fault = GEN6_RING_FAULT_REG_READ(engine);
		if (fault & RING_FAULT_VALID) {
			DRM_DEBUG_DRIVER("Unexpected fault\n"
					 "\tAddr: 0x%08lx\n"
					 "\tAddress space: %s\n"
					 "\tSource ID: %d\n"
					 "\tType: %d\n",
					 fault & PAGE_MASK,
					 fault & RING_FAULT_GTTSEL_MASK ?
					 "GGTT" : "PPGTT",
					 RING_FAULT_SRCID(fault),
					 RING_FAULT_FAULT_TYPE(fault));
		}
	}
}

static void gen8_check_faults(struct intel_gt *gt)
{
	struct intel_uncore *uncore = gt->uncore;
	i915_reg_t fault_reg, fault_data0_reg, fault_data1_reg;
	u32 fault;

	if (INTEL_GEN(gt->i915) >= 12) {
		fault_reg = GEN12_RING_FAULT_REG;
		fault_data0_reg = GEN12_FAULT_TLB_DATA0;
		fault_data1_reg = GEN12_FAULT_TLB_DATA1;
	} else {
		fault_reg = GEN8_RING_FAULT_REG;
		fault_data0_reg = GEN8_FAULT_TLB_DATA0;
		fault_data1_reg = GEN8_FAULT_TLB_DATA1;
	}

	fault = intel_uncore_read(uncore, fault_reg);
	if (fault & RING_FAULT_VALID) {
		u32 fault_data0, fault_data1;
		u64 fault_addr;

		fault_data0 = intel_uncore_read(uncore, fault_data0_reg);
		fault_data1 = intel_uncore_read(uncore, fault_data1_reg);

		fault_addr = ((u64)(fault_data1 & FAULT_VA_HIGH_BITS) << 44) |
			     ((u64)fault_data0 << 12);

		DRM_DEBUG_DRIVER("Unexpected fault\n"
				 "\tAddr: 0x%08x_%08x\n"
				 "\tAddress space: %s\n"
				 "\tEngine ID: %d\n"
				 "\tSource ID: %d\n"
				 "\tType: %d\n",
				 upper_32_bits(fault_addr),
				 lower_32_bits(fault_addr),
				 fault_data1 & FAULT_GTT_SEL ? "GGTT" : "PPGTT",
				 GEN8_RING_FAULT_ENGINE_ID(fault),
				 RING_FAULT_SRCID(fault),
				 RING_FAULT_FAULT_TYPE(fault));
	}
}

void intel_gt_check_and_clear_faults(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;

	/* From GEN8 onwards we only have one 'All Engine Fault Register' */
	if (INTEL_GEN(i915) >= 8)
		gen8_check_faults(gt);
	else if (INTEL_GEN(i915) >= 6)
		gen6_check_faults(gt);
	else
		return;

	intel_gt_clear_error_registers(gt, ALL_ENGINES);
}

void intel_gt_flush_ggtt_writes(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	intel_wakeref_t wakeref;

	/*
	 * No actual flushing is required for the GTT write domain for reads
	 * from the GTT domain. Writes to it "immediately" go to main memory
	 * as far as we know, so there's no chipset flush. It also doesn't
	 * land in the GPU render cache.
	 *
	 * However, we do have to enforce the order so that all writes through
	 * the GTT land before any writes to the device, such as updates to
	 * the GATT itself.
	 *
	 * We also have to wait a bit for the writes to land from the GTT.
	 * An uncached read (i.e. mmio) seems to be ideal for the round-trip
	 * timing. This issue has only been observed when switching quickly
	 * between GTT writes and CPU reads from inside the kernel on recent hw,
	 * and it appears to only affect discrete GTT blocks (i.e. on LLC
	 * system agents we cannot reproduce this behaviour, until Cannonlake
	 * that was!).
	 */

	wmb();

	if (INTEL_INFO(i915)->has_coherent_ggtt)
		return;

	intel_gt_chipset_flush(gt);

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		struct intel_uncore *uncore = gt->uncore;

		spin_lock_irq(&uncore->lock);
		intel_uncore_posting_read_fw(uncore,
					     RING_HEAD(RENDER_RING_BASE));
		spin_unlock_irq(&uncore->lock);
	}
}

void intel_gt_chipset_flush(struct intel_gt *gt)
{
	wmb();
	if (INTEL_GEN(gt->i915) < 6)
		intel_gtt_chipset_flush();
}

int intel_gt_init_scratch(struct intel_gt *gt, unsigned int size)
{
	struct drm_i915_private *i915 = gt->i915;
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	int ret;

	obj = i915_gem_object_create_stolen(i915, size);
	if (!obj)
		obj = i915_gem_object_create_internal(i915, size);
	if (IS_ERR(obj)) {
		DRM_ERROR("Failed to allocate scratch page\n");
		return PTR_ERR(obj);
	}

	vma = i915_vma_instance(obj, &gt->ggtt->vm, NULL);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto err_unref;
	}

	ret = i915_vma_pin(vma, 0, 0, PIN_GLOBAL | PIN_HIGH);
	if (ret)
		goto err_unref;

	gt->scratch = i915_vma_make_unshrinkable(vma);

	return 0;

err_unref:
	i915_gem_object_put(obj);
	return ret;
}

void intel_gt_fini_scratch(struct intel_gt *gt)
{
	i915_vma_unpin_and_release(&gt->scratch, 0);
}

void intel_gt_driver_late_release(struct intel_gt *gt)
{
	intel_uc_driver_late_release(&gt->uc);
	intel_gt_fini_reset(gt);
}

struct reg_and_bit {
	i915_reg_t reg;
	u32 bit;
};

static struct reg_and_bit
get_reg_and_bit(const struct intel_engine_cs *engine, const bool gen8,
		const i915_reg_t *regs, const unsigned int num)
{
	const unsigned int class = engine->class;
	struct reg_and_bit rb = { };

	if (WARN_ON_ONCE(class >= num || !regs[class].reg))
		return rb;

	rb.reg = regs[class];
	if (gen8 && class == VIDEO_DECODE_CLASS)
		rb.reg.reg += 4 * engine->instance; /* GEN8_M2TCR */
	else
		rb.bit = engine->instance;

	rb.bit = BIT(rb.bit);

	return rb;
}

void intel_gt_invalidate_tlbs(struct intel_gt *gt)
{
	static const i915_reg_t gen8_regs[] = {
		[RENDER_CLASS]			= GEN8_RTCR,
		[VIDEO_DECODE_CLASS]		= GEN8_M1TCR, /* , GEN8_M2TCR */
		[VIDEO_ENHANCEMENT_CLASS]	= GEN8_VTCR,
		[COPY_ENGINE_CLASS]		= GEN8_BTCR,
	};
	static const i915_reg_t gen12_regs[] = {
		[RENDER_CLASS]			= GEN12_GFX_TLB_INV_CR,
		[VIDEO_DECODE_CLASS]		= GEN12_VD_TLB_INV_CR,
		[VIDEO_ENHANCEMENT_CLASS]	= GEN12_VE_TLB_INV_CR,
		[COPY_ENGINE_CLASS]		= GEN12_BLT_TLB_INV_CR,
	};
	struct drm_i915_private *i915 = gt->i915;
	struct intel_uncore *uncore = gt->uncore;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	const i915_reg_t *regs;
	unsigned int num = 0;

	if (I915_SELFTEST_ONLY(gt->awake == -ENODEV))
		return;

	if (INTEL_GEN(i915) == 12) {
		regs = gen12_regs;
		num = ARRAY_SIZE(gen12_regs);
	} else if (INTEL_GEN(i915) >= 8 && INTEL_GEN(i915) <= 11) {
		regs = gen8_regs;
		num = ARRAY_SIZE(gen8_regs);
	} else if (INTEL_GEN(i915) < 8) {
		return;
	}

	if (WARN_ONCE(!num, "Platform does not implement TLB invalidation!"))
		return;

	GEM_TRACE("\n");

	assert_rpm_wakelock_held(&i915->runtime_pm);

	mutex_lock(&gt->tlb_invalidate_lock);
	intel_uncore_forcewake_get(uncore, FORCEWAKE_ALL);

	spin_lock_irq(&uncore->lock); /* serialise invalidate with GT reset */

	for_each_engine(engine, gt, id) {
		struct reg_and_bit rb;

		rb = get_reg_and_bit(engine, regs == gen8_regs, regs, num);
		if (!i915_mmio_reg_offset(rb.reg))
			continue;

		if (INTEL_GEN(i915) == 12 && (engine->class == VIDEO_DECODE_CLASS ||
		    engine->class == VIDEO_ENHANCEMENT_CLASS))
			rb.bit = _MASKED_BIT_ENABLE(rb.bit);

		intel_uncore_write_fw(uncore, rb.reg, rb.bit);
	}

	spin_unlock_irq(&uncore->lock);

	for_each_engine(engine, gt, id) {
		/*
		 * HW architecture suggest typical invalidation time at 40us,
		 * with pessimistic cases up to 100us and a recommendation to
		 * cap at 1ms. We go a bit higher just in case.
		 */
		const unsigned int timeout_us = 100;
		const unsigned int timeout_ms = 4;
		struct reg_and_bit rb;

		rb = get_reg_and_bit(engine, regs == gen8_regs, regs, num);
		if (!i915_mmio_reg_offset(rb.reg))
			continue;

		if (__intel_wait_for_register_fw(uncore,
						 rb.reg, rb.bit, 0,
						 timeout_us, timeout_ms,
						 NULL))
			DRM_ERROR_RATELIMITED("%s TLB invalidation did not complete in %ums!\n",
					      engine->name, timeout_ms);
	}

	intel_uncore_forcewake_put(uncore, FORCEWAKE_ALL);
	mutex_unlock(&gt->tlb_invalidate_lock);
}
