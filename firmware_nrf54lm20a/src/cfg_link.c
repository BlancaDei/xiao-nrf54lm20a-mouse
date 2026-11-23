/*
 * 配置通道实现（鼠标侧）
 *
 * 数据来源：dongle 的 PRX 把命令放在 ACK payload 里，鼠标 PTX 在
 * ESB_EVENT_RX_RECEIVED 里读到（见 esb_ptx.c），这里只负责解析和保存状态。
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "cfg_link.h"

#define CFG_MAGIC_LINK    0xC7

/* 命令码：与 dongle 的 hid_cfg.h 完全一致 */
enum cfg_cmd {
	CFG_CMD_GET_STATE    = 0x01,
	CFG_CMD_SET_DPI      = 0x02,
	CFG_CMD_SET_SLOTS    = 0x03,
	CFG_CMD_SET_ACTIVE   = 0x04,
	CFG_CMD_SET_RATE     = 0x05,
	CFG_CMD_SET_MODE     = 0x06,
	CFG_CMD_SET_SYNC     = 0x07,
	CFG_CMD_SET_DEBOUNCE = 0x08,
	CFG_CMD_SET_LIFTOFF  = 0x09,
};

#define CFG_DPI_MIN  100
#define CFG_DPI_MAX  16000

static struct {
	uint8_t active_slots;
	uint8_t active_slot;
	uint16_t report_rate;
	uint8_t system_mode;   /* 0 高速 / 1 竞技 */
	uint8_t motion_sync;
	uint8_t debounce;      /* 0 低 / 1 中 / 2 高 */
	uint8_t lift_off;      /* 0 低 / 1 中 / 2 高 */
	uint16_t dpi[CFG_LINK_DPI_SLOTS];
} state = {
	.active_slots = 1,
	.active_slot = 0,
	.report_rate = 1000,
	.system_mode = 0,
	.motion_sync = 1,
	.debounce = 0,
	.lift_off = 0,
	.dpi = { 1600, 3200, 6400, 8000, 12000, 16000 },
};

/* 空中包队列：ESB 回调只入队，主循环解析 */
K_MSGQ_DEFINE(air_q, 8, 8, 4);

static uint32_t pending_changes;

void cfg_link_init(void)
{
	k_msgq_purge(&air_q);
	pending_changes = 0;
	printk("cfg: link ready (dpi %u, %u Hz, slots %u)\n",
	       cfg_link_cpi(), state.report_rate, state.active_slots);
}

void cfg_link_on_air_packet(const uint8_t *data, uint8_t len)
{
	if (len < 5 || data[0] != CFG_MAGIC_LINK) {
		return;
	}

	/* 队列满就丢最旧包：设置是幂等的，下一包（dongle 会连发 3 次）能补上 */
	if (k_msgq_put(&air_q, data, K_NO_WAIT) != 0) {
		uint8_t drop[8];

		(void)k_msgq_get(&air_q, drop, K_NO_WAIT);
		(void)k_msgq_put(&air_q, data, K_NO_WAIT);
	}
}

/* 返回值表示这一条是否真的改变了状态（没变就不必重设硬件） */
static bool apply_packet(const uint8_t *pkt)
{
	uint8_t cmd = pkt[1];
	uint8_t index = pkt[2];
	uint16_t value = (uint16_t)(pkt[3] | (pkt[4] << 8));
	bool changed = false;

	switch (cmd) {
	case CFG_CMD_GET_STATE:
		return false;
	case CFG_CMD_SET_DPI:
		if (index >= CFG_LINK_DPI_SLOTS) {
			return false;
		}
		value = CLAMP(value, CFG_DPI_MIN, CFG_DPI_MAX);
		if (state.dpi[index] != value) {
			state.dpi[index] = value;
			changed = true;
			/* 只有当前生效挡位变了才需要动传感器 */
			if (index == state.active_slot) {
				pending_changes |= CFG_LINK_CHG_DPI;
			}
		}
		break;
	case CFG_CMD_SET_SLOTS:
		value = CLAMP(value, 1, CFG_LINK_DPI_SLOTS);
		if (state.active_slots != value) {
			state.active_slots = (uint8_t)value;
			if (state.active_slot >= state.active_slots) {
				state.active_slot = state.active_slots - 1;
				pending_changes |= CFG_LINK_CHG_DPI;
			}
			changed = true;
		}
		break;
	case CFG_CMD_SET_ACTIVE:
		if (value >= state.active_slots || state.active_slot == value) {
			return false;
		}
		state.active_slot = (uint8_t)value;
		pending_changes |= CFG_LINK_CHG_DPI;
		changed = true;
		break;
	case CFG_CMD_SET_RATE:
		if (!IN_RANGE(value, 1000, 8000) || state.report_rate == value) {
			return false;
		}
		state.report_rate = value;
		pending_changes |= CFG_LINK_CHG_RATE;
		changed = true;
		break;
	case CFG_CMD_SET_MODE:
		if (value > 1 || state.system_mode == value) {
			return false;
		}
		state.system_mode = (uint8_t)value;
		pending_changes |= CFG_LINK_CHG_MODE;
		changed = true;
		break;
	case CFG_CMD_SET_SYNC:
		if (value > 1 || state.motion_sync == value) {
			return false;
		}
		state.motion_sync = (uint8_t)value;
		pending_changes |= CFG_LINK_CHG_SYNC;
		changed = true;
		break;
	case CFG_CMD_SET_DEBOUNCE:
		if (value > 2 || state.debounce == value) {
			return false;
		}
		state.debounce = (uint8_t)value;
		pending_changes |= CFG_LINK_CHG_DEBOUNCE;
		changed = true;
		break;
	case CFG_CMD_SET_LIFTOFF:
		if (value > 2 || state.lift_off == value) {
			return false;
		}
		state.lift_off = (uint8_t)value;
		pending_changes |= CFG_LINK_CHG_LIFTOFF;
		changed = true;
		break;
	default:
		return false;
	}

	return changed;
}

uint32_t cfg_link_process(void)
{
	uint8_t pkt[8];

	while (k_msgq_get(&air_q, pkt, K_NO_WAIT) == 0) {
		(void)apply_packet(pkt);
	}

	uint32_t changes = pending_changes;

	pending_changes = 0;

	return changes;
}

uint8_t cfg_link_slots(void)
{
	return state.active_slots;
}

uint8_t cfg_link_active_slot(void)
{
	return state.active_slot;
}

uint16_t cfg_link_dpi(uint8_t index)
{
	return state.dpi[MIN(index, CFG_LINK_DPI_SLOTS - 1)];
}

uint16_t cfg_link_cpi(void)
{
	return state.dpi[state.active_slot];
}

uint16_t cfg_link_report_rate(void)
{
	return state.report_rate;
}

uint8_t cfg_link_system_mode(void)
{
	return state.system_mode;
}

bool cfg_link_motion_sync(void)
{
	return state.motion_sync != 0;
}

uint8_t cfg_link_debounce(void)
{
	return state.debounce;
}

uint8_t cfg_link_lift_off(void)
{
	return state.lift_off;
}

void cfg_link_set_active_slot(uint8_t index)
{
	if (index < state.active_slots && index != state.active_slot) {
		state.active_slot = index;
	}
}
