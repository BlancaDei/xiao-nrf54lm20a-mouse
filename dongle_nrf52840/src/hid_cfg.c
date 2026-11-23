/*
 * 厂商 HID 配置通道实现（dongle 侧）
 *
 * 设置改完立刻 NVS 落盘（不阻塞 USB 线程：写 Flash 走系统工作队列），
 * 同时把命令排进队列，由主循环 hid_cfg_poll() 一包一包推给鼠标。
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include "hid_cfg.h"

#if defined(CONFIG_APP_ESB)
#include "esb_rx.h"
#else
#include "ble_ctr.h"
#endif

/* ---- 落盘用的紧凑快照（换版本时用 version 判断，字段顺序不可改）---- */
struct hid_cfg_state {
	uint8_t version;
	uint8_t active_slots;
	uint8_t active_slot;
	uint16_t report_rate;
	uint8_t system_mode;   /* 0 高速 / 1 竞技 */
	uint8_t motion_sync;
	uint8_t debounce;      /* 0 低 / 1 中 / 2 高 */
	uint8_t lift_off;      /* 0 低 / 1 中 / 2 高 */
	uint16_t dpi[HID_CFG_DPI_SLOTS];
} __packed;

BUILD_ASSERT(sizeof(struct hid_cfg_state) == 21, "配置快照布局变了，需要升版本号");

static struct hid_cfg_state state = {
	.version = HID_CFG_VERSION,
	.active_slots = 1,
	.active_slot = 0,
	.report_rate = 1000,
	.system_mode = 0,
	.motion_sync = 1,
	.debounce = 0,
	.lift_off = 0,
	.dpi = { 1600, 3200, 6400, 8000, 12000, 16000 },
};

/* 待下发命令：[cmd, index, value_lo, value_hi] */
K_MSGQ_DEFINE(cmd_q, 4, 16, 4);

static const uint16_t rate_choices[] = { 1000, 2000, 4000, 8000 };

static uint8_t link_seq;
static bool unsupported_logged;
/* NVS 里读到的版本号（0 = 从未保存过）。用于新固件首次启动时做一次迁移：
 * v1 的默认静默高度是 2mm，v2 改成 1mm。 */
static uint8_t stored_version;

/* ---- 链路适配 ---- */
#if defined(CONFIG_APP_ESB)
#define link_send(pkt, len) esb_rx_cfg_send((pkt), (len))

static bool link_up(void)
{
	uint32_t total, err;

	esb_rx_stats(&total, &err);
	/* 鼠标静止时不发位移包，只有 500 ms 一次的保活包，所以判定窗口要
	 * 大于保活间隔，否则驱动里的链路状态会一直在"已连接/未连接"之间跳。 */
	return total > 0 && (k_uptime_get() - esb_rx_last_ms()) <= 1500;
}

static uint8_t link_kind(void)
{
	return 1;
}
#else
#define link_send(pkt, len) ble_ctr_cfg_send((pkt), (len))

static bool link_up(void)
{
	int64_t last = ble_ctr_last_ms();

	return last > 0 && (k_uptime_get() - last) <= 200;
}

static uint8_t link_kind(void)
{
	return 2;
}
#endif

/* ---- 持久化 ---- */
#if IS_ENABLED(CONFIG_SETTINGS)
#define CFG_SETTINGS_KEY "mouse/cfg"

static void save_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)settings_save_one(CFG_SETTINGS_KEY, &state, sizeof(state));
}

static K_WORK_DEFINE(save_work, save_work_fn);

