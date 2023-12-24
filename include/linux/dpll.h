/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright (c) 2023 Meta Platforms, Inc. and affiliates
 *  Copyright (c) 2023 Intel and affiliates
 */

#ifndef __DPLL_H__
#define __DPLL_H__

#include <uapi/linux/dpll.h>
#include <linux/device.h>
#include <linux/netlink.h>

struct dpll_device;
struct dpll_pin;

struct dpll_device_ops {
	int (*mode_get)(const struct dpll_device *dpll, void *dpll_priv,
			enum dpll_mode *mode, struct netlink_ext_ack *extack);
	bool (*mode_supported)(const struct dpll_device *dpll, void *dpll_priv,
			       const enum dpll_mode mode,
			       struct netlink_ext_ack *extack);
	int (*lock_status_get)(const struct dpll_device *dpll, void *dpll_priv,
			       enum dpll_lock_status *status,
			       struct netlink_ext_ack *extack);
	int (*temp_get)(const struct dpll_device *dpll, void *dpll_priv,
			s32 *temp, struct netlink_ext_ack *extack);
};

struct dpll_pin_ops {
	int (*frequency_set)(const struct dpll_pin *pin, void *pin_priv,
			     const struct dpll_device *dpll, void *dpll_priv,
			     const u64 frequency,
			     struct netlink_ext_ack *extack);
	int (*frequency_get)(const struct dpll_pin *pin, void *pin_priv,
			     const struct dpll_device *dpll, void *dpll_priv,
			     u64 *frequency, struct netlink_ext_ack *extack);
	int (*direction_set)(const struct dpll_pin *pin, void *pin_priv,
			     const struct dpll_device *dpll, void *dpll_priv,
			     const enum dpll_pin_direction direction,
			     struct netlink_ext_ack *extack);
	int (*direction_get)(const struct dpll_pin *pin, void *pin_priv,
			     const struct dpll_device *dpll, void *dpll_priv,
			     enum dpll_pin_direction *direction,
			     struct netlink_ext_ack *extack);
	int (*state_on_pin_get)(const struct dpll_pin *pin, void *pin_priv,
				const struct dpll_pin *parent_pin,
				void *parent_pin_priv,
				enum dpll_pin_state *state,
				struct netlink_ext_ack *extack);
	int (*state_on_dpll_get)(const struct dpll_pin *pin, void *pin_priv,
				 const struct dpll_device *dpll,
				 void *dpll_priv, enum dpll_pin_state *state,
				 struct netlink_ext_ack *extack);
	int (*state_on_pin_set)(const struct dpll_pin *pin, void *pin_priv,
				const struct dpll_pin *parent_pin,
				void *parent_pin_priv,
				const enum dpll_pin_state state,
				struct netlink_ext_ack *extack);
	int (*state_on_dpll_set)(const struct dpll_pin *pin, void *pin_priv,
				 const struct dpll_device *dpll,
				 void *dpll_priv,
				 const enum dpll_pin_state state,
				 struct netlink_ext_ack *extack);
	int (*prio_get)(const struct dpll_pin *pin,  void *pin_priv,
			const struct dpll_device *dpll,  void *dpll_priv,
			u32 *prio, struct netlink_ext_ack *extack);
	int (*prio_set)(const struct dpll_pin *pin, void *pin_priv,
			const struct dpll_device *dpll, void *dpll_priv,
			const u32 prio, struct netlink_ext_ack *extack);
	int (*phase_offset_get)(const struct dpll_pin *pin, void *pin_priv,
				const struct dpll_device *dpll, void *dpll_priv,
				s64 *phase_offset,
				struct netlink_ext_ack *extack);
	int (*phase_adjust_get)(const struct dpll_pin *pin, void *pin_priv,
				const struct dpll_device *dpll, void *dpll_priv,
				s32 *phase_adjust,
				struct netlink_ext_ack *extack);
	int (*phase_adjust_set)(const struct dpll_pin *pin, void *pin_priv,
				const struct dpll_device *dpll, void *dpll_priv,
				const s32 phase_adjust,
				struct netlink_ext_ack *extack);
};

struct dpll_pin_frequency {
	u64 min;
	u64 max;
};

#define DPLL_PIN_FREQUENCY_RANGE(_min, _max)	\
	{					\
		.min = _min,			\
		.max = _max,			\
	}

#define DPLL_PIN_FREQUENCY(_val) DPLL_PIN_FREQUENCY_RANGE(_val, _val)
#define DPLL_PIN_FREQUENCY_1PPS \
	DPLL_PIN_FREQUENCY(DPLL_PIN_FREQUENCY_1_HZ)
#define DPLL_PIN_FREQUENCY_10MHZ \
	DPLL_PIN_FREQUENCY(DPLL_PIN_FREQUENCY_10_MHZ)
#define DPLL_PIN_FREQUENCY_IRIG_B \
	DPLL_PIN_FREQUENCY(DPLL_PIN_FREQUENCY_10_KHZ)
#define DPLL_PIN_FREQUENCY_DCF77 \
	DPLL_PIN_FREQUENCY(DPLL_PIN_FREQUENCY_77_5_KHZ)

struct dpll_pin_phase_adjust_range {
	s32 min;
	s32 max;
};

struct dpll_pin_properties {
	const char *board_label;
	const char *panel_label;
	const char *package_label;
	enum dpll_pin_type type;
	unsigned long capabilities;
	u32 freq_supported_num;
	struct dpll_pin_frequency *freq_supported;
	struct dpll_pin_phase_adjust_range phase_range;
};

#if IS_ENABLED(CONFIG_DPLL)
size_t dpll_msg_pin_handle_size(struct dpll_pin *pin);
int dpll_msg_add_pin_handle(struct sk_buff *msg, struct dpll_pin *pin);
#else
static inline size_t dpll_msg_pin_handle_size(struct dpll_pin *pin)
{
	return 0;
}

static inline int dpll_msg_add_pin_handle(struct sk_buff *msg, struct dpll_pin *pin)
{
	return 0;
}
#endif

struct dpll_device *
dpll_device_get(u64 clock_id, u32 dev_driver_id, struct module *module);

void dpll_device_put(struct dpll_device *dpll);

int dpll_device_register(struct dpll_device *dpll, enum dpll_type type,
			 const struct dpll_device_ops *ops, void *priv);

void dpll_device_unregister(struct dpll_device *dpll,
			    const struct dpll_device_ops *ops, void *priv);

struct dpll_pin *
dpll_pin_get(u64 clock_id, u32 dev_driver_id, struct module *module,
	     const struct dpll_pin_properties *prop);

int dpll_pin_register(struct dpll_device *dpll, struct dpll_pin *pin,
		      const struct dpll_pin_ops *ops, void *priv);

void dpll_pin_unregister(struct dpll_device *dpll, struct dpll_pin *pin,
			 const struct dpll_pin_ops *ops, void *priv);

void dpll_pin_put(struct dpll_pin *pin);

int dpll_pin_on_pin_register(struct dpll_pin *parent, struct dpll_pin *pin,
			     const struct dpll_pin_ops *ops, void *priv);

void dpll_pin_on_pin_unregister(struct dpll_pin *parent, struct dpll_pin *pin,
				const struct dpll_pin_ops *ops, void *priv);

int dpll_device_change_ntf(struct dpll_device *dpll);

int dpll_pin_change_ntf(struct dpll_pin *pin);

#endif