static int cfg_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			    void *cb_arg)
{
	if (strcmp(name, "cfg") != 0) {
		return -ENOENT;
	}
	if (len != sizeof(state)) {
		return -EINVAL;
	}
	if (read_cb(cb_arg, &state, sizeof(state)) < 0) {
		return -EINVAL;
	}
	stored_version = state.version;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(hid_cfg, "mouse", NULL, cfg_settings_set, NULL, NULL);
#endif

static void state_sanitize(void)
{
	bool rate_ok = false;

	state.version = HID_CFG_VERSION;

	if (state.active_slots < 1 || state.active_slots > HID_CFG_DPI_SLOTS) {
		state.active_slots = 1;
	}
	if (state.active_slot >= state.active_slots) {
		state.active_slot = state.active_slots - 1;
	}
	if (state.system_mode > 1) {
		state.system_mode = 0;
	}
	if (state.motion_sync > 1) {
		state.motion_sync = 1;
	}
	if (state.debounce > 2) {
		state.debounce = 0;
	}
	if (state.lift_off > 2) {
		state.lift_off = 0;
	}

	for (size_t i = 0; i < ARRAY_SIZE(rate_choices); i++) {
		if (state.report_rate == rate_choices[i]) {
			rate_ok = true;
		}
	}
	if (!rate_ok) {
		state.report_rate = 1000;
	}

	for (int i = 0; i < HID_CFG_DPI_SLOTS; i++) {
		state.dpi[i] = CLAMP(state.dpi[i], HID_CFG_DPI_MIN, HID_CFG_DPI_MAX);
	}
}

/* ---- 命令队列 ---- */
static void queue_cmd(uint8_t cmd, uint8_t index, uint16_t value)
{
	uint8_t pkt[4] = { cmd, index, (uint8_t)(value & 0xFF), (uint8_t)(value >> 8) };

	/* 队列满就丢最旧的一条：设置本身已经在 state 里，下次重连会整套补发 */
	if (k_msgq_put(&cmd_q, pkt, K_NO_WAIT) != 0) {
		uint8_t drop[4];

		(void)k_msgq_get(&cmd_q, drop, K_NO_WAIT);
		(void)k_msgq_put(&cmd_q, pkt, K_NO_WAIT);
	}
}

/* 链路刚连上：把整套设置补一遍，保证鼠标状态与 dongle 一致 */
static void queue_all(void)
{
	k_msgq_purge(&cmd_q);

	for (int i = 0; i < HID_CFG_DPI_SLOTS; i++) {
		queue_cmd(HID_CFG_CMD_SET_DPI, (uint8_t)i, state.dpi[i]);
	}
	queue_cmd(HID_CFG_CMD_SET_SLOTS, 0, state.active_slots);
	queue_cmd(HID_CFG_CMD_SET_ACTIVE, 0, state.active_slot);
	queue_cmd(HID_CFG_CMD_SET_RATE, 0, state.report_rate);
	queue_cmd(HID_CFG_CMD_SET_MODE, 0, state.system_mode);
	queue_cmd(HID_CFG_CMD_SET_SYNC, 0, state.motion_sync);
	queue_cmd(HID_CFG_CMD_SET_DEBOUNCE, 0, state.debounce);
	queue_cmd(HID_CFG_CMD_SET_LIFTOFF, 0, state.lift_off);
}

void hid_cfg_init(void)
{
#if IS_ENABLED(CONFIG_SETTINGS)
	int err = settings_subsys_init();

	if (err != 0) {
		printk("cfg: settings init failed %d\n", err);
	}
	err = settings_load_subtree("mouse");
	if (err != 0) {
		printk("cfg: settings load failed %d\n", err);
	}
#endif
	state_sanitize();

	if (stored_version != HID_CFG_VERSION) {
		/* 版本迁移：v1 默认静默高度是 2mm（level 1），v2 起默认 1mm（level 0）。
		 * 旧 NVS 里如果是"中"，会在这个版本被一次性改成"低"，
		 * 之后用户再在驱动里改成"中"就按用户的选择保存。 */
		state.lift_off = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
		k_work_submit(&save_work);
#endif
	}

	printk("cfg: slots %u active %u rate %u mode %u sync %u deb %u lift %u dpi[0] %u\n",
	       state.active_slots, state.active_slot, state.report_rate, state.system_mode,
	       state.motion_sync, state.debounce, state.lift_off, state.dpi[0]);
}

int hid_cfg_get_report(uint8_t *buf, uint16_t len)
{
	if (len < HID_CFG_REPORT_LEN) {
		return -EINVAL;
	}

	memset(buf, 0, HID_CFG_REPORT_LEN);
	buf[0] = HID_CFG_MAGIC_GET;
	buf[1] = HID_CFG_VERSION;
	buf[2] = state.active_slots;
	buf[3] = state.active_slot;
	buf[4] = (uint8_t)(state.report_rate & 0xFF);
	buf[5] = (uint8_t)(state.report_rate >> 8);
	buf[6] = state.system_mode;
	buf[7] = state.motion_sync;
	buf[8] = state.debounce;
	buf[9] = state.lift_off;

	for (int i = 0; i < HID_CFG_DPI_SLOTS; i++) {
		buf[10 + i * 2] = (uint8_t)(state.dpi[i] & 0xFF);
		buf[11 + i * 2] = (uint8_t)(state.dpi[i] >> 8);
	}

	buf[22] = link_up() ? link_kind() : 0;

	return HID_CFG_REPORT_LEN;
}

int hid_cfg_set_report(const uint8_t *buf, uint16_t len)
{
	uint8_t cmd;
	uint8_t index;
	uint16_t value;

	/* 宿主（Windows 的 hidclass）在带 Report ID 的描述符下，数据阶段的首字节
	 * 可能仍是 Report ID。这里两种布局都接受。 */
	if (len >= 6 && buf[0] == HID_CFG_REPORT_ID && buf[1] == HID_CFG_MAGIC_SET) {
		buf++;
		len--;
	}

	if (len < 5 || buf[0] != HID_CFG_MAGIC_SET) {
		return -EINVAL;
	}

	cmd = buf[1];
	index = buf[2];
	value = (uint16_t)(buf[3] | (buf[4] << 8));

	switch (cmd) {
	case HID_CFG_CMD_GET_STATE:
		return 0;
	case HID_CFG_CMD_SET_DPI:
		if (index >= HID_CFG_DPI_SLOTS) {
			return -EINVAL;
		}
		value = CLAMP(value, HID_CFG_DPI_MIN, HID_CFG_DPI_MAX);
		if (state.dpi[index] == value) {
			return 0;
		}
		state.dpi[index] = value;
		break;
	case HID_CFG_CMD_SET_SLOTS:
		value = CLAMP(value, 1, HID_CFG_DPI_SLOTS);
		if (state.active_slots == value) {
			return 0;
		}
		state.active_slots = (uint8_t)value;
		if (state.active_slot >= state.active_slots) {
			state.active_slot = state.active_slots - 1;
		}
		break;
	case HID_CFG_CMD_SET_ACTIVE:
		if (value >= state.active_slots) {
			return -EINVAL;
		}
		if (state.active_slot == value) {
			return 0;
		}
		state.active_slot = (uint8_t)value;
		break;
	case HID_CFG_CMD_SET_RATE:
		if (!IN_RANGE(value, 1000, 8000)) {
			return -EINVAL;
		}
		if (state.report_rate == value) {
			return 0;
		}
		state.report_rate = value;
		break;
	case HID_CFG_CMD_SET_MODE:
		if (value > 1) {
			return -EINVAL;
		}
		if (state.system_mode == value) {
			return 0;
		}
		state.system_mode = (uint8_t)value;
		break;
	case HID_CFG_CMD_SET_SYNC:
		if (value > 1) {
			return -EINVAL;
		}
		if (state.motion_sync == value) {
			return 0;
		}
		state.motion_sync = (uint8_t)value;
		break;
	case HID_CFG_CMD_SET_DEBOUNCE:
		if (value > 2) {
			return -EINVAL;
		}
		if (state.debounce == value) {
			return 0;
		}
		state.debounce = (uint8_t)value;
		break;
	case HID_CFG_CMD_SET_LIFTOFF:
		if (value > 2) {
			return -EINVAL;
		}
		if (state.lift_off == value) {
			return 0;
		}
		state.lift_off = (uint8_t)value;
		break;
	default:
		return -ENOTSUP;
	}

#if IS_ENABLED(CONFIG_SETTINGS)
	k_work_submit(&save_work);
#endif
	queue_cmd(cmd, index, value);

	return 0;
}

void hid_cfg_poll(void)
{
	static bool was_up;
	static bool synced;
	static int64_t down_since;

	static uint8_t tx_cmd[4];
	static bool tx_busy;
	static uint8_t tx_left;
	static int64_t tx_last_ms;

	bool up = link_up();
	int64_t now = k_uptime_get();

	if (!up) {
		if (was_up) {
			down_since = now;
			was_up = false;
		}
		/* 不动 tx_*：半途的命令等链路恢复后接着发，不丢 */
		return;
	}

	if (!was_up) {
		was_up = true;

		/* 鼠标静止 200 ms 也会被判成"链路断开"，那种情况只把待发命令补完即可；
		 * 真正断过（掉出范围、关机）2 s 以上才整套重发一遍。
		 *
		 * synced 处理"dongle 刚上电、鼠标已经在发保活包"的首次连接：
		 * 鼠标端设置只存在 RAM，Dongle 里持久化的 DPI/回报率等必须在这里
		 * 主动补一遍，否则两边会一直用各自的默认值（旧版本漏掉了这一步）。 */
		bool first_link = !synced;
		bool long_gap = (down_since != 0) && ((now - down_since) >= 2000);

		if (first_link || long_gap) {
			synced = true;
			queue_all();
			printk("cfg: link up, sync all settings (%s)\n",
			       first_link ? "first link" : "link recovered");
		}
	}

	if (!tx_busy) {
		if (k_msgq_get(&cmd_q, tx_cmd, K_NO_WAIT) != 0) {
			return;
		}
		tx_busy = true;
		tx_left = 3;
		tx_last_ms = 0;
	}

	if (now - tx_last_ms < 20) {
		return;
	}

	uint8_t pkt[8] = {
		HID_CFG_MAGIC_LINK, tx_cmd[0], tx_cmd[1], tx_cmd[2], tx_cmd[3], link_seq++, 0, 0,
	};

	int err = link_send(pkt, sizeof(pkt));

	if (err == -ENOTSUP) {
		if (!unsupported_logged) {
			printk("cfg: 当前链路没有配置通道，设置只保存在 dongle\n");
			unsupported_logged = true;
		}
		tx_left = 1;
	} else if (err == -ENOMEM) {
		/* ACK 队列满：鼠标静止时不发包，队列里的副本要等它开始移动才会发出去。
		 * 这不是错误，直接等下一轮重试即可。 */
	} else if (err != 0) {
		printk("cfg: link send failed %d\n", err);
	}

	tx_last_ms = now;

	if (--tx_left == 0) {
		tx_busy = false;
	}
}
